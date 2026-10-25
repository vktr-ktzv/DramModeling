#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <tlm.h>

namespace dram
{

class DramStorage
{
  public:
    explicit DramStorage(unsigned size_bytes) : mem_(size_bytes, 0) {}

    unsigned size() const { return static_cast<unsigned>(mem_.size()); }

    tlm::tlm_response_status access(tlm::tlm_generic_payload& trans)
    {
        const std::uint64_t addr = trans.get_address();
        const unsigned len = trans.get_data_length();
        unsigned char* data = trans.get_data_ptr();

        if (data == nullptr)
        {
            return tlm::TLM_COMMAND_ERROR_RESPONSE;
        }
        if (addr > mem_.size() || len > mem_.size() - addr)
        {
            return tlm::TLM_ADDRESS_ERROR_RESPONSE;
        }

        switch (trans.get_command())
        {
        case tlm::TLM_WRITE_COMMAND:
            std::memcpy(mem_.data() + addr, data, len);
            return tlm::TLM_OK_RESPONSE;
        case tlm::TLM_READ_COMMAND:
            std::memcpy(data, mem_.data() + addr, len);
            return tlm::TLM_OK_RESPONSE;
        default:
            return tlm::TLM_COMMAND_ERROR_RESPONSE;
        }
    }

  private:
    std::vector<unsigned char> mem_;
};

} // namespace dram
