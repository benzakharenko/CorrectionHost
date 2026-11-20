#include "MainComponent.h"
#include "Updater.h"

namespace Keys
{
    static const char* captureMode        = "captureMode";
    static const char* sourceName         = "sourceName";
    static const char* sourceFirstChannel = "sourceFirstChannel";
    static const char* outputType         = "outputType";
    static const char* outputDevice       = "outputDevice";
    static const char* outputFirstChannel = "outputFirstChannel";
    static const char* safetyMs           = "safetyMs";
    static const char* pluginDescription  = "pluginDescription";
    static const char* pluginState        = "pluginState";
    static const char* bypassed           = "bypassed";
    static const char* closeToTray        = "closeToTray";
    static const char* minimiseToTray     = "minimiseToTray";
    static const char* inputEnabled       = "inputEnabled";
    static const char* inputDevice        = "inputDevice";
    static const char* inputFirstChannel  = "inputFirstChannel";
    static const char* inputLevelDb       = "inputLevelDb";
}

#if JUCE_WINDOWS
namespace
{
    const char* runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\CorrectionHost";

    bool isStartingWithWindows()
    {
        return juce::WindowsRegistry::valueExists (runKey);
    }

    void setStartWithWindows (bool shouldStart)
    {
        if (shouldStart)
        {
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
            juce::WindowsRegistry::setValue (runKey, "\"" + exe + "\" --minimised");
        }
        else
        {
            juce::WindowsRegistry::deleteValue (runKey);
        }
    }
}
#endif

//==============================================================================
MainComponent::MainComponent (HostEngine& e, juce::PropertiesFile& p)
    : engine (e), props (p)
{
    for (auto* l : { &captureModeLabel, &sourceLabel, &sourceChannelsLabel, &outputTypeLabel,
                     &outputDeviceLabel, &outputChannelsLabel, &safetyLabel, &pluginLabel,
                     &inputDeviceLabel, &inputChannelsLabel, &inputLevelLabel })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }

    for (auto* l : { &sectionCapture, &sectionInput, &sectionOutput, &sectionPlugin, &sectionStatus })
    {
        l->setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (*l);
    }

    hintLabel.setFont (juce::FontOptions (12.5f));
    hintLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    hintLabel.setJustificationType (juce::Justification::topLeft);
    hintLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (hintLabel);

    inputHintLabel.setFont (juce::FontOptions (12.5f));
    inputHintLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    inputHintLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (inputHintLabel);

    for (auto* b : { &captureModeBox, &sourceBox, &sourceChannelsBox, &outputTypeBox,
                     &outputDeviceBox, &outputChannelsBox, &safetyBox, &pluginBox,
                     &inputDeviceBox, &inputChannelsBox })
        addAndMakeVisible (*b);

    inputLevelSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    inputLevelSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    inputLevelSlider.setRange (-30.0, 12.0, 0.5);
    inputLevelSlider.setTextValueSuffix (" dB");
    inputLevelSlider.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (inputLevelSlider);

    for (auto* b : { &rescanButton, &editorButton, &removeButton, &restartButton })
        addAndMakeVisible (*b);

    for (auto* t : { &inputToggle, &bypassToggle, &closeToTrayToggle, &minimiseToTrayToggle })
        addAndMakeVisible (*t);

   #if JUCE_WINDOWS
    addAndMakeVisible (startWithSystemToggle);
    startWithSystemToggle.setToggleState (isStartingWithWindows(), juce::dontSendNotification);
    if (isStartingWithWindows() && Updater::isUpdatableBuild())
        setStartWithWindows (true);   // keep the entry pointing at the installed copy
    startWithSystemToggle.onClick = [this] { setStartWithWindows (startWithSystemToggle.getToggleState()); };
   #endif

    statusText.setMultiLine (true);
    statusText.setReadOnly (true);
    statusText.setCaretVisible (false);
    statusText.setScrollbarsShown (false);
    statusText.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    addAndMakeVisible (statusText);

    // Safety buffer choices (extra latency in exchange for robustness against glitches).
    for (int ms : { 2, 5, 10, 20, 40 })
        safetyBox.addItem (juce::String (ms) + " ms", ms);

    //==============================================================================
    updatingUI = true;

    populateCaptureModes();
    populateSources (true);
    populateOutputTypes();
    populateOutputDevices (true);

    fillChannelPairs (sourceChannelsBox, 8, props.getIntValue (Keys::sourceFirstChannel, 0));
    fillChannelPairs (outputChannelsBox, 8, props.getIntValue (Keys::outputFirstChannel, 2));   // D3Vs on 3-4 by default
    safetyBox.setSelectedId (juce::jlimit (2, 40, (int) props.getDoubleValue (Keys::safetyMs, 5.0)), juce::dontSendNotification);
    if (safetyBox.getSelectedId() == 0)
        safetyBox.setSelectedId (5, juce::dontSendNotification);

    inputToggle.setToggleState (props.getBoolValue (Keys::inputEnabled, false), juce::dontSendNotification);
    populateInputDevices (true);
    fillChannelPairs (inputChannelsBox, 16, props.getIntValue (Keys::inputFirstChannel, 0));
    inputLevelSlider.setValue (props.getDoubleValue (Keys::inputLevelDb, 0.0), juce::dontSendNotification);
    engine.setInputLevelDb ((float) inputLevelSlider.getValue());
    updateInputEnablement();

    bypassToggle.setToggleState (props.getBoolValue (Keys::bypassed, false), juce::dontSendNotification);
    engine.setBypassed (bypassToggle.getToggleState());
    closeToTrayToggle.setToggleState (props.getBoolValue (Keys::closeToTray, true), juce::dontSendNotification);
    minimiseToTrayToggle.setToggleState (props.getBoolValue (Keys::minimiseToTray, true), juce::dontSendNotification);

    populatePlugins();
    updatingUI = false;

    //==============================================================================
    captureModeBox.onChange = [this]
    {
        if (updatingUI) return;
        updatingUI = true;
        populateSources (false);
        updatingUI = false;
        applyAudio();
    };

    outputTypeBox.onChange = [this]
    {
        if (updatingUI) return;
        updatingUI = true;
        populateOutputDevices (false);
        populateInputDevices (true);      // ASIO switches the input to "same device"
        updatingUI = false;
        updateInputEnablement();
        applyAudio();
    };

    outputDeviceBox.onChange = [this]
    {
        if (updatingUI) return;

        if (isDirectInput())
        {
            updatingUI = true;
            populateInputDevices (true);
            updatingUI = false;
        }

        applyAudio();
    };

    for (auto* b : { &sourceBox, &sourceChannelsBox, &outputChannelsBox, &safetyBox })
        b->onChange = [this] { if (! updatingUI) applyAudio(); };

    pluginBox.onChange = [this] { if (! updatingUI) loadSelectedPlugin(); };

    inputToggle.onClick = [this] { updateInputEnablement(); applyInput(); };

    for (auto* b : { &inputDeviceBox, &inputChannelsBox })
        b->onChange = [this] { if (! updatingUI) applyInput(); };

    inputLevelSlider.onValueChange = [this] { engine.setInputLevelDb ((float) inputLevelSlider.getValue()); };
    inputLevelSlider.onDragEnd     = [this] { saveSettings(); };

    rescanButton.onClick  = [this] { updatingUI = true; populatePlugins(); selectCurrentPluginInList(); updatingUI = false; };
    editorButton.onClick  = [this] { openPluginEditor(); };
    restartButton.onClick = [this] { applyAudio(); };

    removeButton.onClick = [this]
    {
        closePluginEditor();
        engine.unloadPlugin();
        updatingUI = true;
        pluginBox.setSelectedId (1, juce::dontSendNotification);
        updatingUI = false;
        saveState();
    };

    bypassToggle.onClick = [this]
    {
        engine.setBypassed (bypassToggle.getToggleState());
        saveSettings();
    };

    closeToTrayToggle.onClick = [this] { saveSettings(); };
    minimiseToTrayToggle.onClick = [this] { saveSettings(); };

    engine.onPluginAboutToChange = [this] { closePluginEditor(); };
    engine.onStateChanged = [this] { updateStatus(); };

    setSize (720, 800);

    applyAudio();
    restorePlugin();
    startTimerHz (4);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine.onPluginAboutToChange = nullptr;
    engine.onStateChanged = nullptr;
    closePluginEditor();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (14);
    const int rowH = 26, gap = 6, labelW = 96;

    auto section = [&] (juce::Label& l)
    {
        l.setBounds (area.removeFromTop (22));
        area.removeFromTop (4);
    };

    auto row = [&]
    {
        auto r = area.removeFromTop (rowH);
        area.removeFromTop (gap);
        return r;
    };

    auto labelled = [&] (juce::Rectangle<int>& r, juce::Label& l, juce::Component& c, int width)
    {
        l.setBounds (r.removeFromLeft (labelW));
        r.removeFromLeft (6);
        c.setBounds (width > 0 ? r.removeFromLeft (width) : r);
        r.removeFromLeft (8);
    };

    section (sectionCapture);
    {
        auto r = row();
        labelled (r, captureModeLabel, captureModeBox, 0);
    }
    {
        auto r = row();
        auto chans = r.removeFromRight (260);
        labelled (r, sourceLabel, sourceBox, 0);
        labelled (chans, sourceChannelsLabel, sourceChannelsBox, 0);
        sourceChannelsLabel.setBounds (sourceChannelsLabel.getBounds().withTrimmedLeft (labelW - 64));
    }
    hintLabel.setBounds (area.removeFromTop (36).withTrimmedLeft (labelW + 6));
    area.removeFromTop (8);

    section (sectionInput);
    {
        auto r = row();
        inputToggle.setBounds (r.withTrimmedLeft (labelW + 6));
    }
    {
        auto r = row();
        auto chans = r.removeFromRight (260);
        labelled (r, inputDeviceLabel, inputDeviceBox, 0);
        labelled (chans, inputChannelsLabel, inputChannelsBox, 0);
        inputChannelsLabel.setBounds (inputChannelsLabel.getBounds().withTrimmedLeft (labelW - 64));
    }
    {
        auto r = row();
        labelled (r, inputLevelLabel, inputLevelSlider, 300);
    }
    inputHintLabel.setBounds (area.removeFromTop (20).withTrimmedLeft (labelW + 6));
    area.removeFromTop (8);

    section (sectionOutput);
    {
        auto r = row();
        labelled (r, outputTypeLabel, outputTypeBox, 220);
    }
    {
        auto r = row();
        auto chans = r.removeFromRight (260);
        labelled (r, outputDeviceLabel, outputDeviceBox, 0);
        labelled (chans, outputChannelsLabel, outputChannelsBox, 0);
        outputChannelsLabel.setBounds (outputChannelsLabel.getBounds().withTrimmedLeft (labelW - 64));
    }
    {
        auto r = row();
        labelled (r, safetyLabel, safetyBox, 100);
        restartButton.setBounds (r.removeFromRight (120));
    }
    area.removeFromTop (8);

    section (sectionPlugin);
    {
        auto r = row();
        auto buttons = r.removeFromRight (258);
        buttons.removeFromLeft (8);
        labelled (r, pluginLabel, pluginBox, 0);
        removeButton.setBounds (buttons.removeFromRight (70));
        buttons.removeFromRight (6);
        editorButton.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (6);
        rescanButton.setBounds (buttons.removeFromRight (68));
    }
    {
        auto r = row();
        bypassToggle.setBounds (r.withTrimmedLeft (labelW + 6));
    }
    area.removeFromTop (8);

    auto bottom = area.removeFromBottom (rowH);
    closeToTrayToggle.setBounds (bottom.removeFromLeft (270));
    minimiseToTrayToggle.setBounds (bottom.removeFromLeft (190));
    startWithSystemToggle.setBounds (bottom);
    area.removeFromBottom (gap);

    section (sectionStatus);
    statusText.setBounds (area);
}

//==============================================================================
juce::String MainComponent::pairText (int firstChannel)
{
    return juce::String (firstChannel + 1) + "-" + juce::String (firstChannel + 2);
}

void MainComponent::fillChannelPairs (juce::ComboBox& box, int numChannels, int selectedFirstChannel)
{
    box.clear (juce::dontSendNotification);
    const int count = juce::jmax (2, numChannels);

    for (int first = 0; first + 1 < count; first += 2)
        box.addItem (pairText (first), first + 1);

    box.setSelectedId (selectedFirstChannel + 1, juce::dontSendNotification);

    if (box.getSelectedId() == 0)
        box.setSelectedItemIndex (0, juce::dontSendNotification);
}

void MainComponent::fillChannelPairs (juce::ComboBox& box, const juce::StringArray& names, int selectedFirstChannel)
{
    if (names.size() < 2)
    {
        fillChannelPairs (box, names.size(), selectedFirstChannel);
        return;
    }

    box.clear (juce::dontSendNotification);

    for (int first = 0; first + 1 < names.size(); first += 2)
    {
        auto label = pairText (first);
        const auto& a = names[first];
        const auto& b = names[first + 1];

        // Show the driver's own names (e.g. "ADAT 1 / ADAT 2") unless they're just generic numbering.
        if (! a.containsIgnoreCase ("channel") && a.isNotEmpty())
            label << "   " << a << " / " << b;

        box.addItem (label, first + 1);
    }

    box.setSelectedId (selectedFirstChannel + 1, juce::dontSendNotification);

    if (box.getSelectedId() == 0)
        box.setSelectedItemIndex (0, juce::dontSendNotification);
}

void MainComponent::populateCaptureModes()
{
    captureModeBox.clear (juce::dontSendNotification);

    for (int i = 0; i < engine.getNumCaptureModes(); ++i)
        captureModeBox.addItem (engine.getCaptureMode (i).getModeName(), i + 1);

    captureModeBox.setSelectedId (juce::jlimit (0, engine.getNumCaptureModes() - 1, props.getIntValue (Keys::captureMode, 0)) + 1,
                                  juce::dontSendNotification);
    captureModeBox.setEnabled (engine.getNumCaptureModes() > 1);
}

void MainComponent::populateSources (bool preferSaved)
{
    auto& mode = engine.getCaptureMode (captureModeBox.getSelectedId() - 1);
    hintLabel.setText (mode.getModeHint(), juce::dontSendNotification);

    const auto names = mode.getAvailableSources();
    sourceBox.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        sourceBox.addItem (names[i], i + 1);

    juce::String wanted = preferSaved ? props.getValue (Keys::sourceName) : juce::String();

    if (! names.contains (wanted))
        wanted = mode.getDefaultSource();

    sourceBox.setSelectedItemIndex (juce::jmax (0, names.indexOf (wanted)), juce::dontSendNotification);
}

void MainComponent::populateOutputTypes()
{
    const auto types = engine.getOutputTypeNames();
    outputTypeBox.clear (juce::dontSendNotification);

    for (int i = 0; i < types.size(); ++i)
        outputTypeBox.addItem (types[i], i + 1);

    auto wanted = props.getValue (Keys::outputType, engine.getDefaultOutputTypeName());

    if (! types.contains (wanted))
        wanted = engine.getDefaultOutputTypeName();

    outputTypeBox.setSelectedItemIndex (juce::jmax (0, types.indexOf (wanted)), juce::dontSendNotification);
}

void MainComponent::populateOutputDevices (bool preferSaved)
{
    const auto type  = outputTypeBox.getText();
    const auto names = engine.getOutputDeviceNames (type);
    outputDeviceBox.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        outputDeviceBox.addItem (names[i], i + 1);

    juce::String wanted = preferSaved ? props.getValue (Keys::outputDevice) : juce::String();

    if (! names.contains (wanted))
        wanted = engine.getDefaultOutputDeviceName (type);

    outputDeviceBox.setSelectedItemIndex (juce::jmax (0, names.indexOf (wanted)), juce::dontSendNotification);
}

void MainComponent::populateInputDevices (bool preferSaved)
{
    inputDeviceBox.clear (juce::dontSendNotification);

    if (isDirectInput())
    {
        // ASIO: the input comes from the output device itself.
        inputDeviceBox.addItem (outputDeviceBox.getText() + "  (same device as the output)", 1);
        inputDeviceBox.setSelectedId (1, juce::dontSendNotification);
        inputHintLabel.setText ("ASIO: the input pair comes straight from the output device - same clock, no extra latency.",
                                juce::dontSendNotification);
        return;
    }

    inputHintLabel.setText ("Read in shared mode, so your DAW can keep using these inputs at the same time.",
                            juce::dontSendNotification);

    const auto names = engine.getInputSource().getAvailableSources();

    for (int i = 0; i < names.size(); ++i)
        inputDeviceBox.addItem (names[i], i + 1);

    juce::String wanted = preferSaved ? props.getValue (Keys::inputDevice) : juce::String();

    // First run: prefer a real interface input over virtual cables.
    if (! names.contains (wanted))
    {
        wanted = names.isEmpty() ? juce::String() : names[0];

        for (const auto& n : names)
        {
            if (! n.containsIgnoreCase ("CABLE") && ! n.containsIgnoreCase ("BlackHole") && ! n.containsIgnoreCase ("Microphone"))
            {
                wanted = n;
                break;
            }
        }
    }

    inputDeviceBox.setSelectedItemIndex (juce::jmax (0, names.indexOf (wanted)), juce::dontSendNotification);
}

void MainComponent::updateInputEnablement()
{
    const bool on = inputToggle.getToggleState();

    for (juce::Component* c : { (juce::Component*) &inputDeviceBox, (juce::Component*) &inputChannelsBox,
                                (juce::Component*) &inputLevelSlider, (juce::Component*) &inputDeviceLabel,
                                (juce::Component*) &inputChannelsLabel, (juce::Component*) &inputLevelLabel })
        c->setEnabled (on);

    // With ASIO the device is fixed to the output device.
    inputDeviceBox.setEnabled (on && ! isDirectInput());
}

void MainComponent::applyInput()
{
    const auto c = configFromUI();
    engine.setInputConfig (c.inputEnabled, c.inputDeviceName, c.inputFirstChannel);

    // Offer the channel pairs the input device really has once it's open.
    if (const auto names = engine.getOpenInputChannelNames(); names.size() > 0)
    {
        updatingUI = true;
        fillChannelPairs (inputChannelsBox, names, c.inputFirstChannel);
        updatingUI = false;
    }

    saveSettings();
    updateStatus();
}

void MainComponent::populatePlugins()
{
    pluginIds = engine.findPluginIdentifiers();

    juce::StringArray names;
    for (const auto& id : pluginIds)
        names.add (engine.getPluginDisplayName (id));

    // Sort by display name, keeping ids in step.
    std::vector<int> order ((size_t) pluginIds.size());
    for (int i = 0; i < pluginIds.size(); ++i) order[(size_t) i] = i;
    std::sort (order.begin(), order.end(), [&] (int a, int b) { return names[a].compareNatural (names[b]) < 0; });

    juce::StringArray sortedIds;
    pluginBox.clear (juce::dontSendNotification);
    pluginBox.addItem ("(none)", 1);

    for (size_t i = 0; i < order.size(); ++i)
    {
        sortedIds.add (pluginIds[order[i]]);
        pluginBox.addItem (names[order[i]], (int) i + 2);
    }

    pluginIds = sortedIds;
    pluginBox.setSelectedId (1, juce::dontSendNotification);
}

void MainComponent::selectCurrentPluginInList()
{
    const auto id = engine.getPlugin() != nullptr ? engine.getPluginDescription().fileOrIdentifier : juce::String();
    const int index = id.isNotEmpty() ? pluginIds.indexOf (id) : -1;

    if (index >= 0)
    {
        pluginBox.setSelectedId (index + 2, juce::dontSendNotification);
    }
    else if (engine.getPlugin() != nullptr)
    {
        // Loaded from somewhere outside the standard folders: show it anyway.
        pluginIds.add (id);
        pluginBox.addItem (engine.getPlugin()->getName(), pluginIds.size() + 1);
        pluginBox.setSelectedId (pluginIds.size() + 1, juce::dontSendNotification);
    }
    else
    {
        pluginBox.setSelectedId (1, juce::dontSendNotification);
    }
}

//==============================================================================
HostEngine::Config MainComponent::configFromUI() const
{
    HostEngine::Config c;
    c.captureMode        = juce::jmax (0, captureModeBox.getSelectedId() - 1);
    c.sourceName         = sourceBox.getText();
    c.sourceFirstChannel = juce::jmax (0, sourceChannelsBox.getSelectedId() - 1);
    c.outputTypeName     = outputTypeBox.getText();
    c.outputDeviceName   = outputDeviceBox.getText();
    c.outputFirstChannel = juce::jmax (0, outputChannelsBox.getSelectedId() - 1);
    c.safetyMs           = (double) juce::jmax (2, safetyBox.getSelectedId());
    c.inputEnabled       = inputToggle.getToggleState();
    c.inputDeviceName    = isDirectInput() ? outputDeviceBox.getText() : inputDeviceBox.getText();
    c.inputFirstChannel  = juce::jmax (0, inputChannelsBox.getSelectedId() - 1);
    return c;
}

void MainComponent::applyAudio()
{
    const auto config = configFromUI();
    engine.start (config);

    // Now that the devices are open, offer the channel pairs they really have.
    updatingUI = true;

    if (const auto names = engine.getOpenOutputChannelNames(); names.size() > 0)
        fillChannelPairs (outputChannelsBox, names, config.outputFirstChannel);

    if (const int n = engine.getOpenSourceChannelCount(); n > 0)
        fillChannelPairs (sourceChannelsBox, n, config.sourceFirstChannel);

    if (const auto names = engine.getOpenInputChannelNames(); names.size() > 0)
        fillChannelPairs (inputChannelsBox, names, config.inputFirstChannel);

    updatingUI = false;

    saveSettings();
    updateStatus();
}

void MainComponent::saveSettings()
{
    const auto c = configFromUI();
    props.setValue (Keys::captureMode,        c.captureMode);
    props.setValue (Keys::sourceName,         c.sourceName);
    props.setValue (Keys::sourceFirstChannel, c.sourceFirstChannel);
    props.setValue (Keys::outputType,         c.outputTypeName);
    props.setValue (Keys::outputDevice,       c.outputDeviceName);
    props.setValue (Keys::outputFirstChannel, c.outputFirstChannel);
    props.setValue (Keys::safetyMs,           c.safetyMs);
    props.setValue (Keys::bypassed,           bypassToggle.getToggleState());
    props.setValue (Keys::closeToTray,        closeToTrayToggle.getToggleState());
    props.setValue (Keys::minimiseToTray,     minimiseToTrayToggle.getToggleState());
    props.setValue (Keys::inputEnabled,       c.inputEnabled);
    if (! isDirectInput())
        props.setValue (Keys::inputDevice,    c.inputDeviceName);   // keep the shared-mode choice for later
    props.setValue (Keys::inputFirstChannel,  c.inputFirstChannel);
    props.setValue (Keys::inputLevelDb,       inputLevelSlider.getValue());
    props.saveIfNeeded();
}

void MainComponent::saveState()
{
    saveSettings();

    if (pluginLoading)
        return;   // don't overwrite the saved state with a half-restored plugin

    if (auto* plugin = engine.getPlugin())
    {
        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        const auto encoded = state.toBase64Encoding();

        if (encoded != lastSavedPluginState)
        {
            props.setValue (Keys::pluginState, encoded);
            lastSavedPluginState = encoded;
        }

        if (auto xml = engine.getPluginDescription().createXml())
            props.setValue (Keys::pluginDescription, xml.get());
    }
    else
    {
        props.removeValue (Keys::pluginState);
        props.removeValue (Keys::pluginDescription);
        lastSavedPluginState.clear();
    }

    props.saveIfNeeded();
}

//==============================================================================
void MainComponent::restorePlugin()
{
    auto xml = props.getXmlValue (Keys::pluginDescription);
    if (xml == nullptr)
        return;

    juce::PluginDescription description;
    if (! description.loadFromXml (*xml))
        return;

    juce::MemoryBlock state;
    const auto encoded = props.getValue (Keys::pluginState);
    if (encoded.isNotEmpty())
        state.fromBase64Encoding (encoded);

    pluginLoading = true;
    lastSavedPluginState = encoded;

    juce::Component::SafePointer<MainComponent> safe (this);
    engine.loadPlugin (description, state, [safe] (const juce::String& error)
    {
        if (safe == nullptr)
            return;

        safe->pluginLoading = false;
        safe->updatingUI = true;
        safe->selectCurrentPluginInList();
        safe->updatingUI = false;

        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Couldn't restore the plugin", error);
        safe->updateStatus();
    });
}

void MainComponent::loadSelectedPlugin()
{
    const int index = pluginBox.getSelectedId() - 2;

    if (index < 0)
    {
        closePluginEditor();
        engine.unloadPlugin();
        saveState();
        return;
    }

    closePluginEditor();
    pluginLoading = true;

    juce::Component::SafePointer<MainComponent> safe (this);
    engine.loadPlugin (pluginIds[index], {}, [safe] (const juce::String& error)
    {
        if (safe == nullptr)
            return;

        safe->pluginLoading = false;

        if (error.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Couldn't load the plugin", error);
            safe->updatingUI = true;
            safe->selectCurrentPluginInList();
            safe->updatingUI = false;
            return;
        }

        safe->saveState();
        safe->openPluginEditor();   // you'll usually want to pick the correction profile right away
        safe->updateStatus();
    });
}

void MainComponent::openPluginEditor()
{
    auto* plugin = engine.getPlugin();
    if (plugin == nullptr)
        return;

    if (pluginWindow != nullptr)
    {
        pluginWindow->toFront (true);
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    pluginWindow = std::make_unique<PluginWindow> (*plugin, [safe]
    {
        // Can't delete the window from inside its own callback.
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
            {
                safe->closePluginEditor();
                safe->saveState();   // e.g. a newly chosen ARC X profile
            }
        });
    });
}

void MainComponent::closePluginEditor()
{
    pluginWindow.reset();
}

void MainComponent::setBypassed (bool shouldBypass)
{
    bypassToggle.setToggleState (shouldBypass, juce::dontSendNotification);
    engine.setBypassed (shouldBypass);
    saveSettings();
}

//==============================================================================
void MainComponent::timerCallback()
{
    updateStatus();

    // Save the plugin's state every ~15 s so a crash or power cut doesn't lose a new profile.
    if (++ticksSinceStateSave >= 60)
    {
        ticksSinceStateSave = 0;
        saveState();
    }
}

void MainComponent::updateStatus()
{
    const auto s = engine.getStatus();
    const auto& f = s.fifo;
    juce::String text;

    juce::String state;
    if (s.running && f.running)                 state = "Running";
    else if (s.running && f.sourceStalled)      state = "Running - waiting for audio (nothing is playing)";
    else if (s.running)                         state = "Running - buffering";
    else if (s.wantRunning)                     state = "Not running - retrying";
    else                                        state = "Stopped";

    text << "State:    " << state << "\n";

    if (s.error.isNotEmpty())
        text << "Problem:  " << s.error << "\n";

    if (s.running)
    {
        text << "Capture:  " << s.sourceName << "  [" << pairText (engine.getConfig().sourceFirstChannel) << "]  "
             << juce::String (s.sourceRate, 0) << " Hz, " << s.sourceChannels << " ch, "
             << s.sourceBlock << "-frame blocks\n";

        text << "Output:   " << s.outputName << "  [" << pairText (engine.getConfig().outputFirstChannel) << "]  "
             << juce::String (s.outputRate, 0) << " Hz, " << s.outputBlock << "-frame buffer\n";

        text << "Clock:    " << (f.correctionPpm >= 0 ? "+" : "") << juce::String (f.correctionPpm, 1)
             << " ppm drift correction, buffer " << juce::String (f.fillFrames, 0) << " / "
             << juce::String (f.targetFrames, 0) << " frames\n";

        text << "Glitches: " << f.underruns << " underruns, " << f.resyncs << " rebuffers, "
             << f.overflows << " overflows, " << s.automaticRestarts << " automatic restarts\n";

        text << "Latency:  about " << juce::String (s.addedLatencyMs, 0) << " ms behind your main monitors";
        if (s.pluginLatencySamples > 0)
            text << " (plugin: " << s.pluginLatencySamples << " samples)";
        text << "\n";
    }

    const auto& in = s.input;

    if (in.enabled)
    {
        if (in.error.isNotEmpty())
        {
            text << "Input:    problem - " << in.error << " (retrying)\n";
        }
        else if (in.direct)
        {
            text << "Input:    " << in.name << "  [" << pairText (engine.getConfig().inputFirstChannel) << "]  "
                 << "direct from ASIO, about " << juce::String (in.latencyMs, 0) << " ms in to out\n";
        }
        else if (in.name.isNotEmpty())
        {
            text << "Input:    " << in.name << "  [" << pairText (engine.getConfig().inputFirstChannel) << "]  "
                 << juce::String (in.rate, 0) << " Hz, " << (in.fifo.correctionPpm >= 0 ? "+" : "")
                 << juce::String (in.fifo.correctionPpm, 1) << " ppm, "
                 << in.fifo.underruns << " underruns, about " << juce::String (in.latencyMs, 0) << " ms\n";
        }
    }

    text << "Plugin:   " << (s.pluginName.isNotEmpty() ? s.pluginName : juce::String ("none"));
    if (pluginLoading)                               text << " (loading...)";
    else if (s.pluginName.isNotEmpty() && s.bypassed) text << " (bypassed)";
    text << "\n";

    if (versionText.isNotEmpty())
        text << "Version:  " << versionText << "\n";

    if (statusText.getText() != text)
        statusText.setText (text, false);

    editorButton.setEnabled (s.pluginName.isNotEmpty());
    removeButton.setEnabled (s.pluginName.isNotEmpty());
}
