#include "config.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "devices.h"
#include "sim.h"

#include "dram_controller_async.h"
#include "dram_controller_sync.h"
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

// TLM-инициатор: превращает MMIO-доступ Spike в b_transport контроллера.
class TlmBridge : public sc_core::sc_module
{
  public:
    tlm_utils::simple_initiator_socket<TlmBridge> socket;

    explicit TlmBridge(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}

    bool transfer(bool write, std::uint64_t addr, std::uint8_t* data, std::size_t len)
    {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        trans.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        trans.set_address(addr);
        trans.set_data_ptr(data);
        trans.set_data_length(static_cast<unsigned>(len));
        trans.set_streaming_width(static_cast<unsigned>(len));
        trans.set_byte_enable_ptr(nullptr);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        // Блокирующий: внутри контроллера время уже продвигается serve().
        socket->b_transport(trans, delay);
        return trans.get_response_status() == tlm::TLM_OK_RESPONSE;
    }
};

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

// Гоняет Spike внутри SystemC-потока: тогда блокирующий b_transport
// (wait на событии) работает легально.
class SpikeRunner : public sc_core::sc_module
{
  public:
    SpikeRunner(sc_core::sc_module_name name, CosimSim* sim, StoreBuffer* buffer)
        : sc_core::sc_module(name), sim_(sim), buffer_(buffer)
    {
        SC_THREAD(run);
    }

  private:
    void run()
    {
        const int code = sim_->run();
        // Дожидаемся, пока отложенные записи дойдут до DRAM, иначе финальный
        // снимок памяти будет неполным.
        buffer_->flush();
        std::cout << "Spike finished: insns=" << sim_->insns() << ", time=" << sc_core::sc_time_stamp()
                  << ", exit code = " << code << std::endl;
        sc_core::sc_stop();
    }

    CosimSim* sim_;
    StoreBuffer* buffer_;
};

} // namespace

int sc_main(int argc, char** argv)
{
    using namespace sc_core;

    bool use_sync = false;
    std::string elf = "tests/spike/dram_test.elf";
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
        else
        {
            elf = arg;
        }
    }

    // Один и тот же такт задаёт и период инструкции, и (для sync) период clk.
    const sc_time clock_period(10, SC_NS);

    std::unique_ptr<sc_clock> clk;
    std::unique_ptr<DramControllerBase> ctrl;
    if (use_sync)
    {
        auto sync = std::make_unique<DramControllerSync>("dram_ctrl", kDramSize, clock_period, 3, 2, 1);
        clk = std::make_unique<sc_clock>("clk", clock_period);
        sync->clk(*clk);
        ctrl = std::move(sync);
    }
    else
    {
        ctrl = std::make_unique<DramControllerAsync>(
            "dram_ctrl", kDramSize, sc_time(70, SC_NS), sc_time(50, SC_NS), sc_time(20, SC_NS));
    }

    // Доступы Spike идут через write-back буфер: чтения — порт 0, записи
    // (несколько lane параллельно) — порт 1. Оставшиеся target-сокеты
    // контроллера тоже должны быть связаны — заводим пустые мосты.
    StoreBuffer buffer("store_buffer");
    buffer.read_socket.bind(ctrl->socket[0]);
    buffer.write_socket.bind(ctrl->socket[1]);

    std::vector<std::unique_ptr<TlmBridge>> bridges;
    for (int i = 2; i < DramControllerBase::kPorts; ++i)
    {
        auto b = std::make_unique<TlmBridge>(sc_gen_unique_name("bridge"));
        b->socket.bind(ctrl->socket[i]);
        bridges.push_back(std::move(b));
    }

    auto device = std::make_shared<SpikeTlmDevice>(
        kDramSize, [buf = &buffer](bool write, std::uint64_t offset, std::uint8_t* data, std::size_t len)
        { return write ? buf->store(offset, data, len) : buf->load(offset, data, len); });

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

        std::cout << (use_sync ? "sync" : "async") << " controller: reads=" << ctrl->read_count()
                  << " writes=" << ctrl->write_count() << " conflicts=" << ctrl->conflicts() << std::endl;

        std::cout << "DRAM[0..29]:";
        for (unsigned i = 0; i < 30; ++i)
        {
            std::uint64_t value = 0;
            for (unsigned b = 0; b < 8; ++b)
            {
                value |= static_cast<std::uint64_t>(ctrl->peek(i * 8 + b)) << (8 * b);
            }
            std::cout << ' ' << value;
        }
        std::cout << std::endl;
    }

    for (auto& m : mems)
    {
        delete m.second;
    }
    return 0;
}
