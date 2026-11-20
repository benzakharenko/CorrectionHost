// Fake WASAPI backend for testing LoopbackCaptureSource on non-Windows machines.
// Pretends there are two playback endpoints; capture produces a 4-channel, 48 kHz stream
// in 480-frame packets where channel c is a sine of amplitude 0.1 * (c + 1).

#include "WasapiLoopback.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <thread>

namespace wasapi
{

std::vector<RenderEndpoint> listRenderEndpoints()
{
    return {
        { L"{id-main}",   L"Speakers (Test Interface)",     true  },
        { L"{id-second}", L"Speakers 3-4 (Test Interface)", false },
    };
}

struct LoopbackCapture::Impl
{
    std::thread thread;
    std::atomic<bool> stopRequested { false }, running { false };
    DataCallback callback;
    std::mutex errorLock;
    std::wstring lastError;
};

LoopbackCapture::LoopbackCapture() : impl (std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { stop(); }

std::wstring LoopbackCapture::start (const std::wstring& endpointId, DataCallback callback)
{
    stop();

    if (endpointId != L"{id-main}" && endpointId != L"{id-second}" && ! endpointId.empty())
        return L"Couldn't find that playback device (0x88890004)";

    impl->callback = std::move (callback);
    impl->stopRequested = false;
    impl->running = true;

    impl->thread = std::thread ([this]
    {
        constexpr int channels = 4, frames = 480;
        std::vector<std::vector<float>> data (channels, std::vector<float> (frames));
        std::vector<const float*> ptrs (channels);
        long long position = 0;
        auto next = std::chrono::steady_clock::now();

        while (! impl->stopRequested)
        {
            next += std::chrono::milliseconds (10);
            std::this_thread::sleep_until (next);

            for (int c = 0; c < channels; ++c)
            {
                for (int i = 0; i < frames; ++i)
                    data[(size_t) c][(size_t) i] = 0.1f * (float) (c + 1)
                                                   * (float) std::sin (6.283185307179586 * 1000.0 * (double) (position + i) / 48000.0);
                ptrs[(size_t) c] = data[(size_t) c].data();
            }

            position += frames;

            if (impl->callback)
                impl->callback (ptrs.data(), channels, frames);
        }

        impl->running = false;
    });

    return {};
}

void LoopbackCapture::stop()
{
    impl->stopRequested = true;
    if (impl->thread.joinable())
        impl->thread.join();
    impl->running = false;
}

bool   LoopbackCapture::isRunning() const noexcept       { return impl->running; }
double LoopbackCapture::getSampleRate() const noexcept   { return 48000.0; }
int    LoopbackCapture::getNumChannels() const noexcept  { return 4; }
int    LoopbackCapture::getPeriodFrames() const noexcept { return 480; }

std::wstring LoopbackCapture::getLastError() const
{
    const std::lock_guard<std::mutex> lock (impl->errorLock);
    return impl->lastError;
}

} // namespace wasapi
