#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "HostEngine.h"
#include "PluginWindow.h"

//==============================================================================
class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent (HostEngine&, juce::PropertiesFile&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Writes settings and the plugin's state (e.g. the loaded ARC X profile) to disk. */
    void saveState();

    void setBypassed (bool);
    bool isBypassed() const        { return engine.isBypassed(); }
    bool shouldCloseToTray() const    { return closeToTrayToggle.getToggleState(); }
    bool shouldMinimiseToTray() const { return minimiseToTrayToggle.getToggleState(); }

    /** One line about the app version / update state, shown in the status box. */
    void setVersionText (const juce::String& text)   { versionText = text; updateStatus(); }

    void openPluginEditor();
    void closePluginEditor();

private:
    void timerCallback() override;

    void populateCaptureModes();
    void populateSources (bool preferSaved);
    void populateOutputTypes();
    void populateOutputDevices (bool preferSaved);
    void populateInputDevices (bool preferSaved);
    void applyInput();
    void updateInputEnablement();
    void populatePlugins();
    static void fillChannelPairs (juce::ComboBox&, int numChannels, int selectedFirstChannel);
    static void fillChannelPairs (juce::ComboBox&, const juce::StringArray& channelNames, int selectedFirstChannel);
    bool isDirectInput() const   { return HostEngine::usesDirectInput (outputTypeBox.getText()); }

    HostEngine::Config configFromUI() const;
    void applyAudio();
    void saveSettings();
    void restorePlugin();
    void loadSelectedPlugin();
    void selectCurrentPluginInList();
    void updateStatus();

    static juce::String pairText (int firstChannel);

    HostEngine& engine;
    juce::PropertiesFile& props;

    juce::Label captureModeLabel { {}, "Capture" },  sourceLabel { {}, "From" },   sourceChannelsLabel { {}, "Channels" },
                outputTypeLabel  { {}, "Driver" },   outputDeviceLabel { {}, "To" }, outputChannelsLabel { {}, "Channels" },
                safetyLabel      { {}, "Safety buffer" }, pluginLabel { {}, "Plugin" },
                inputDeviceLabel { {}, "From" }, inputChannelsLabel { {}, "Channels" }, inputLevelLabel { {}, "Level" };

    juce::ComboBox captureModeBox, sourceBox, sourceChannelsBox,
                   outputTypeBox, outputDeviceBox, outputChannelsBox,
                   safetyBox, pluginBox, inputDeviceBox, inputChannelsBox;

    juce::Slider inputLevelSlider;

    juce::Label hintLabel, inputHintLabel, sectionCapture { {}, "Capture (what to correct)" },
                sectionInput { {}, "Interface input (optional, mixed in too)" },
                sectionOutput { {}, "Output (your second pair of speakers)" },
                sectionPlugin { {}, "Correction plugin" },
                sectionStatus { {}, "Status" };

    juce::TextButton rescanButton { "Rescan" }, editorButton { "Plugin window" },
                     removeButton { "Remove" }, restartButton { "Restart audio" };

    juce::ToggleButton inputToggle { "Also mix in an input from your interface (e.g. ADAT 9/10)" },
                       bypassToggle { "Bypass correction" },
                       closeToTrayToggle { "Closing keeps it running in the tray" },
                       minimiseToTrayToggle { "Minimise to the tray" },
                       startWithSystemToggle { "Start with Windows" };

    juce::TextEditor statusText;

    juce::StringArray pluginIds;
    std::unique_ptr<PluginWindow> pluginWindow;

    bool updatingUI = false;
    bool pluginLoading = false;
    int  ticksSinceStateSave = 0;
    juce::String lastSavedPluginState;
    juce::String versionText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
