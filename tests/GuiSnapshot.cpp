// Axell De-Reverb - headless GUI snapshot: the real processor and editor, fed a reverberant voice-like signal (or a WAV
// given as the second argument), saved as PNGs.   DrGuiSnapshot [outputDir] [vocal.wav]
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <random>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir (juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/screenshots"));
    dir.createDirectory();
    juce::AudioBuffer<float> voice;
    if (argc > 2)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (argv[2])));
        if (r != nullptr)
        {
            const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (10.0 * r->sampleRate));
            voice.setSize (2, len);
            r->read (&voice, 0, len, std::min<juce::int64> (r->lengthInSamples - len, (juce::int64) (8.0 * r->sampleRate)), true, true);
            if (r->numChannels == 1)
                voice.copyFrom (1, 0, voice, 0, 0, len);
        }
    }
    if (voice.getNumSamples() == 0)
    {
        voice.setSize (2, 48000 * 8);
        std::mt19937 g (1);
        std::normal_distribution<float> nd;
        double ph = 0, tail = 0;
        for (int i = 0; i < voice.getNumSamples(); ++i)
        {
            const double t = i / 48000.0;
            ph += 2 * juce::MathConstants<double>::pi * 140 / 48000.0;
            double y = 0;
            for (int h = 1; h < 20; ++h)
                y += std::sin (h * ph) / h;
            y *= std::fmod (t, 0.6) < 0.35 ? 0.15 : 0.0;
            tail = tail * 0.9997 + y * 0.002;
            const float v = (float) (y + tail * nd (g));
            voice.setSample (0, i, v);
            voice.setSample (1, i, v);
        }
    }
    auto shoot = [&] (const juce::String& name, float scale, std::function<void (DeReverbProcessor&)> setup, float pixels = 1.0f) {
        DeReverbProcessor proc;
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        setup (proc);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        ed->setSize (juce::roundToInt (gui::kWidth * scale), juce::roundToInt (gui::kHeight * scale));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + 512 <= voice.getNumSamples(); pos += 512)
        {
            for (int c = 0; c < 2; ++c)
                buf.copyFrom (c, 0, voice, c, pos, 512);
            proc.processBlock (buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, pixels);
        auto f = dir.getChildFile (name + ".png");
        f.deleteFile();
        juce::FileOutputStream os (f);
        juce::PNGImageFormat png;
        if (os.openedOk() && png.writeImageToStream (img, os))
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
    };
    shoot ("dereverb", 1.0f, [] (DeReverbProcessor&) {});
    shoot ("dereverb-listen", 1.0f, [] (DeReverbProcessor& p) { p.apvts.getParameter ("listen")->setValueNotifyingHost (1.0f); });
    shoot ("dereverb-75pct", 0.75f, [] (DeReverbProcessor& p) { p.setCurrentProgram (1); });
    shoot ("dereverb-hidpi2x", 1.0f, [] (DeReverbProcessor&) {}, 2.0f);   // HiDPI: twice the pixels
    return 0;
}
