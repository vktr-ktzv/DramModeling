#include "dram_controller_base.h"

namespace dram
{

DramControllerBase::DramControllerBase(sc_core::sc_module_name name)
    : sc_core::sc_module(name), mem_socket("mem_socket")
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

sc_core::sc_time DramControllerBase::issue(const Request& req)
{
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    // Блокирующий LT-вызов в модель памяти: DramChip обновляет банки и
    // возвращает задержку доступа, SimpleMemory применяет данные и возвращает
    // ноль. Время продвигает уже serve().
    mem_socket->b_transport(*req.trans, delay);
    return delay;
}

void DramControllerBase::finish_request(const Request& req, Dir dir)
{
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

void DramControllerBase::b_transport(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay)
{
    const tlm::tlm_command cmd = trans.get_command();
    if (cmd != tlm::TLM_READ_COMMAND && cmd != tlm::TLM_WRITE_COMMAND)
    {
        trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        delay = sc_core::SC_ZERO_TIME;
        return;
    }

    sc_core::sc_event done;
    const Request req{&trans, &done};

    if (cmd == tlm::TLM_WRITE_COMMAND)
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
