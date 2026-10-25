#include "dram_chip.h"

#include <fstream>

#include <tlm.h>

namespace dram
{

DramChip::DramChip(
    sc_core::sc_module_name name, unsigned size_bytes, DramTiming timing, unsigned bank_bits, unsigned col_bits)
    : sc_core::sc_module(name), socket("socket"), storage_(size_bytes), timing_(timing), bank_bits_(bank_bits),
      col_bits_(col_bits), banks_(1u << bank_bits)
{
    socket.register_b_transport(this, &DramChip::b_transport);
}

// Адрес раскладывается с младших бит: [столбец | банк | строка]. Столбцы внутри
// строки (2^col_bits байт), затем биты выбора банка, остальное — номер строки.
DramChip::Decoded DramChip::decode(std::uint64_t addr) const
{
    const std::uint64_t col_mask = (std::uint64_t{1} << col_bits_) - 1;
    const std::uint64_t bank_mask = (std::uint64_t{1} << bank_bits_) - 1;

    Decoded d;
    d.col = static_cast<unsigned>(addr & col_mask);
    d.bank = static_cast<unsigned>((addr >> col_bits_) & bank_mask);
    d.row = static_cast<unsigned>(addr >> (col_bits_ + bank_bits_));
    return d;
}

// Конечный автомат банка
//   Idle --Activate--> Active --Read/Write--> Active --Precharge--> Idle
// Если открыта другая строка, сначала PRECHARGE, затем ACTIVATE новой.
sc_core::sc_time DramChip::issue(tlm::tlm_generic_payload& trans, Decoded addr, bool write)
{
    Bank& bank = banks_[addr.bank];
    const sc_core::sc_time now = sc_core::sc_time_stamp();
    sc_core::sc_time cursor = now;
    sc_core::sc_time latency = sc_core::SC_ZERO_TIME;

    RowAccess access = RowAccess::Empty;
    switch (bank.state)
    {
    case BankState::Idle:
        ++row_empties_;
        break;
    case BankState::Active:
        if (bank.open_row == static_cast<int>(addr.row))
        {
            access = RowAccess::Hit;
            ++row_hits_;
        }
        else
        {
            access = RowAccess::Miss;
            ++row_misses_;
        }
        break;
    }

    if (access != RowAccess::Hit)
    {
        if (access == RowAccess::Miss)
        {
            const sc_core::sc_time d_rp = timing_.t_rp;
            trace_.push_back({cursor, Command::Precharge, access, write, addr.bank,
                              static_cast<unsigned>(bank.open_row), addr.col, d_rp});
            cursor += d_rp;
            latency += d_rp;
            ++precharges_;
        }

        const sc_core::sc_time d_rcd = timing_.t_rcd;
        trace_.push_back({cursor, Command::Activate, access, write, addr.bank, addr.row, addr.col, d_rcd});
        cursor += d_rcd;
        latency += d_rcd;
        ++activations_;

        bank.open_row = static_cast<int>(addr.row);
        bank.state = BankState::Active;
    }

    const sc_core::sc_time d_cas = timing_.t_cas;
    if (write)
    {
        const sc_core::sc_time d_wr = timing_.t_wr;
        trace_.push_back({cursor, Command::Write, access, write, addr.bank, addr.row, addr.col, d_cas + d_wr});
        latency += d_cas + d_wr;
        ++writes_;
    }
    else
    {
        trace_.push_back({cursor, Command::Read, access, write, addr.bank, addr.row, addr.col, d_cas});
        latency += d_cas;
        ++reads_;
    }

    trans.set_response_status(storage_.access(trans));
    total_latency_ += latency;
    return latency;
}

void DramChip::b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay)
{
    const tlm::tlm_command cmd = trans.get_command();
    if (cmd != tlm::TLM_READ_COMMAND && cmd != tlm::TLM_WRITE_COMMAND)
    {
        trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }

    const std::uint64_t addr = trans.get_address();
    const unsigned len = trans.get_data_length();
    const unsigned size = storage_.size();
    if (trans.get_data_ptr() == nullptr || addr > size || len > size - addr)
    {
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return;
    }

    delay += issue(trans, decode(addr), cmd == tlm::TLM_WRITE_COMMAND);
}

void DramChip::dump_trace(const std::string& path) const
{
    std::ofstream out(path);
    out << "time_ns,command,row_access,type,bank,row,col,latency_ns\n";
    for (const TraceEntry& e : trace_)
    {
        const char* command = "?";
        switch (e.command)
        {
        case Command::Activate:
            command = "ACT";
            break;
        case Command::Read:
            command = "RD";
            break;
        case Command::Write:
            command = "WR";
            break;
        case Command::Precharge:
            command = "PRE";
            break;
        }

        const char* access = "?";
        switch (e.row_access)
        {
        case RowAccess::Empty:
            access = "empty";
            break;
        case RowAccess::Hit:
            access = "hit";
            break;
        case RowAccess::Miss:
            access = "miss";
            break;
        }

        out << e.time / sc_core::sc_time(1, sc_core::SC_NS) << ',' << command << ',' << access << ','
            << (e.write ? "write" : "read") << ',' << e.bank << ',' << e.row << ',' << e.col << ','
            << e.latency / sc_core::sc_time(1, sc_core::SC_NS) << '\n';
    }
}

} // namespace dram
