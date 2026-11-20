// Renders the tray/app logo at the sizes Windows and macOS use, for visual checking.
#include <juce_graphics/juce_graphics.h>
#include "AppIcon.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    dir.createDirectory();

    const std::pair<AppIcon::Style, const char*> styles[] { { AppIcon::Style::colour, "colour" },
                                                            { AppIcon::Style::bypassed, "bypassed" },
                                                            { AppIcon::Style::monochrome, "template" } };
    for (int size : { 16, 20, 24, 32, 64 })
        for (auto& [style, name] : styles)
        {
            juce::FileOutputStream out (dir.getChildFile (juce::String (name) + "_" + juce::String (size) + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (AppIcon::render (size, style), out);
        }
    return 0;
}
