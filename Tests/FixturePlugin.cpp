// Test plug-ins for the scanning tests. One source, three builds:
//   LATHE_FIXTURE_GAIN   passes audio through (must be found and load)
//   LATHE_FIXTURE_CRASH  crashes as soon as the host creates it
//   LATHE_FIXTURE_HANG   never finishes being created
//   LATHE_FIXTURE_FLAKY  for "Run separately": halves the audio (Gain), keeps its settings in
//                        its state, and crashes in processBlock once "Crash" is switched on

#include <juce_audio_processors/juce_audio_processors.h>

class FixtureProcessor : public juce::AudioProcessor
{
public:
    FixtureProcessor()
        : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo())
                                           .withOutput ("Out", juce::AudioChannelSet::stereo())) {}

    const juce::String getName() const override             { return JucePlugin_Name; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}   // audio passes through
    double getTailLengthSeconds() const override            { return 0.0; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    juce::AudioProcessorEditor* createEditor() override     { return nullptr; }
    bool hasEditor() const override                         { return false; }
    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

#if LATHE_FIXTURE_FLAKY
class FlakyProcessor : public FixtureProcessor
{
public:
    FlakyProcessor()
    {
        addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 0.5f));
        addParameter (crash = new juce::AudioParameterBool (juce::ParameterID { "crash", 1 }, "Crash", false));
    }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        if (crash->get())
        {
            volatile int* nowhere = nullptr;
            *nowhere = 1;
        }
        b.applyGain (gain->get());
    }
    void getStateInformation (juce::MemoryBlock& out) override
    {
        juce::MemoryOutputStream (out, false).writeFloat (gain->get());
    }
    void setStateInformation (const void* data, int size) override
    {
        if (size >= 4)
            *gain = juce::MemoryInputStream (data, (size_t) size, false).readFloat();
    }
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterBool* crash = nullptr;
};
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
   #if LATHE_FIXTURE_CRASH
    volatile int* nowhere = nullptr;
    *nowhere = 1;
   #elif LATHE_FIXTURE_HANG
    for (;;)
        juce::Thread::sleep (1000);
   #endif
   #if LATHE_FIXTURE_FLAKY
    return new FlakyProcessor();
   #else
    return new FixtureProcessor();
   #endif
}
