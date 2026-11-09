#pragma once

#include "../Model/Edit.h"
#include "AudioPool.h"
#include "PluginHost.h"

namespace lathe
{
struct RenderGraph;
struct RenderTrack;
struct RecordSession;
struct RecordTake;
struct MidiTake;

/** Tempo and position for plug-ins (tempo-synced delays, arpeggiators, ...). */
struct HostPlayHead : public AudioPlayHead
{
    Optional<PositionInfo> getPosition() const override;

    double sampleRate = 48000.0, bpm = 120.0, samplesPerBeat = 24000.0;
    int num = 4, den = 4;
    int64 position = 0;
    bool playing = false, recording = false, looping = false;
    double loopStartBeats = 0.0, loopEndBeats = 0.0;
};

//==============================================================================
class Engine : public AudioIODeviceCallback,
               private MidiInputCallback,
               private AsyncUpdater,
               private Timer
{
public:
    Engine (PluginHost&, AudioPool&, PropertiesFile&);
    ~Engine() override;

    AudioDeviceManager deviceManager;

    /** Opens the saved audio device, or looks for a UMC interface on first run. */
    void openDevice();
    void saveDeviceSettings();

    void setEdit (Edit*);
    Edit* getEdit() const noexcept   { return edit; }

    /** Rebuilds the render snapshot. Call (on the message thread) after any change to the edit. */
    void rebuild();

    // --- transport (message thread)
    void play();
    void stop();
    void togglePlay()                  { if (isPlaying()) stop(); else play(); }
    void toggleRecord();
    bool isPlaying() const noexcept    { return playing.load(); }
    bool isRecording() const noexcept  { return recording.load(); }
    double getPlayheadBeats() const;
    void seekTo (double beats);
    std::atomic<bool> metronomeOn { false };
    /** Bars of click before recording starts (when you press record while stopped). 0 = off. */
    std::atomic<int> countInBars { 0 };
    /** Clicking before a recording: the playhead waits at the start marker. */
    bool isCountingIn() const noexcept   { return countInLeft.load() > 0; }
    /** Beats left in the count-in (rounded up), for the display. */
    int getCountInBeatsLeft() const;
    /** Where the current (or last) recording started, in beats. */
    std::atomic<double> recordStartBeats { 0.0 };

    // --- device info
    double getSampleRate() const noexcept   { return deviceRate.load(); }
    int getBlockSize() const noexcept       { return deviceBlock.load(); }
    bool isDeviceOpen() const;
    StringArray getInputChannelNames() const;
    StringArray getOutputChannelNames() const;
    StringArray getMidiInputNames() const   { return midiInputNames; }
    StringArray getMidiOutputNames() const;
    double getCpuUsage() const              { return deviceManager.getCpuUsage(); }
    int getRecordOffsetSamples() const;
    int getGraphLatency() const noexcept    { return graphLatency; }
    String describeDevice() const;

    // --- plug-ins
    /** Loads a plug-in (or a placeholder that keeps its state if it isn't installed). */
    std::shared_ptr<PluginSlot> makeSlot (const PluginDescription&, const MemoryBlock& state, String& error);
    void prepareSlot (PluginSlot&);

    // --- export
    struct ExportSettings
    {
        bool mix = true, stems = true;
        double startBeat = 0.0, endBeat = 0.0;
        double tailSeconds = 2.0;
        int bitDepth = 24;               // 16, 24 or 32 (float)
        File folder;
        String baseName;
        /** Empty: everything, as you hear it. Otherwise the ids of the tracks to export:
            only those audio/MIDI tracks play (even if muted or not soloed), and only the
            tracks and returns/buses listed here get stems. Returns and buses not listed
            still carry what the listed tracks send them into the mix. If only returns or
            buses are listed, every track plays and just their stems are limited. */
        std::set<int> trackIds;
    };
    /** Renders offline. Runs on a background thread; progress returns false to cancel. */
    Result exportAudio (const ExportSettings&, const std::function<bool (double)>& progress, StringArray& writtenFiles);
    bool isExporting() const noexcept   { return exporting.load(); }
    /** The mix file name for an export of some tracks: "Song - Drums + Bass", "Song (5 tracks)". */
    static String exportMixName (const String& baseName, const Edit&, const std::set<int>& trackIds);

    // --- notifications (message thread)
    std::function<void (const String&)> onStatus;
    std::function<void()> onDeviceChanged;
    std::function<void()> onRecordingFinished;

    // --- AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (AudioIODevice*) override;
    void audioDeviceStopped() override;

    static constexpr int kMaxChannels = 64;

private:
    void handleIncomingMidiMessage (MidiInput*, const MidiMessage&) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    std::unique_ptr<RenderGraph> buildGraph();
    void installGraph (std::unique_ptr<RenderGraph>);
    void refreshMidiDevices();
    MidiOutput* getMidiOutput (const String& name);
    void startRecording();
    void finishRecording();
    Array<int> latencySignature() const;
    void status (const String&);

    // rendering (audio thread, or the export thread while the device is detached)
    void renderChunk (RenderGraph&, int64 pos, int len, int offset, bool rolling,
                      const float* const* inDev, int numIn, float* const* outDev, int numOut,
                      bool realtime, bool notesOff);
    void processTrack (RenderGraph&, RenderTrack&, int64 pos, int len, int offset, bool rolling,
                       const float* const* inDev, int numIn, float* const* outDev, int numOut,
                       bool realtime, bool notesOff, bool anySolo);
    void processBus (RenderGraph&, RenderTrack&, int len, int offset, float* const* outDev, int numOut, bool anySolo, bool realtime);
    void applyFader (RenderTrack&, int len, bool silenced);
    void renderClips (RenderGraph&, RenderTrack&, int64 pos, int len);
    void renderMetronome (RenderGraph&, int64 pos, int len);   // into RenderGraph::click
    static void addToOutput (const AudioBuffer<float>&, int len, int offset, int channel, bool stereo, float* const* outDev, int numOut);

    PluginHost& host;
    AudioPool& pool;
    PropertiesFile& props;
    Edit* edit = nullptr;

    CriticalSection graphLock;                 // held by the audio callback while it renders
    std::unique_ptr<RenderGraph> graph;
    std::unique_ptr<RecordSession> recordSession;   // guarded by graphLock

    // live MIDI input: one collector per device
    SpinLock midiLock;
    std::vector<std::unique_ptr<MidiMessageCollector>> collectors;   // guarded by graphLock + midiLock
    std::vector<MidiBuffer> liveMidi;                                // audio thread
    StringArray midiInputIds, midiInputNames;
    std::map<String, std::unique_ptr<MidiOutput>> midiOutputs;
    MidiDeviceListConnection midiDeviceConnection;

    std::atomic<bool> playing { false }, recording { false }, exporting { false };
    std::set<int> exportOnly;   // during an export of some tracks: the ids that play (export thread only)
    std::atomic<bool> notesOffRequest { false };
    std::atomic<int64> positionSamples { 0 }, seekRequest { -1 };
    std::atomic<double> deviceRate { 0.0 };
    std::atomic<int> deviceBlock { 0 };
    bool rebuildAfterExport = false;

    // device channel maps: device channel index -> callback array index (or -1)
    int inputMap[kMaxChannels] {}, outputMap[kMaxChannels] {};
    int numDeviceInputs = 0, numDeviceOutputs = 0;
    int inputLatency = 0, outputLatency = 0;

    int graphLatency = 0;
    Array<int> lastLatencySignature;
    TimeSliceThread diskThread { "Lathe disk writer" };
    HostPlayHead playHead;
    int reopenCountdown = 0;

    // count-in: samples of click left before recording starts (set on the message thread,
    // counted down and finished by the audio thread)
    std::atomic<int64> countInLeft { 0 };
    int64 countInTotal = 0;
    double countInSamplesPerClick = 0.0;
    int64 countInClickPos = -1;      // audio thread: where in the count-in this block's click is

    // metronome voice (audio thread)
    double clickPhase = 0.0, clickFreq = 1000.0;
    int clickRemaining = 0;
    float clickAmp = 0.0f;

    // zero input for missing channels
    HeapBlock<float> zeros;
    int zerosSize = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
} // namespace lathe
