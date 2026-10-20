#pragma once

#include <abstract_device.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace dram
{

class SpikeTlmDevice : public abstract_device_t
{
  public:
    using Transfer = std::function<bool(bool write, std::uint64_t offset, std::uint8_t* data, std::size_t len)>;

    SpikeTlmDevice(std::uint64_t size, Transfer transfer);

    bool load(reg_t addr, size_t len, uint8_t* bytes) override;
    bool store(reg_t addr, size_t len, const uint8_t* bytes) override;
    reg_t size() override;

  private:
    bool in_range(reg_t offset, size_t len) const;

    std::uint64_t size_;
    Transfer transfer_;
};

} // namespace dram
