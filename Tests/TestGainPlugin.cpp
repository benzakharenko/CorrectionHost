// A minimal VST3 used only by EngineTest: multiplies by a 'gain' parameter whose value
// is saved and restored through the plugin state, like ARC X's profile would be.

#include <juce_audio_processors/juce_audio_processors.h>

class TestGainProcessor final : public juce::AudioProcessor
{
public:
    TestGainProcessor()
        : AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override         { return "TestGain"; }
    void prepareToPlay (double, int) override             {}
    void releaseResources() override                      {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (gain->get());
    }

    juce::AudioProcessorEditor* createEditor() override   { return nullptr; }
    bool hasEditor() const override                       { return false; }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 0.0; }
    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream (dest, false).writeFloat (gain->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= 4)
            *gain = juce::MemoryInputStream (data, (size_t) size, false).readFloat();
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestGainProcessor();
}
