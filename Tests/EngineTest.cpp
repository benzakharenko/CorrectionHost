// Drives HostEngine's real audio callback with a fake output device and a real VST3
// (TestGain), checking plugin loading, processing, bypass crossfades, state
// save/restore and unloading - everything except the OS audio drivers themselves.
//
// Usage: EngineTest <path to TestGain.vst3>

#include "HostEngine.h"

#include <cstdio>

struct HostEngineTestAccess
{
    static void aboutToStart (HostEngine& e, juce::AudioIODevice& d)
    {
        e.fifo.prepare (48000.0, 48000.0, 480, 480, 5.0);
        e.fifoOutputRate = 48000.0;
        e.audioDeviceAboutToStart (&d);
        e.running = true;   // so plugin swaps prepare immediately, as when audio is live
    }

    static void stopped (HostEngine& e)
    {
        e.audioDeviceStopped();
        e.running = false;
    }

    static void push (HostEngine& e, const float* l, const float* r, int n)
    {
        const float* chans[] = { l, r };
        e.fifo.push (chans, 2, n, DriftFifo::now());
    }

    static void enableInput (HostEngine& e, bool on)
    {
        if (on)
            e.inputFifo.prepare (48000.0, 48000.0, 480, 480, 5.0);

        const juce::SpinLock::ScopedLockType lock (e.inputLock);
        e.inputActive = on;
    }

    static void pushInput (HostEngine& e, const float* l, const float* r, int n)
    {
        const float* chans[] = { l, r };
        e.inputFifo.push (chans, 2, n, DriftFifo::now());
    }

    static void callback (HostEngine& e, float* l, float* r, int n)
    {
        float* outs[] = { l, r };
        e.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, n, {});
    }

    // ASIO-style: the interface input arrives in the same callback as the output.
    static void setDirectInput (HostEngine& e, bool on)  { e.directInputActive.store (on); }

    static void callbackWithInput (HostEngine& e, const float* inL, const float* inR, float* l, float* r, int n)
    {
        const float* ins[] = { inL, inR };
        float* outs[] = { l, r };
        e.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, n, {});
    }
};

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::printf ("  %-62s %s\n", what.toRawUTF8(), condition ? "ok" : "FAILED");
        if (! condition) ++failures;
    }

    class FakeDevice final : public juce::AudioIODevice
    {
    public:
        FakeDevice() : AudioIODevice ("Fake Interface", "Fake") {}

        juce::StringArray getOutputChannelNames() override   { return { "Out 1", "Out 2", "Out 3", "Out 4" }; }
        juce::StringArray getInputChannelNames() override    { return {}; }
        juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override  { return { 480 }; }
        int getDefaultBufferSize() override                  { return 480; }
        juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
        void close() override                                {}
        bool isOpen() override                               { return true; }
        void start (juce::AudioIODeviceCallback*) override   {}
        void stop() override                                 {}
        bool isPlaying() override                            { return true; }
        juce::String getLastError() override                 { return {}; }
        int getCurrentBufferSizeSamples() override           { return 480; }
        double getCurrentSampleRate() override               { return 48000.0; }
        int getCurrentBitDepth() override                    { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (2, 2, true); return b; }
        juce::BigInteger getActiveInputChannels() const override  { return {}; }
        int getOutputLatencyInSamples() override             { return 0; }
        int getInputLatencyInSamples() override              { return 0; }
    };

    // Feed a 0.5-amplitude 1 kHz sine for 'blocks' 10 ms blocks; return the output peak
    // over the last 10 blocks (well after any 20 ms crossfade).
    // mainAmp: level of the main (loopback) sine; inputAmp < 0 means don't feed the interface input.
    float runBlocks (HostEngine& engine, int blocks, long long& phase, float mainAmp = 0.5f, float inputAmp = -1.0f)
    {
        std::vector<float> inL (480), inR (480), outL (480), outR (480), xL (480), xR (480);
        float peak = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 480; ++i, ++phase)
            {
                const float s = (float) std::sin (6.283185307179586 * 1000.0 * (double) phase / 48000.0);
                inL[(size_t) i] = inR[(size_t) i] = mainAmp * s;
                xL[(size_t) i]  = xR[(size_t) i]  = juce::jmax (0.0f, inputAmp) * s;
            }

            HostEngineTestAccess::push (engine, inL.data(), inR.data(), 480);

            if (inputAmp >= 0.0f)
                HostEngineTestAccess::pushInput (engine, xL.data(), xR.data(), 480);

            HostEngineTestAccess::callback (engine, outL.data(), outR.data(), 480);

            if (b >= blocks - 10)
                for (int i = 0; i < 480; ++i)
                    peak = std::max ({ peak, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
        }

        return peak;
    }

    // Run the message loop until 'done' becomes true (async plugin creation).
    bool waitFor (const bool& done, int timeoutMs = 10000)
    {
        const auto start = juce::Time::getMillisecondCounter();

        while (! done && juce::Time::getMillisecondCounter() - start < (juce::uint32) timeoutMs)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        return done;
    }
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: EngineTest <TestGain.vst3>\n");
        return 2;
    }

    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String pluginPath (argv[1]);

    HostEngine engine;
    FakeDevice device;
    HostEngineTestAccess::aboutToStart (engine, device);
    long long phase = 0;

    std::printf ("No plugin\n");
    float peak = runBlocks (engine, 200, phase);
    check (std::abs (peak - 0.5f) < 0.005f, "clean signal passes through (peak " + juce::String (peak, 4) + ")");

    std::printf ("Loading the VST3\n");
    check (engine.getPluginDisplayName (pluginPath).containsIgnoreCase ("TestGain"), "display name from the plugin file");

    bool loaded = false;
    juce::String loadError;
    engine.loadPlugin (pluginPath, {}, [&] (const juce::String& e) { loadError = e; loaded = true; });
    check (waitFor (loaded), "async load completes");
    check (loadError.isEmpty(), "loads without error " + loadError);
    check (engine.getPlugin() != nullptr && engine.getPlugin()->getName() == "TestGain", "plugin is hosted");
    check (engine.getPluginDescription().fileOrIdentifier.isNotEmpty(), "description kept for saving");

    peak = runBlocks (engine, 100, phase);
    check (std::abs (peak - 0.25f) < 0.005f, "plugin processes the audio: 0.5 x gain 0.5 (peak " + juce::String (peak, 4) + ")");

    std::printf ("Bypass\n");
    engine.setBypassed (true);
    peak = runBlocks (engine, 50, phase);
    check (std::abs (peak - 0.5f) < 0.005f, "bypass crossfades to the clean signal (peak " + juce::String (peak, 4) + ")");
    engine.setBypassed (false);
    peak = runBlocks (engine, 50, phase);
    check (std::abs (peak - 0.25f) < 0.005f, "un-bypass returns to processed (peak " + juce::String (peak, 4) + ")");

    std::printf ("State save / restore (like an ARC X profile)\n");
    auto* gainParam = engine.getPlugin()->getParameters()[0];
    gainParam->setValueNotifyingHost (0.8f);
    peak = runBlocks (engine, 50, phase);
    check (std::abs (peak - 0.4f) < 0.005f, "parameter change applies (peak " + juce::String (peak, 4) + ")");

    juce::MemoryBlock saved;
    engine.getPlugin()->getStateInformation (saved);
    const auto encoded = saved.toBase64Encoding();
    auto descriptionXml = engine.getPluginDescription().createXml();

    engine.unloadPlugin();
    peak = runBlocks (engine, 50, phase);
    check (engine.getPlugin() == nullptr, "unloaded");
    check (std::abs (peak - 0.5f) < 0.005f, "clean signal after unloading (peak " + juce::String (peak, 4) + ")");

    juce::PluginDescription restoredDescription;
    check (descriptionXml != nullptr && restoredDescription.loadFromXml (*descriptionXml), "description round-trips through XML");

    juce::MemoryBlock restoredState;
    restoredState.fromBase64Encoding (encoded);

    loaded = false;
    engine.loadPlugin (restoredDescription, restoredState, [&] (const juce::String& e) { loadError = e; loaded = true; });
    check (waitFor (loaded) && loadError.isEmpty(), "reloads from saved description");
    peak = runBlocks (engine, 100, phase);
    check (std::abs (peak - 0.4f) < 0.005f, "saved state restored (peak " + juce::String (peak, 4) + ")");

    std::printf ("Device restart\n");
    HostEngineTestAccess::stopped (engine);
    HostEngineTestAccess::aboutToStart (engine, device);
    peak = runBlocks (engine, 100, phase);
    check (std::abs (peak - 0.4f) < 0.005f, "plugin re-prepared after a device restart (peak " + juce::String (peak, 4) + ")");

    std::printf ("Interface input mixed in (plugin gain is 0.8)\n");
    HostEngineTestAccess::enableInput (engine, true);
    engine.setInputLevelDb (0.0f);
    peak = runBlocks (engine, 150, phase, 0.0f, 0.5f);
    check (std::abs (peak - 0.4f) < 0.005f, "input alone goes through the plugin (peak " + juce::String (peak, 4) + ")");

    engine.setInputLevelDb (-6.0206f);
    peak = runBlocks (engine, 100, phase, 0.0f, 0.5f);
    check (std::abs (peak - 0.2f) < 0.005f, "input level -6 dB halves it (peak " + juce::String (peak, 4) + ")");

    engine.setInputLevelDb (0.0f);
    peak = runBlocks (engine, 100, phase, 0.25f, 0.25f);
    check (std::abs (peak - 0.4f) < 0.01f, "main + input are summed before the plugin (peak " + juce::String (peak, 4) + ")");

    HostEngineTestAccess::enableInput (engine, false);
    peak = runBlocks (engine, 100, phase, 0.25f, 0.25f);
    check (std::abs (peak - 0.2f) < 0.005f, "turning the input off leaves just the main source (peak " + juce::String (peak, 4) + ")");

    std::printf ("Direct (ASIO) interface input, same callback as the output\n");
    {
        HostEngineTestAccess::setDirectInput (engine, true);
        engine.setInputLevelDb (0.0f);
        std::vector<float> silence (480, 0.0f), inL (480), outL (480), outR (480);
        float directPeak = 0.0f;

        for (int b = 0; b < 100; ++b)
        {
            for (int i = 0; i < 480; ++i, ++phase)
                inL[(size_t) i] = 0.5f * (float) std::sin (6.283185307179586 * 1000.0 * (double) phase / 48000.0);

            HostEngineTestAccess::push (engine, silence.data(), silence.data(), 480);
            HostEngineTestAccess::callbackWithInput (engine, inL.data(), inL.data(), outL.data(), outR.data(), 480);

            if (b >= 90)
                for (int i = 0; i < 480; ++i)
                    directPeak = std::max (directPeak, std::abs (outL[(size_t) i]));
        }

        check (std::abs (directPeak - 0.4f) < 0.005f, "direct input goes through the plugin (peak " + juce::String (directPeak, 4) + ")");
        HostEngineTestAccess::setDirectInput (engine, false);
    }

    std::printf ("Bad plugin path\n");
    loaded = false;
    engine.loadPlugin ("/nonexistent/Nothing.vst3", {}, [&] (const juce::String& e) { loadError = e; loaded = true; });
    check (loaded && loadError.isNotEmpty(), "reports an error instead of crashing");
    check (engine.getPlugin() != nullptr, "keeps the current plugin");

    HostEngineTestAccess::stopped (engine);

    std::printf ("\n%s\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
