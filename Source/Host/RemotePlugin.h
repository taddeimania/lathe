#pragma once

#include "../Common.h"

namespace lathe
{
//==============================================================================
/** A plug-in "run separately": it lives in its own process (LatheHost, next to Lathe), so if
    it crashes, only that process goes down. Lathe keeps playing; the plug-in goes quiet (an
    effect lets the sound through untouched) until you restart it.

    To Lathe this is an ordinary AudioPluginInstance. Each block's audio and MIDI go through
    shared memory: processBlock() writes them, wakes the host with a semaphore and waits for
    the result, so it costs about what the plug-in itself costs plus a few microseconds. If
    the host doesn't answer in time (it crashed or hung), the block is let through or silent
    and the plug-in counts as gone. Everything else (loading, settings, its window) is sent
    over a connection to the host on the message thread.

    The plug-in's window opens in the host process, as a window of its own. */
class RemotePluginInstance : public AudioPluginInstance
{
public:
    /** Starts a host and loads the plug-in in it. Null (and `error`) if that fails, or if
        LatheHost isn't next to Lathe. Message thread. */
    static std::unique_ptr<RemotePluginInstance> create (const PluginDescription&, double sampleRate, int blockSize,
                                                         const MemoryBlock& state, String& error);
    /** LatheHost: next to the running program (Lathe, or the tests). */
    static File hostExecutable();
    ~RemotePluginInstance() override;

    /** The host process went away (the plug-in crashed or hung). */
    bool hasStopped() const noexcept   { return stopped.load(); }
    String whyStopped() const;
    /** Starts a new host with the settings the plug-in had last. Message thread. */
    bool restart (String& error);
    /** Told (on the message thread) when the host goes away. */
    std::function<void()> onStopped;

    /** Opens or closes the plug-in's own window (in the host). */
    void showWindow();
    void hideWindow();

    // --- AudioPluginInstance
    void fillInPluginDescription (PluginDescription&) const override;
    const String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void reset() override;
    void setNonRealtime (bool) noexcept override;
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const String&) override {}
    void getStateInformation (MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    class Connection;
    class Parameter;

private:
    RemotePluginInstance (const BusesProperties&, std::unique_ptr<Connection>, const var& loaded, const PluginDescription&);

    std::unique_ptr<Connection> connection;
    PluginDescription description;
    var info;   // what the host said when it loaded the plug-in
    MemoryBlock lastState;
    CriticalSection stateLock;
    std::atomic<bool> stopped { false };
    std::atomic<bool> offline { false };
    uint32_t lastSeq = 0;      // audio thread
    double rate = 48000.0;
    int block = 512;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    void adopt (std::unique_ptr<Connection>, const var& loaded);
    static BusesProperties busesFor (const var& loaded);
    void connectionLost();
    void letThrough (AudioBuffer<float>&, int start, int n, MidiBuffer&);
    friend class Connection;
};
} // namespace lathe
