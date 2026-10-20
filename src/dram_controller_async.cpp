#include "dram_controller_async.h"

namespace dram
{

DramControllerAsync::DramControllerAsync(sc_core::sc_module_name name, sc_core::sc_time switch_penalty)
    : DramControllerBase(name), switch_penalty_(switch_penalty)
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

        // Задержку считает автомат банков DramChip (точные sc_time).
        const sc_core::sc_time access = issue(req);
        wait(access);
        finish_request(req, dir);
        remember(dir);
    }
}

} // namespace dram
