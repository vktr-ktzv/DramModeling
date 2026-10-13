#pragma once

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

namespace dram
{

// Write-back буфер между мастером (Spike) и DRAM-контроллером.
// store() только копирует данные в очередь и сразу возвращает управление,
// поэтому CPU не ждёт запись. Несколько write-lane дренируют очередь в
// контроллер параллельно, то есть в полёте могут находиться сразу несколько
// транзакций: пока контроллер обслуживает чтение CPU, буфер держит backlog
// записей. load() форвардит ещё не применённые записи (store-to-load).
class StoreBuffer : public sc_core::sc_module
{
  public:
    static constexpr int kWriteLanes{3};

    tlm_utils::simple_initiator_socket<StoreBuffer> read_socket;
    tlm_utils::simple_initiator_socket<StoreBuffer> write_socket;

    explicit StoreBuffer(sc_core::sc_module_name name);

    bool load(std::uint64_t offset, std::uint8_t* bytes, std::size_t len);
    bool store(std::uint64_t offset, const std::uint8_t* bytes, std::size_t len);

    // Ждёт, пока все отложенные записи дойдут до контроллера.
    void flush();

  private:
    enum class State
    {
        Pending,
        InFlight
    };

    struct Write
    {
        std::uint64_t offset;
        std::vector<std::uint8_t> data;
        State state;
    };

    bool transfer(
        tlm_utils::simple_initiator_socket<StoreBuffer>& sock, bool write, std::uint64_t offset, std::uint8_t* data,
        std::size_t len);

    void drain();

    bool overlaps_inflight(const Write& w) const;
    bool any_claimable() const;

    std::list<Write> pending_;
    sc_core::sc_event work_;
    sc_core::sc_event drained_;
};

} // namespace dram
