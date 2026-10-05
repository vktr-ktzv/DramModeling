#pragma once

#include <cstdint>
#include <deque>

#include "dram_common.h"

namespace dram
{

class DramControllerBase : public sc_core::sc_module
{
  public:
    static constexpr int kPorts{4};

    tlm_utils::simple_target_socket_tagged<DramControllerBase> socket[kPorts];

    unsigned read_count() const { return read_count_; }
    unsigned write_count() const { return write_count_; }
    unsigned conflicts() const { return conflicts_; }

  protected:
    struct Request
    {
        tlm::tlm_generic_payload *trans;
        int port;
        sc_core::sc_event *done;
    };

    enum class Dir
    {
        Read,
        Write
    };

    DramControllerBase(sc_core::sc_module_name name, unsigned size_bytes);

    bool has_request() const { return !read_queue_.empty() || !write_queue_.empty(); }

    Dir next_request(Request &req);

    void finish_request(const Request &req, Dir dir);

    bool turnaround(Dir dir) const { return have_last_ && ((dir == Dir::Write) != last_write_); }

    void remember(Dir dir)
    {
        last_write_ = (dir == Dir::Write);
        have_last_ = true;
    }

    sc_core::sc_event new_request_;

  private:
    void b_transport(int port, tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    virtual void serve() = 0;

    DramStorage storage_;
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
