#pragma once

#include "Widgets.h"

namespace lathe
{
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
    void importFiles (const Array<File>&, int trackIndex, double atBeat);
    void zoom (double factor, float anchorX);
    void scrollToPlayhead();

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
    class Ruler;
    class Lanes;
    class Header;
    class GroupHeader;
    friend class Ruler;
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
