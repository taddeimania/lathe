#pragma once

#include "../Common.h"
#include "ChannelEq.h"
#include "Notes.h"

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
    int track = -1;        // or another track's id. Audio tracks: take what it plays (after its
                           // plug-ins and EQ, before its fader), always stereo. MIDI tracks: take
                           // the notes it plays or its plug-ins generate. Wins over the device input.
    bool isTrack() const noexcept   { return track >= 0; }
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
    int busId = -1;           // the group's bus (a return the members play through), if it has one
};

/** One track's (or return's) level in a cue mix. */
struct CueLevel
{
    std::atomic<float> levelDb { 0.0f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };

    // Audio-thread smoothing state.
    float rtGainL = 0.0f, rtGainR = 0.0f;
    bool rtGainValid = false;
};

/** A monitor mix ("cue mix") on its own pair of hardware outputs, e.g. in-ear
    monitors. Every track and return has its own level, pan and mute in it,
    independent of the main mix. It hears the channels after their plug-ins and
    EQ and before their main faders, so main fader, pan, mute and solo don't
    affect it. Returns come in after their plug-ins, fed by the main sends. */
struct CueMix
{
    int id = 0;
    String name;
    Colour colour;
    std::atomic<int> channel { 2 };         // first of the stereo output pair, 0-based
    std::atomic<float> volumeDb { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<float> clickDb { 0.0f };    // metronome level in this mix (when the metronome is on)
    std::map<int, std::shared_ptr<CueLevel>> levels;   // by track / return id

    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };
    float rtGain = 0.0f;
    bool rtGainValid = false;
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
    ChannelEq eq;                 // after the plug-ins, before the fader

    // Peak meters: the audio thread raises them, the UI reads and clears them.
    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };

    // Audio-thread-only fader smoothing state.
    float rtGainL = 0.0f, rtGainR = 0.0f;
    bool rtGainValid = false;
    EqProcessor rtEq;

    // While the engine measures (Engine::beginMeasurement): time spent on this track, in µs.
    std::atomic<double> measuredUs { 0.0 };

    bool isAudio() const noexcept   { return kind == TrackKind::audio; }
    bool isMidi() const noexcept    { return kind == TrackKind::midi; }
    bool isBus() const noexcept     { return kind == TrackKind::bus; }
    bool isMaster() const noexcept  { return kind == TrackKind::master; }
    bool holdsClips() const noexcept { return isAudio() || isMidi(); }

    std::shared_ptr<Send> getSend (int busId, bool create);
    Clip* findClip (int clipId);
    int indexOfClip (int clipId) const;
};

struct EditSnapshot;

//==============================================================================
class Edit : public ChangeBroadcaster,
             private Timer
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
    std::vector<std::shared_ptr<CueMix>> cues;      // monitor mixes
    std::atomic<bool> clickInMain { true };         // metronome in the main outputs

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
    void ungroup (int groupId);                      // keeps the tracks (and any group bus, as a plain return)

    /** Gives a group its own bus: a return named "<group> Bus" that every member plays
        through, so plug-ins, EQ and a fader on it apply to the whole group. Tracks that
        join the group later go through it too. Returns the bus (the existing one if any). */
    std::shared_ptr<Track> makeGroupBus (int groupId);
    /** Removes the group's bus; its members go straight to the master again. */
    void removeGroupBus (int groupId);
    std::shared_ptr<Track> findGroupBus (int groupId) const;
    /** Renames a group, and its bus if the bus still has the group's name. */
    void renameGroup (int groupId, const String& name);
    /** Arms every member, or disarms them all if they're all armed already. */
    void toggleGroupArm (int groupId);
    bool isGroupArmed (int groupId) const;
    /** Effective states, combining a track's own with its group's. */
    bool isEffectivelyMuted (const Track&) const;
    bool isEffectivelySoloed (const Track&) const;
    std::shared_ptr<Track> findTrack (int trackId) const;

    // --- cue (monitor) mixes
    /** New cue mix starting as a copy of the main mix's levels and pans. */
    std::shared_ptr<CueMix> addCue (const String& name, int firstOutputChannel);
    void removeCue (int cueId);
    std::shared_ptr<CueMix> findCue (int cueId) const;
    /** A track's (or return's) level in a cue mix. Created on first use from the main fader and pan. */
    std::shared_ptr<CueLevel> cueLevel (CueMix&, const Track&);
    /** Sets every level in the cue back to the main mix's fader, pan and mute. */
    void copyMainMixToCue (CueMix&);
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
    /** Notes on a song that isn't in the cloud (cloud songs keep theirs on the server). */
    std::vector<SongNote> localNotes;

    void changed()  { dirty = true; needsRebuild = true; sendChangeMessage(); scheduleUndoStep(); }
    /** For changes the engine already reads live (mute, solo, arm, monitor): UI refresh only. */
    void touched()  { dirty = true; sendChangeMessage(); scheduleUndoStep(); }
    /** For continuous controls (faders, knobs): no notification at all. */
    void markDirty() { dirty = true; }

    bool dirty = false;
    bool needsRebuild = false;

    // --- undo
    // Covers tracks, returns, groups, cue mixes, routing, sends, plug-ins (added, removed,
    // moved, bypassed), names, colours, tempo, time signature, clips and notes. Not the
    // continuous controls (faders, pans, send levels, EQ), mute/solo/arm, the loop or the
    // playhead: those live in the shared Track objects and keep their current values.
    //
    // Steps are recorded automatically: changed() schedules one, and every change made
    // until things go quiet (a few hundred ms) becomes that one step, so an action that
    // touches several things (a group bus, a recorded take) undoes in one go.

    /** Ends the pending step now, so what follows is a step of its own. Call before an
        edit that should undo separately from what came just before it (a clip drag, an
        assistant's batch of changes). */
    void commitUndoStep();
    /** Older name for commitUndoStep(), used before clip and note edits. */
    void beginClipEdit()   { commitUndoStep(); }
    /** Puts back the state as of the last step, without making an undo or redo step of it
        (for an edit that failed halfway). */
    void discardUncommitted();
    /** Forgets all steps; the current state becomes the starting point (after opening a project). */
    void clearUndo();
    bool undo();
    bool redo();
    bool canUndo() const   { return ! undoStack.empty() || stepPending; }
    bool canRedo() const   { return ! redoStack.empty(); }
    int getUndoDepth() const   { return (int) undoStack.size(); }

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
    std::vector<std::shared_ptr<const EditSnapshot>> undoStack, redoStack;
    std::shared_ptr<const EditSnapshot> committed;   // the state after the last step
    bool stepPending = false;
    bool restoring = false;

    std::shared_ptr<const EditSnapshot> capture() const;
    void restore (const EditSnapshot&);
    void scheduleUndoStep();
    void timerCallback() override   { commitUndoStep(); }

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
