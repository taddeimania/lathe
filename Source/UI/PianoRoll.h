#pragma once

#include "Widgets.h"
#include "../Model/NoteEdit.h"
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

    // The Edit menu's note commands, while the piano roll has the keyboard focus.
    bool editsNotes() const;
    int numSelectedNotes() const        { return (int) selected.size(); }
    static bool canPasteNotes()         { return ! clipboard().empty(); }
    void cutNotes();
    void copyNotes();
    void pasteNotes();
    void duplicateNotes();
    void deleteNotes();
    void selectAllNotes();
    void splitNotes();
    void glueNotes();

    void paint (Graphics&) override;
    void resized() override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseMove (const MouseEvent&) override;
    void mouseExit (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    void mouseMagnify (const MouseEvent&, float scaleFactor) override;
    bool keyPressed (const KeyPress&) override;

private:
    Clip* getClip();
    const Clip* getClip() const      { return const_cast<PianoRoll*> (this)->getClip(); }
    void scrollBarMoved (ScrollBar*, double) override;
    void centreOnNotes();
    void zoomToFit();
    void zoomTime (double factor, float anchorX);

    // one undo step: snapshot, change the notes, announce
    void editNotes (const String& status, const std::function<void (Clip&)>&);
    void quantizeSelection();
    void showQuantizeOptions();
    void showContextMenu();
    noteedit::QuantizeOptions quantizeOptions() const;
    static std::vector<Note>& clipboard();

    // hearing notes as you click, drag and draw them
    bool hearNotes() const;
    void hear (int pitch, float velocity);   // the note sounds until stopHearing() or another hear()
    void stopHearing();
    void blip (int pitch, float velocity);   // a short note, for drawing and arrow keys
    bool onKeyboard (Point<int>) const;

    // geometry
    Rectangle<int> gridArea() const;
    Rectangle<int> rulerArea() const;
    Rectangle<int> velocityArea() const;
    float beatToX (double beat) const;
    double xToBeat (float x) const;
    int yToPitch (int y) const;
    int pitchToY (int pitch) const;
    double gridStep() const;          // the grid's step (for Off, what the lines show)
    bool snapping() const;
    double snap (double beat, bool bypass) const;
    double snapDown (double beat, bool bypass) const;
    enum class Zone { body, left, right };
    int noteAt (Point<int>, Zone&) const;
    int velocityStemAt (Point<int>) const;
    float velocityAtY (int y) const;
    double startMarkerBeat() const;   // the song's start marker, relative to the clip

    // the readout in the toolbar
    void showInfo (const String&);
    String noteInfo (const Note&) const;
    String positionName (double clipBeat) const;
    static String lengthName (double beats);

    AppContext& ctx;
    int trackId = -1, clipId = -1;

    double pxPerBeat = 64.0, scrollBeat = 0.0;
    int noteHeight = 11, scrollY = 0;
    static constexpr int keyboardWidth = 46, toolbarHeight = 28, rulerHeight = 18, velocityHeight = 64;

    ComboBox gridBox;
    TextButton quantizeButton { "Quantize" }, quantizeMenuButton { "..." }, closeButton { "Close" },
               hearButton { "Hear notes" }, drawButton { "Draw" };
    Label title, info;
    ScrollBar hbar { false }, vbar { true };

    std::set<int> selected;
    enum class Mode { none, move, resizeEnd, resizeStart, draw, marquee, velocity, velocityLine, keyboard, clipEnd, marker } mode = Mode::none;
    int heardPitch = -1, heardTrack = -1, dragNote = -1;
    std::vector<Note> dragOriginal;
    double dragOriginalLength = 0.0;
    std::set<int> velocityTargets;
    Point<int> dragStart, dragNow;
    Rectangle<int> marquee;
    bool dragChanged = false, copyOnDrag = false, narrowOnRelease = false;
    double lastLength = -1.0;   // the length new notes get: the last one drawn or resized
    float lastPlayheadX = -100.0f;
};
} // namespace lathe
