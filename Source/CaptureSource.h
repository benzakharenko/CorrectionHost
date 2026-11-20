#pragma once

#include <juce_core/juce_core.h>
#include "DriftFifo.h"

#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
/**
    Something that delivers the audio we want to correct: a WASAPI loopback copy
    of a playback device (Windows), or an input device such as BlackHole or
    VB-Cable.

    Sources are started first (so we know their sample rate and block size),
    then connected to a DriftFifo with setTarget(). Until then, delivered audio
    is simply dropped.
*/
class CaptureSource
{
public:
    virtual ~CaptureSource() = default;

    /** Shown in the UI, e.g. "Loopback (copy of a playback device)". */
    virtual juce::String getModeName() const = 0;

    /** Short explanation shown under the source selector. */
    virtual juce::String getModeHint() const = 0;

    /** Names of the devices this mode can capture from. */
    virtual juce::StringArray getAvailableSources() = 0;

    /** Best guess for a first run (the default playback device, BlackHole, VB-Cable...). */
    virtual juce::String getDefaultSource() = 0;

    /** Opens the named source. firstChannel selects which channel pair to use (0 = 1-2).
        Returns an empty string on success, otherwise an error message.
    */
    virtual juce::String start (const juce::String& sourceName, int firstChannel) = 0;

    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    virtual juce::String getCurrentSourceName() const = 0;
    virtual double getSampleRate() const = 0;
    virtual int    getNumChannels() const = 0;       // total channels the source offers
    virtual int    getTypicalBlockSize() const = 0;

    /** Set when the source stopped by itself (device unplugged, format changed...). */
    virtual juce::String getLastError() const = 0;

    /** True if this mode taps the same device that we must not output to on the same channels. */
    virtual bool isLoopback() const { return false; }

    //==============================================================================
    void setTarget (DriftFifo* fifo) noexcept   { target.store (fifo, std::memory_order_release); }

protected:
    /** Call from the capture thread. channels may contain nullptrs (silence). */
    void deliver (const float* const* channels, int numChannels, int numFrames) noexcept
    {
        if (auto* f = target.load (std::memory_order_acquire))
            f->push (channels, numChannels, numFrames, DriftFifo::now());
    }

private:
    std::atomic<DriftFifo*> target { nullptr };
};

/** All capture modes available on this platform, in UI order. */
std::vector<std::unique_ptr<CaptureSource>> createPlatformCaptureSources();
