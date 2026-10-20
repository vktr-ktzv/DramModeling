#pragma once

#include "dram_controller_base.h"

namespace dram
{

// Асинхронный контроллер: время продвигается произвольными sc_time,
// а задержку доступа целиком определяет автомат банков DramChip. Штраф за
// смену направления чтение↔запись остаётся за контроллером.
class DramControllerAsync : public DramControllerBase
{
  public:
    DramControllerAsync(
        sc_core::sc_module_name name, sc_core::sc_time switch_penalty = sc_core::sc_time(20, sc_core::SC_NS));

  private:
    void serve() override;

    sc_core::sc_time switch_penalty_;
};

} // namespace dram
