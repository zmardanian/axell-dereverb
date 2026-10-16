// Independent black-box host for the audit (tests/independent/README.md; shared with Axell Penta EQ): loads a built .vst3
// through JUCE's VST3 *hosting* code - not the plugin's classes - sets parameters by display name and text
// (the plugin's own string parser, as a host would) and renders raw float32 interleaved files.
// Job file: lines accumulate settings, "run" renders them with a fresh plugin instance.
//   rate 48000 | channels 2 | blocks 512 | blocks random 1 4096 7 | nonrealtime 1 | preroll 24000 | sleep ms
//   pdc ms (a live host: run the message loop for ms after every block and log "pdc <sample> <latency>" whenever
//   the latency the host compensates changes, as the plugin reported it through restartComponent)
//   set <Param Name>=<text> | setn <Param Name>=<normalised 0..1> | at <sample> set <Param Name>=<text>
//   input a.f32 | sidechain b.f32 | output c.f32 | run | params (list the parameters of a fresh instance)
// Command line: AuditHost plugin.vst3 job.txt [--plugin <name>]   (a bundle may hold several plugins)
//               AuditHost plugin.vst3 --list                       (their names)
// Each run reports the plugin's processing time ("cpu <seconds>"), for comparisons.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <random>
#include <chrono>
#include <thread>

using namespace juce;

struct Job
{
    double rate = 48000;
    int channels = 2, blockMin = 512, blockMax = 512, preroll = 24000, sleepMs = 0, pdcMs = 0;
    unsigned seed = 1;
    bool nonRealtime = true;
    std::vector<std::pair<String, String>> sets, setns;
    std::vector<std::tuple<int, String, String>> ats;
    String input, side, output;
};

static AudioBuffer<float> readRaw (const String& path, int ch)
{
    MemoryBlock mb;
    if (! File (path).loadFileAsData (mb)) { std::fprintf (stderr, "cannot read %s\n", path.toRawUTF8()); std::exit (2); }
    const int n = (int) (mb.getSize() / sizeof (float) / (size_t) ch);
    AudioBuffer<float> b (ch, n);
    auto* d = static_cast<const float*> (mb.getData());
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c)
            b.setSample (c, i, d[(size_t) i * (size_t) ch + (size_t) c]);
    return b;
}

static void writeRaw (const String& path, const AudioBuffer<float>& b)
{
    std::vector<float> d ((size_t) b.getNumSamples() * (size_t) b.getNumChannels());
    for (int i = 0; i < b.getNumSamples(); ++i)
        for (int c = 0; c < b.getNumChannels(); ++c)
            d[(size_t) i * (size_t) b.getNumChannels() + (size_t) c] = b.getSample (c, i);
    File (path).replaceWithData (d.data(), d.size() * sizeof (float));
}

static AudioProcessorParameter* findParam (AudioPluginInstance& p, const String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (256) == name)
            return prm;
    std::fprintf (stderr, "no parameter '%s'\n", name.toRawUTF8());
    std::exit (3);
}

static void applySet (AudioPluginInstance& p, const String& name, const String& text, bool log)
{
    auto* prm = findParam (p, name);
    const float v = prm->getValueForText (text);
    prm->setValueNotifyingHost (v);
    if (log)
        std::printf ("  set %-34s = %-12s -> norm %.9f text '%s'\n", name.toRawUTF8(), text.toRawUTF8(), v,
                     prm->getText (prm->getValue(), 64).toRawUTF8());
}

static void listParams (AudioPluginFormatManager& fm, const PluginDescription& desc)
{
    String err;
    auto inst = fm.createPluginInstance (desc, 48000.0, 512, err);
    if (inst == nullptr) { std::fprintf (stderr, "instance: %s\n", err.toRawUTF8()); std::exit (4); }
    for (auto* prm : inst->getParameters())
    {
        std::printf ("param '%s' steps %d default '%s' |", prm->getName (256).toRawUTF8(), prm->getNumSteps(),
                     prm->getText (prm->getDefaultValue(), 64).toRawUTF8());
        for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            std::printf (" %.2f='%s'", v, prm->getText (v, 64).toRawUTF8());
        std::printf ("\n");
    }
    std::printf ("latency %d, buses in %d out %d\n", inst->getLatencySamples(), inst->getBusCount (true), inst->getBusCount (false));
    std::fflush (stdout);
}

static void runJob (AudioPluginFormatManager& fm, const PluginDescription& desc, const Job& j)
{
    String err;
    auto inst = fm.createPluginInstance (desc, j.rate, j.blockMax, err);
    if (inst == nullptr) { std::fprintf (stderr, "instance: %s\n", err.toRawUTF8()); std::exit (4); }

    auto in = readRaw (j.input, j.channels);
    AudioBuffer<float> sc;
    const bool useSc = j.side.isNotEmpty();
    if (useSc)
        sc = readRaw (j.side, 2);

    auto layout = inst->getBusesLayout();
    const auto set = j.channels == 1 ? AudioChannelSet::mono() : AudioChannelSet::stereo();
    layout.inputBuses.getReference (0) = set;
    layout.outputBuses.getReference (0) = set;
    if (layout.inputBuses.size() > 1)
        layout.inputBuses.getReference (1) = useSc ? AudioChannelSet::stereo() : AudioChannelSet::disabled();
    if (! inst->setBusesLayout (layout)) { std::fprintf (stderr, "layout refused\n"); std::exit (5); }

    for (auto& [n, t] : j.sets)
        applySet (*inst, n, t, true);
    for (auto& [n, t] : j.setns)
    {
        auto* prm = findParam (*inst, n);
        prm->setValueNotifyingHost (t.getFloatValue());
        std::printf ("  setn %-33s = %-12s -> text '%s'\n", n.toRawUTF8(), t.toRawUTF8(), prm->getText (prm->getValue(), 64).toRawUTF8());
    }
    inst->setNonRealtime (j.nonRealtime);
    inst->prepareToPlay (j.rate, j.blockMax);

    const int nIn = inst->getTotalNumInputChannels(), nOut = inst->getTotalNumOutputChannels();
    const int nBuf = jmax (nIn, nOut);
    AudioBuffer<float> buf (nBuf, j.blockMax);
    MidiBuffer midi;
    std::mt19937 rng (j.seed);
    std::uniform_int_distribution<int> bd (j.blockMin, j.blockMax);

    // pre-roll in silence (parameter changes, warm-ups, designs)
    for (int pass = 0; pass < (j.sleepMs > 0 ? 2 : 1); ++pass)
    {
        for (int pos = 0; pos < j.preroll;)
        {
            const int n = jmin (j.blockMax, j.preroll - pos);
            buf.setSize (nBuf, n, false, false, true);
            buf.clear();
            inst->processBlock (buf, midi);
            if (j.pdcMs > 0)
                MessageManager::getInstance()->runDispatchLoopUntil (j.pdcMs);
            pos += n;
        }
        if (j.sleepMs > 0 && pass == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (j.sleepMs));
    }

    const int len = in.getNumSamples();
    AudioBuffer<float> out (j.channels, len);
    size_t nextAt = 0;
    auto ats = j.ats;
    std::sort (ats.begin(), ats.end(), [] (auto& a, auto& b) { return std::get<0> (a) < std::get<0> (b); });
    int hostLat = -1;
    double cpu = 0.0;
    for (int pos = 0; pos < len;)
    {
        if (j.pdcMs > 0 && inst->getLatencySamples() != hostLat)
        {
            hostLat = inst->getLatencySamples();
            std::printf ("  pdc %d %d\n", pos, hostLat);
        }
        int n = jmin (bd (rng), len - pos);
        if (nextAt < ats.size())
        {
            const int at = std::get<0> (ats[nextAt]);
            if (at <= pos)
            {
                while (nextAt < ats.size() && std::get<0> (ats[nextAt]) <= pos)
                {
                    const auto& nm = std::get<1> (ats[nextAt]);
                    if (nm.startsWith ("#n#"))
                        findParam (*inst, nm.substring (3))->setValueNotifyingHost (std::get<2> (ats[nextAt]).getFloatValue());
                    else
                        applySet (*inst, nm, std::get<2> (ats[nextAt]), false);
                    ++nextAt;
                }
                continue;
            }
            n = jmin (n, at - pos);
        }
        buf.setSize (nBuf, n, false, false, true);
        buf.clear();
        for (int c = 0; c < j.channels; ++c)
            buf.copyFrom (c, 0, in, jmin (c, in.getNumChannels() - 1), pos, n);
        if (useSc)
            for (int c = 0; c < 2; ++c)
                buf.copyFrom (j.channels + c, 0, sc, jmin (c, sc.getNumChannels() - 1), pos, n);
        const auto t0 = std::chrono::steady_clock::now();
        inst->processBlock (buf, midi);
        cpu += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        if (j.pdcMs > 0)
            MessageManager::getInstance()->runDispatchLoopUntil (j.pdcMs);
        for (int c = 0; c < j.channels; ++c)
            out.copyFrom (c, pos, buf, c, 0, n);
        pos += n;
    }
    writeRaw (j.output, out);
    std::printf ("  cpu %.6f\n", cpu);
    std::printf ("  latency %d  -> %s\n", inst->getLatencySamples(), j.output.toRawUTF8());
    std::fflush (stdout);
    inst->releaseResources();
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "AuditHost plugin.vst3 job.txt [--plugin name] | AuditHost plugin.vst3 --list\n"); return 1; }
    ScopedJuceInitialiser_GUI juceInit;
    AudioPluginFormatManager fm;
    fm.addFormat (std::make_unique<VST3PluginFormat>());
    OwnedArray<PluginDescription> found;
    VST3PluginFormat vst3;
    vst3.findAllTypesForFile (found, argv[1]);
    if (found.isEmpty()) { std::fprintf (stderr, "no plugin in %s\n", argv[1]); return 1; }
    if (String (argv[2]) == "--list")
    {
        for (auto* d : found)
            std::printf ("%s | %s | %s\n", d->name.toRawUTF8(), d->category.toRawUTF8(), d->manufacturerName.toRawUTF8());
        return 0;
    }
    auto desc = *found[0];
    if (argc >= 5 && String (argv[3]) == "--plugin")
    {
        bool ok = false;
        for (auto* d : found)
            if (d->name == String (argv[4])) { desc = *d; ok = true; break; }
        if (! ok) { std::fprintf (stderr, "no plugin named '%s' in the bundle\n", argv[4]); return 1; }
    }
    std::printf ("plugin: %s %s by %s\n", desc.name.toRawUTF8(), desc.version.toRawUTF8(), desc.manufacturerName.toRawUTF8());

    StringArray lines;
    File (argv[2]).readLines (lines);
    Job j;
    int runs = 0;
    for (auto line : lines)
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWith ("#"))
            continue;
        const auto key = line.upToFirstOccurrenceOf (" ", false, false);
        const auto rest = line.fromFirstOccurrenceOf (" ", false, false).trim();
        auto parseSet = [] (const String& s) { return std::make_pair (s.upToFirstOccurrenceOf ("=", false, false).trim(),
                                                                      s.fromFirstOccurrenceOf ("=", false, false).trim()); };
        if (key == "rate") j.rate = rest.getDoubleValue();
        else if (key == "channels") j.channels = rest.getIntValue();
        else if (key == "preroll") j.preroll = rest.getIntValue();
        else if (key == "sleep") j.sleepMs = rest.getIntValue();
        else if (key == "pdc") j.pdcMs = rest.getIntValue();
        else if (key == "nonrealtime") j.nonRealtime = rest.getIntValue() != 0;
        else if (key == "blocks")
        {
            auto t = StringArray::fromTokens (rest, " ", "");
            if (t[0] == "random") { j.blockMin = t[1].getIntValue(); j.blockMax = t[2].getIntValue(); j.seed = (unsigned) t[3].getIntValue(); }
            else j.blockMin = j.blockMax = t[0].getIntValue();
        }
        else if (key == "set") j.sets.push_back (parseSet (rest));
        else if (key == "setn") j.setns.push_back (parseSet (rest));
        else if (key == "params") listParams (fm, desc);
        else if (key == "at")
        {
            const int at = rest.upToFirstOccurrenceOf (" ", false, false).getIntValue();
            const bool norm = rest.contains (" setn ");
            auto s = parseSet (rest.fromFirstOccurrenceOf (norm ? "setn " : "set ", false, false));
            j.ats.emplace_back (at, (norm ? "#n#" : "") + s.first, s.second);
        }
        else if (key == "input") j.input = rest;
        else if (key == "sidechain") j.side = rest;
        else if (key == "output") j.output = rest;
        else if (key == "run")
        {
            std::printf ("run %d (fs %.0f, ch %d, blocks %d..%d, %s)\n", ++runs, j.rate, j.channels, j.blockMin, j.blockMax,
                         j.nonRealtime ? "offline" : "realtime");
            runJob (fm, desc, j);
            j = Job {};
        }
        else { std::fprintf (stderr, "bad line: %s\n", line.toRawUTF8()); return 1; }
    }
    return 0;
}
