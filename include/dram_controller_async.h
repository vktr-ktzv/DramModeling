#pragma once

#include "dram_controller_base.h"

namespace dram
{

class DramControllerAsync : public DramControllerBase
{
  public:
    DramControllerAsync(
        sc_core::sc_module_name name, unsigned size_bytes,
        sc_core::sc_time read_latency = sc_core::sc_time(70, sc_core::SC_NS),
        sc_core::sc_time write_latency = sc_core::sc_time(50, sc_core::SC_NS),
        sc_core::sc_time switch_penalty = sc_core::sc_time(20, sc_core::SC_NS));

  private:
    void serve() override;

    sc_core::sc_time read_latency_;
    sc_core::sc_time write_latency_;
    sc_core::sc_time switch_penalty_;
};

} // namespace dram
