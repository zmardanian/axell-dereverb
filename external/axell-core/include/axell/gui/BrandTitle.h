#pragma once
// Axell's Dojo - the plugin title: "Axell" in gold and the plugin's name in a brush-written face (Yuji Syuku, SIL OFL
// 1.1), followed by a vermilion hanko seal holding the plugin's kanji. Used in the header of every Axell plugin.
#include "BrushFontData.h"
#include "DojoEmblem.h"

namespace axell::brand
{
// the brush face (Latin + the suite's kanji: 五 双 門 静 絹 匠 道 場)
inline juce::Typeface::Ptr brushTypeface()
{
    static juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (fontdata::yujiSyuku, (size_t) fontdata::yujiSyukuSize);
    return tf;
}
inline juce::Font brushFont (float height, float tracking = 0.0f)
{
    return juce::Font (juce::FontOptions().withTypeface (brushTypeface()).withHeight (height).withKerningFactor (tracking));
}

// the outline of `text` in the brush face, scaled so that a capital letter is `capHeight` tall, baseline at y = 0, x = 0.
// (Sized by the real glyphs: this Japanese face's line metrics are much taller than its Latin letters.)
inline juce::Path brushPath (const juce::String& text, float capHeight, float tracking = 0.0f)
{
    using namespace juce;
    static const float capAt100 = []
    {
        GlyphArrangement ga;
        ga.addLineOfText (brushFont (100.0f), "H", 0.0f, 0.0f);
        Path p;
        ga.createPath (p);
        return jmax (1.0f, p.getBounds().getHeight());
    }();
    GlyphArrangement ga;
    ga.addLineOfText (brushFont (100.0f, tracking), text, 0.0f, 0.0f);
    Path p;
    ga.createPath (p);
    p.applyTransform (AffineTransform::scale (capHeight / capAt100));
    return p;
}

// fills a brush outline with a little extra ink (a stroke of the same paint), so thin strokes read as brushed
inline void fillInk (juce::Graphics& g, const juce::Path& p, float weight)
{
    g.fillPath (p);
    if (weight > 0.0f)
        g.strokePath (p, juce::PathStrokeType (weight, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// a hanko seal: a vermilion square with the kanji cut out in ivory, its edge slightly worn like stamped ink
inline void drawSeal (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& kanji)
{
    using namespace juce;
    const float s = r.getWidth();
    Path p;
    p.addRoundedRectangle (r, 0.12f * s);
    DropShadow (Colours::black.withAlpha (0.45f), (int) (0.25f * s), { 0, (int) (0.06f * s) + 1 }).drawForPath (g, p);
    ColourGradient ink (kSun.brighter (0.12f), r.getX(), r.getY(), kSun.darker (0.35f), r.getRight(), r.getBottom(), false);
    g.setGradientFill (ink);
    g.fillPath (p);
    static const float flecks[][3] = { { 0.08f, 0.02f, 0.10f }, { 0.62f, 0.00f, 0.07f }, { 0.97f, 0.40f, 0.08f },
                                       { 0.30f, 0.98f, 0.09f }, { 0.01f, 0.70f, 0.06f }, { 0.85f, 0.93f, 0.05f } };
    g.setColour (Colours::black.withAlpha (0.28f));
    for (auto& f : flecks)
        g.fillEllipse (r.getX() + f[0] * s - 0.5f * f[2] * s, r.getY() + f[1] * s - 0.5f * f[2] * s, f[2] * s, f[2] * s);
    g.setColour (kIvory.withAlpha (0.55f));
    g.drawRoundedRectangle (r.reduced (0.09f * s), 0.08f * s, jmax (0.8f, 0.045f * s));
    // the kanji fills 56 % of the seal, centred on its own outline
    auto k = brushPath (kanji, 100.0f);
    const auto b = k.getBounds();
    const float sc = 0.56f * s / jmax (b.getWidth(), b.getHeight());
    k.applyTransform (AffineTransform::translation (-b.getCentreX(), -b.getCentreY()).scaled (sc).translated (r.getCentreX(), r.getCentreY()));
    g.setColour (kIvory);
    fillInk (g, k, 0.02f * s);
}

// the title: "Axell" in gold, the plugin's name in brushed ivory, the plugin's seal. `capHeight` = the name's capital
// height, `baseline` its baseline. Returns the right edge.
inline float drawTitle (juce::Graphics& g, float x, float baseline, const juce::String& name, const juce::String& kanji,
                        float capHeight = 16.5f)
{
    using namespace juce;
    auto small = brushPath ("Axell", 0.60f * capHeight, 0.06f);
    auto big = brushPath (name, capHeight, -0.01f);
    const float wA = small.getBounds().getRight(), wN = big.getBounds().getRight();
    small.applyTransform (AffineTransform::translation (x, baseline));
    g.setGradientFill (ColourGradient (kGoldLight, 0.0f, baseline - 0.6f * capHeight, kGoldDeep, 0.0f, baseline + 2.0f, false));
    fillInk (g, small, 0.025f * capHeight);
    const float xn = x + wA + 0.38f * capHeight;
    big.applyTransform (AffineTransform::translation (xn, baseline));
    DropShadow (Colours::black.withAlpha (0.6f), (int) (0.25f * capHeight), { 0, 1 }).drawForPath (g, big);
    g.setGradientFill (ColourGradient (kIvory, 0.0f, baseline - capHeight, kIvoryShade, 0.0f, baseline + 0.3f * capHeight, false));
    fillInk (g, big, 0.022f * capHeight);
    float right = xn + wN;
    if (kanji.isNotEmpty())
    {
        const float sz = 1.32f * capHeight;
        drawSeal (g, { right + 0.55f * capHeight, baseline - 0.5f * capHeight - 0.5f * sz, sz, sz }, kanji);
        right += 0.55f * capHeight + sz;
    }
    return right;
}

// the plugin's version after the title ("v4.0.0"): small and quiet, there to read when it matters (which build is
// running), never competing with the title. `x` = drawTitle's right edge, `baseline` the title's. Returns the right edge.
inline float drawVersion (juce::Graphics& g, float x, float baseline, const juce::String& version, float capHeight = 16.5f)
{
    using namespace juce;
    const String s = "v" + version;
    const Font f (FontOptions (0.62f * capHeight));
    const float x0 = x + 0.5f * capHeight;
    g.setFont (f);
    g.setColour (kIvory.withAlpha (0.42f));
    g.drawSingleLineText (s, roundToInt (x0), roundToInt (baseline));
    return x0 + GlyphArrangement::getStringWidth (f, s);
}

// the same label placed in `area` (a plugin whose header has no room after the title puts it in a margin)
inline void drawVersionIn (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& version,
                           juce::Justification just = juce::Justification::centredRight, float capHeight = 16.5f)
{
    using namespace juce;
    g.setFont (Font (FontOptions (0.62f * capHeight)));
    g.setColour (kIvory.withAlpha (0.42f));
    g.drawText ("v" + version, area, just, false);
}
} // namespace axell::brand
