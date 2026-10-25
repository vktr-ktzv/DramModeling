#include "dram_controller_sync.h"

namespace dram
{

DramControllerSync::DramControllerSync(
    sc_core::sc_module_name name, sc_core::sc_time clock_period, unsigned read_latency_cycles,
    unsigned write_latency_cycles, unsigned switch_penalty_cycles)
    : DramControllerBase(name), clk("clk"), clock_period_(clock_period), read_latency_cycles_(read_latency_cycles),
      write_latency_cycles_(write_latency_cycles), switch_penalty_cycles_(switch_penalty_cycles)
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
            wait(cycles(switch_penalty_cycles_));
        }

        // Простая модель: данные применяет SimpleMemory, а задержку задаёт
        // сам контроллер фиксированным числом тактов на чтение/запись.
        (void)issue(req);
        wait(cycles(dir == Dir::Write ? write_latency_cycles_ : read_latency_cycles_));
        finish_request(req, dir);
        remember(dir);
    }
}

} // namespace dram
