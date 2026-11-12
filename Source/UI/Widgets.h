#pragma once

#include "../Engine/Engine.h"
#include "Theme.h"
#include <set>

namespace lathe
{
class NotesController;

/** Everything a view needs to reach. Owned by MainComponent. */
struct AppContext
{
    Edit& edit;
    Engine& engine;
    PluginHost& host;
    AudioPool& pool;
    PropertiesFile& settings;

    struct Selection
    {
        int trackId = -1;               // the focused track (device panel, record auto-arm)
        std::set<int> trackIds;         // all selected tracks (Cmd/Ctrl- and Shift-click add more)
        int clipTrackId = -1, clipId = -1;     // the clip clicked last (menus, piano roll)
        std::set<std::pair<int, int>> clips;   // every selected clip as (trackId, clipId), including that one
    } selection;

    std::function<void()> selectionChanged;
    std::function<void (int trackId, int clipId)> openClipEditor;
    std::function<void (const String&)> showStatus;
    std::function<void()> openPluginBrowser;
    std::function<void()> showDevices;   // opens the device panel (plug-ins and EQ) for the selected track
    NotesController* notes = nullptr;    // notes on the open song (set once the cloud is up)

    /** Which mix the mixer's faders, pans and mutes edit: -1 = the main mix, else a cue mix id. */
    int mixLayer = -1;

    /** The open song was shared with you so you push only some of its tracks (a guitarist gets
        the guitar). Those are drawn as yours; the rest is dimmed: you can change it, but it stays
        on this computer. Empty pushTracks: no limit. Set by the cloud (CloudController). */
    std::set<int> pushTracks;
    String pushTrackNames;               // "Guitar", or "Guitar and Vocal"
    bool isPushLimited() const noexcept              { return ! pushTracks.empty(); }
    bool isYourTrack (int trackId) const noexcept    { return pushTracks.count (trackId) > 0; }
    bool isLocalOnly (int trackId) const noexcept    { return isPushLimited() && ! isYourTrack (trackId); }
    std::function<void()> pushTracksChanged;

    void selectTrack (int id)
    {
        selection.trackId = id;
        selection.trackIds = { id };
        if (selectionChanged) selectionChanged();
    }
    /** Cmd/Ctrl-click toggles a track in the selection, Shift-click selects a range. */
    void extendSelection (int id, bool range)
    {
        if (range && selection.trackId >= 0)
        {
            int a = -1, b = -1;
            for (size_t i = 0; i < edit.tracks.size(); ++i)
            {
                if (edit.tracks[i]->id == selection.trackId) a = (int) i;
                if (edit.tracks[i]->id == id) b = (int) i;
            }
            if (a >= 0 && b >= 0)
                for (int i = jmin (a, b); i <= jmax (a, b); ++i)
                    selection.trackIds.insert (edit.tracks[(size_t) i]->id);
        }
        else if (selection.trackIds.count (id) && selection.trackIds.size() > 1)
        {
            selection.trackIds.erase (id);
            if (selection.trackId == id)
                selection.trackId = *selection.trackIds.begin();
            if (selectionChanged) selectionChanged();
            return;
        }
        else
        {
            selection.trackIds.insert (id);
        }
        selection.trackId = id;
        if (selectionChanged) selectionChanged();
    }
    bool isSelected (int id) const   { return selection.trackIds.count (id) > 0 || selection.trackId == id; }
    void selectClip (int trackId, int clipId)
    {
        selection.trackId = trackId;
        selection.clipTrackId = trackId;
        selection.clipId = clipId;
        selection.clips = { { trackId, clipId } };
        if (selectionChanged) selectionChanged();
    }
    /** Cmd/Ctrl- or Shift-click on a clip: adds it to the selection, or takes it out. */
    void toggleClip (int trackId, int clipId)
    {
        const std::pair<int, int> key { trackId, clipId };
        if (selection.clips.count (key) > 0)
        {
            selection.clips.erase (key);
            if (selection.clipId == clipId)
            {
                selection.clipTrackId = selection.clipId = -1;
                if (! selection.clips.empty())
                    std::tie (selection.clipTrackId, selection.clipId) = *selection.clips.begin();
            }
        }
        else
        {
            selection.clips.insert (key);
            selection.trackId = selection.clipTrackId = trackId;
            selection.clipId = clipId;
        }
        if (selectionChanged) selectionChanged();
    }
    /** Replaces the clip selection with these (trackId, clipId) pairs. */
    void setClipSelection (std::set<std::pair<int, int>> clips)
    {
        selection.clips = std::move (clips);
        if (selection.clips.empty())
            selection.clipTrackId = selection.clipId = -1;
        else if (selection.clips.count ({ selection.clipTrackId, selection.clipId }) == 0)
            std::tie (selection.clipTrackId, selection.clipId) = *selection.clips.begin();
        if (selectionChanged) selectionChanged();
    }
    bool isClipSelected (int clipId) const
    {
        if (selection.clipId == clipId)
            return true;
        for (auto& c : selection.clips)
            if (c.second == clipId)
                return true;
        return false;
    }
    void clearClipSelection()
    {
        selection.clipTrackId = selection.clipId = -1;
        selection.clips.clear();
        if (selectionChanged) selectionChanged();
    }
};

//==============================================================================
/** Arm / solo / mute style toggle: lit with a colour when on. */
void styleToggle (TextButton&, const String& text, Colour onColour, const String& tooltip);

/** Fader whose value is the 0..1 throw position (see theme::dbToFaderPos). */
class Fader : public Slider
{
public:
    Fader();
    String getTextFromValue (double v) override   { return theme::formatDb (theme::faderPosToDb ((float) v)) + " dB"; }
};

/** Pan or send knob. */
class Knob : public Slider
{
public:
    Knob (bool bipolar);
};

/** Stereo peak meter fed by a track's atomics. Call tick() from a UI timer. */
class LevelMeter : public Component
{
public:
    explicit LevelMeter (std::shared_ptr<Track>);
    /** Meter something other than a track (a cue mix). The owner keeps the atomics alive. */
    void setSource (std::shared_ptr<void> owner, std::atomic<float>* left, std::atomic<float>* right);
    void tick();
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override   { clipped = false; repaint(); }

private:
    std::shared_ptr<void> owner;
    std::atomic<float>* sourceL = nullptr;
    std::atomic<float>* sourceR = nullptr;
    float left = 0.0f, right = 0.0f;
    bool clipped = false;
};

//==============================================================================
// Routing selectors. Item ids encode the choice; apply*() writes it to the track.
/** Device inputs, then "From a track": the other audio/MIDI tracks (record what they play). */
void fillAudioInputs (ComboBox&, const Edit&, const Track&, int numDeviceInputs);
void applyAudioInput (ComboBox&, Track&);
/** Changes whenever the audio input choices or the current one could have. */
String audioInputKey (const Edit&, const Track&, int numDeviceInputs);
void fillOutputs (ComboBox&, const Edit&, const Track&, int numDeviceOutputs);
void applyOutput (ComboBox&, Track&, const Edit&);
/** MIDI devices, then "From a track": other MIDI tracks (what they play or their plug-ins generate). */
void fillMidiInputs (ComboBox&, const Edit&, const Track&, const StringArray& deviceNames);
String midiInputKey (const Edit&, const Track&, const StringArray& deviceNames);
void applyMidiInput (ComboBox&, Track&, const StringArray& deviceNames);
void fillMidiOutputs (ComboBox&, const Track&, const StringArray& deviceNames);
void applyMidiOutput (ComboBox&, Track&, const StringArray& deviceNames);

/** Short label for a track's input, e.g. "In 3", "In 1/2" or "From Synth". */
String describeInput (const Track&, const Edit* = nullptr);

//==============================================================================
// Plug-in actions shared by the mixer and the device chain.
void openPluginEditor (AppContext&, const std::shared_ptr<PluginSlot>&, const String& trackName);
/** Loads a plug-in into a track's chain at `index` (-1 = end) and opens its window. */
std::shared_ptr<PluginSlot> insertPlugin (AppContext&, std::shared_ptr<Track>, const PluginDescription&, int index);
/** Adds a plug-in where it makes sense: effects go on the selected track; instruments
    and MIDI tools go on the selected MIDI track, or a new one. Returns the track used. */
std::shared_ptr<Track> addPluginToSelection (AppContext&, const PluginDescription&, bool isMidiTool);
/** Where addPluginToSelection would put it, for button labels ("Add to Vocal", "Add on new MIDI track"). */
String describeAddTarget (AppContext&, bool isInstrument, bool isMidiTool);
void showAddPluginMenu (AppContext&, Component& anchor, std::shared_ptr<Track>, int insertIndex);
void showPluginSlotMenu (AppContext&, Component& anchor, std::shared_ptr<Track>, std::shared_ptr<PluginSlot>);
void removePlugin (AppContext&, Track&, const std::shared_ptr<PluginSlot>&);
void movePlugin (AppContext&, Track&, const std::shared_ptr<PluginSlot>&, int delta);

/** Track context menu (rename, colour, group, delete, ...). */
void showTrackMenu (AppContext&, Component& anchor, std::shared_ptr<Track>);
/** Group context menu (rename, colour, arm all, collapse, ungroup). */
void showGroupMenu (AppContext&, Component& anchor, std::shared_ptr<TrackGroup>);
/** Opens the group's bus in the device panel, making it first if need be. */
void showGroupBus (AppContext&, std::shared_ptr<TrackGroup>);
void renameGroup (AppContext&, std::shared_ptr<TrackGroup>);
/** Groups the selected tracks (or just `fallback` if one or none is selected). */
void groupSelectedTracks (AppContext&, int fallbackTrackId);

/** Solo button behaviour: a plain click solos only this (clearing every other track and
    group solo); Cmd/Ctrl-click adds to or removes from the soloed set. Pass a track or a group. */
void toggleSolo (AppContext&, Track*, TrackGroup*);
void renameTrack (AppContext&, std::shared_ptr<Track>);

/** Asks for a line of text; calls back with the result only if confirmed. */
void askForText (const String& title, const String& prompt, const String& initial, std::function<void (const String&)> onOk);
} // namespace lathe
