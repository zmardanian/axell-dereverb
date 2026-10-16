#pragma once
// Axell core - single-producer / single-consumer ring for display frames (audio thread pushes, GUI pops).
#include <array>
#include <atomic>
#include <cstdint>

namespace axell
{
template <typename Frame, uint32_t Size = 2048>
class HistoryFifo
{
public:
    void push (const Frame& f) noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        if (w - read.load (std::memory_order_acquire) >= Size)
            return; // GUI closed or too slow: drop
        buf[w % Size] = f;
        write.store (w + 1, std::memory_order_release);
    }
    bool pop (Frame& f) noexcept
    {
        const auto r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire))
            return false;
        f = buf[r % Size];
        read.store (r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<Frame, Size> buf {};
    std::atomic<uint32_t> write { 0 }, read { 0 };
};
} // namespace axell
