#pragma once

#include "CaptureSource.h"
#include "WasapiLoopback.h"

#include <mutex>

//==============================================================================
/** Windows: captures a copy of what a playback device is playing (WASAPI loopback). */
class LoopbackCaptureSource final : public CaptureSource
{
public:
    LoopbackCaptureSource();
    ~LoopbackCaptureSource() override;

    juce::String getModeName() const override;
    juce::String getModeHint() const override;
    juce::StringArray getAvailableSources() override;
    juce::String getDefaultSource() override;

    juce::String start (const juce::String& sourceName, int firstChannel) override;
    void stop() override;
    bool isRunning() const override;

    juce::String getCurrentSourceName() const override  { return currentName; }
    double getSampleRate() const override;
    int    getNumChannels() const override;
    int    getTypicalBlockSize() const override;
    juce::String getLastError() const override;
    bool   isLoopback() const override                  { return true; }

private:
    struct NamedEndpoint
    {
        juce::String name;
        std::wstring id;
        bool isDefault = false;
    };

    std::vector<NamedEndpoint> refreshEndpoints();

    wasapi::LoopbackCapture capture;
    std::vector<NamedEndpoint> endpoints;
    juce::String currentName;
    int selectedFirstChannel = 0;

    JUCE_DECLARE_NON_COPYABLE (LoopbackCaptureSource)
};
