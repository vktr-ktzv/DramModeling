#pragma once

#include "dram_controller_base.h"

namespace dram
{

// Синхронный контроллер: тактируется сигналом clk. Команды выдаются по фронту,
// а задержки выравниваются по тактовой сетке. Банковая модель DramChip
// настраивается с quantum = clock_period, поэтому её задержки уже кратны такту;
// контроллер дополнительно квантует штраф за смену направления.
class DramControllerSync : public DramControllerBase
{
  public:
    sc_core::sc_in<bool> clk;

    DramControllerSync(
        sc_core::sc_module_name name, sc_core::sc_time clock_period, unsigned switch_penalty_cycles = 1);

  private:
    sc_core::sc_time cycles(unsigned count) const;

    void serve() override;

    sc_core::sc_time clock_period_;
    unsigned switch_penalty_;
};

} // namespace dram
