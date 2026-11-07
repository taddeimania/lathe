#pragma once

#include "../Engine/Engine.h"
#include "Theme.h"

namespace lathe
{
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
        int trackId = -1;
        int clipTrackId = -1, clipId = -1;
    } selection;

    std::function<void()> selectionChanged;
    std::function<void (int trackId, int clipId)> openClipEditor;
    std::function<void (const String&)> showStatus;
    std::function<void()> openPluginBrowser;

    void selectTrack (int id)
    {
        selection.trackId = id;
        if (selectionChanged) selectionChanged();
    }
    void selectClip (int trackId, int clipId)
    {
        selection.trackId = trackId;
        selection.clipTrackId = trackId;
        selection.clipId = clipId;
        if (selectionChanged) selectionChanged();
    }
    void clearClipSelection()
    {
        selection.clipTrackId = selection.clipId = -1;
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
    void tick();
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override   { clipped = false; repaint(); }

private:
    std::shared_ptr<Track> track;
    float left = 0.0f, right = 0.0f;
    bool clipped = false;
};

//==============================================================================
// Routing selectors. Item ids encode the choice; apply*() writes it to the track.
void fillAudioInputs (ComboBox&, const Track&, int numDeviceInputs);
void applyAudioInput (ComboBox&, Track&);
void fillOutputs (ComboBox&, const Edit&, const Track&, int numDeviceOutputs);
void applyOutput (ComboBox&, Track&, const Edit&);
void fillMidiInputs (ComboBox&, const Track&, const StringArray& deviceNames);
void applyMidiInput (ComboBox&, Track&, const StringArray& deviceNames);
void fillMidiOutputs (ComboBox&, const Track&, const StringArray& deviceNames);
void applyMidiOutput (ComboBox&, Track&, const StringArray& deviceNames);

/** Short label for a track's input, e.g. "In 3" or "In 1/2". */
String describeInput (const Track&);

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

/** Track context menu (rename, colour, delete, ...). */
void showTrackMenu (AppContext&, Component& anchor, std::shared_ptr<Track>);
void renameTrack (AppContext&, std::shared_ptr<Track>);

/** Asks for a line of text; calls back with the result only if confirmed. */
void askForText (const String& title, const String& prompt, const String& initial, std::function<void (const String&)> onOk);
} // namespace lathe
