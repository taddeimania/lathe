#pragma once

#include "../Model/Edit.h"
#include "AudioPool.h"
#include "PluginHost.h"

namespace lathe
{
class RenderPool;
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

    /** Opens the saved audio device, or on first run the interface with the most inputs. */
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

    /** Lets playback stop by itself, exactly on the sample (message thread; call after play()):
        when the playhead reaches `stopAtBeats` (negative: no stop point), or at the loop end
        once the loop has played `loopPasses` times (0: loop as long as it plays; counts the
        first pass from wherever playback is). Stop, record and any seek cancel it. When it
        stops, the playhead goes back to the insert marker, as Stop does. */
    void setAutoStop (double stopAtBeats, int loopPasses);
    void clearAutoStop() noexcept           { stopAtSamples = -1; loopPassesLeft = 0; }
    /** Where playback will stop by itself, in beats (negative: it won't). */
    double getAutoStopBeats() const;
    /** Loop passes left (this one included) before playback stops at the loop end; 0 = no limit. */
    int getLoopPassesLeft() const noexcept  { return loopPassesLeft.load(); }

    // --- multi-core (docs/multicore.md)
    /** How many threads render tracks: 1 = only the audio thread (the old engine), 0 = auto
        (autoProcessingThreads()), or a number. Safe to call any time; takes effect at the next block. */
    void setProcessingThreads (int threads);
    int getProcessingThreads() const noexcept   { return requestedThreads.load(); }
    /** What "auto" means on this machine: one per performance core (all cores elsewhere), at most 16. */
    static int autoProcessingThreads();
    /** For tests: share out even tiny blocks (normally only blocks with real work are). */
    void setAlwaysShareWork (bool always) noexcept   { parallelMinimumUs = always ? -1.0 : 60.0; }
    /** Threads rendering right now (1 = only the audio thread). */
    int getActiveProcessingThreads() const noexcept;

    // --- hearing notes (the piano roll)
    /** Plays a note on a MIDI track's instrument right away, whether or not the track is
        armed or the transport is running, so you can hear a note before drawing it.
        Velocity 0..1; 0 ends the note. Never recorded. Message thread. */
    void previewNote (int trackId, int note, float velocity);

    // --- playing something other than the song (the Compare window)
    /** Plays instead of the song while it says it's playing: what the master would send to the
        interface is replaced by what this renders. Audio thread: render() must not block or
        allocate; it fills two channels of `out` (len <= kPreviewBlock) and returns false when
        it isn't playing, which leaves the song's output alone. */
    struct OutsidePlayer
    {
        virtual ~OutsidePlayer() = default;
        virtual bool isPlaying() const noexcept = 0;
        virtual void render (AudioBuffer<float>& out, int len) noexcept = 0;
    };
    static constexpr int kPreviewBlock = 4096;
    /** Installs (or, with nullptr, removes) the player. Message thread; once it returns, the
        audio thread no longer uses the old one. */
    void setOutsidePlayer (std::shared_ptr<OutsidePlayer>);

    // --- measuring (the AI assistant's measure_performance tool)
    /** Starts timing every audio block and every track, after skipping the first `skipBlocks`
        (a warm-up). Message thread. */
    void beginMeasurement (int skipBlocks);
    struct Measured
    {
        std::vector<float> blockUs;   // time each measured block took, in microseconds
        int blockSize = 0;
        double sampleRate = 0.0;
        int threads = 1;
        int64 trackBlocks = 0;        // blocks the per-track figures cover
    };
    /** Stops timing and hands over what was measured. Per-track times are in each Track's
        measuredUs (summed over Measured::trackBlocks blocks). Message thread. */
    Measured endMeasurement();
    bool isMeasuring() const noexcept   { return measuring.load(); }

    // --- device info
    double getSampleRate() const noexcept   { return deviceRate.load(); }
    int getBlockSize() const noexcept       { return deviceBlock.load(); }
    bool isDeviceOpen() const;
    /** Switches the interface to this sample rate if it can do it. True if it's running at it now. */
    bool setDeviceRate (double rate);
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
    /** `separate`: run it in its own process (LatheHost), so its crash can't take Lathe down.
        Lathe's own devices always run inside. A plug-in that won't start separately is loaded
        the usual way, with `error` saying so. */
    std::shared_ptr<PluginSlot> makeSlot (const PluginDescription&, const MemoryBlock& state, String& error, bool separate = false);

    /** Crash-loop guard. While a plug-in loads inside Lathe, its id is written to this file and
        removed once it's loaded. If Lathe crashes during the load (a plug-in that can't take
        settings saved on another computer, say), the file is still there next time:
        takeCrashedLoad() returns the plug-in, and the caller adds it to `runSeparately`, so the
        song opens instead of crashing again. Empty file: no guard (tests, benchmarks). */
    File loadGuardFile;
    /** Plug-ins (identifier strings) that always run separately, whatever the song says. */
    std::set<String> runSeparately;
    /** The plug-in that was loading when Lathe last crashed, if any (and clears the guard). */
    String takeCrashedLoad();
    /** Turns "Run separately" on or off: a new slot with the same settings, the other way. */
    std::shared_ptr<PluginSlot> remakeSlot (PluginSlot& slot, bool separate, String& error);
    void prepareSlot (PluginSlot&);
    PluginHost& getPluginHost() noexcept   { return host; }

    // --- export
    struct ExportSettings
    {
        bool mix = true, stems = true;
        double startBeat = 0.0, endBeat = 0.0;
        double tailSeconds = 2.0;
        /** wav: 16, 24 or 32-bit float. aiff and flac: 16 or 24 (32 is written as 24).
            mp3 and ogg are lossy: `kbps` sets the size (mp3 constant, ogg about that on average). */
        enum class Format { wav, aiff, flac, mp3, ogg };
        Format format = Format::wav;
        int bitDepth = 24;               // 16, 24 or 32 (float)
        int kbps = 320;                  // mp3: 320, 256, 192 or 128. ogg: 128 to 320
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

    /** One signal an offline render hands out: the mix, or one track's, return's or bus's own (its stem). */
    struct RenderTap
    {
        String name;          // "Mix", or the track's name
        int trackId = -1;     // -1 for the mix
        bool isMix = false;
        bool isReturn = false;
    };
    /** Receives a stretch of one tap's audio: the tap's index, the buffer, where in it, how many
        samples, and where that is on the timeline (samples from the start of the render). */
    using RenderListener = std::function<void (int tap, const AudioBuffer<float>&, int start, int count, int64 timelineSample)>;
    /** Renders offline exactly as exportAudio does (s.mix and s.stems say which taps; folder,
        baseName and bitDepth aren't used), handing every tap to `listen` instead of writing files.
        `prepare` gets the taps before the render starts and can refuse it. Background thread. */
    Result renderOffline (const ExportSettings&, const std::function<bool (double)>& progress,
                          const std::function<Result (const std::vector<RenderTap>&, int64 lengthSamples)>& prepare,
                          const RenderListener& listen);
    bool isExporting() const noexcept   { return exporting.load(); }
    /** "wav", "aiff", "flac", "mp3", "ogg": the file extension (no dot), and the name the
        assistant and settings use. */
    static String formatName (ExportSettings::Format);
    /** The other way; false if `name` isn't one (case doesn't matter, "aif" means aiff). */
    static bool parseFormat (const String& name, ExportSettings::Format& out);
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

    std::shared_ptr<OutsidePlayer> outsideOwner;          // message thread
    std::atomic<OutsidePlayer*> outsideLive { nullptr };  // what the audio thread reads
    AudioBuffer<float> outsideScratch { 2, kPreviewBlock };

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
    void renderTracks (RenderGraph&, int64 pos, int len, int offset, bool rolling,
                       const float* const* inDev, int numIn, bool realtime, bool notesOff);
    void renderTrack (RenderGraph&, RenderTrack&, int64 pos, int len, int offset, bool rolling,
                      const float* const* inDev, int numIn, bool realtime, bool notesOff);
    void mixTrack (RenderGraph&, RenderTrack&, int len, int offset, float* const* outDev, int numOut, bool realtime, bool anySolo);
    void renderBuses (RenderGraph&, int len);
    void renderTrackTimed (RenderGraph&, RenderTrack&, int64 pos, int len, int offset, bool rolling,
                           const float* const* inDev, int numIn, bool realtime, bool notesOff);
    void renderBusTimed (RenderGraph&, RenderTrack&, int len);
    bool worthSharing (const std::vector<RenderTrack>&) const noexcept;
    void updatePool();
    void renderBus (RenderGraph&, RenderTrack&, int len);
    void mixBus (RenderGraph&, RenderTrack&, int len, int offset, float* const* outDev, int numOut, bool anySolo, bool realtime);
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
    std::atomic<int> requestedThreads { 0 };
    // multi-core: the worker pool. Swapped only under graphLock, and never while exporting
    // (the export thread uses it without the lock); poolDirty asks for a swap later.
    std::unique_ptr<RenderPool> renderPool;
    int poolWorkers = 0, poolBlock = 0;
    double poolRate = 0.0;
    AudioWorkgroup poolWorkgroup;
    bool poolWorkgroupChanged = false;
    std::atomic<bool> poolDirty { false };
    std::atomic<double> parallelMinimumUs { 60.0 };
    // hearing notes: the message thread queues them, the audio thread takes them at the start
    // of each block, and the tracks (on any render thread) read that block's list
    struct PreviewEvent { int trackId = -1; uint8 note = 0, velocity = 0; };
    AbstractFifo previewFifo { 256 };
    std::array<PreviewEvent, 256> previewQueue;
    std::array<PreviewEvent, 64> previewBlock;
    int previewCount = 0;

    // measuring: the message thread sets it up and reads it; the audio thread records
    std::atomic<bool> measuring { false };
    std::atomic<int> measureSkip { 0 }, measureCount { 0 };
    std::atomic<int64> measureTrackBlocks { 0 };
    std::vector<float> measureBlocks;   // sized before measuring starts, written by the audio thread
    std::atomic<bool> measureThisBlock { false }, measureWriting { false };   // the audio thread is timing this block (read by workers)
    // auto-stop (setAutoStop): set on the message thread, reached and cleared by the audio thread
    std::atomic<int64> stopAtSamples { -1 };
    std::atomic<int> loopPassesLeft { 0 };
    std::atomic<bool> autoStopped { false };   // the audio thread stopped; the timer moves the playhead back
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
