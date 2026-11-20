#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "CaptureSource.h"

//==============================================================================
/**
    Captures from an audio *input* device using JUCE: BlackHole on macOS, or
    VB-Cable ("CABLE Output") on Windows if you'd rather use a virtual cable
    than loopback.
*/
class JuceDeviceCaptureSource final : public CaptureSource,
                                      private juce::AudioIODeviceCallback
{
public:
    JuceDeviceCaptureSource();
    ~JuceDeviceCaptureSource() override;

    juce::String getModeName() const override;
    juce::String getModeHint() const override;
    juce::StringArray getAvailableSources() override;
    juce::String getDefaultSource() override;

    juce::String start (const juce::String& sourceName, int firstChannel) override;
    void stop() override;
    bool isRunning() const override;

    juce::String getCurrentSourceName() const override;
    double getSampleRate() const override;
    int    getNumChannels() const override;
    int    getTypicalBlockSize() const override;
    juce::String getLastError() const override;

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& message) override;

    juce::AudioIODeviceType* getType();

    juce::AudioDeviceManager manager;
    bool initialised = false;
    bool callbackAdded = false;
    int totalChannels = 0;

    juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE (JuceDeviceCaptureSource)
};
