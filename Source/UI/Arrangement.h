#pragma once

#include "Widgets.h"
#include "../Model/MidiExport.h"

namespace lathe
{
struct JoinPlan;
class ArrangementView : public Component,
                        public FileDragAndDropTarget,
                        private ScrollBar::Listener
{
public:
    explicit ArrangementView (AppContext&);
    ~ArrangementView() override;

    /** Re-reads the edit: rebuilds track headers if tracks changed, then repaints. */
    void refresh();
    /** Playhead and meters, ~30x a second. */
    void tick();

    void resized() override;
    void paint (Graphics&) override;

    // clip commands (keyboard shortcuts and menus). They act on every selected clip.
    bool deleteSelectedClip();
    void duplicateSelectedClip();
    void splitSelectedClip();
    /** Copies the selected clips (Cmd+C). Cut also removes them. */
    bool copySelectedClips();
    bool cutSelectedClips();
    /** Pastes at the start marker on the selected track (Cmd+V). Clips copied from
        several tracks keep their layout, going down from the selected track. */
    void pasteClips();
    /** Joins the selected clips on each track into one clip (Cmd+J). */
    void joinSelectedClips();
    void selectAllClips();
    int numSelectedClips() const;
    bool canPaste() const   { return ! clipboard.empty(); }
    /** One file of an import: the track it went on, or why it didn't. */
    struct ImportedFile
    {
        File source;
        int trackId = -1;   // -1: not imported (see error)
        String error;
    };
    /** Audio files: the first onto the audio track at trackIndex, the rest onto new tracks named
        after them, at atBeat. The files are copied (and converted if needed) into the project's
        Audio folder on a background thread, in a progress window naming each file, with Cancel;
        the clips go in afterwards, as one undo step, and then `then` gets one entry per file, in
        order (not called if the person cancels). `inBackground` false does it all before
        returning (the scripted screenshots). */
    void importFiles (const Array<File>&, int trackIndex, double atBeat,
                      std::function<void (const std::vector<ImportedFile>&)> then = {}, bool inBackground = true);
    /** MIDI files: onto the MIDI track at trackIndex as one clip each, else onto new MIDI tracks
        (one per part of the file). */
    void importMidiFiles (const Array<File>&, int trackIndex, double atBeat);
    /** Saves a .mid file: of the selected MIDI clips, or (selectedOnly false) of every MIDI track. */
    void exportMidi (bool selectedOnly);
    /** The selected MIDI clips as a .mid file in a temporary folder, for dragging out of Lathe;
        a non-existent File when none of the selected clips is MIDI. */
    File writeSelectedMidiForDrag();
    int numSelectedMidiClips() const;
    /** Convert to MIDI: finds the notes in an audio clip (and the other selected audio clips on its
        track) and puts them on a new MIDI track under it. `kind` is an audiotomidi::Kind. */
    void convertToMidi (int trackId, int clipId, int kind);
    /** View > Key Lane: the strip under the bar ruler that shows (and edits) the song's keys. */
    void setKeyLaneShown (bool);
    bool isKeyLaneShown() const noexcept   { return keyLaneShown; }
    void zoom (double factor, float anchorX);
    void scrollToPlayhead();
    /** Shows a spot: scrolls to the track and beat, selects the track, moves the start marker there. */
    void reveal (int trackId, double beat);
    /** A small screen rectangle at a track and beat, for pointing a pop-up at. */
    Rectangle<int> screenAreaFor (int trackId, double beat) const;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const StringArray&) override;
    void filesDropped (const StringArray&, int x, int y) override;

    // geometry shared with the inner components
    float beatToX (double beat) const noexcept       { return (float) ((beat - scrollBeats) * pxPerBeat); }
    double xToBeat (float x) const noexcept          { return scrollBeats + x / pxPerBeat; }
    double gridStep() const;
    double snap (double beat, bool bypass) const;
    /** Track index (into edit.tracks) at a y in lane coordinates, or -1 (group rows, empty space). */
    int trackIndexAtY (int laneY) const;
    /** Top of a track's row in lane coordinates; far off-screen if it's hidden in a collapsed group. */
    int trackTop (int index) const noexcept
    {
        return (index >= 0 && (size_t) index < trackTops.size() && trackTops[(size_t) index] >= 0)
                 ? trackTops[(size_t) index] - scrollY : -1000000;
    }
    bool isTrackShown (int index) const noexcept
    {
        return index >= 0 && (size_t) index < trackTops.size() && trackTops[(size_t) index] >= 0;
    }
    int contentBottom() const noexcept   { return contentHeight - scrollY; }

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;

    AppContext& ctx;

private:
    void applyJoins (const std::vector<JoinPlan>&, const std::vector<File>& wavs, const std::vector<String>& errors);
    class Ruler;
    class KeyLane;
    class Lanes;
    class Header;
    class GroupHeader;
    friend class Ruler;
    friend class KeyLane;
    friend class Lanes;
    friend class Header;
    friend class GroupHeader;

    /** One row of the arrangement: a group's header, or a track. */
    struct Row
    {
        std::shared_ptr<TrackGroup> group;   // set for group header rows
        int trackIndex = -1;                 // set for track rows
        int y = 0, height = 0;
    };
    void layoutRows();

    /** A copied clip: its start is relative to the copied block, its track relative to the top track. */
    struct CopiedClip
    {
        Clip clip;
        int trackOffset = 0;
        TrackKind kind = TrackKind::audio;
    };
    std::vector<CopiedClip> clipboard;
    std::unique_ptr<FileChooser> midiChooser;
    /** The selected MIDI clips grouped by track, measured from the bar the first one starts in. */
    std::vector<midiexport::Item> selectedMidi (double& fromBeat, String& name) const;
    double clipboardLength = 0.0;

    struct SelectedClip { std::shared_ptr<Track> track; int trackIndex = -1; Clip* clip = nullptr; };
    /** The selected clips that still exist, in track order then time order. */
    std::vector<SelectedClip> getSelectedClips() const;
    std::vector<CopiedClip> gatherSelection (double& blockStart, double& blockEnd, int& topTrack) const;
    /** Places copies at a beat, from a track index down. Returns how many couldn't be placed. */
    int placeClips (const std::vector<CopiedClip>&, double atBeat, int topTrackIndex);

    // recording display
    bool wasRecording = false;

    void scrollBarMoved (ScrollBar*, double newRangeStart) override;
    void updateScrollBars();
    String structureSignature() const;

    static constexpr int kGroupHeaderHeight = 30;
    double pxPerBeat = 24.0;
    double scrollBeats = 0.0;
    int scrollY = 0;
    float lastPlayheadX = -100.0f;

    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<KeyLane> keyLane;
    bool keyLaneShown = true;
    std::unique_ptr<Lanes> lanes;
    Component headerHolder;
    OwnedArray<Header> headers;                 // one per track, in edit.tracks order
    OwnedArray<GroupHeader> groupHeaders;
    std::vector<Row> rows;
    std::vector<int> trackTops;                 // by track index; -1 = hidden
    int contentHeight = 0;
    ScrollBar hbar { false }, vbar { true };
    TextButton addAudioButton { "+ Audio" }, addMidiButton { "+ MIDI" };
    String signature;
};
} // namespace lathe
