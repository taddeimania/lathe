// Test plug-ins for the scanning tests. One source, three builds:
//   LATHE_FIXTURE_GAIN   passes audio through (must be found and load)
//   LATHE_FIXTURE_CRASH  crashes as soon as the host creates it
//   LATHE_FIXTURE_HANG   never finishes being created

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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
   #if LATHE_FIXTURE_CRASH
    volatile int* nowhere = nullptr;
    *nowhere = 1;
   #elif LATHE_FIXTURE_HANG
    for (;;)
        juce::Thread::sleep (1000);
   #endif
    return new FixtureProcessor();
}
