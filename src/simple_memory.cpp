#include "simple_memory.h"

#include <fstream>

#include <tlm.h>

namespace dram
{

SimpleMemory::SimpleMemory(sc_core::sc_module_name name, unsigned size_bytes)
    : sc_core::sc_module(name), socket("socket"), storage_(size_bytes)
{
    socket.register_b_transport(this, &SimpleMemory::b_transport);
}

void SimpleMemory::b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay)
{
    delay = sc_core::SC_ZERO_TIME;

    const tlm::tlm_command cmd = trans.get_command();
    if (cmd != tlm::TLM_READ_COMMAND && cmd != tlm::TLM_WRITE_COMMAND)
    {
        trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }

    trans.set_response_status(storage_.access(trans));
    if (trans.get_response_status() != tlm::TLM_OK_RESPONSE)
    {
        return;
    }

    trace_.push_back(
        {sc_core::sc_time_stamp(), cmd == tlm::TLM_WRITE_COMMAND, trans.get_address(), trans.get_data_length()});
}

void SimpleMemory::dump_trace(const std::string& path) const
{
    std::ofstream out(path);
    out << "time_ns,command,type,addr,len\n";
    for (const AccessEntry& e : trace_)
    {
        out << e.time / sc_core::sc_time(1, sc_core::SC_NS) << ',' << (e.write ? "WR" : "RD") << ','
            << (e.write ? "write" : "read") << ',' << e.addr << ',' << e.len << '\n';
    }
}

} // namespace dram
