#include "spike_dram_device.h"

#include <utility>

namespace dram
{

SpikeTlmDevice::SpikeTlmDevice(std::uint64_t size, Load load, Store store)
    : size_(size), load_(std::move(load)), store_(std::move(store))
{
}

bool SpikeTlmDevice::in_range(reg_t offset, size_t len) const { return offset <= size_ && len <= size_ - offset; }

bool SpikeTlmDevice::load(reg_t offset, size_t len, uint8_t* bytes)
{
    if (!in_range(offset, len))
    {
        return false;
    }
    return load_(offset, bytes, len);
}

bool SpikeTlmDevice::store(reg_t offset, size_t len, const uint8_t* bytes)
{
    if (!in_range(offset, len))
    {
        return false;
    }
    return store_(offset, bytes, len);
}

reg_t SpikeTlmDevice::size() { return size_; }

} // namespace dram
