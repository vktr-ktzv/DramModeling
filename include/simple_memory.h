#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <systemc.h>
#include <tlm_utils/simple_target_socket.h>

#include "dram_common.h"

namespace dram
{

// Простая модель памяти для синхронного контроллера: только массив байт,
// без банков, открытых строк и row hit/miss. Время доступа целиком задаёт
// контроллер фиксированным числом тактов, поэтому b_transport не добавляет
// задержку — он только применяет данные и пишет минимальную трассу обращений.
class SimpleMemory : public sc_core::sc_module
{
  public:
    struct AccessEntry
    {
        sc_core::sc_time time;
        bool write;
        std::uint64_t addr;
        unsigned len;
    };

    tlm_utils::simple_target_socket<SimpleMemory> socket;

    SimpleMemory(sc_core::sc_module_name name, unsigned size_bytes);

    const std::vector<AccessEntry>& accesses() const { return trace_; }

    // Выгружает трассу обращений (без банковских команд) в CSV.
    void dump_trace(const std::string& path) const;

  private:
    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay);

    DramStorage storage_;
    std::vector<AccessEntry> trace_;
};

} // namespace dram
