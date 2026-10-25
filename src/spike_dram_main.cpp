#include "config.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <systemc.h>

#include "devices.h"
#include "sim.h"

#include "dram_chip.h"
#include "dram_controller_async.h"
#include "dram_controller_sync.h"
#include "simple_memory.h"
#include "spike_dram_device.h"
#include "store_buffer.h"

namespace
{

using namespace dram;

// Диапазон, который обслуживает TLM-контроллер (MMIO-устройство Spike).
constexpr std::uint64_t kDramBase = 0x4000'0000ULL;
constexpr std::uint64_t kDramSize = 0x1000'0000ULL; // 256 MiB

// ОЗУ Spike: код/стек/tohost.
constexpr std::uint64_t kMemBase = 0x8000'0000ULL;
constexpr std::uint64_t kMemSize = 0x8000'0000ULL; // 2 GiB

// SystemC-хост владеет планировщиком: каждый вызов idle() исполняет ровно
// одну инструкцию и сдвигает время SystemC на такт, поэтому время не
// привязано только к обращениям в память.
class CosimSim : public sim_t
{
  public:
    CosimSim(
        const cfg_t* cfg, bool halted, std::vector<std::pair<reg_t, abstract_mem_t*>> mems,
        const std::vector<device_factory_sargs_t>& plugin_device_factories, bool dtb_discovery,
        const std::vector<std::string>& args, const debug_module_config_t& dm_config, const char* log_path,
        bool dtb_enabled, const char* dtb_file, bool socket_enabled, FILE* cmd_file,
        std::optional<unsigned long long> instruction_limit, sc_core::sc_time cycle)
        : sim_t(
              cfg, halted, mems, plugin_device_factories, dtb_discovery, args, dm_config, log_path, dtb_enabled,
              dtb_file, socket_enabled, cmd_file, instruction_limit),
          cycle_(cycle), insn_limit_(instruction_limit.value_or(0))
    {
    }

    unsigned long long insns() const { return insns_; }

  private:
    void idle() override
    {
        if (done())
        {
            return;
        }

        if (insn_limit_ != 0 && insns_ >= insn_limit_)
        {
            std::cerr << "instruction limit reached" << std::endl;
            htif_exit(3);
            return;
        }

        get_core(0)->step(1);
        ++insns_;
        sc_core::wait(cycle_);
    }

    sc_core::sc_time cycle_;
    unsigned long long insn_limit_;
    unsigned long long insns_ = 0;
};

class SpikeRunner : public sc_core::sc_module
{
  public:
    SpikeRunner(sc_core::sc_module_name name, CosimSim* sim, StoreBuffer* buffer)
        : sc_core::sc_module(name), sim_(sim), buffer_(buffer)
    {
        SC_THREAD(run);
    }

    int exit_code() const { return exit_code_; }

  private:
    void run()
    {
        exit_code_ = sim_->run();
        // Дожидаемся, пока отложенные записи дойдут до DRAM
        buffer_->flush();
        std::cout << "Spike finished: insns=" << sim_->insns() << ", time=" << sc_core::sc_time_stamp()
                  << ", exit code = " << exit_code_ << std::endl;
        sc_core::sc_stop();
    }

    CosimSim* sim_;
    StoreBuffer* buffer_;
    int exit_code_{0};
};

} // namespace

int sc_main(int argc, char** argv)
{
    using namespace sc_core;

    bool use_sync = false;
    std::string elf = "tests/spike/dram_test.elf";
    std::string trace_path;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--sync")
        {
            use_sync = true;
        }
        else if (arg == "--async")
        {
            use_sync = false;
        }
        else if (arg == "--trace")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--trace requires a file path" << std::endl;
                return 1;
            }
            trace_path = argv[++i];
        }
        else
        {
            elf = arg;
        }
    }
    if (trace_path.empty())
    {
        trace_path = use_sync ? "dram_trace_sync.csv" : "dram_trace_async.csv";
    }

    // Один и тот же такт задаёт и период инструкции, и (для sync) период clk.
    const sc_time clock_period(10, SC_NS);

    // Тайминги детальной банковой модели (только async): точные наносекунды.
    const DramTiming timing{
        sc_time(12, SC_NS), sc_time(22, SC_NS), sc_time(12, SC_NS), sc_time(12, SC_NS)};

    // Простая синхронная модель: фиксированные задержки в тактах, без банков.
    // Это независимые параметры приближённой модели (не выводятся из DramTiming).
    // Turnaround выровнен с async: 2 такта по 10 нс = 20 нс.
    constexpr unsigned kSyncReadCycles = 3;
    constexpr unsigned kSyncWriteCycles = 2;
    constexpr unsigned kSyncSwitchCycles = 2;

    std::unique_ptr<sc_clock> clk;
    std::unique_ptr<DramControllerBase> ctrl;
    std::unique_ptr<DramChip> chip;
    std::unique_ptr<SimpleMemory> simple_memory;
    if (use_sync)
    {
        auto sync = std::make_unique<DramControllerSync>(
            "dram_ctrl", clock_period, kSyncReadCycles, kSyncWriteCycles, kSyncSwitchCycles);
        clk = std::make_unique<sc_clock>("clk", clock_period);
        sync->clk(*clk);
        simple_memory = std::make_unique<SimpleMemory>("simple_memory", kDramSize);
        sync->mem_socket.bind(simple_memory->socket);
        ctrl = std::move(sync);
    }
    else
    {
        chip = std::make_unique<DramChip>("dram_chip", kDramSize, timing);
        auto async = std::make_unique<DramControllerAsync>("dram_ctrl", sc_time(20, SC_NS));
        async->mem_socket.bind(chip->socket);
        ctrl = std::move(async);
    }

    // Доступы Spike идут через буфер: чтения — порт 0, записи  — порт 1.
    StoreBuffer buffer("store_buffer");
    buffer.read_socket.bind(ctrl->socket[0]);
    buffer.write_socket.bind(ctrl->socket[1]);

    auto device = std::make_shared<SpikeTlmDevice>(
        kDramSize,
        [buf = &buffer](std::uint64_t offset, std::uint8_t* data, std::size_t len)
        { return buf->load(offset, data, len); },
        [buf = &buffer](std::uint64_t offset, const std::uint8_t* data, std::size_t len)
        { return buf->store(offset, data, len); });

    cfg_t cfg;
    cfg.mem_layout = {mem_cfg_t(kMemBase, kMemSize)};
    cfg.hartids = {0};
    cfg.explicit_hartids = true;
    cfg.start_pc.set_global(kMemBase);

    std::vector<std::pair<reg_t, abstract_mem_t*>> mems;
    for (const auto& m : cfg.mem_layout)
    {
        mems.push_back({m.get_base(), new mem_t(m.get_size())});
    }

    debug_module_config_t dm_config;
    std::vector<std::string> htif_args{elf};

    constexpr unsigned long long kMaxInsns = 50000000ULL;
    const sc_time cycle = clock_period;
    int exit_code = 0;

    {
        CosimSim sim(
            &cfg, /*halted=*/false, mems,
            /*plugin_device_factories=*/{}, /*dtb_discovery=*/false, htif_args, dm_config, /*log_path=*/nullptr,
            /*dtb_enabled=*/false,
            /*dtb_file=*/nullptr, /*socket_enabled=*/false,
            /*cmd_file=*/nullptr, /*instruction_limit=*/kMaxInsns, cycle);

        sim.add_device(kDramBase, device);

        SpikeRunner runner("runner", &sim, &buffer);

        sc_start();
        exit_code = runner.exit_code();

        std::cout << (use_sync ? "sync" : "async") << " controller: reads=" << ctrl->read_count()
                  << " writes=" << ctrl->write_count() << " conflicts=" << ctrl->conflicts() << std::endl;

        if (use_sync)
        {
            const unsigned accesses = ctrl->read_count() + ctrl->write_count();
            const double total_latency_ns =
                (static_cast<double>(ctrl->read_count()) * kSyncReadCycles +
                 static_cast<double>(ctrl->write_count()) * kSyncWriteCycles) *
                (clock_period / sc_time(1, SC_NS));
            const double avg_latency = accesses == 0 ? 0.0 : total_latency_ns / accesses;
            std::cout << "sync memory: reads=" << ctrl->read_count() << " writes=" << ctrl->write_count()
                      << " read_latency=" << (clock_period * static_cast<double>(kSyncReadCycles)) / sc_time(1, SC_NS)
                      << " ns write_latency="
                      << (clock_period * static_cast<double>(kSyncWriteCycles)) / sc_time(1, SC_NS)
                      << " ns avg_latency=" << avg_latency << " ns" << std::endl;

            simple_memory->dump_trace(trace_path);
            std::cout << "trace -> " << trace_path << " (" << simple_memory->accesses().size() << " accesses)"
                      << std::endl;
        }
        else
        {
            const unsigned accesses = chip->reads() + chip->writes();
            const double avg_latency =
                accesses == 0 ? 0.0 : (chip->total_latency() / sc_time(1, SC_NS)) / accesses;
            std::cout << "async dram: reads=" << chip->reads() << " writes=" << chip->writes()
                      << " activations=" << chip->activations() << " precharges=" << chip->precharges()
                      << " row_hits=" << chip->row_hits() << " row_misses=" << chip->row_misses()
                      << " row_empties=" << chip->row_empties() << " avg_latency=" << avg_latency << " ns"
                      << std::endl;

            chip->dump_trace(trace_path);
            std::cout << "trace -> " << trace_path << " (" << chip->trace_entries().size() << " commands)"
                      << std::endl;
        }
    }

    for (auto& m : mems)
    {
        delete m.second;
    }
    return exit_code;
}
