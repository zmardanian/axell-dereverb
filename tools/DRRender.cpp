// Axell De-Reverb - offline render through the C++ engine: DRRender weights.bin in.wav out.wav [--raw]
// The output is aligned with the input (the engine's latency removed; LIVE weights have none). --raw: float32 mono files
// instead of wav.
#include "../Source/Engine.h"
#include "WavIO.h"

#include <cstdio>
#include <fstream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: DRRender weights.bin in.wav out.wav [--raw] [--truetone] [--amount=0..1]\n"
                              "  --truetone prints the final TRUE TONE curve and its measured G (23 bands, dB) to stdout;\n"
                              "  --measure does the same without applying the correction\n");
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
    bool raw = false, tt = false, measure = false;
    float amount = 1.0f;
    for (int i = 4; i < argc; ++i)
    {
        const std::string o = argv[i];
        raw = raw || o == "--raw";
        tt = tt || o == "--truetone" || o == "--measure";
        measure = measure || o == "--measure";
        if (o.rfind ("--amount=", 0) == 0)
            amount = std::stof (o.substr (9));
    }
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
    e.setTrueTone (tt);
    e.setTrueToneMeasureOnly (measure);
    e.setAmount (amount);
    e.reset();
    const int L = e.latency();          // 0 for LIVE weights
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
    if (tt)
    {
        for (int b = 0; b < 23; ++b)
            std::printf ("%.4f%s", e.trueToneDb (b), b < 22 ? " " : "\n");
        for (int b = 0; b < 23; ++b)
            std::printf ("%.4f%s", e.trueToneMeasuredDb (b), b < 22 ? " " : "\n");
    }
    if (raw)
    {
        std::ofstream f (argv[3], std::ios::binary);
        f.write ((const char*) a.ch[0].data(), (std::streamsize) (n * 4));
        return 0;
    }
    return wav::write (argv[3], a) ? 0 : 1;
}
