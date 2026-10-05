#include "dram_controller_async.h"

namespace dram
{

DramControllerAsync::DramControllerAsync(
    sc_core::sc_module_name name, unsigned size_bytes, sc_core::sc_time read_latency, sc_core::sc_time write_latency,
    sc_core::sc_time switch_penalty)
    : DramControllerBase(name, size_bytes), read_latency_(read_latency), write_latency_(write_latency),
      switch_penalty_(switch_penalty)
{
    SC_THREAD(serve);
}

void DramControllerAsync::serve()
{
    while (true)
    {
        while (!has_request())
        {
            wait(new_request_);
        }

        Request req;
        const Dir dir = next_request(req);
        if (turnaround(dir))
        {
            wait(switch_penalty_);
        }

        wait(dir == Dir::Write ? write_latency_ : read_latency_);
        finish_request(req, dir);
        remember(dir);
    }
}

} // namespace dram
