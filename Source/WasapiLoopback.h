#pragma once

// Plain C++ interface to WASAPI loopback capture (no JUCE or Windows headers here, so
// the JUCE-side adapter can be compiled and tested on any platform with a stub).

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wasapi
{

struct RenderEndpoint
{
    std::wstring id;            // stable endpoint ID
    std::wstring friendlyName;  // e.g. "Speakers (Focusrite USB Audio)"
    bool isDefault = false;     // the Windows default playback device
};

/** Active playback endpoints. */
std::vector<RenderEndpoint> listRenderEndpoints();

//==============================================================================
/**
    Captures a copy of everything Windows is mixing to one playback endpoint,
    without affecting what that endpoint plays.

    Notes:
      - Uses the endpoint's shared-mode mix format (usually 32-bit float, 48 kHz).
      - When nothing is playing to the endpoint, Windows delivers no packets at all.
        That's expected; the FIFO treats it as the source going quiet.
*/
class LoopbackCapture
{
public:
    /** Called on the capture thread with de-interleaved float data. */
    using DataCallback = std::function<void (const float* const* channels, int numChannels, int numFrames)>;

    LoopbackCapture();
    ~LoopbackCapture();

    /** Opens the endpoint (empty id = current default) and starts capturing.
        Returns an empty string on success, otherwise an error message.
    */
    std::wstring start (const std::wstring& endpointId, DataCallback callback);
    void stop();

    bool   isRunning() const noexcept;
    double getSampleRate() const noexcept;
    int    getNumChannels() const noexcept;
    int    getPeriodFrames() const noexcept;

    /** Why capture stopped by itself (e.g. the device was unplugged), if it did. */
    std::wstring getLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    LoopbackCapture (const LoopbackCapture&) = delete;
    LoopbackCapture& operator= (const LoopbackCapture&) = delete;
};

} // namespace wasapi
