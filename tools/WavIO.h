#pragma once
// Minimal WAV reader / writer for the command-line tools (PCM 16/24/32, float32; JUCE-free)
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace wav
{
struct Audio
{
    double fs = 48000.0;
    std::vector<std::vector<float>> ch;
    size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

inline bool read (const std::string& path, Audio& a)
{
    std::ifstream f (path, std::ios::binary);
    std::vector<char> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
        return false;
    auto u16 = [&] (size_t i) { uint16_t v; std::memcpy (&v, d.data() + i, 2); return v; };
    auto u32 = [&] (size_t i) { uint32_t v; std::memcpy (&v, d.data() + i, 4); return v; };
    size_t i = 12;
    int tag = 0, nch = 0, bits = 0;
    while (i + 8 <= d.size())
    {
        const uint32_t sz = u32 (i + 4);
        if (std::memcmp (d.data() + i, "fmt ", 4) == 0)
        {
            tag = u16 (i + 8);
            nch = u16 (i + 10);
            a.fs = u32 (i + 12);
            bits = u16 (i + 22);
            if (tag == 0xFFFE)
                tag = u16 (i + 32);
        }
        else if (std::memcmp (d.data() + i, "data", 4) == 0)
        {
            const size_t bps = (size_t) bits / 8, n = std::min<size_t> (sz, d.size() - i - 8) / (bps * (size_t) nch);
            a.ch.assign ((size_t) nch, std::vector<float> (n));
            const char* p = d.data() + i + 8;
            for (size_t k = 0; k < n; ++k)
                for (int c = 0; c < nch; ++c, p += bps)
                {
                    float v = 0;
                    if (tag == 3 && bits == 32) std::memcpy (&v, p, 4);
                    else if (bits == 16) { int16_t s; std::memcpy (&s, p, 2); v = s / 32768.0f; }
                    else if (bits == 24) { int32_t s = (uint8_t) p[0] | ((uint8_t) p[1] << 8) | ((int8_t) p[2] << 16); v = (float) s / 8388608.0f; }
                    else if (bits == 32) { int32_t s; std::memcpy (&s, p, 4); v = (float) ((double) s / 2147483648.0); }
                    a.ch[(size_t) c][k] = v;
                }
            return true;
        }
        i += 8 + sz + (sz & 1);
    }
    return false;
}

inline bool write (const std::string& path, const Audio& a) // float32
{
    std::ofstream f (path, std::ios::binary);
    const uint32_t nch = (uint32_t) a.ch.size(), n = (uint32_t) a.frames(), data = n * nch * 4, fs = (uint32_t) a.fs;
    auto w32 = [&] (uint32_t v) { f.write ((const char*) &v, 4); };
    auto w16 = [&] (uint16_t v) { f.write ((const char*) &v, 2); };
    f.write ("RIFF", 4); w32 (36 + data); f.write ("WAVEfmt ", 8); w32 (16); w16 (3); w16 ((uint16_t) nch); w32 (fs);
    w32 (fs * nch * 4); w16 ((uint16_t) (nch * 4)); w16 (32); f.write ("data", 4); w32 (data);
    for (uint32_t k = 0; k < n; ++k)
        for (uint32_t c = 0; c < nch; ++c)
            f.write ((const char*) &a.ch[c][k], 4);
    return (bool) f;
}
} // namespace wav
