// Axell De-Reverb - offline render through the C++ engine: DRRender weights.bin in.wav out.wav [--raw]
// The output is aligned with the input (the engine's latency removed). --raw: float32 mono files instead of wav.
#include "../Source/Engine.h"
#include "WavIO.h"

#include <cstdio>
#include <fstream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: DRRender weights.bin in.wav out.wav [--raw]\n");
        return 2;
    }
    std::ifstream wf (argv[1], std::ios::binary);
    std::vector<char> wb ((std::istreambuf_iterator<char> (wf)), std::istreambuf_iterator<char>());
    axdr::Weights W;
    if (! W.load (wb.data(), wb.size()))
    {
        std::fprintf (stderr, "bad weights\n");
        return 1;
    }
    const bool raw = argc > 4 && std::string (argv[4]) == "--raw";
    wav::Audio a;
    if (raw)
    {
        std::ifstream f (argv[2], std::ios::binary | std::ios::ate);
        const auto bytes = (size_t) f.tellg();
        a.ch.assign (1, std::vector<float> (bytes / 4));
        f.seekg (0);
        f.read ((char*) a.ch[0].data(), (std::streamsize) bytes);
    }
    else if (! wav::read (argv[2], a))
        return 1;
    axdr::Engine e;
    e.prepare (W, (int) a.ch.size());
    const int L = axdr::Engine::latency();
    const size_t n = a.frames();
    for (auto& c : a.ch)
        c.resize (n + (size_t) L, 0.0f);
    std::vector<float*> p (a.ch.size());
    for (size_t pos = 0; pos < n + (size_t) L; pos += 256)
    {
        const int m = (int) std::min<size_t> (256, n + (size_t) L - pos);
        for (size_t c = 0; c < a.ch.size(); ++c)
            p[c] = a.ch[c].data() + pos;
        e.process (p.data(), (int) a.ch.size(), m);
    }
    for (auto& c : a.ch)
        c.erase (c.begin(), c.begin() + L);
    if (raw)
    {
        std::ofstream f (argv[3], std::ios::binary);
        f.write ((const char*) a.ch[0].data(), (std::streamsize) (n * 4));
        return 0;
    }
    return wav::write (argv[3], a) ? 0 : 1;
}
