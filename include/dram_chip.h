#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <systemc.h>
#include <tlm_utils/simple_target_socket.h>

#include "dram_common.h"

namespace dram
{

struct DramTiming
{
    sc_core::sc_time t_rcd; // ACTIVATE -> колоночная команда
    sc_core::sc_time t_cas; // CAS latency: доступ к столбцу
    sc_core::sc_time t_rp;  // PRECHARGE
    sc_core::sc_time t_wr;  // write recovery
    sc_core::sc_time quantum{sc_core::SC_ZERO_TIME};
};

class DramChip : public sc_core::sc_module
{
  public:
    enum class BankState
    {
        Idle,   
        Active, // строка открыта в row buffer
    };

    enum class Command
    {
        Activate,
        Read,
        Write,
        Precharge,
    };

   
    enum class RowAccess
    {
        Empty, // строка не была открыта
        Hit,   
        Miss,  
    };

    struct TraceEntry
    {
        sc_core::sc_time time; 
        Command command;
        RowAccess row_access;
        bool write;
        unsigned bank;
        unsigned row;
        unsigned col;
        sc_core::sc_time latency; // вклад команды в задержку
    };

    tlm_utils::simple_target_socket<DramChip> socket;

    DramChip(
        sc_core::sc_module_name name, unsigned size_bytes, DramTiming timing, unsigned bank_bits = 2,
        unsigned col_bits = 6);

    unsigned row_hits() const { return row_hits_; }
    unsigned row_misses() const { return row_misses_; }
    unsigned row_empties() const { return row_empties_; }
    unsigned activations() const { return activations_; }
    unsigned precharges() const { return precharges_; }
    unsigned reads() const { return reads_; }
    unsigned writes() const { return writes_; }
    sc_core::sc_time total_latency() const { return total_latency_; }

    const std::vector<TraceEntry>& trace_entries() const { return trace_; }

    // Выгружает полную трассу обращений в CSV (для diff между sync и async).
    void dump_trace(const std::string& path) const;

    std::uint8_t peek(unsigned addr) const { return storage_.peek(addr); }

  private:
    struct Bank
    {
        BankState state{BankState::Idle};
        int open_row{-1};
    };

    struct Decoded
    {
        unsigned bank;
        unsigned row;
        unsigned col;
    };

    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay);

    Decoded decode(std::uint64_t addr) const;
    sc_core::sc_time quantize(sc_core::sc_time delay) const;

    // Автомат: обслужить запрос, изменить состояние банков, записать трассу
    // и вернуть суммарную задержку доступа.
    sc_core::sc_time issue(tlm::tlm_generic_payload& trans, Decoded addr, bool write);

    DramStorage storage_;
    DramTiming timing_;
    unsigned bank_bits_;
    unsigned col_bits_;
    std::vector<Bank> banks_;
    std::vector<TraceEntry> trace_;

    unsigned row_hits_{0};
    unsigned row_misses_{0};
    unsigned row_empties_{0};
    unsigned activations_{0};
    unsigned precharges_{0};
    unsigned reads_{0};
    unsigned writes_{0};
    sc_core::sc_time total_latency_{sc_core::SC_ZERO_TIME};
};

} // namespace dram
