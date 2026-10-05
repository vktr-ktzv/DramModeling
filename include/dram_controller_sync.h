#pragma once

#include "dram_controller_base.h"

namespace dram
{

// Синхронный контроллер: тактируется сигналом clk. Команда принимается
// по фронту, а задержки выражаются целым числом тактов, поэтому все
// операции выравниваются по тактовой сетке.
class DramControllerSync : public DramControllerBase
{
  public:
    sc_core::sc_in<bool> clk;

    DramControllerSync(
        sc_core::sc_module_name name, unsigned size_bytes, sc_core::sc_time clock_period,
        unsigned read_latency_cycles = 3, unsigned write_latency_cycles = 2, unsigned switch_penalty_cycles = 1);

  private:
    sc_core::sc_time cycles(unsigned count) const;

    void serve() override;

    sc_core::sc_time clock_period_;
    unsigned read_latency_;
    unsigned write_latency_;
    unsigned switch_penalty_;
};

} // namespace dram
