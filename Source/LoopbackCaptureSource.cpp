#include "LoopbackCaptureSource.h"

LoopbackCaptureSource::LoopbackCaptureSource() = default;

LoopbackCaptureSource::~LoopbackCaptureSource()
{
    stop();
}

juce::String LoopbackCaptureSource::getModeName() const
{
    return "Loopback (copy of a playback device)";
}

juce::String LoopbackCaptureSource::getModeHint() const
{
    return "Pick the device your main monitors play from (usually the Windows default). "
           "That device keeps playing untouched; this app gets a copy of its audio.";
}

std::vector<LoopbackCaptureSource::NamedEndpoint> LoopbackCaptureSource::refreshEndpoints()
{
    std::vector<NamedEndpoint> result;
    juce::StringArray seen;

    for (const auto& ep : wasapi::listRenderEndpoints())
    {
        juce::String name (ep.friendlyName.c_str());

        if (name.isEmpty())
            name = "Unnamed device";

        // Two devices can share a friendly name; keep the UI names unique.
        const juce::String base = name;
        for (int n = 2; seen.contains (name); ++n)
            name = base + " (" + juce::String (n) + ")";

        seen.add (name);
        result.push_back ({ name, ep.id, ep.isDefault });
    }

    endpoints = result;
    return result;
}

juce::StringArray LoopbackCaptureSource::getAvailableSources()
{
    juce::StringArray names;

    for (const auto& ep : refreshEndpoints())
        names.add (ep.name);

    return names;
}

juce::String LoopbackCaptureSource::getDefaultSource()
{
    for (const auto& ep : refreshEndpoints())
        if (ep.isDefault)
            return ep.name;

    return endpoints.empty() ? juce::String() : endpoints.front().name;
}

juce::String LoopbackCaptureSource::start (const juce::String& sourceName, int firstChannel)
{
    stop();

    std::wstring id;
    bool found = false;

    for (const auto& ep : refreshEndpoints())
    {
        if (ep.name == sourceName)
        {
            id = ep.id;
            found = true;
            break;
        }
    }

    if (! found)
        return "Playback device \"" + sourceName + "\" isn't available.";

    selectedFirstChannel = juce::jmax (0, firstChannel);

    const auto error = capture.start (id, [this] (const float* const* channels, int numChannels, int numFrames)
    {
        const float* pair[2] = { nullptr, nullptr };

        if (numChannels > 0)
        {
            const int first = juce::jmin (selectedFirstChannel, numChannels - 1);
            pair[0] = channels[first];
            pair[1] = channels[juce::jmin (first + 1, numChannels - 1)];
        }

        deliver (pair, 2, numFrames);
    });

    if (! error.empty())
        return juce::String (error.c_str());

    if (selectedFirstChannel + 1 >= getNumChannels() && getNumChannels() > 1)
    {
        stop();
        return "\"" + sourceName + "\" only has " + juce::String (getNumChannels()) + " channels.";
    }

    currentName = sourceName;
    return {};
}

void LoopbackCaptureSource::stop()
{
    capture.stop();
    currentName.clear();
}

bool   LoopbackCaptureSource::isRunning() const            { return capture.isRunning(); }
double LoopbackCaptureSource::getSampleRate() const        { return capture.getSampleRate(); }
int    LoopbackCaptureSource::getNumChannels() const       { return capture.getNumChannels(); }

int LoopbackCaptureSource::getTypicalBlockSize() const
{
    const int period = capture.getPeriodFrames();
    return period > 0 ? period : (int) (getSampleRate() * 0.01);
}

juce::String LoopbackCaptureSource::getLastError() const
{
    return juce::String (capture.getLastError().c_str());
}
