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

    // clip commands (keyboard shortcuts and menus)
    bool deleteSelectedClip();
    void duplicateSelectedClip();
    void splitSelectedClip();
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
    int trackIndexAtY (int laneY) const;
    int trackTop (int index) const noexcept           { return index * theme::trackHeight - scrollY; }

    AppContext& ctx;

private:
    class Ruler;
    class Lanes;
    class Header;
    friend class Ruler;
    friend class Lanes;
    friend class Header;

    void scrollBarMoved (ScrollBar*, double newRangeStart) override;
    void updateScrollBars();
    String structureSignature() const;

    double pxPerBeat = 24.0;
    double scrollBeats = 0.0;
    int scrollY = 0;
    float lastPlayheadX = -100.0f;

    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<Lanes> lanes;
    Component headerHolder;
    OwnedArray<Header> headers;
    ScrollBar hbar { false }, vbar { true };
    TextButton addAudioButton { "+ Audio" }, addMidiButton { "+ MIDI" };
    String signature;
};
} // namespace lathe
