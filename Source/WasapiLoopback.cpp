// WASAPI loopback capture (Windows only).

#include "WasapiLoopback.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
 #pragma comment (lib, "ole32.lib")
 #pragma comment (lib, "avrt.lib")
 #pragma comment (lib, "winmm.lib")
#endif

namespace wasapi
{
namespace
{
    // Minimal COM smart pointer (avoids depending on WRL/ATL).
    template <typename T>
    struct Com
    {
        T* p = nullptr;

        Com() = default;
        ~Com()                        { reset(); }
        Com (const Com&) = delete;
        Com& operator= (const Com&) = delete;

        void reset()                  { if (p != nullptr) { p->Release(); p = nullptr; } }
        T** put()                     { reset(); return &p; }
        void** putVoid()              { return reinterpret_cast<void**> (put()); }
        T* operator->() const         { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    struct ComScope
    {
        explicit ComScope (DWORD mode) : hr (CoInitializeEx (nullptr, mode)) {}
        ~ComScope()                   { if (SUCCEEDED (hr)) CoUninitialize(); }
        HRESULT hr;
    };

    // PKEY_Device_FriendlyName, defined locally so we don't need functiondiscoverykeys_devpkey.h
    const PROPERTYKEY friendlyNameKey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    std::wstring hex (HRESULT hr)
    {
        static const wchar_t digits[] = L"0123456789ABCDEF";
        std::wstring s = L"0x";
        const auto v = static_cast<unsigned long> (hr);

        for (int shift = 28; shift >= 0; shift -= 4)
            s += digits[(v >> shift) & 0xf];

        return s;
    }

    std::wstring describe (const wchar_t* what, HRESULT hr)
    {
        std::wstring s (what);

        switch (hr)
        {
            case AUDCLNT_E_DEVICE_INVALIDATED:  s += L": the device was removed, disabled, or its format changed"; break;
            case AUDCLNT_E_DEVICE_IN_USE:       s += L": the device is in use in exclusive mode by another app"; break;
            case AUDCLNT_E_UNSUPPORTED_FORMAT:  s += L": unsupported format"; break;
            case E_ACCESSDENIED:                s += L": access denied"; break;
            default: break;
        }

        return s + L" (" + hex (hr) + L")";
    }

    std::wstring friendlyNameOf (IMMDevice* device)
    {
        std::wstring name;
        Com<IPropertyStore> store;

        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, store.put())))
        {
            PROPVARIANT value;
            PropVariantInit (&value);

            if (SUCCEEDED (store->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
                name = value.pwszVal;

            PropVariantClear (&value);
        }

        return name;
    }

    std::wstring idOf (IMMDevice* device)
    {
        std::wstring id;
        LPWSTR raw = nullptr;

        if (SUCCEEDED (device->GetId (&raw)) && raw != nullptr)
        {
            id = raw;
            CoTaskMemFree (raw);
        }

        return id;
    }

    struct SampleFormat
    {
        int  channels = 0;
        int  sampleRate = 0;
        int  containerBytes = 0;
        bool isFloat = false;
        bool supported = false;
    };

    SampleFormat parseFormat (const WAVEFORMATEX* wf)
    {
        SampleFormat f;
        f.channels = wf->nChannels;
        f.sampleRate = (int) wf->nSamplesPerSec;
        f.containerBytes = wf->nChannels > 0 ? wf->nBlockAlign / wf->nChannels : 0;

        WORD tag = wf->wFormatTag;

        if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
            tag = (WORD) reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (wf)->SubFormat.Data1;

        f.isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT);
        const bool isPcm = (tag == WAVE_FORMAT_PCM);

        f.supported = f.channels > 0 && f.sampleRate > 0
                      && ((f.isFloat && f.containerBytes == 4)
                          || (isPcm && (f.containerBytes == 2 || f.containerBytes == 3 || f.containerBytes == 4)));
        return f;
    }

    void convert (const BYTE* data, const SampleFormat& f, UINT32 frames, std::vector<std::vector<float>>& out)
    {
        const int nc = f.channels;

        if (f.isFloat)
        {
            const float* src = reinterpret_cast<const float*> (data);
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < nc; ++c)
                    out[(size_t) c][i] = src[(size_t) i * (size_t) nc + (size_t) c];
            return;
        }

        switch (f.containerBytes)
        {
            case 2:
            {
                const int16_t* src = reinterpret_cast<const int16_t*> (data);
                for (UINT32 i = 0; i < frames; ++i)
                    for (int c = 0; c < nc; ++c)
                        out[(size_t) c][i] = (float) src[(size_t) i * (size_t) nc + (size_t) c] * (1.0f / 32768.0f);
                break;
            }
            case 3:
            {
                for (UINT32 i = 0; i < frames; ++i)
                    for (int c = 0; c < nc; ++c)
                    {
                        const BYTE* b = data + ((size_t) i * (size_t) nc + (size_t) c) * 3;
                        const int32_t v = (int32_t) ((uint32_t) b[0] << 8 | (uint32_t) b[1] << 16 | (uint32_t) b[2] << 24);
                        out[(size_t) c][i] = (float) v * (1.0f / 2147483648.0f);
                    }
                break;
            }
            case 4:
            {
                const int32_t* src = reinterpret_cast<const int32_t*> (data);
                for (UINT32 i = 0; i < frames; ++i)
                    for (int c = 0; c < nc; ++c)
                        out[(size_t) c][i] = (float) src[(size_t) i * (size_t) nc + (size_t) c] * (1.0f / 2147483648.0f);
                break;
            }
            default:
                break;
        }
    }
} // namespace

//==============================================================================
std::vector<RenderEndpoint> listRenderEndpoints()
{
    std::vector<RenderEndpoint> result;
    ComScope com (COINIT_APARTMENTTHREADED);

    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), enumerator.putVoid())))
        return result;

    std::wstring defaultId;
    {
        Com<IMMDevice> def;
        if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, def.put())))
            defaultId = idOf (def.p);
    }

    Com<IMMDeviceCollection> collection;
    if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, collection.put())))
        return result;

    UINT count = 0;
    collection->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        Com<IMMDevice> device;
        if (FAILED (collection->Item (i, device.put())))
            continue;

        RenderEndpoint ep;
        ep.id = idOf (device.p);
        ep.friendlyName = friendlyNameOf (device.p);
        ep.isDefault = ! defaultId.empty() && ep.id == defaultId;

        if (! ep.id.empty())
            result.push_back (std::move (ep));
    }

    return result;
}

//==============================================================================
struct LoopbackCapture::Impl
{
    std::thread thread;
    std::atomic<bool> stopRequested { false }, running { false };
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> numChannels { 0 }, periodFrames { 0 };

    mutable std::mutex errorLock;
    std::wstring lastError;
    DataCallback callback;

    void setError (std::wstring e)
    {
        const std::lock_guard<std::mutex> lock (errorLock);
        lastError = std::move (e);
    }

    void run (std::wstring endpointId, std::promise<std::wstring> ready)
    {
        ComScope com (COINIT_MULTITHREADED);
        bool readySent = false;

        auto fail = [&] (const wchar_t* what, HRESULT hr)
        {
            auto message = describe (what, hr);

            if (! readySent) { ready.set_value (message); readySent = true; }
            else             { setError (message); }
        };

        Com<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof (IMMDeviceEnumerator), enumerator.putVoid());
        if (FAILED (hr)) { fail (L"Couldn't create the Windows audio device enumerator", hr); return; }

        Com<IMMDevice> device;
        hr = endpointId.empty() ? enumerator->GetDefaultAudioEndpoint (eRender, eConsole, device.put())
                                : enumerator->GetDevice (endpointId.c_str(), device.put());
        if (FAILED (hr)) { fail (L"Couldn't find that playback device", hr); return; }

        Com<IAudioClient> client;
        hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
        if (FAILED (hr)) { fail (L"Couldn't open the playback device", hr); return; }

        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat (&mix);
        if (FAILED (hr) || mix == nullptr) { fail (L"Couldn't read the device's mix format", hr); return; }

        const SampleFormat format = parseFormat (mix);

        if (! format.supported)
        {
            CoTaskMemFree (mix);
            fail (L"The device's mix format isn't supported", AUDCLNT_E_UNSUPPORTED_FORMAT);
            return;
        }

        const REFERENCE_TIME bufferDuration = 2000000; // 200 ms (100-ns units); we drain it every ~1 ms
        hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, mix, nullptr);
        CoTaskMemFree (mix);
        if (FAILED (hr)) { fail (L"Couldn't start loopback capture", hr); return; }

        REFERENCE_TIME defaultPeriod = 100000, minPeriod = 0;
        client->GetDevicePeriod (&defaultPeriod, &minPeriod);

        Com<IAudioCaptureClient> capture;
        hr = client->GetService (__uuidof (IAudioCaptureClient), capture.putVoid());
        if (FAILED (hr)) { fail (L"Couldn't get the capture service", hr); return; }

        sampleRate.store ((double) format.sampleRate);
        numChannels.store (format.channels);
        periodFrames.store ((int) ((double) defaultPeriod * 1.0e-7 * format.sampleRate + 0.5));

        hr = client->Start();
        if (FAILED (hr)) { fail (L"Couldn't start the capture stream", hr); return; }

        running.store (true);
        ready.set_value (std::wstring());
        readySent = true;

        // 1 ms timer resolution so Sleep(1) really is ~1 ms, and "Pro Audio" scheduling.
        timeBeginPeriod (1);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

        std::vector<std::vector<float>> channels ((size_t) format.channels, std::vector<float> (4096));
        std::vector<const float*> pointers ((size_t) format.channels, nullptr);
        bool ok = true;

        while (ok && ! stopRequested.load())
        {
            Sleep (1);

            for (;;)
            {
                UINT32 packetFrames = 0;
                hr = capture->GetNextPacketSize (&packetFrames);
                if (FAILED (hr)) { fail (L"Loopback capture stopped", hr); ok = false; break; }
                if (packetFrames == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
                if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
                if (FAILED (hr)) { fail (L"Loopback capture stopped", hr); ok = false; break; }

                if (frames > channels[0].size())
                    for (auto& c : channels)
                        c.resize (frames);

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    for (auto& c : channels)
                        std::fill (c.begin(), c.begin() + frames, 0.0f);
                }
                else
                {
                    convert (data, format, frames, channels);
                }

                for (size_t c = 0; c < channels.size(); ++c)
                    pointers[c] = channels[c].data();

                if (callback)
                    callback (pointers.data(), format.channels, (int) frames);

                capture->ReleaseBuffer (frames);
            }
        }

        client->Stop();

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);

        timeEndPeriod (1);
        running.store (false);
    }
};

//==============================================================================
LoopbackCapture::LoopbackCapture() : impl (std::make_unique<Impl>()) {}

LoopbackCapture::~LoopbackCapture()
{
    stop();
}

std::wstring LoopbackCapture::start (const std::wstring& endpointId, DataCallback callback)
{
    stop();

    impl->stopRequested.store (false);
    impl->setError ({});
    impl->callback = std::move (callback);

    std::promise<std::wstring> ready;
    auto result = ready.get_future();
    impl->thread = std::thread (&Impl::run, impl.get(), endpointId, std::move (ready));

    auto error = result.get();

    if (! error.empty())
    {
        impl->thread.join();
        return error;
    }

    return {};
}

void LoopbackCapture::stop()
{
    impl->stopRequested.store (true);

    if (impl->thread.joinable())
        impl->thread.join();

    impl->running.store (false);
}

bool   LoopbackCapture::isRunning() const noexcept       { return impl->running.load(); }
double LoopbackCapture::getSampleRate() const noexcept   { return impl->sampleRate.load(); }
int    LoopbackCapture::getNumChannels() const noexcept  { return impl->numChannels.load(); }
int    LoopbackCapture::getPeriodFrames() const noexcept { return impl->periodFrames.load(); }

std::wstring LoopbackCapture::getLastError() const
{
    const std::lock_guard<std::mutex> lock (impl->errorLock);
    return impl->lastError;
}

} // namespace wasapi
