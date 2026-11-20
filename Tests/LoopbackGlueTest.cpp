// Runs LoopbackCaptureSource (the Windows loopback adapter) against the fake WASAPI
// backend in real time, feeding a DriftFifo the way the app does, and checks the
// device list, error handling and channel-pair selection.

#include "LoopbackCaptureSource.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %-60s %s\n", what, condition ? "ok" : "FAILED");
        if (! condition) ++failures;
    }

    // Pull for 'seconds' in real time with 10 ms output blocks; return peak level on each channel.
    std::pair<float, float> runFor (DriftFifo& fifo, double seconds)
    {
        std::vector<float> l (480), r (480);
        float peakL = 0, peakR = 0;
        const auto start = std::chrono::steady_clock::now();
        auto next = start;

        while (std::chrono::steady_clock::now() - start < std::chrono::duration<double> (seconds))
        {
            next += std::chrono::milliseconds (10);
            std::this_thread::sleep_until (next);

            float* chans[] = { l.data(), r.data() };
            fifo.pull (chans, 480, DriftFifo::now());

            for (int i = 0; i < 480; ++i)
            {
                peakL = std::max (peakL, std::abs (l[(size_t) i]));
                peakR = std::max (peakR, std::abs (r[(size_t) i]));
            }
        }

        return { peakL, peakR };
    }
}

int main()
{
    LoopbackCaptureSource source;

    std::printf ("Device list\n");
    const auto names = source.getAvailableSources();
    check (names.size() == 2, "two endpoints listed");
    check (source.getDefaultSource() == "Speakers (Test Interface)", "default endpoint chosen");
    check (source.isLoopback(), "reports itself as loopback");

    std::printf ("Errors\n");
    check (source.start ("No Such Device", 0).isNotEmpty(), "unknown device is rejected");
    check (source.start ("Speakers (Test Interface)", 4).isNotEmpty(), "channels 5-6 on a 4-channel device are rejected");
    check (! source.isRunning(), "not running after a failed start");

    for (int firstChannel : { 0, 2 })
    {
        std::printf ("Capture channels %d-%d\n", firstChannel + 1, firstChannel + 2);

        const auto error = source.start ("Speakers (Test Interface)", firstChannel);
        check (error.isEmpty(), "starts");
        check (source.isRunning(), "running");
        check (source.getSampleRate() == 48000.0 && source.getNumChannels() == 4, "reports 48 kHz, 4 channels");

        DriftFifo fifo;
        fifo.prepare (source.getSampleRate(), 48000.0, source.getTypicalBlockSize(), 480, 5.0);
        source.setTarget (&fifo);

        const auto [peakL, peakR] = runFor (fifo, 3.0);
        const auto stats = fifo.getStats();

        source.stop();
        source.setTarget (nullptr);

        const float expectL = 0.1f * (float) (firstChannel + 1);
        const float expectR = 0.1f * (float) (firstChannel + 2);
        std::printf ("    peaks %.4f / %.4f (expected %.1f / %.1f), underruns %d, running %d\n",
                     peakL, peakR, expectL, expectR, stats.underruns, (int) stats.running);

        check (std::abs (peakL - expectL) < 0.002f, "left carries the selected first channel");
        check (std::abs (peakR - expectR) < 0.002f, "right carries the selected second channel");
        check (stats.running, "FIFO is running (not stuck buffering)");
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
