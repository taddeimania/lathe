#pragma once

#include "../Common.h"

namespace lathe
{
//==============================================================================
// The project model. Everything here is owned and edited on the message thread.
// The audio engine never walks these structures directly: it reads atomics
// (fader, pan, mute, solo, arm, monitor, send levels, bypass) and works from a
// render snapshot that Engine::rebuild() builds after every structural change.
//==============================================================================

struct Note
{
    int pitch = 60;
    double start = 0.0;    // beats, relative to the clip start
    double length = 1.0;   // beats
    float velocity = 0.8f;
};

/** Non-note MIDI recorded from a controller: CC, pitch bend, aftertouch. */
struct MidiEvent
{
    double beat = 0.0;     // relative to the clip start
    uint8 data[3] {};
    int size = 0;
};

enum class ClipKind { audio, midi };

struct Clip
{
    int id = 0;
    ClipKind kind = ClipKind::audio;
    String name;
    double start = 0.0;    // beats
    double length = 4.0;   // beats

    // audio
    File file;
    int64 sourceOffset = 0;    // samples into the file at the clip's start
    float gainDb = 0.0f;

    // midi
    std::vector<Note> notes;
    std::vector<MidiEvent> events;

    double end() const noexcept { return start + length; }
};

enum class TrackKind { audio, midi, bus, master };

enum class Monitor { automatic = 0, in = 1, off = 2 };

struct Send
{
    int busId = 0;
    std::atomic<float> levelDb { kSilenceDb };
    std::atomic<bool> preFader { false };
};

struct OutputRoute
{
    enum class Kind { master, bus, hardware };
    Kind kind = Kind::master;
    int busId = -1;        // when kind == bus
    int channel = 0;       // first hardware output channel (0-based) when kind == hardware
    bool stereo = true;
};

struct AudioInput
{
    int channel = -1;      // first device input channel, 0-based; -1 = none
    bool stereo = false;
};

struct PluginSlot
{
    PluginDescription description;
    std::unique_ptr<AudioPluginInstance> instance;   // null when the plug-in is missing
    std::atomic<bool> bypassed { false };
    MemoryBlock savedState;   // kept for a missing plug-in so saving doesn't lose it
    double preparedRate = 0.0;
    int preparedBlock = 0;
    std::unique_ptr<Component> window;   // editor window; destroyed before the instance

    ~PluginSlot()   { window.reset(); instance.reset(); }

    String getName() const          { return description.name.isNotEmpty() ? description.name : String ("Plug-in"); }
    bool isMissing() const noexcept { return instance == nullptr; }
    bool isInstrument() const       { return description.isInstrument; }
};

/** A set of audio/MIDI tracks controlled together: the group's mute and solo apply
    to every member (on top of their own), and arm arms or disarms them all. */
struct TrackGroup
{
    int id = 0;
    String name;
    Colour colour;
    std::atomic<bool> mute { false }, solo { false };
    bool collapsed = false;   // arrangement view hides the member rows
};

struct Track
{
    int id = 0;
    TrackKind kind = TrackKind::audio;
    String name;
    Colour colour;

    std::atomic<float> volumeDb { 0.0f };
    std::atomic<float> pan { 0.0f };          // -1 .. 1
    std::atomic<bool> mute { false }, solo { false }, arm { false };
    std::atomic<int> monitor { (int) Monitor::automatic };

    AudioInput input;
    String midiInput { "All" };   // "All", "None" or a device name
    String midiOutput;            // hardware MIDI output device name; empty = none
    int midiChannel = 1;          // 1..16, used for clip playback and hardware out
    int groupId = -1;             // TrackGroup id, or -1

    OutputRoute output;
    std::vector<std::shared_ptr<Send>> sends;
    std::vector<std::shared_ptr<PluginSlot>> plugins;
    std::vector<Clip> clips;

    // Peak meters: the audio thread raises them, the UI reads and clears them.
    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };

    // Audio-thread-only fader smoothing state.
    float rtGainL = 0.0f, rtGainR = 0.0f;
    bool rtGainValid = false;

    bool isAudio() const noexcept   { return kind == TrackKind::audio; }
    bool isMidi() const noexcept    { return kind == TrackKind::midi; }
    bool isBus() const noexcept     { return kind == TrackKind::bus; }
    bool isMaster() const noexcept  { return kind == TrackKind::master; }
    bool holdsClips() const noexcept { return isAudio() || isMidi(); }

    std::shared_ptr<Send> getSend (int busId, bool create);
    Clip* findClip (int clipId);
    int indexOfClip (int clipId) const;
};

//==============================================================================
class Edit : public ChangeBroadcaster
{
public:
    Edit();

    /** Back to an empty, unsaved project (one Edit object lives for the whole session). */
    void reset();

    // --- song settings
    double tempo = 120.0;
    int timeSigNum = 4, timeSigDen = 4;
    bool loopEnabled = false;
    double loopStart = 0.0, loopEnd = 16.0;   // beats
    double insertMarker = 0.0;                // where Play starts from, beats
    double sampleRate = 48000.0;              // rate the project's audio was recorded at

    std::vector<std::shared_ptr<Track>> tracks;   // audio + MIDI, in display order
    std::vector<std::shared_ptr<Track>> buses;    // return tracks
    std::shared_ptr<Track> master;
    std::vector<std::shared_ptr<TrackGroup>> groups;

    // --- where the project lives
    File projectFile;   // empty until saved
    File workDir;       // holds Audio/ and Exports/
    File getAudioDir() const    { return workDir.getChildFile ("Audio"); }
    File getExportsDir() const  { return workDir.getChildFile ("Exports"); }
    String getName() const      { return projectFile.existsAsFile() ? projectFile.getFileNameWithoutExtension() : String ("Untitled"); }
    void ensureWorkDir();

    // --- structure
    std::shared_ptr<Track> addTrack (TrackKind kind, const String& name = {});
    void removeTrack (int trackId);
    void moveTrack (int trackId, int newIndex);

    // --- groups
    std::shared_ptr<TrackGroup> findGroup (int groupId) const;
    std::vector<std::shared_ptr<Track>> groupMembers (int groupId) const;
    /** Groups the given audio/MIDI tracks (taking them out of any other group). */
    std::shared_ptr<TrackGroup> createGroup (const std::vector<int>& trackIds, const String& name = {});
    void setTrackGroup (int trackId, int groupId);   // -1 removes it from its group
    void ungroup (int groupId);                      // keeps the tracks
    /** Arms every member, or disarms them all if they're all armed already. */
    void toggleGroupArm (int groupId);
    bool isGroupArmed (int groupId) const;
    /** Effective states, combining a track's own with its group's. */
    bool isEffectivelyMuted (const Track&) const;
    bool isEffectivelySoloed (const Track&) const;
    std::shared_ptr<Track> findTrack (int trackId) const;
    int newId()     { return ++lastId; }

    // --- time
    double beatsPerBar() const noexcept        { return timeSigNum * 4.0 / timeSigDen; }
    double samplesPerBeat (double sr) const    { return sr * 60.0 / tempo; }
    double getEndBeat() const;   // end of the last clip
    void setTempo (double newTempo);   // keeps audio clips the same length in seconds

    // --- clip editing helpers (all positions in beats)
    /** Removes everything on the track between from and to, trimming and splitting clips that overlap. */
    void clearRange (Track&, double from, double to, int exceptClipId = -1);
    /** Splits a clip in two. Returns the new right-hand clip's id, or -1. */
    int splitClip (Track&, int clipId, double atBeat);
    void trimClipStart (Clip&, double newStart);

    // --- change notification
    /** Call after a change the engine must see (tracks, clips, routing, plug-ins).
        Marks the project dirty; the UI and engine are told asynchronously. */
    void changed()  { dirty = true; needsRebuild = true; sendChangeMessage(); }
    /** For changes the engine already reads live (mute, solo, arm, monitor): UI refresh only. */
    void touched()  { dirty = true; sendChangeMessage(); }
    /** For continuous controls (faders, knobs): no notification at all. */
    void markDirty() { dirty = true; }

    bool dirty = false;
    bool needsRebuild = false;

    // --- undo for clip and note edits (track and plug-in changes are not undoable)
    void beginClipEdit();
    bool undo();
    bool redo();
    bool canUndo() const   { return ! undoStack.empty(); }
    bool canRedo() const   { return ! redoStack.empty(); }

    // --- files
    using PluginFactory = std::function<std::shared_ptr<PluginSlot> (const PluginDescription&, const MemoryBlock& state)>;

    /** Serialises the project. Templates leave out clips and the playhead. */
    var toVar (bool asTemplate) const;
    /** Loads from a parsed project file. baseDir resolves relative audio paths. */
    Result loadFrom (const var& data, const File& baseDir, const PluginFactory& makePlugin);

    static String trackKindToString (TrackKind);
    static TrackKind trackKindFromString (const String&);

private:
    int lastId = 0;
    int colourIndex = 0;
    std::vector<var> undoStack, redoStack;

    var clipsToVar() const;
    void clipsFromVar (const var&);
    var trackToVar (const Track&, bool asTemplate) const;
    void removeEmptyGroups();
    std::shared_ptr<Track> trackFromVar (const var&, const File& baseDir, const PluginFactory&);
    var clipToVar (const Clip&) const;
    Clip clipFromVar (const var&, const File& baseDir) const;
    Colour nextColour();

    JUCE_DECLARE_NON_COPYABLE (Edit)
};

/** The fixed palette for track colours. Muted on purpose: they sit on dark grey. */
const Array<Colour>& getTrackPalette();

} // namespace lathe
