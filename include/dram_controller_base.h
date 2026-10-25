#pragma once

#include <deque>

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace dram
{

// Общая часть контроллера: два tagged target-сокета (порт 0 — чтения,
// порт 1 — записи), очереди, round-robin арбитраж и учёт конфликтов.
// Сам контроллер данные не хранит: он лишь арбитрирует и через mem_socket
// передаёт запрос модели памяти (DramChip — детальной, SimpleMemory — простой),
// получая от неё задержку доступа (для SimpleMemory она нулевая).
class DramControllerBase : public sc_core::sc_module
{
  public:
    static constexpr int kPorts{2};

    tlm_utils::simple_target_socket_tagged<DramControllerBase> socket[kPorts];
    tlm_utils::simple_initiator_socket<DramControllerBase> mem_socket;

    unsigned read_count() const { return read_count_; }
    unsigned write_count() const { return write_count_; }
    unsigned conflicts() const { return conflicts_; }

  protected:
    struct Request
    {
        tlm::tlm_generic_payload* trans;
        sc_core::sc_event* done;
    };

    enum class Dir
    {
        Read,
        Write
    };

    explicit DramControllerBase(sc_core::sc_module_name name);

    bool has_request() const { return !read_queue_.empty() || !write_queue_.empty(); }

    Dir next_request(Request& req);

    // Выдаёт запрос в модель памяти и возвращает её задержку доступа.
    // Для DramChip состояние банков и трасса обновляются на её стороне;
    // SimpleMemory только применяет данные и возвращает ноль.
    sc_core::sc_time issue(const Request& req);

    void finish_request(const Request& req, Dir dir);

    bool turnaround(Dir dir) const { return have_last_ && ((dir == Dir::Write) != last_write_); }

    void remember(Dir dir)
    {
        last_write_ = (dir == Dir::Write);
        have_last_ = true;
    }

    sc_core::sc_event new_request_;

  private:
    // Первый аргумент — тег порта от simple_target_socket_tagged; он никем не
    // читается, очереди выбираются по trans.get_command().
    void b_transport(int tag, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay);
    virtual void serve() = 0;

    std::deque<Request> read_queue_;
    std::deque<Request> write_queue_;

    bool have_last_ = false;
    bool last_write_ = false;
    int rr_ = 0;

    unsigned read_count_ = 0;
    unsigned write_count_ = 0;
    unsigned conflicts_ = 0;
};

} // namespace dram
