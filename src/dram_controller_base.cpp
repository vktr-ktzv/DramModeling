#include "dram_controller_base.h"

namespace dram
{

DramControllerBase::DramControllerBase(sc_core::sc_module_name name, unsigned size_bytes)
    : sc_core::sc_module(name), storage_(size_bytes)
{
    for (int i = 0; i < kPorts; ++i)
    {
        socket[i].register_b_transport(this, &DramControllerBase::b_transport, i);
    }
}

DramControllerBase::Dir DramControllerBase::next_request(Request& req)
{
    // Конфликт: в момент выбора готовы обе очереди.
    if (!read_queue_.empty() && !write_queue_.empty())
    {
        ++conflicts_;
    }

    Dir dir;
    if (read_queue_.empty())
    {
        dir = Dir::Write;
    }
    else if (write_queue_.empty())
    {
        dir = Dir::Read;
    }
    else
    {
        dir = (rr_++ % 2) == 1 ? Dir::Write : Dir::Read;
    }

    std::deque<Request>& queue = (dir == Dir::Write) ? write_queue_ : read_queue_;
    req = queue.front();
    queue.pop_front();
    return dir;
}

void DramControllerBase::finish_request(const Request& req, Dir dir)
{
    req.trans->set_response_status(storage_.access(*req.trans));
    if (dir == Dir::Write)
    {
        ++write_count_;
    }
    else
    {
        ++read_count_;
    }
    req.done->notify(sc_core::SC_ZERO_TIME);
}

void DramControllerBase::b_transport(int port, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay)
{
    sc_core::sc_event done;
    const Request req{&trans, port, &done};

    if (trans.get_command() == tlm::TLM_WRITE_COMMAND)
    {
        write_queue_.push_back(req);
    }
    else
    {
        read_queue_.push_back(req);
    }
    new_request_.notify(sc_core::SC_ZERO_TIME);

    // ждём, пока планировщик обслужит именно этот запрос.
    wait(done);

    delay = sc_core::SC_ZERO_TIME;
}

} // namespace dram
