#pragma once

#include "dram_controller_base.h"

namespace dram
{

// Простая синхронная модель контроллера: тактируется clk и не хранит
// состояние банков. Каждое чтение/запись имеет фиксированную задержку в
// тактах, row hit/miss не различаются. Данные хранит SimpleMemory; это
// менее детальная модель по сравнению с DramControllerAsync.
class DramControllerSync : public DramControllerBase
{
  public:
    sc_core::sc_in<bool> clk;

    DramControllerSync(
        sc_core::sc_module_name name, sc_core::sc_time clock_period, unsigned read_latency_cycles = 3,
        unsigned write_latency_cycles = 2, unsigned switch_penalty_cycles = 2);

  private:
    sc_core::sc_time cycles(unsigned count) const;

    void serve() override;

    sc_core::sc_time clock_period_;
    unsigned read_latency_cycles_;
    unsigned write_latency_cycles_;
    unsigned switch_penalty_cycles_;
};

} // namespace dram
