#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/** A window showing a hosted plugin's own editor (or a generic one if it has none). */
class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& processor, std::function<void()> onCloseRequested)
        : juce::DocumentWindow (processor.getName(),
                                juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          closeRequested (std::move (onCloseRequested))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;

        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (processor);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    ~PluginWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (closeRequested)
            closeRequested();
    }

private:
    std::function<void()> closeRequested;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
