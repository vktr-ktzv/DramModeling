#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "dram_controller_async.h"
#include "dram_controller_sync.h"

using namespace dram;

struct BenchState
{
    unsigned remaining = 0;
    sc_core::sc_event done;
    sc_core::sc_event start;
};

class ControllerInitiator : public sc_core::sc_module
{
  public:
    tlm_utils::simple_initiator_socket<ControllerInitiator> socket;

    ControllerInitiator(sc_core::sc_module_name name, const char *tag, int id, BenchState *state, bool wait_start)
        : sc_core::sc_module(name), socket("socket"), tag_(tag), id_(id), state_(state), wait_start_(wait_start)
    {
        SC_THREAD(run);
    }

    void run()
    {
        if (wait_start_)
        {
            wait(state_->start);
        }

        const std::uint64_t base = static_cast<std::uint64_t>(id_) * 0x100;
        const unsigned words = 16;
        bool ok = true;

        for (unsigned i = 0; i < words; ++i)
        {
            const std::uint64_t addr = base + i * 4;
            write_word(addr, value(i));
            if (read_word(addr) != value(i))
            {
                ok = false;
            }
        }

        std::cout << tag_ << " initiator " << id_ << ": " << (ok ? "OK" : "MISMATCH") << std::endl;

        if (state_->remaining > 0 && --state_->remaining == 0)
        {
            state_->done.notify(sc_core::SC_ZERO_TIME);
        }
    }

  private:
    std::uint32_t value(unsigned i) const { return (static_cast<std::uint32_t>(id_) << 24) | i; }

    void write_word(std::uint64_t addr, std::uint32_t value)
    {
        unsigned char buf[4];
        std::memcpy(buf, &value, sizeof(value));
        transact(tlm::TLM_WRITE_COMMAND, addr, buf);
    }

    std::uint32_t read_word(std::uint64_t addr)
    {
        unsigned char buf[4] = {0, 0, 0, 0};
        transact(tlm::TLM_READ_COMMAND, addr, buf);
        std::uint32_t value = 0;
        std::memcpy(&value, buf, sizeof(value));
        return value;
    }

    void transact(tlm::tlm_command cmd, std::uint64_t addr, unsigned char *data)
    {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

        trans.set_command(cmd);
        trans.set_address(addr);
        trans.set_data_ptr(data);
        trans.set_data_length(4);
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(nullptr);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        socket->b_transport(trans, delay);
        // b_transport блокирующий: время уже продвинуто внутри контроллера.
    }

    const char *tag_;
    int id_;
    BenchState *state_;
    bool wait_start_;
};

class Director : public sc_core::sc_module
{
  public:
    Director(
        sc_core::sc_module_name name, DramControllerBase *async_ctrl, DramControllerBase *sync_ctrl,
        BenchState *async_state, BenchState *sync_state)
        : sc_core::sc_module(name), async_ctrl_(async_ctrl), sync_ctrl_(sync_ctrl), async_state_(async_state),
          sync_state_(sync_state)
    {
        SC_THREAD(run);
    }

    void run()
    {
        if (async_state_->remaining > 0)
        {
            wait(async_state_->done);
        }
        print("async", *async_ctrl_);

        sync_state_->start.notify(sc_core::SC_ZERO_TIME);
        if (sync_state_->remaining > 0)
        {
            wait(sync_state_->done);
        }
        print("sync", *sync_ctrl_);

        sc_core::sc_stop();
    }

  private:
    static void print(const char *tag, DramControllerBase &ctrl)
    {
        std::cout << tag << " controller: reads=" << ctrl.read_count() << " writes=" << ctrl.write_count()
                  << " conflicts=" << ctrl.conflicts() << std::endl;
    }

    DramControllerBase *async_ctrl_;
    DramControllerBase *sync_ctrl_;
    BenchState *async_state_;
    BenchState *sync_state_;
};

int sc_main(int, char **)
{
    using namespace sc_core;

    sc_clock clk("clk", sc_time(10, SC_NS));

    DramControllerAsync async_ctrl("async_ctrl", 1024, sc_time(70, SC_NS), sc_time(50, SC_NS), sc_time(20, SC_NS));
    DramControllerSync sync_ctrl("sync_ctrl", 1024, sc_time(10, SC_NS), 3, 2, 1);
    sync_ctrl.clk(clk);

    BenchState async_state;
    BenchState sync_state;
    async_state.remaining = DramControllerBase::kPorts;
    sync_state.remaining = DramControllerBase::kPorts;

    std::vector<std::unique_ptr<ControllerInitiator>> initiators;
    initiators.reserve(2 * DramControllerBase::kPorts);
    for (int i = 0; i < DramControllerBase::kPorts; ++i)
    {
        auto init = std::make_unique<ControllerInitiator>(sc_gen_unique_name("ainit"), "async", i, &async_state, false);
        init->socket.bind(async_ctrl.socket[i]);
        initiators.push_back(std::move(init));
    }

    for (int i = 0; i < DramControllerBase::kPorts; ++i)
    {
        auto init = std::make_unique<ControllerInitiator>(sc_gen_unique_name("sinit"), "sync", i, &sync_state, true);
        init->socket.bind(sync_ctrl.socket[i]);
        initiators.push_back(std::move(init));
    }

    Director director("director", &async_ctrl, &sync_ctrl, &async_state, &sync_state);

    const sc_time start = sc_time_stamp();
    sc_start();
    const sc_time elapsed = sc_time_stamp() - start;

    std::cout << "Total simulation time: " << elapsed << std::endl;

    return 0;
}
