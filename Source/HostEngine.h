#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "CaptureSource.h"
#include "DriftFifo.h"

//==============================================================================
/**
    Capture source -> DriftFifo -> hosted plugin (e.g. ARC X) -> output channel pair.

    The output side is a normal JUCE AudioDeviceManager (WASAPI shared mode on
    Windows, CoreAudio on macOS) opened on just the two channels that feed the
    second pair of speakers. A watchdog restarts everything if either device
    disappears or changes format.

    All public methods are for the message thread.
*/
class HostEngine final : private juce::AudioIODeviceCallback,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    struct Config
    {
        int          captureMode        = 0;
        juce::String sourceName;
        int          sourceFirstChannel = 0;    // 0 = channels 1-2
        juce::String outputTypeName;
        juce::String outputDeviceName;
        int          outputFirstChannel = 2;    // 2 = channels 3-4
        int          bufferSize         = 0;    // 0 = device default
        double       safetyMs           = 5.0;  // extra FIFO margin

        // Optional second source: an input pair on your interface (e.g. ADAT 9/10),
        // mixed with the main source before the plugin.
        bool         inputEnabled       = false;
        juce::String inputDeviceName;
        int          inputFirstChannel  = 0;
    };

    struct InputStatus
    {
        bool   enabled = false;
        bool   running = false;
        bool   direct  = false;   // taken from the output device itself (ASIO): same clock, no FIFO
        juce::String name, error;
        double rate = 0.0;
        int    channels = 0, block = 0;
        DriftFifo::Stats fifo;
        double latencyMs = 0.0;
    };

    struct Status
    {
        bool   running = false;
        bool   wantRunning = false;
        juce::String error;
        juce::String sourceName, outputName;
        double sourceRate = 0.0, outputRate = 0.0;
        int    sourceChannels = 0, outputChannels = 0;
        int    sourceBlock = 0, outputBlock = 0;
        DriftFifo::Stats fifo;
        juce::String pluginName;
        int    pluginLatencySamples = 0;
        bool   bypassed = false;
        double addedLatencyMs = 0.0;
        int    automaticRestarts = 0;
        InputStatus input;
    };

    HostEngine();
    ~HostEngine() override;

    //==============================================================================
    int getNumCaptureModes() const                     { return (int) sources.size(); }
    CaptureSource& getCaptureMode (int index)          { return *sources[(size_t) juce::jlimit (0, getNumCaptureModes() - 1, index)]; }

    /** The interface-input source (for listing input devices). */
    CaptureSource& getInputSource()                    { return *inputSource; }

    /** Changes the optional interface input while audio keeps running.
        Returns an error message (the main path is unaffected), or empty.
    */
    juce::String setInputConfig (bool enabled, const juce::String& deviceName, int firstChannel);

    /** Level of the interface input in the mix, in dB (applied smoothly, live). */
    void  setInputLevelDb (float dB) noexcept          { inputGain.store (dB <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (dB)); }

    int getOpenInputChannelCount() const;

    /** True when the output driver can also supply the interface input directly (ASIO):
        the input pair then comes from the same device, with no second stream or drift handling.
    */
    static bool usesDirectInput (const juce::String& outputTypeName)   { return outputTypeName == "ASIO"; }

    /** Channel names of the open output device (e.g. "Analogue 3", "ADAT 1"), for labelling. */
    juce::StringArray getOpenOutputChannelNames() const;
    juce::StringArray getOpenInputChannelNames() const;

    juce::StringArray getOutputTypeNames();
    juce::String      getDefaultOutputTypeName();
    juce::StringArray getOutputDeviceNames (const juce::String& typeName);
    juce::String      getDefaultOutputDeviceName (const juce::String& typeName);

    /** Starts (or restarts) audio with this configuration. Returns an error message, or empty. */
    juce::String start (const Config&);

    /** Stops audio and the automatic retries. */
    void stop();

    bool isRunning() const noexcept                    { return running; }
    const Config& getConfig() const noexcept           { return config; }
    int  getOpenOutputChannelCount() const;
    int  getOpenSourceChannelCount() const;

    //==============================================================================
    juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }

    /** Every plugin file/identifier we can find in the standard locations (cheap, nothing is loaded). */
    juce::StringArray findPluginIdentifiers();
    juce::String getPluginDisplayName (const juce::String& fileOrIdentifier);

    using LoadCallback = std::function<void (const juce::String& error)>;

    /** Loads a plugin asynchronously, optionally restoring saved state, then swaps it in. */
    void loadPlugin (const juce::String& fileOrIdentifier, const juce::MemoryBlock& state, LoadCallback);
    void loadPlugin (const juce::PluginDescription&, const juce::MemoryBlock& state, LoadCallback);
    void unloadPlugin();

    juce::AudioPluginInstance* getPlugin() const noexcept         { return plugin.get(); }
    const juce::PluginDescription& getPluginDescription() const   { return pluginDescription; }

    void setBypassed (bool shouldBypass) noexcept      { bypassed.store (shouldBypass); }
    bool isBypassed() const noexcept                   { return bypassed.load(); }

    Status getStatus() const;

    /** Seconds since the output last carried sound (used to install updates only when quiet). */
    double getSecondsSinceAudible() const noexcept;

    /** Called (message thread) just before the hosted plugin is replaced or removed,
        so the UI can close its editor window first.
    */
    std::function<void()> onPluginAboutToChange;

    /** Called (message thread) whenever running state, errors or the plugin change. */
    std::function<void()> onStateChanged;

private:
    friend struct HostEngineTestAccess;   // headless tests drive the audio callbacks directly

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

    void handleAsyncUpdate() override;   // restart after a device format change
    void timerCallback() override;       // watchdog

    void stopAudio();
    juce::String startInput();
    void stopInput();
    void checkInput();
    juce::String openOutput (const Config&);
    void closeOutput();
    juce::AudioIODeviceType* findOutputType (const juce::String& name);
    void configurePluginLayout (juce::AudioPluginInstance&) const;
    void setPlugin (std::unique_ptr<juce::AudioPluginInstance>);
    void notify();

    //==============================================================================
    std::vector<std::unique_ptr<CaptureSource>> sources;
    CaptureSource* activeSource = nullptr;

    juce::AudioDeviceManager outputManager;              // only used to enumerate driver types; never opened
    std::unique_ptr<juce::AudioIODevice> outputDevice;   // declared after outputManager: destroyed first

    DriftFifo fifo;
    Config config;
    bool running = false;       // audio actually flowing
    bool wantRunning = false;   // user asked for audio; the watchdog keeps trying
    juce::String lastError;
    int automaticRestarts = 0;
    int ticksUntilRetry = 0;
    double fifoOutputRate = 0.0;

    // Plugin + realtime state. Everything below is guarded by pluginLock where the audio
    // thread can see it; the audio thread only ever try-locks.
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription pluginDescription;
    juce::SpinLock pluginLock;
    bool   pluginPrepared = false;
    double currentRate = 0.0;
    int    currentBlock = 0;
    int    pluginBlock  = 512;

    juce::AudioBuffer<float> dryBuffer, wetBuffer, processBuffer;
    juce::MidiBuffer midi;
    juce::LinearSmoothedValue<float> wetGain;
    std::atomic<bool> bypassed { false };
    std::atomic<double> lastAudibleTime { DriftFifo::now() };

    // Optional interface input, mixed in before the plugin. inputActive is guarded by
    // inputLock; the audio thread only try-locks it.
    std::unique_ptr<CaptureSource> inputSource;
    DriftFifo inputFifo;
    juce::SpinLock inputLock;
    bool inputActive = false;
    juce::String inputError;
    int inputRetryTicks = 0;
    std::atomic<float> inputGain { 1.0f };
    std::atomic<bool>  directInputActive { false };   // ASIO: input arrives in the output callback
    juce::LinearSmoothedValue<float> inputGainSmoothed;
    juce::AudioBuffer<float> inputBuffer;

    std::shared_ptr<bool> aliveToken = std::make_shared<bool> (true);

    static constexpr int chunkFrames = 4096;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostEngine)
};
