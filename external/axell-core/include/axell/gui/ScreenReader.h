#pragma once
// Axell core - screen readers (Narrator on Windows, VoiceOver on macOS): every slider, button and menu under root that
// has a tooltip but no title gets one - the tooltip's opening words ("QUALITY - oversampling: ..." -> "QUALITY") - and
// the tooltip as its description. Call it at the end of an editor's constructor; a control with a title keeps it.
// Header-only, so a plugin with its own look and feel can use it too.
#include <juce_gui_basics/juce_gui_basics.h>

namespace axell
{
inline void nameControlsForScreenReaders (juce::Component& root)
{
    for (auto* child : root.getChildren())
    {
        const bool control = dynamic_cast<juce::Slider*> (child) != nullptr || dynamic_cast<juce::Button*> (child) != nullptr
                          || dynamic_cast<juce::ComboBox*> (child) != nullptr;
        if (auto* tc = dynamic_cast<juce::TooltipClient*> (child); control && tc != nullptr && child->getTitle().isEmpty())
        {
            const auto tip = tc->getTooltip().trim();
            if (tip.isNotEmpty())
            {
                auto name = tip;
                for (auto sep : { " - ", ": ", ". " })
                    if (name.contains (sep))
                        name = name.upToFirstOccurrenceOf (sep, false, false);
                child->setTitle (name.trimCharactersAtEnd (".").substring (0, 60).trim());
                if (child->getDescription().isEmpty())
                    child->setDescription (tip);
            }
        }
        nameControlsForScreenReaders (*child);
    }
}
} // namespace axell
