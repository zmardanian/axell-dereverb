#pragma once
// Axell core - running maximum (monotonic deque)
// JUCE-free; tested with plain g++ (tests/). Shared by the Axell plugins.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Math.h"

namespace axell
{
//==============================================================================
// Running maximum over the last `window` samples (monotonic deque, amortised O(1) per sample).
// For any steady periodic |x| whose period fits in the window the output is exactly constant: a
// ripple-free peak detector with an instant attack (the Clean FET detector, DECISIONS D19).
// prepare() allocates; setWindow() and process() never do.
class SlidingMax
{
public:
    void prepare (int maxWindow)
    {
        cap = 1;
        while (cap < maxWindow + 2)
            cap <<= 1;
        for (auto& b : buf)
            b.assign ((size_t) cap, Entry {});
        setWindow (std::min (maxWindow, 1));
        resetState();
    }
    void setWindow (int w) noexcept { window = std::clamp (w, 1, cap - 2); }
    void resetState() noexcept
    {
        head.fill (0);
        tail.fill (0);
        n.fill (0);
    }
    float process (float a, int c) noexcept
    {
        auto& b = buf[(size_t) c];
        int& h = head[(size_t) c];
        int& t = tail[(size_t) c];
        const int64_t now = n[(size_t) c]++;
        const int mask = cap - 1;
        while (t != h && b[(size_t) ((t - 1) & mask)].v <= a)
            t = (t - 1) & mask;
        b[(size_t) t] = { a, now };
        t = (t + 1) & mask;
        while (b[(size_t) h].at <= now - window)
            h = (h + 1) & mask;
        return b[(size_t) h].v;
    }

private:
    struct Entry { float v = 0.0f; int64_t at = 0; };
    std::array<std::vector<Entry>, kMaxCh> buf;
    std::array<int, kMaxCh> head {}, tail {};
    std::array<int64_t, kMaxCh> n {};
    int cap = 2, window = 1;
};

} // namespace axell
