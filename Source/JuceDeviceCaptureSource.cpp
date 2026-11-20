#include "JuceDeviceCaptureSource.h"

namespace
{
    const char* preferredTypeName()
    {
       #if JUCE_WINDOWS
        return "Windows Audio";   // WASAPI shared mode
       #elif JUCE_MAC
        return "CoreAudio";
       #else
        return "ALSA";
       #endif
    }
}

JuceDeviceCaptureSource::JuceDeviceCaptureSource() = default;

JuceDeviceCaptureSource::~JuceDeviceCaptureSource()
{
    stop();
}

juce::String JuceDeviceCaptureSource::getModeName() const
{
   #if JUCE_WINDOWS
    return "Input device (VB-Cable)";
   #else
    return "Input device (BlackHole)";
   #endif
}

juce::String JuceDeviceCaptureSource::getModeHint() const
{
   #if JUCE_WINDOWS
    return "Pick \"CABLE Output\". Windows (or an app) must be playing to \"CABLE Input\", "
           "and your main monitors then need their own copy (e.g. Listen to this device).";
   #elif JUCE_MAC
    return "Pick \"BlackHole 2ch\". Set macOS output to a Multi-Output Device containing your "
           "interface and BlackHole, so your main monitors keep playing untouched.";
   #else
    return "Pick the input device that carries the audio you want to correct.";
   #endif
}

juce::AudioIODeviceType* JuceDeviceCaptureSource::getType()
{
    const auto& types = manager.getAvailableDeviceTypes();

    for (auto* t : types)
        if (t->getTypeName() == preferredTypeName())
            return t;

    return types.getFirst();
}

juce::StringArray JuceDeviceCaptureSource::getAvailableSources()
{
    if (auto* type = getType())
    {
        type->scanForDevices();
        return type->getDeviceNames (true);
    }

    return {};
}

juce::String JuceDeviceCaptureSource::getDefaultSource()
{
    const auto names = getAvailableSources();

    for (const auto* hint : { "BlackHole", "CABLE Output", "Hi-Fi Cable Output" })
        for (const auto& n : names)
            if (n.containsIgnoreCase (hint))
                return n;

    if (auto* type = getType())
    {
        const int index = type->getDefaultDeviceIndex (true);
        if (juce::isPositiveAndBelow (index, names.size()))
            return names[index];
    }

    return names.isEmpty() ? juce::String() : names[0];
}

juce::String JuceDeviceCaptureSource::start (const juce::String& sourceName, int firstChannel)
{
    stop();

    {
        const juce::ScopedLock sl (errorLock);
        lastError.clear();
    }

    auto* type = getType();
    if (type == nullptr)
        return "No audio device types are available.";

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = sourceName;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (juce::jmax (0, firstChannel), 2, true);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    setup.sampleRate = 0;   // whatever the device is set to
    setup.bufferSize = 0;

    juce::String error;

    if (! initialised)
    {
        error = manager.initialise (2, 0, nullptr, false, {}, &setup);
        initialised = true;

        if (error.isEmpty() && manager.getCurrentAudioDeviceType() != type->getTypeName())
        {
            manager.setCurrentAudioDeviceType (type->getTypeName(), true);
            error = manager.setAudioDeviceSetup (setup, true);
        }
    }
    else
    {
        if (manager.getCurrentAudioDeviceType() != type->getTypeName())
            manager.setCurrentAudioDeviceType (type->getTypeName(), true);

        error = manager.setAudioDeviceSetup (setup, true);
    }

    if (error.isNotEmpty())
        return error;

    auto* device = manager.getCurrentAudioDevice();

    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
    {
        manager.closeAudioDevice();
        return "Couldn't open the requested input channels on \"" + sourceName + "\".";
    }

    totalChannels = device->getInputChannelNames().size();
    manager.addAudioCallback (this);
    callbackAdded = true;
    return {};
}

void JuceDeviceCaptureSource::stop()
{
    if (callbackAdded)
    {
        manager.removeAudioCallback (this);
        callbackAdded = false;
    }

    if (initialised)
        manager.closeAudioDevice();
}

bool JuceDeviceCaptureSource::isRunning() const
{
    auto* device = manager.getCurrentAudioDevice();
    return callbackAdded && device != nullptr && device->isPlaying();
}

juce::String JuceDeviceCaptureSource::getCurrentSourceName() const
{
    auto* device = manager.getCurrentAudioDevice();
    return device != nullptr ? device->getName() : juce::String();
}

double JuceDeviceCaptureSource::getSampleRate() const
{
    auto* device = manager.getCurrentAudioDevice();
    return device != nullptr ? device->getCurrentSampleRate() : 0.0;
}

int JuceDeviceCaptureSource::getNumChannels() const
{
    return totalChannels;
}

int JuceDeviceCaptureSource::getTypicalBlockSize() const
{
    auto* device = manager.getCurrentAudioDevice();
    return device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
}

juce::String JuceDeviceCaptureSource::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

void JuceDeviceCaptureSource::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                                float* const* outputChannelData, int numOutputChannels,
                                                                int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    deliver (inputChannelData, numInputChannels, numSamples);
}

void JuceDeviceCaptureSource::audioDeviceError (const juce::String& message)
{
    const juce::ScopedLock sl (errorLock);
    lastError = message;
}
