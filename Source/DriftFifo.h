#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

//==============================================================================
/**
    A stereo single-producer / single-consumer FIFO that bridges two audio
    devices running on different clocks.

    The capture side (WASAPI loopback, BlackHole, VB-Cable...) pushes blocks on
    its own thread at its own rate. The output side pulls exactly the number of
    frames its device asks for. The two clocks never run at exactly the same
    speed, so the consumer resamples by a tiny, continuously adjusted ratio that
    keeps the buffered amount (and therefore the latency) constant, instead of
    letting it creep until it clicks.

    How the drift is measured: the raw FIFO level only changes in whole-block
    jumps, which hides slow drift for minutes at a time. So the producer also
    publishes *when* it delivered each block. From the (frames, time) pairs on
    each side the consumer tracks two offsets:
        produced(t) ~= o + rate * t      consumed(t) ~= q + rate * t
    and the continuous fill level is simply o - q. Scheduling jitter can only
    ever make a block look *late* (which lowers o or q), so each offset is
    tracked with a slowly-decaying maximum that ignores late arrivals. A slow,
    critically damped PI loop then steers that level to stay where it started.

    Resampling uses JUCE's windowed-sinc interpolator (flat to within ~0.002 dB
    across the audio band, with its fixed -0.087 dB gain compensated here).

    Threading:
      - prepare() must only be called while neither side is running.
      - push() is called from exactly one producer thread.
      - pull() is called from exactly one consumer thread.
      - getStats() may be called from any thread.
    Timestamps must come from the same monotonic clock on both sides, e.g.
    DriftFifo::now().
*/
class DriftFifo
{
public:
    static constexpr int numChannels = 2;

    struct Stats
    {
        double fillFrames    = 0.0;   // smoothed fill estimate, in input frames
        double targetFrames  = 0.0;
        double correctionPpm = 0.0;   // + = consuming faster than nominal (source clock is fast)
        double inputRate     = 0.0;
        double outputRate    = 0.0;
        int    underruns     = 0;
        int    overflows     = 0;     // producer had to drop data, or consumer had to skip ahead
        int    resyncs       = 0;     // rebuffered after a long gap on either side
        bool   running       = false; // false while (re)buffering
        bool   sourceStalled = false; // no data from the source recently (e.g. nothing playing)
    };

    /** Monotonic time in seconds, for the timestamps. */
    static double now() noexcept   { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    DriftFifo() = default;

    /** (Re)initialises everything. Only call while push()/pull() can't be running. */
    void prepare (double inputRate, double outputRate,
                  int typicalInputBlock, int typicalOutputBlock,
                  double safetyMs, int capacityFrames = 1 << 16)
    {
        jassert (inputRate > 0.0 && outputRate > 0.0);

        capacity = juce::jmax (4096, capacityFrames);
        storage.setSize (numChannels, capacity, false, true, false);
        storage.clear();
        fifo.setTotalSize (capacity);
        fifo.reset();

        inRate       = inputRate;
        outRate      = outputRate;
        nominalRatio = inputRate / outputRate;

        const int inBlock  = juce::jmax (1, typicalInputBlock);
        const int outBlock = juce::jmax (1, typicalOutputBlock);

        // Enough to absorb one block arriving late on either side, plus a safety margin.
        const double blocks = 1.5 * ((double) inBlock + (double) outBlock * nominalRatio);
        target = juce::jlimit (64.0, capacity * 0.25, blocks + safetyMs * 0.001 * inputRate);

        // Treat the source as stalled if nothing arrives for a while (loopback goes quiet
        // when nothing is playing): stop extrapolating and freeze the controller.
        stallSeconds = juce::jmax (0.05, 4.0 * inBlock / inputRate);

        // Critically damped PI loop, bandwidth ~0.05 rad/s. Clock drift is constant, so a
        // slow loop is fine and keeps the ratio from wobbling with timing jitter.
        const double a = inputRate / target;          // fill error (fraction) per unit correction per second
        kP = 2.0 * damping * loopBandwidth / a;
        kI = loopBandwidth * loopBandwidth / a;
        maxIntegral = maxCorrection / kI;
        trackerDecay = trackerDecayPpm * 1.0e-6 * inputRate;   // frames per second

        scratchSize = (int) std::ceil (maxChunk * nominalRatio * (1.0 + maxCorrection)) + 16;
        scratch.setSize (numChannels, scratchSize, false, true, false);
        scratch.clear();

        producerWritten = 0;
        consumerRead    = 0;
        seq.store (0);
        pubWritten.store (0);
        pubTime.store (-1.0);
        snapWritten = 0;
        snapTime    = -1.0;
        lastSnapWritten = -1;
        lastSnapTime = -1.0;
        lastPullTime = -1.0;
        resyncs.store (0);

        integral = 0.0;
        resetConsumer();
        underruns.store (0);
        overflows.store (0);
        publishStats();
    }

    //==============================================================================
    /** Producer side. channels[i] may be nullptr (treated as silence). A mono source is
        copied to both outputs.
    */
    void push (const float* const* channels, int numSourceChannels, int numFrames, double timeSeconds) noexcept
    {
        if (numFrames <= 0 || capacity == 0)
            return;

        int start1, size1, start2, size2;
        fifo.prepareToWrite (numFrames, start1, size1, start2, size2);

        const int written = size1 + size2;
        if (written < numFrames)
            overflows.fetch_add (1, std::memory_order_relaxed);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = nullptr;
            if (channels != nullptr && numSourceChannels > 0)
                src = channels[juce::jmin (ch, numSourceChannels - 1)];

            float* dst = storage.getWritePointer (ch);

            if (src != nullptr)
            {
                if (size1 > 0) juce::FloatVectorOperations::copy (dst + start1, src, size1);
                if (size2 > 0) juce::FloatVectorOperations::copy (dst + start2, src + size1, size2);
            }
            else
            {
                if (size1 > 0) juce::FloatVectorOperations::clear (dst + start1, size1);
                if (size2 > 0) juce::FloatVectorOperations::clear (dst + start2, size2);
            }
        }

        fifo.finishedWrite (written);
        producerWritten += written;

        // Publish (frames delivered, time delivered) as a consistent pair (seqlock).
        const auto s = seq.load (std::memory_order_relaxed);
        seq.store (s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        pubWritten.store (producerWritten, std::memory_order_relaxed);
        pubTime.store (timeSeconds, std::memory_order_relaxed);
        seq.store (s + 2, std::memory_order_release);
    }

    //==============================================================================
    /** Consumer side: fills out[0] and out[1] with numFrames samples at the output rate.
        Writes silence while (re)buffering.
    */
    void pull (float* const* out, int numFrames, double timeSeconds) noexcept
    {
        readProducerSnapshot();
        updateController (timeSeconds, numFrames);

        int done = 0;

        while (done < numFrames)
        {
            const int n = juce::jmin (numFrames - done, maxChunk);
            pullChunk (out, done, n);
            done += n;
        }

        publishStats();
    }

    Stats getStats() const noexcept
    {
        Stats s;
        s.fillFrames    = statFill.load (std::memory_order_relaxed);
        s.targetFrames  = statTarget.load (std::memory_order_relaxed);
        s.correctionPpm = statPpm.load (std::memory_order_relaxed);
        s.inputRate     = statInRate.load (std::memory_order_relaxed);
        s.outputRate    = statOutRate.load (std::memory_order_relaxed);
        s.underruns     = underruns.load (std::memory_order_relaxed);
        s.overflows     = overflows.load (std::memory_order_relaxed);
        s.resyncs       = resyncs.load (std::memory_order_relaxed);
        s.running       = statRunning.load (std::memory_order_relaxed);
        s.sourceStalled = statStalled.load (std::memory_order_relaxed);
        return s;
    }

    static constexpr double maxCorrection = 0.01;   // never resample more than +/-1% from nominal
    static constexpr int    maxChunk      = 1024;

private:
    //==============================================================================
    void readProducerSnapshot() noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto s1 = seq.load (std::memory_order_acquire);
            const auto w  = pubWritten.load (std::memory_order_relaxed);
            const auto t  = pubTime.load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            const auto s2 = seq.load (std::memory_order_relaxed);

            if (s1 == s2 && (s1 & 1u) == 0)
            {
                snapWritten = w;
                snapTime    = t;
                return;
            }
        }
        // Writer was busy every time; keep the previous snapshot.
    }

    void updateController (double nowSeconds, int numFrames) noexcept
    {
        const double previousPull = lastPullTime;
        lastPullTime = nowSeconds;

        // Snapshot bookkeeping happens even while buffering, so a gap that has already
        // been dealt with (by rebuffering) isn't detected a second time.
        const bool   newSnapshot      = snapWritten != lastSnapWritten || ! juce::exactlyEqual (snapTime, lastSnapTime);
        const double previousSnapTime = lastSnapTime;

        if (newSnapshot)
        {
            lastSnapWritten = snapWritten;
            lastSnapTime    = snapTime;
        }

        stalled = snapTime < 0.0 || (nowSeconds - snapTime) > stallSeconds;

        if (! running || stalled)
            return; // hold the learned correction; nothing meaningful to measure

        // A long gap on either side (source paused, output device hiccup) breaks the
        // offset trackers' assumptions, so rebuffer cleanly rather than trust them.
        const bool producerGap = newSnapshot && previousSnapTime >= 0.0 && (snapTime - previousSnapTime) > stallSeconds;
        const bool consumerGap = previousPull >= 0.0 && (nowSeconds - previousPull) > stallSeconds;

        if (producerGap || consumerGap)
        {
            resyncs.fetch_add (1, std::memory_order_relaxed);
            resetConsumer();
            return;
        }

        if (newSnapshot)
        {
            const double o = (double) snapWritten - inRate * snapTime;
            oHat = (oPrimed && previousSnapTime >= 0.0)
                       ? juce::jmax (o, oHat - trackerDecay * (snapTime - previousSnapTime))
                       : o;
            oPrimed = true;
        }

        if (! oPrimed)
            return;

        const double q = (double) consumerRead - inRate * nowSeconds;
        qHat = (qPrimed && previousPull >= 0.0)
                   ? juce::jmax (q, qHat - trackerDecay * (nowSeconds - previousPull))
                   : q;
        qPrimed = true;

        const double level = oHat - qHat;

        if (! smootherPrimed)
        {
            // Hold the level we (re)started at: that's when the FIFO held exactly 'target'.
            controlTarget = level;
            smooth1 = smooth2 = level;
            smootherPrimed = true;
        }

        const double dt = (double) numFrames / outRate;
        const double alpha = 1.0 - std::exp (-dt / smoothingSeconds);
        smooth1 += alpha * (level - smooth1);
        smooth2 += alpha * (smooth1 - smooth2);

        const double error = (smooth2 - controlTarget) / target;
        integral   = juce::jlimit (-maxIntegral, maxIntegral, integral + error * dt);
        correction = juce::jlimit (-maxCorrection, maxCorrection, kP * error + kI * integral);
        levelError = smooth2 - controlTarget;
    }

    void resetConsumer() noexcept
    {
        // Note: 'integral' (the learned clock difference) is deliberately kept across
        // underruns so we don't have to re-learn the drift after every pause.
        running = false;
        smootherPrimed = false;
        oPrimed = qPrimed = false;
        levelError = 0.0;
        correction = juce::jlimit (-maxCorrection, maxCorrection, kI * integral);

        for (auto& i : interpolators)
            i.reset();
    }

    void discard (int frames) noexcept
    {
        if (frames <= 0)
            return;

        fifo.finishedRead (frames);
        consumerRead += frames;
    }

    void pullChunk (float* const* out, int offset, int n) noexcept
    {
        auto silence = [&]
        {
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::clear (out[ch] + offset, n);
        };

        int ready = fifo.getNumReady();

        if (! running)
        {
            if ((double) ready < target)
            {
                silence();
                return;
            }

            // Start (or restart) at the intended latency rather than whatever piled up.
            const int excess = ready - (int) target;
            discard (excess);
            ready -= juce::jmax (0, excess);

            running = true;
            smootherPrimed = false;
            oPrimed = qPrimed = false;

            for (auto& i : interpolators)
                i.reset();
        }

        // Miles too much buffered (e.g. the output device stalled): jump back to the target.
        if ((double) ready > target * 4.0 && ready > n * 4)
        {
            discard (ready - (int) target);
            ready = (int) target;
            smootherPrimed = false;
            oPrimed = qPrimed = false;
            overflows.fetch_add (1, std::memory_order_relaxed);
        }

        const double ratio  = nominalRatio * (1.0 + correction);
        const int    needed = juce::jmin (scratchSize, (int) std::ceil (n * ratio) + 4);

        if (ready < needed)
        {
            underruns.fetch_add (1, std::memory_order_relaxed);
            resetConsumer();
            silence();
            return;
        }

        int start1, size1, start2, size2;
        fifo.prepareToRead (needed, start1, size1, start2, size2);

        int used = 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = storage.getReadPointer (ch);
            float* tmp = scratch.getWritePointer (ch);

            if (size1 > 0) juce::FloatVectorOperations::copy (tmp, src + start1, size1);
            if (size2 > 0) juce::FloatVectorOperations::copy (tmp + size1, src + start2, size2);

            used = interpolators[(size_t) ch].process (ratio, tmp, out[ch] + offset, n);
            juce::FloatVectorOperations::multiply (out[ch] + offset, sincGainCompensation, n);
        }

        const int consumed = juce::jmin (used, size1 + size2);
        fifo.finishedRead (consumed);
        consumerRead += consumed;
    }

    void publishStats() noexcept
    {
        statFill.store (smootherPrimed ? target + levelError : (double) fifo.getNumReady(), std::memory_order_relaxed);
        statTarget.store (target, std::memory_order_relaxed);
        statPpm.store (correction * 1.0e6, std::memory_order_relaxed);
        statInRate.store (inRate, std::memory_order_relaxed);
        statOutRate.store (outRate, std::memory_order_relaxed);
        statRunning.store (running, std::memory_order_relaxed);
        statStalled.store (stalled, std::memory_order_relaxed);
    }

    //==============================================================================
    static constexpr double loopBandwidth    = 0.07;  // rad/s
    static constexpr double damping          = 1.0;
    static constexpr double smoothingSeconds = 1.0;   // each of two cascaded one-pole smoothers
    static constexpr double trackerDecayPpm  = 2000.0; // how fast the max-trackers may fall (must exceed real drift)
    static constexpr float  sincGainCompensation = 1.0f / 0.99f; // JUCE's sinc kernel has a fixed 0.99 gain

    juce::AbstractFifo fifo { 4096 };
    juce::AudioBuffer<float> storage, scratch;
    std::array<juce::WindowedSincInterpolator, numChannels> interpolators;

    int    capacity     = 0;
    int    scratchSize  = 0;
    double inRate       = 48000.0;
    double outRate      = 48000.0;
    double nominalRatio = 1.0;
    double target       = 1024.0;
    double stallSeconds = 0.05;
    double kP = 0.0, kI = 0.0, maxIntegral = 0.0, trackerDecay = 100.0;

    // producer-thread state
    std::int64_t producerWritten = 0;

    // shared (seqlock-published by the producer)
    std::atomic<std::uint32_t> seq { 0 };
    std::atomic<std::int64_t>  pubWritten { 0 };
    std::atomic<double>        pubTime { -1.0 };

    // consumer-thread state
    std::int64_t consumerRead = 0;
    std::int64_t snapWritten  = 0;
    double snapTime     = -1.0;
    bool   running      = false;
    bool   stalled      = true;
    bool   smootherPrimed = false;
    double smooth1 = 0.0, smooth2 = 0.0;
    std::int64_t lastSnapWritten = -1;
    double lastSnapTime  = -1.0;
    double lastPullTime  = -1.0;
    double oHat = 0.0, qHat = 0.0;
    bool   oPrimed = false, qPrimed = false;
    double controlTarget = 0.0;
    double levelError    = 0.0;
    double integral   = 0.0;
    double correction = 0.0;

    std::atomic<int>    underruns { 0 }, overflows { 0 }, resyncs { 0 };
    std::atomic<double> statFill { 0.0 }, statTarget { 0.0 }, statPpm { 0.0 },
                        statInRate { 0.0 }, statOutRate { 0.0 };
    std::atomic<bool>   statRunning { false }, statStalled { true };

    JUCE_DECLARE_NON_COPYABLE (DriftFifo)
};
