// Headless simulation of two audio devices on independent clocks, connected by DriftFifo.
//
// It checks that the FIFO
//   - locks onto the real clock difference (and so keeps latency constant),
//   - never under/overruns once settled, even with realistic scheduling jitter,
//   - recovers cleanly when the source goes quiet (WASAPI loopback stops
//     delivering packets when nothing is playing),
// and measures how transparent the resampling is (FFT-based THD+N).
//
// Run with DRIFT_TRACE=1 to print the controller state every 25 simulated seconds.

#include "DriftFifo.h"

#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace
{
constexpr double twoPi = 6.283185307179586476925286766559;

struct Scenario
{
    const char* name;
    double inNominalRate;       // what the capture device claims
    double driftPpm;            // how far its real clock is from that claim
    double outRate;
    int    inBlock, outBlock;
    bool   jitter;              // realistic late deliveries / late callbacks
    double gapStart, gapLength; // seconds during which the source delivers nothing
    double seconds;
    double toneHz;
};

struct Result
{
    int    underrunsAfterSettle = 0;
    int    overflowsAfterSettle = 0;
    int    resyncs = 0;
    double finalPpm = 0.0;
    double fillErrorFrames = 0.0;
    double thdnDb = 0.0;
    double amplitude = 0.0;
};

//==============================================================================
void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -twoPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));

        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// THD+N relative to the tone, ignoring +/-15 Hz around it (slow pitch wander from
// clock tracking is not distortion) and everything below 20 Hz.
double measureThdN (const std::vector<float>& x, double sampleRate, double toneHz, double& amplitudeOut)
{
    const size_t n = x.size();
    std::vector<std::complex<double>> a (n);
    double sumW2 = 0;

    for (size_t i = 0; i < n; ++i)
    {
        // 7-term Blackman-Harris: sidelobes far below anything we measure.
        const double t = twoPi * (double) i / (double) n;
        const double w = 0.27105140069342 - 0.43329793923448 * std::cos (t) + 0.21812299954311 * std::cos (2 * t)
                       - 0.06592544638803 * std::cos (3 * t) + 0.01081174209837 * std::cos (4 * t)
                       - 0.00077658482522 * std::cos (5 * t) + 0.00001388721735 * std::cos (6 * t);
        a[i] = x[i] * w;
        sumW2 += w * w;
    }

    fft (a);

    const double binHz = sampleRate / (double) n;
    const double guardHz = 15.0;
    double signal = 0, noise = 0;

    for (size_t k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * binHz;
        const double p = std::norm (a[k]);

        if (std::abs (f - toneHz) <= guardHz) signal += p;
        else if (f >= 20.0)                    noise += p;
    }

    // Parseval: sum over positive bins of |X|^2 = (N/2) * sum (x*w)^2, and a sine of
    // amplitude A contributes (A^2/2) * sum w^2.
    amplitudeOut = std::sqrt (4.0 * signal / ((double) n * sumW2));
    return 10.0 * std::log10 (noise / signal);
}

//==============================================================================
Result run (const Scenario& sc)
{
    DriftFifo fifo;
    fifo.prepare (sc.inNominalRate, sc.outRate, sc.inBlock, sc.outBlock, 5.0);

    const double inTrueRate = sc.inNominalRate * (1.0 + sc.driftPpm * 1.0e-6);
    const double settleTime = 30.0;
    const size_t analyseSamples = 1 << 16;
    const bool trace = std::getenv ("DRIFT_TRACE") != nullptr;

    std::mt19937 rng (1234);
    std::uniform_real_distribution<double> uni (0.0, 1.0);

    // Realistic lateness: mostly prompt, sometimes a few ms late, occasionally very late
    // (which also makes packets arrive in bunches).
    auto lateness = [&] (double scale)
    {
        if (! sc.jitter) return 0.0;
        const double r = uni (rng);
        if (r < 0.75) return scale * 0.0003 * uni (rng);
        if (r < 0.97) return scale * (0.0003 + 0.0027 * uni (rng));
        return scale * (0.003 + 0.009 * uni (rng));
    };

    std::vector<float> inL ((size_t) sc.inBlock), inR ((size_t) sc.inBlock);
    std::vector<float> outL ((size_t) sc.outBlock), outR ((size_t) sc.outBlock);
    std::vector<float> history;
    history.reserve ((size_t) (sc.outRate * (sc.seconds + 1)));

    long long producedFrames = 0;
    double nextDelivery = sc.inBlock / inTrueRate;    // first packet completes here
    long long pullIndex = 0;
    double lastPullTime = 0.0;
    double nextPull = 0.0013;

    int underrunsAtSettle = -1, overflowsAtSettle = -1;
    double nextTrace = 0.0;

    while (nextPull < sc.seconds)
    {
        if (nextDelivery <= nextPull)
        {
            const double now = nextDelivery;
            const bool inGap = sc.gapLength > 0 && now >= sc.gapStart && now < sc.gapStart + sc.gapLength;

            for (int i = 0; i < sc.inBlock; ++i)
            {
                const double ph = twoPi * sc.toneHz * (double) (producedFrames + i) / sc.inNominalRate;
                inL[(size_t) i] = (float) (0.5 * std::sin (ph));
                inR[(size_t) i] = -inL[(size_t) i];
            }

            if (! inGap)
            {
                const float* chans[] = { inL.data(), inR.data() };
                fifo.push (chans, 2, sc.inBlock, now);
            }

            producedFrames += sc.inBlock;

            // The next packet is complete one block later (source clock), and we get it
            // after some scheduling delay - but never before the current one.
            const double completes = (double) (producedFrames + sc.inBlock) / inTrueRate;
            nextDelivery = juce::jmax (now, completes + lateness (1.0));
        }
        else
        {
            const double now = nextPull;
            float* chans[] = { outL.data(), outR.data() };
            fifo.pull (chans, sc.outBlock, now);
            history.insert (history.end(), outL.begin(), outL.end());
            lastPullTime = now;

            if (now >= settleTime && underrunsAtSettle < 0)
            {
                const auto s = fifo.getStats();
                underrunsAtSettle = s.underruns;
                overflowsAtSettle = s.overflows;
            }

            if (trace && now >= nextTrace)
            {
                const auto t = fifo.getStats();
                std::printf ("      t=%6.1f fill=%9.2f target=%8.2f ppm=%9.3f running=%d\n",
                             now, t.fillFrames, t.targetFrames, t.correctionPpm, (int) t.running);
                nextTrace += 25.0;
            }

            ++pullIndex;
            const double nominal = (double) pullIndex * sc.outBlock / sc.outRate + 0.0013;
            nextPull = juce::jmax (lastPullTime, nominal + lateness (0.7));
        }
    }

    const auto s = fifo.getStats();
    Result r;
    r.underrunsAfterSettle = s.underruns - juce::jmax (0, underrunsAtSettle);
    r.overflowsAfterSettle = s.overflows - juce::jmax (0, overflowsAtSettle);
    r.resyncs = s.resyncs;
    r.finalPpm = s.correctionPpm;
    r.fillErrorFrames = s.fillFrames - s.targetFrames;

    std::vector<float> tail (history.end() - (long) analyseSamples, history.end());
    r.thdnDb = measureThdN (tail, sc.outRate, sc.toneHz * (inTrueRate / sc.inNominalRate), r.amplitude);
    return r;
}
} // namespace

int main()
{
    const Scenario scenarios[] =
    {
        { "Same clock, 48k loopback -> 48k WASAPI",          48000,    0.0, 48000,  480, 480, false,   0, 0, 200,  1000 },
        { "+80 ppm, 480 in / 441 out",                        48000,   80.0, 48000,  480, 441, false,   0, 0, 300,  1000 },
        { "+80 ppm, 480 in / 441 out, 15 kHz tone",           48000,   80.0, 48000,  480, 441, false,   0, 0, 300, 15000 },
        { "-150 ppm, 1024 in / 256 out (CoreAudio-ish)",      48000, -150.0, 48000, 1024, 256, false,   0, 0, 300,  1000 },
        { "44.1k source -> 48k output, +30 ppm",              44100,   30.0, 48000,  441, 480, false,   0, 0, 300,  1000 },
        { "Scheduling jitter (late/bunched packets), +40 ppm",48000,   40.0, 48000,  480, 480, true,    0, 0, 300,  1000 },
        { "Scheduling jitter, 15 kHz tone, -60 ppm",          48000,  -60.0, 48000,  480, 480, true,    0, 0, 300, 15000 },
        { "Source idle for 2 s at t=150 s (nothing playing)", 48000,   60.0, 48000,  480, 480, true,  150, 2, 300,  1000 },
    };

    bool allPassed = true;

    for (const auto& sc : scenarios)
    {
        const auto r = run (sc);

        const bool ppmOk   = std::abs (r.finalPpm - sc.driftPpm) < 3.0;
        const int  allowedXruns = sc.gapLength > 0 ? 1 : 0;   // the gap itself
        const bool xrunOk  = r.underrunsAfterSettle + r.resyncs <= allowedXruns && r.overflowsAfterSettle == 0;
        const bool cleanOk = r.thdnDb < -90.0 && std::abs (r.amplitude - 0.5) < 0.0005;
        const bool passed  = ppmOk && xrunOk && cleanOk;
        allPassed = allPassed && passed;

        std::printf ("%-52s %s\n", sc.name, passed ? "PASS" : "FAIL");
        std::printf ("    correction %+8.2f ppm (true %+.0f)  fill %+7.2f frames from target  "
                     "underruns %d  resyncs %d  overflows %d\n",
                     r.finalPpm, sc.driftPpm, r.fillErrorFrames, r.underrunsAfterSettle, r.resyncs, r.overflowsAfterSettle);
        std::printf ("    %5.0f Hz tone: amplitude %.5f (sent 0.50000), THD+N %.1f dB\n",
                     sc.toneHz, r.amplitude, r.thdnDb);
    }

    std::printf ("\n%s\n", allPassed ? "ALL PASSED" : "SOME TESTS FAILED");
    return allPassed ? 0 : 1;
}
