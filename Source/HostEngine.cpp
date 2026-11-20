#include "HostEngine.h"
#include "JuceDeviceCaptureSource.h"

namespace
{
    const char* preferredOutputTypeName()
    {
       #if JUCE_WINDOWS
        return "Windows Audio";   // WASAPI shared mode: coexists with your DAW's ASIO driver
       #elif JUCE_MAC
        return "CoreAudio";
       #else
        return "ALSA";
       #endif
    }

    juce::String stripDuplicateSuffix (const juce::String& name)
    {
        // "Speakers (Focusrite USB Audio) (2)" -> "Speakers (Focusrite USB Audio)"
        const auto trimmed = name.trim();
        if (trimmed.endsWithChar (')'))
        {
            const auto open = trimmed.lastIndexOfChar ('(');
            if (open > 0 && trimmed.substring (open + 1, trimmed.length() - 1).containsOnly ("0123456789"))
                return trimmed.substring (0, open).trim();
        }
        return trimmed;
    }
}

//==============================================================================
HostEngine::HostEngine()
    : sources (createPlatformCaptureSources()),
      inputSource (std::make_unique<JuceDeviceCaptureSource>())
{
    juce::addDefaultFormatsToManager (formatManager);
    wetGain.reset (48000.0, 0.02);
}

HostEngine::~HostEngine()
{
    *aliveToken = false;
    stop();

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        old = std::move (plugin);
    }
    old.reset();
}

//==============================================================================
juce::AudioIODeviceType* HostEngine::findOutputType (const juce::String& name)
{
    const auto& types = outputManager.getAvailableDeviceTypes();

    for (auto* t : types)
        if (t->getTypeName() == name)
            return t;

    for (auto* t : types)
        if (t->getTypeName() == preferredOutputTypeName())
            return t;

    return types.getFirst();
}

juce::StringArray HostEngine::getOutputTypeNames()
{
    juce::StringArray names;

    for (auto* t : outputManager.getAvailableDeviceTypes())
        names.add (t->getTypeName());

    return names;
}

juce::String HostEngine::getDefaultOutputTypeName()
{
    auto* t = findOutputType (preferredOutputTypeName());
    return t != nullptr ? t->getTypeName() : juce::String();
}

juce::StringArray HostEngine::getOutputDeviceNames (const juce::String& typeName)
{
    if (auto* t = findOutputType (typeName))
    {
        t->scanForDevices();
        return t->getDeviceNames (false);
    }

    return {};
}

juce::String HostEngine::getDefaultOutputDeviceName (const juce::String& typeName)
{
    const auto names = getOutputDeviceNames (typeName);

    if (auto* t = findOutputType (typeName))
    {
        const int index = t->getDefaultDeviceIndex (false);
        if (juce::isPositiveAndBelow (index, names.size()))
            return names[index];
    }

    return names.isEmpty() ? juce::String() : names[0];
}

//==============================================================================
juce::String HostEngine::openOutput (const Config& c)
{
    auto* type = findOutputType (c.outputTypeName);
    if (type == nullptr)
        return "No audio output types are available.";

    if (c.outputDeviceName.isEmpty())
        return "No output device is selected (or none is available).";

    const bool wantDirectInput = usesDirectInput (type->getTypeName()) && c.inputEnabled;

    // We open the device ourselves rather than through an AudioDeviceManager, so nothing
    // gets opened with remembered or default settings first (which could retune the interface).
    type->scanForDevices();
    std::unique_ptr<juce::AudioIODevice> device (type->createDevice (c.outputDeviceName,
                                                                     wantDirectInput ? c.outputDeviceName : juce::String()));
    if (device == nullptr)
        return "Couldn't open \"" + c.outputDeviceName + "\".";

    // Follow the interface: open at whatever rate it's running at right now (your DAW and the
    // driver decide), never a default. ASIO and CoreAudio report it before opening.
    double rate = device->getCurrentSampleRate();
    const auto rates = device->getAvailableSampleRates();

    if (rate <= 0.0)
        rate = rates.contains (48000.0) ? 48000.0 : (rates.isEmpty() ? 48000.0 : rates[0]);

    const int bufferSize = c.bufferSize > 0 ? c.bufferSize : device->getDefaultBufferSize();

    juce::BigInteger outs, ins;
    outs.setRange (juce::jmax (0, c.outputFirstChannel), 2, true);
    if (wantDirectInput)
        ins.setRange (juce::jmax (0, c.inputFirstChannel), 2, true);

    if (auto error = device->open (ins, outs, rate, bufferSize); error.isNotEmpty())
        return "Couldn't open \"" + c.outputDeviceName + "\": " + error;

    const int total  = device->getOutputChannelNames().size();
    const int active = device->getActiveOutputChannels().countNumberOfSetBits();

    if (active < 2 || c.outputFirstChannel + 2 > total)
    {
        device->close();
        return "\"" + c.outputDeviceName + "\" has " + juce::String (total) + " output channels, so channels "
               + juce::String (c.outputFirstChannel + 1) + "-" + juce::String (c.outputFirstChannel + 2)
               + " aren't available. If Windows shows your interface's outputs as separate devices, "
                 "pick the device for your second speakers and channels 1-2.";
    }

    outputDevice = std::move (device);
    return {};
}

void HostEngine::closeOutput()
{
    if (outputDevice != nullptr)
    {
        outputDevice->stop();
        outputDevice->close();
        outputDevice.reset();
    }
}

juce::String HostEngine::start (const Config& c)
{
    stopAudio();

    config = c;
    wantRunning = true;
    lastError.clear();

    auto fail = [this] (const juce::String& message)
    {
        lastError = message;

        // Missing/busy devices often come back (interface switched on, wake from sleep...).
        if (wantRunning)
        {
            ticksUntilRetry = 3;
            startTimer (1000);
        }

        notify();
        return message;
    };

    if (sources.empty())
        return fail ("No capture modes are available on this system.");

    if (c.sourceName.isEmpty())
        return fail ("No capture device is selected (or none is available).");

    auto& source = getCaptureMode (c.captureMode);

    // Loopback of a device we also play into, on the same channels, would feed back forever.
    if (source.isLoopback()
        && stripDuplicateSuffix (c.sourceName).equalsIgnoreCase (stripDuplicateSuffix (c.outputDeviceName))
        && c.sourceFirstChannel == c.outputFirstChannel)
    {
        wantRunning = false;
        return fail ("The output is the same device and channels that are being captured, which would feed back. "
                     "Choose the output channels for your second speakers.");
    }

    if (auto error = openOutput (c); error.isNotEmpty())
        return fail (error);

    if (auto error = source.start (c.sourceName, c.sourceFirstChannel); error.isNotEmpty())
    {
        closeOutput();
        return fail (error);
    }

    auto* device = outputDevice.get();

    if (device == nullptr || source.getSampleRate() <= 0.0 || device->getCurrentSampleRate() <= 0.0)
    {
        source.stop();
        closeOutput();
        return fail ("A device reported an invalid sample rate.");
    }

    fifoOutputRate = device->getCurrentSampleRate();
    fifo.prepare (source.getSampleRate(), fifoOutputRate,
                  source.getTypicalBlockSize(), device->getCurrentBufferSizeSamples(), c.safetyMs);

    activeSource = &source;
    source.setTarget (&fifo);
    device->start (this);   // calls audioDeviceAboutToStart first
    running = true;

    startInput();   // optional; a problem here doesn't stop the main path

    startTimer (1000);
    notify();
    return {};
}

//==============================================================================
juce::String HostEngine::startInput()
{
    stopInput();
    inputRetryTicks = 0;

    if (! config.inputEnabled)
    {
        inputError.clear();
        return {};
    }

    if (usesDirectInput (config.outputTypeName))
    {
        // The output device was opened with the input pair too (see openOutput).
        auto* device = outputDevice.get();
        const int active = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;

        if (! running || active == 0)
        {
            const int total = device != nullptr ? device->getInputChannelNames().size() : 0;
            inputError = "Input channels " + juce::String (config.inputFirstChannel + 1) + "-"
                         + juce::String (config.inputFirstChannel + 2) + " aren't available on \""
                         + config.outputDeviceName + "\" (it has " + juce::String (total) + " inputs).";
            return inputError;
        }

        directInputActive.store (true);
        inputError.clear();
        return {};
    }

    auto fail = [this] (const juce::String& message)
    {
        inputError = message;
        inputRetryTicks = 3;   // the watchdog tries again in a few seconds
        return message;
    };

    if (config.inputDeviceName.isEmpty())
        return fail ("No input device is selected.");

    auto* device = outputDevice.get();
    if (! running || device == nullptr)
        return fail ("The output isn't running.");

    if (auto error = inputSource->start (config.inputDeviceName, config.inputFirstChannel); error.isNotEmpty())
        return fail (error);

    if (inputSource->getSampleRate() <= 0.0)
    {
        inputSource->stop();
        return fail ("The input device reported an invalid sample rate.");
    }

    inputFifo.prepare (inputSource->getSampleRate(), device->getCurrentSampleRate(),
                       inputSource->getTypicalBlockSize(), device->getCurrentBufferSizeSamples(), config.safetyMs);
    inputSource->setTarget (&inputFifo);

    {
        const juce::SpinLock::ScopedLockType lock (inputLock);
        inputActive = true;
    }

    inputError.clear();
    return {};
}

void HostEngine::stopInput()
{
    directInputActive.store (false);

    {
        const juce::SpinLock::ScopedLockType lock (inputLock);
        inputActive = false;           // the audio thread stops reading first...
    }

    inputSource->stop();               // ...then the producer thread stops
    inputSource->setTarget (nullptr);
}

juce::String HostEngine::setInputConfig (bool enabled, const juce::String& deviceName, int firstChannel)
{
    config.inputEnabled      = enabled;
    config.inputDeviceName   = deviceName;
    config.inputFirstChannel = juce::jmax (0, firstChannel);

    juce::String error;

    if (usesDirectInput (config.outputTypeName))
    {
        // ASIO: the input pair is opened together with the outputs, so reopen the device.
        if (wantRunning)
            error = start (config);
        if (! enabled)
            inputError.clear();
    }
    else if (running)
    {
        error = startInput();
    }
    else if (! enabled)
    {
        inputError.clear();
    }

    notify();
    return error;
}

juce::StringArray HostEngine::getOpenOutputChannelNames() const
{
    auto* device = outputDevice.get();
    return device != nullptr ? device->getOutputChannelNames() : juce::StringArray();
}

juce::StringArray HostEngine::getOpenInputChannelNames() const
{
    if (usesDirectInput (config.outputTypeName))
    {
        auto* device = outputDevice.get();
        return device != nullptr ? device->getInputChannelNames() : juce::StringArray();
    }

    juce::StringArray names;
    for (int i = 0; i < getOpenInputChannelCount(); ++i)
        names.add ("Input " + juce::String (i + 1));
    return names;
}

int HostEngine::getOpenInputChannelCount() const
{
    if (usesDirectInput (config.outputTypeName))
    {
        auto* device = outputDevice.get();
        return device != nullptr ? device->getInputChannelNames().size() : 0;
    }

    return inputActive ? inputSource->getNumChannels() : 0;
}

void HostEngine::checkInput()
{
    if (! running || ! config.inputEnabled || usesDirectInput (config.outputTypeName))
        return;   // a direct (ASIO) input lives and dies with the output device

    if (inputActive && inputSource->isRunning())
        return;

    if (inputActive)
    {
        // It was running and stopped by itself (unplugged, format change...).
        const auto reason = inputSource->getLastError();
        ++automaticRestarts;

        if (startInput().isNotEmpty() && reason.isNotEmpty())
            inputError = reason + " - " + inputError;

        notify();
        return;
    }

    if (inputRetryTicks > 0)
    {
        --inputRetryTicks;
        return;
    }

    const bool hadError = inputError.isNotEmpty();
    startInput();

    if (hadError || inputError.isNotEmpty())
        notify();
}

void HostEngine::stop()
{
    wantRunning = false;
    stopTimer();
    stopAudio();
}

void HostEngine::stopAudio()
{
    cancelPendingUpdate();

    if (outputDevice != nullptr)
        outputDevice->stop();                       // consumer first...

    stopInput();

    if (activeSource != nullptr)
    {
        activeSource->stop();                       // ...then the producer thread
        activeSource->setTarget (nullptr);
        activeSource = nullptr;
    }

    closeOutput();

    const bool wasRunning = running;
    running = false;

    if (wasRunning)
        notify();
}

int HostEngine::getOpenOutputChannelCount() const
{
    auto* device = outputDevice.get();
    return device != nullptr ? device->getOutputChannelNames().size() : 0;
}

int HostEngine::getOpenSourceChannelCount() const
{
    return activeSource != nullptr ? activeSource->getNumChannels() : 0;
}

//==============================================================================
void HostEngine::timerCallback()
{
    if (! wantRunning)
    {
        stopTimer();
        return;
    }

    auto* device = outputDevice.get();
    const bool outputOk = running && device != nullptr && device->isPlaying();
    const bool sourceOk = running && activeSource != nullptr && activeSource->isRunning();

    if (outputOk && sourceOk)
    {
        ticksUntilRetry = 0;
        checkInput();
        return;
    }

    if (ticksUntilRetry > 0)
    {
        --ticksUntilRetry;
        return;
    }

    // Something went away (device unplugged, Windows format change, sleep/wake...): retry.
    juce::String reason;
    if (activeSource != nullptr && activeSource->getLastError().isNotEmpty())
        reason = activeSource->getLastError();

    ++automaticRestarts;
    const auto error = start (config);

    if (error.isNotEmpty() && wantRunning)
    {
        lastError = (reason.isNotEmpty() ? reason + " - " : juce::String()) + error + " (retrying)";
        notify();
    }
}

void HostEngine::handleAsyncUpdate()
{
    if (wantRunning)
    {
        ++automaticRestarts;
        start (config);
    }
}

//==============================================================================
void HostEngine::configurePluginLayout (juce::AudioPluginInstance& p) const
{
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());

    if (p.checkBusesLayoutSupported (stereo))
        p.setBusesLayout (stereo);
    else
        p.enableAllBuses();

    p.setNonRealtime (false);
}

void HostEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    const int block   = device->getCurrentBufferSizeSamples();

    // If Windows/macOS changed the device's rate under us, the FIFO needs re-preparing.
    if (running && fifoOutputRate > 0.0 && std::abs (rate - fifoOutputRate) > 0.5)
        triggerAsyncUpdate();

    const juce::SpinLock::ScopedLockType lock (pluginLock);

    currentRate  = rate;
    currentBlock = block;
    pluginBlock  = juce::jlimit (64, chunkFrames, block > 0 ? block : 512);

    dryBuffer.setSize (2, chunkFrames, false, true, false);
    wetBuffer.setSize (2, chunkFrames, false, true, false);
    inputBuffer.setSize (2, chunkFrames, false, true, false);
    inputGainSmoothed.reset (rate, 0.05);
    inputGainSmoothed.setCurrentAndTargetValue (inputGain.load());
    wetGain.reset (rate, 0.02);
    wetGain.setCurrentAndTargetValue ((plugin != nullptr && ! bypassed.load()) ? 1.0f : 0.0f);

    if (plugin != nullptr)
    {
        configurePluginLayout (*plugin);
        plugin->setRateAndBufferSizeDetails (rate, pluginBlock);
        plugin->prepareToPlay (rate, pluginBlock);
        processBuffer.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()),
                               pluginBlock, false, true, false);
        pluginPrepared = true;
    }
}

void HostEngine::audioDeviceStopped()
{
    const juce::SpinLock::ScopedLockType lock (pluginLock);

    if (plugin != nullptr && pluginPrepared)
        plugin->releaseResources();

    pluginPrepared = false;
}

void HostEngine::audioDeviceError (const juce::String& message)
{
    juce::MessageManager::callAsync ([this, message, alive = std::weak_ptr<bool> (aliveToken)]
    {
        if (auto a = alive.lock(); a && *a)
        {
            lastError = message;
            notify();
        }
    });
}

void HostEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                   float* const* outputChannelData, int numOutputChannels,
                                                   int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const bool direct = directInputActive.load (std::memory_order_relaxed)
                        && inputChannelData != nullptr && numInputChannels > 0;

    const double now = DriftFifo::now();
    float peak = 0.0f;

    for (int offset = 0; offset < numSamples; offset += chunkFrames)
    {
        const int n = juce::jmin (chunkFrames, numSamples - offset);

        float* dry[2] = { dryBuffer.getWritePointer (0), dryBuffer.getWritePointer (1) };
        fifo.pull (dry, n, now);

        // Interface input straight from this device (ASIO): same clock, sample-aligned.
        if (direct)
        {
            const float* inL = inputChannelData[0];
            const float* inR = inputChannelData[juce::jmin (1, numInputChannels - 1)];
            inputGainSmoothed.setTargetValue (inputGain.load (std::memory_order_relaxed));

            if (inL != nullptr && inR != nullptr)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float g = inputGainSmoothed.getNextValue();
                    dry[0][i] += g * inL[offset + i];
                    dry[1][i] += g * inR[offset + i];
                }
            }
        }

        // Mix in the optional interface input (its own clock, so its own FIFO).
        {
            const juce::SpinLock::ScopedTryLockType lock (inputLock);

            if (lock.isLocked() && inputActive)
            {
                float* in[2] = { inputBuffer.getWritePointer (0), inputBuffer.getWritePointer (1) };
                inputFifo.pull (in, n, now);
                inputGainSmoothed.setTargetValue (inputGain.load (std::memory_order_relaxed));

                for (int i = 0; i < n; ++i)
                {
                    const float g = inputGainSmoothed.getNextValue();
                    dry[0][i] += g * in[0][i];
                    dry[1][i] += g * in[1][i];
                }
            }
        }

        bool haveWet = false;

        {
            const juce::SpinLock::ScopedTryLockType lock (pluginLock);

            if (lock.isLocked() && plugin != nullptr && pluginPrepared)
            {
                const int procChannels = processBuffer.getNumChannels();
                const int outChannels  = juce::jmax (1, plugin->getTotalNumOutputChannels());

                for (int pos = 0; pos < n; pos += pluginBlock)
                {
                    const int m = juce::jmin (pluginBlock, n - pos);

                    for (int ch = 0; ch < procChannels; ++ch)
                    {
                        if (ch < 2) processBuffer.copyFrom (ch, 0, dryBuffer, ch, pos, m);
                        else        processBuffer.clear (ch, 0, m);
                    }

                    juce::AudioBuffer<float> view (processBuffer.getArrayOfWritePointers(), procChannels, m);
                    midi.clear();
                    plugin->processBlock (view, midi);

                    wetBuffer.copyFrom (0, pos, processBuffer, 0, 0, m);
                    wetBuffer.copyFrom (1, pos, processBuffer, outChannels > 1 ? 1 : 0, 0, m);
                }

                haveWet = true;
            }
        }

        // Click-free bypass / plugin swaps: crossfade between the clean and processed signal.
        wetGain.setTargetValue (haveWet && ! bypassed.load() ? 1.0f : 0.0f);

        const float* dryL = dryBuffer.getReadPointer (0);
        const float* dryR = dryBuffer.getReadPointer (1);
        const float* wetL = haveWet ? wetBuffer.getReadPointer (0) : dryL;
        const float* wetR = haveWet ? wetBuffer.getReadPointer (1) : dryR;

        float* outL = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
        float* outR = numOutputChannels > 1 ? outputChannelData[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float g = wetGain.getNextValue();
            const float l = dryL[i] + g * (wetL[i] - dryL[i]);
            const float r = dryR[i] + g * (wetR[i] - dryR[i]);
            peak = juce::jmax (peak, std::abs (l), std::abs (r));

            if (outR != nullptr)
            {
                outL[offset + i] = l;
                outR[offset + i] = r;
            }
            else if (outL != nullptr)
            {
                outL[offset + i] = 0.5f * (l + r);
            }
        }
    }

    for (int ch = 2; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (peak > 1.0e-4f)   // above -80 dBFS counts as playing
        lastAudibleTime.store (now, std::memory_order_relaxed);
}

double HostEngine::getSecondsSinceAudible() const noexcept
{
    return DriftFifo::now() - lastAudibleTime.load (std::memory_order_relaxed);
}

//==============================================================================
void HostEngine::setPlugin (std::unique_ptr<juce::AudioPluginInstance> newPlugin)
{
    if (onPluginAboutToChange)
        onPluginAboutToChange();

    juce::AudioBuffer<float> newProcessBuffer;
    double rate;
    int block;

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        rate = currentRate;
        block = pluginBlock;
    }

    const bool deviceActive = running && rate > 0.0;

    if (newPlugin != nullptr)
    {
        configurePluginLayout (*newPlugin);

        if (deviceActive)
        {
            newPlugin->setRateAndBufferSizeDetails (rate, block);
            newPlugin->prepareToPlay (rate, block);
        }

        newProcessBuffer.setSize (juce::jmax (2, newPlugin->getTotalNumInputChannels(), newPlugin->getTotalNumOutputChannels()),
                                  block, false, true, false);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    bool oldWasPrepared;

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        oldWasPrepared = pluginPrepared;
        old = std::move (plugin);
        plugin = std::move (newPlugin);
        std::swap (processBuffer, newProcessBuffer);
        pluginPrepared = plugin != nullptr && deviceActive;
    }

    if (old != nullptr)
    {
        if (oldWasPrepared)
            old->releaseResources();

        old.reset();
    }

    notify();
}

juce::StringArray HostEngine::findPluginIdentifiers()
{
    juce::StringArray result;

    for (auto* format : formatManager.getFormats())
    {
        const auto ids = format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, true);

        for (const auto& id : ids)
            result.addIfNotAlreadyThere (id);
    }

    return result;
}

juce::String HostEngine::getPluginDisplayName (const juce::String& fileOrIdentifier)
{
    for (auto* format : formatManager.getFormats())
    {
        if (format->fileMightContainThisPluginType (fileOrIdentifier))
        {
            const auto name = format->getNameOfPluginFromIdentifier (fileOrIdentifier);
            if (name.isNotEmpty())
                return name + "  (" + format->getName() + ")";
        }
    }

    return juce::File::createFileWithoutCheckingPath (fileOrIdentifier).getFileNameWithoutExtension();
}

void HostEngine::loadPlugin (const juce::String& fileOrIdentifier, const juce::MemoryBlock& state, LoadCallback done)
{
    juce::OwnedArray<juce::PluginDescription> found;

    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (fileOrIdentifier))
            format->findAllTypesForFile (found, fileOrIdentifier);

    if (found.isEmpty())
    {
        if (done) done ("No plugin could be loaded from \"" + fileOrIdentifier + "\".");
        return;
    }

    // Prefer an effect over an instrument, and anything that looks like ARC.
    const juce::PluginDescription* best = found.getFirst();

    for (auto* d : found)
    {
        if (! d->isInstrument && d->name.containsIgnoreCase ("ARC")) { best = d; break; }
        if (! d->isInstrument && best->isInstrument)                   best = d;
    }

    loadPlugin (*best, state, std::move (done));
}

void HostEngine::loadPlugin (const juce::PluginDescription& description, const juce::MemoryBlock& state, LoadCallback done)
{
    double rate;
    int block;

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        rate  = currentRate > 0.0 ? currentRate : 48000.0;
        block = pluginBlock;
    }

    formatManager.createPluginInstanceAsync (description, rate, block,
        [this, description, state, done = std::move (done), alive = std::weak_ptr<bool> (aliveToken)]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            auto a = alive.lock();
            if (a == nullptr || ! *a)
                return;

            if (instance == nullptr)
            {
                if (done) done (error.isNotEmpty() ? error : juce::String ("The plugin couldn't be created."));
                return;
            }

            if (state.getSize() > 0)
                instance->setStateInformation (state.getData(), (int) state.getSize());

            pluginDescription = description;
            setPlugin (std::move (instance));

            if (done) done ({});
        });
}

void HostEngine::unloadPlugin()
{
    pluginDescription = {};
    setPlugin (nullptr);
}

//==============================================================================
HostEngine::Status HostEngine::getStatus() const
{
    Status s;
    s.running = running;
    s.wantRunning = wantRunning;
    s.error = lastError;
    s.bypassed = bypassed.load();
    s.automaticRestarts = automaticRestarts;

    if (activeSource != nullptr)
    {
        s.sourceName     = activeSource->getCurrentSourceName();
        s.sourceRate     = activeSource->getSampleRate();
        s.sourceChannels = activeSource->getNumChannels();
        s.sourceBlock    = activeSource->getTypicalBlockSize();
    }

    if (auto* device = outputDevice.get())
    {
        s.outputName     = device->getName();
        s.outputRate     = device->getCurrentSampleRate();
        s.outputChannels = device->getOutputChannelNames().size();
        s.outputBlock    = device->getCurrentBufferSizeSamples();
    }

    s.fifo = fifo.getStats();

    if (plugin != nullptr)
    {
        s.pluginName = plugin->getName();
        s.pluginLatencySamples = plugin->getLatencySamples();
    }

    const double sincLatency = 100.0;   // WindowedSincInterpolator's algorithmic latency (input frames)

    if (s.running && s.sourceRate > 0 && s.outputRate > 0)
    {
        s.addedLatencyMs = 1000.0 * ((s.fifo.targetFrames + sincLatency + 0.5 * s.sourceBlock) / s.sourceRate
                                     + (s.outputBlock + s.pluginLatencySamples) / s.outputRate);
    }

    s.input.enabled = config.inputEnabled;
    s.input.error   = inputError;

    if (directInputActive.load())
    {
        if (auto* device = outputDevice.get())
        {
            s.input.direct   = true;
            s.input.running  = device->isPlaying();
            s.input.name     = device->getName();
            s.input.rate     = device->getCurrentSampleRate();
            s.input.channels = device->getInputChannelNames().size();
            s.input.block    = device->getCurrentBufferSizeSamples();

            if (s.input.rate > 0)
                s.input.latencyMs = 1000.0 * (device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()
                                              + s.pluginLatencySamples) / s.input.rate;
        }
    }
    else if (inputActive)
    {
        s.input.running  = inputSource->isRunning();
        s.input.name     = inputSource->getCurrentSourceName();
        s.input.rate     = inputSource->getSampleRate();
        s.input.channels = inputSource->getNumChannels();
        s.input.block    = inputSource->getTypicalBlockSize();
        s.input.fifo     = inputFifo.getStats();

        if (s.input.rate > 0 && s.outputRate > 0)
            s.input.latencyMs = 1000.0 * ((s.input.fifo.targetFrames + sincLatency + s.input.block) / s.input.rate
                                          + (s.outputBlock + s.pluginLatencySamples) / s.outputRate);
    }

    return s;
}

void HostEngine::notify()
{
    if (onStateChanged)
        onStateChanged();
}
