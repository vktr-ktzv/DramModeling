#include "dram_controller_sync.h"

namespace dram
{

DramControllerSync::DramControllerSync(
    sc_core::sc_module_name name, sc_core::sc_time clock_period, unsigned switch_penalty_cycles)
    : DramControllerBase(name), clk("clk"), clock_period_(clock_period), switch_penalty_(switch_penalty_cycles)
{
    SC_THREAD(serve);
}

sc_core::sc_time DramControllerSync::cycles(unsigned count) const { return clock_period_ * static_cast<double>(count); }

void DramControllerSync::serve()
{
    while (true)
    {
        wait(clk.posedge_event());
        if (!has_request())
        {
            continue;
        }

        Request req;
        const Dir dir = next_request(req);
        if (turnaround(dir))
        {
            wait(cycles(switch_penalty_));
        }

        // Задержка из DramChip уже квантована по такту (quantum = clock_period).
        const sc_core::sc_time access = issue(req);
        wait(access);
        finish_request(req, dir);
        remember(dir);
    }
}

} // namespace dram
