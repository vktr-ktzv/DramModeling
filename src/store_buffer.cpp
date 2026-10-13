#include "store_buffer.h"

#include <algorithm>

namespace dram
{

StoreBuffer::StoreBuffer(sc_core::sc_module_name name)
    : sc_core::sc_module(name), read_socket("read_socket"), write_socket("write_socket")
{
    for (int i = 0; i < kWriteLanes; ++i)
    {
        sc_core::sc_spawn(sc_core::sc_bind(&StoreBuffer::drain, this));
    }
}

bool StoreBuffer::transfer(
    tlm_utils::simple_initiator_socket<StoreBuffer>& sock, bool write, std::uint64_t offset, std::uint8_t* data,
    std::size_t len)
{
    tlm::tlm_generic_payload trans;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    trans.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    trans.set_address(offset);
    trans.set_data_ptr(data);
    trans.set_data_length(static_cast<unsigned>(len));
    trans.set_streaming_width(static_cast<unsigned>(len));
    trans.set_byte_enable_ptr(nullptr);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    // Блокирует вызывающий SC_THREAD, пока контроллер не обслужится.
    sock->b_transport(trans, delay);
    return trans.get_response_status() == tlm::TLM_OK_RESPONSE;
}

bool StoreBuffer::store(std::uint64_t offset, const std::uint8_t* bytes, std::size_t len)
{
    pending_.push_back(Write{offset, std::vector<std::uint8_t>(bytes, bytes + len), State::Pending});
    work_.notify(sc_core::SC_ZERO_TIME);
    return true;
}

bool StoreBuffer::load(std::uint64_t offset, std::uint8_t* bytes, std::size_t len)
{
    std::vector<std::uint8_t> value(len);
    if (!transfer(read_socket, false, offset, value.data(), len))
    {
        return false;
    }

    // Более поздние (в порядке программы) не применённые записи перекрывают
    // прочитанное из памяти.
    for (const Write& w : pending_)
    {
        for (std::size_t i = 0; i < w.data.size(); ++i)
        {
            const std::uint64_t addr = w.offset + i;
            if (addr >= offset && addr < offset + len)
            {
                value[addr - offset] = w.data[i];
            }
        }
    }

    std::copy(value.begin(), value.end(), bytes);
    return true;
}

bool StoreBuffer::overlaps_inflight(const Write& w) const
{
    for (const Write& other : pending_)
    {
        if (other.state == State::InFlight && other.offset < w.offset + w.data.size() &&
            w.offset < other.offset + other.data.size())
        {
            return true;
        }
    }
    return false;
}

bool StoreBuffer::any_claimable() const
{
    for (const Write& w : pending_)
    {
        if (w.state == State::Pending && !overlaps_inflight(w))
        {
            return true;
        }
    }
    return false;
}

void StoreBuffer::drain()
{
    while (true)
    {
        auto it = pending_.begin();
        for (; it != pending_.end(); ++it)
        {
            // В очередь к контроллеру отправляем по порядку программы; запись к
            // тому же адресу ждёт завершения более ранней (порядок сохраняется).
            if (it->state == State::Pending && !overlaps_inflight(*it))
            {
                break;
            }
        }

        if (it == pending_.end())
        {
            sc_core::wait(work_);
            continue;
        }

        it->state = State::InFlight;
        transfer(write_socket, true, it->offset, it->data.data(), it->data.size());
        pending_.erase(it);

        if (any_claimable())
        {
            work_.notify(sc_core::SC_ZERO_TIME);
        }
        if (pending_.empty())
        {
            drained_.notify(sc_core::SC_ZERO_TIME);
        }
    }
}

void StoreBuffer::flush()
{
    while (!pending_.empty())
    {
        sc_core::wait(drained_);
    }
}

} // namespace dram
