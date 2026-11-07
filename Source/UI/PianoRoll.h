#pragma once

#include "Widgets.h"
#include <set>

namespace lathe
{
class PianoRoll : public Component, private ScrollBar::Listener
{
public:
    explicit PianoRoll (AppContext&);
    ~PianoRoll() override;

    void setClip (int trackId, int clipId);
    bool hasClip() const     { return const_cast<PianoRoll*> (this)->getClip() != nullptr; }
    void refresh();
    void tick();
    std::function<void()> onClose;

    void paint (Graphics&) override;
    void resized() override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseMove (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    bool keyPressed (const KeyPress&) override;

private:
    Clip* getClip();
    void scrollBarMoved (ScrollBar*, double) override;
    void quantizeSelection();
    void centreOnNotes();

    // geometry
    Rectangle<int> gridArea() const;
    Rectangle<int> velocityArea() const;
    float beatToX (double beat) const;
    double xToBeat (float x) const;
    int yToPitch (int y) const;
    int pitchToY (int pitch) const;
    double gridStep() const;
    double snap (double beat, bool bypass) const;
    int noteAt (Point<int>, bool& onRightEdge);

    AppContext& ctx;
    int trackId = -1, clipId = -1;

    double pxPerBeat = 64.0, scrollBeat = 0.0;
    int noteHeight = 11, scrollY = 0;
    static constexpr int keyboardWidth = 46, toolbarHeight = 28, velocityHeight = 48;

    ComboBox gridBox;
    TextButton quantizeButton { "Quantize" }, closeButton { "Close" };
    Label title;
    ScrollBar hbar { false }, vbar { true };

    std::set<int> selected;
    enum class Mode { none, move, resize, marquee, velocity } mode = Mode::none;
    std::vector<Note> dragOriginal;
    Point<int> dragStart;
    Rectangle<int> marquee;
    bool dragChanged = false;
    float lastPlayheadX = -100.0f;
};
} // namespace lathe
