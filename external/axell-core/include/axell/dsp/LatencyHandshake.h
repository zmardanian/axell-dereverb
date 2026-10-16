#pragma once
// Axell core - latency handshake between the audio thread and the plugin wrapper's message-thread timer
// (proven in Axell Penta EQ 2.0.1 and Axell Duo Compressor 2.0; their DECISIONS D24 / D23).
//
// The engine never plays a configuration whose latency the host does not compensate. To change the latency it fades
// out, posts a request (one atomic word: state | sequence | latency); the timer reports the latency to the host and
// acknowledges; only then the engine switches and fades back in. Unanswered for 0.5 s, the request is withdrawn
// atomically (a report can never land afterwards) and the old configuration keeps playing until the message thread
// shows a heartbeat again. Offline rendering and tools without a host switch at once.
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace axell
{
class LatencyHandshake
{
public:
    //== audio thread ==============================================================================================
    void prepared (int latency, double sampleRate) noexcept
    {
        fs = sampleRate;
        hostLat = latency;
        reqOpen = holding = false;
        req.store (pack (kIdle, 0, latency), std::memory_order_release);
    }

    bool tracking() const noexcept
    {
        return hostTracking.load (std::memory_order_acquire) && ! offline.load (std::memory_order_acquire);
    }

    // per processing chunk of n samples: true when the host compensates `need`. Posts a request only while the output
    // is silent (mayPost). While holding after a timeout, `need` should be the latency that plays.
    bool ready (int need, bool mayPost, int n) noexcept
    {
        if (! tracking())
        {
            holding = reqOpen = false;
            hostLat = need;
            return true;
        }
        if (holding && beat.load (std::memory_order_acquire) != holdBeat)
            holding = false;
        if (reqOpen)
        {
            auto r = req.load (std::memory_order_acquire);
            if (seqOf (r) == seq && stateOf (r) == kAcked)
            {
                hostLat = latOf (r);
                reqOpen = false;
            }
            else
            {
                wait += n;
                const bool stale = latOf (r) != need, late = wait >= (int) (0.5 * fs);
                if ((stale || late) && stateOf (r) == kPosted
                    && req.compare_exchange_strong (r, pack (kWithdrawn, seq, latOf (r)), std::memory_order_acq_rel))
                {
                    reqOpen = false;
                    if (late && ! stale)
                    {
                        holding = true;
                        holdBeat = beat.load (std::memory_order_acquire);
                    }
                }
            }
        }
        if (! reqOpen && hostLat != need && mayPost && ! holding)
        {
            seq = (seq + 1) & 0x1fffffffu;
            req.store (pack (kPosted, seq, need), std::memory_order_release);
            reqOpen = true;
            wait = 0;
        }
        return ! reqOpen && hostLat == need;
    }

    bool isHolding() const noexcept { return holding; }

    //== any thread ================================================================================================
    void setHostLatency (int samples) noexcept { hostTracking.store (samples >= 0, std::memory_order_release); }
    void setNonRealtime (bool nonRealtime) noexcept { offline.store (nonRealtime, std::memory_order_release); }

    //== message thread (the wrapper's timer) =======================================================================
    void heartbeat() noexcept { beat.fetch_add (1, std::memory_order_acq_rel); }
    bool take (int& samples) noexcept
    {
        auto r = req.load (std::memory_order_acquire);
        if (stateOf (r) != kPosted)
            return false;
        if (! req.compare_exchange_strong (r, pack (kReporting, seqOf (r), latOf (r)), std::memory_order_acq_rel))
            return false;
        samples = latOf (r);
        return true;
    }
    void ack() noexcept
    {
        const auto r = req.load (std::memory_order_acquire);
        if (stateOf (r) == kReporting)
            req.store (pack (kAcked, seqOf (r), latOf (r)), std::memory_order_release);
    }

private:
    enum : std::uint64_t { kIdle = 0, kPosted = 1, kReporting = 2, kAcked = 3, kWithdrawn = 4 };
    static std::uint64_t pack (std::uint64_t st, std::uint64_t s, int lat) noexcept
    {
        return st | ((s & 0x1fffffffu) << 3) | ((std::uint64_t) (std::uint32_t) lat << 32);
    }
    static std::uint64_t stateOf (std::uint64_t r) noexcept { return r & 7u; }
    static std::uint64_t seqOf (std::uint64_t r) noexcept { return (r >> 3) & 0x1fffffffu; }
    static int latOf (std::uint64_t r) noexcept { return (int) (std::uint32_t) (r >> 32); }

    double fs = 48000.0;
    std::atomic<bool> hostTracking { false }, offline { false };
    std::atomic<std::uint64_t> req { 0 };
    std::atomic<std::uint32_t> beat { 0 };
    int hostLat = 0, wait = 0;
    std::uint64_t seq = 0;
    std::uint32_t holdBeat = 0;
    bool reqOpen = false, holding = false;
};
} // namespace axell
