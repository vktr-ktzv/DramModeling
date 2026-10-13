#include "spike_dram_device.h"

namespace dram
{

SpikeTlmDevice::SpikeTlmDevice(std::uint64_t size, Transfer transfer) : size_(size), transfer_(std::move(transfer)) {}

bool SpikeTlmDevice::in_range(reg_t offset, size_t len) const { return offset <= size_ && len <= size_ - offset; }

bool SpikeTlmDevice::load(reg_t offset, size_t len, uint8_t* bytes)
{
    if (!in_range(offset, len))
    {
        return false;
    }
    return transfer_(false, offset, bytes, len);
}

bool SpikeTlmDevice::store(reg_t offset, size_t len, const uint8_t* bytes)
{
    if (!in_range(offset, len))
    {
        return false;
    }
    return transfer_(true, offset, const_cast<uint8_t*>(bytes), len);
}

reg_t SpikeTlmDevice::size() { return size_; }

} // namespace dram
