#include "PianoRoll.h"

namespace lathe
{
PianoRoll::PianoRoll (AppContext& c) : ctx (c)
{
    setWantsKeyboardFocus (true);

    title.setFont (theme::font (12.5f, true));
    addAndMakeVisible (title);

    gridBox.addItem ("1/4", 1);
    gridBox.addItem ("1/8", 2);
    gridBox.addItem ("1/16", 3);
    gridBox.addItem ("1/32", 4);
    gridBox.addItem ("1/8 triplet", 5);
    gridBox.addItem ("1/16 triplet", 6);
    gridBox.setSelectedId (3, dontSendNotification);
    gridBox.setTooltip ("Grid for drawing, moving and quantizing (hold Alt to ignore it)");
    gridBox.onChange = [this] { repaint(); };
    gridBox.setWantsKeyboardFocus (false);
    addAndMakeVisible (gridBox);

    quantizeButton.setTooltip ("Snap the selected notes (or all notes) to the grid  (Q)");
    quantizeButton.onClick = [this] { quantizeSelection(); };
    closeButton.onClick = [this] { if (onClose) onClose(); };
    for (auto* b : { &quantizeButton, &closeButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    hbar.addListener (this);
    vbar.addListener (this);
    hbar.setAutoHide (false);
    vbar.setAutoHide (false);
    addAndMakeVisible (hbar);
    addAndMakeVisible (vbar);
}

PianoRoll::~PianoRoll()
{
    hbar.removeListener (this);
    vbar.removeListener (this);
}

Clip* PianoRoll::getClip()
{
    if (auto t = ctx.edit.findTrack (trackId))
        return t->findClip (clipId);
    return nullptr;
}

void PianoRoll::setClip (int newTrackId, int newClipId)
{
    const bool changedClip = newTrackId != trackId || newClipId != clipId;
    trackId = newTrackId;
    clipId = newClipId;
    if (changedClip)
    {
        selected.clear();
        scrollBeat = 0.0;
        centreOnNotes();
    }
    refresh();
}

void PianoRoll::centreOnNotes()
{
    int pitch = 60;
    if (auto* c = getClip(); c != nullptr && ! c->notes.empty())
    {
        int lo = 127, hi = 0;
        for (auto& n : c->notes) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
        pitch = (lo + hi) / 2;
    }
    scrollY = jmax (0, (127 - pitch) * noteHeight - gridArea().getHeight() / 2);
}

void PianoRoll::refresh()
{
    auto* c = getClip();
    auto t = ctx.edit.findTrack (trackId);
    title.setText (c != nullptr && t != nullptr ? t->name + "  /  " + c->name : String ("No clip"), dontSendNotification);

    if (c != nullptr)
    {
        // Drop selections that point past the end (after undo, for instance).
        for (auto it = selected.begin(); it != selected.end();)
            it = (*it >= (int) c->notes.size()) ? selected.erase (it) : std::next (it);

        const double visible = gridArea().getWidth() / pxPerBeat;
        hbar.setRangeLimits (0.0, jmax (c->length + 4.0, scrollBeat + visible), dontSendNotification);
        hbar.setCurrentRange (scrollBeat, visible, dontSendNotification);
    }

    vbar.setRangeLimits (0.0, 128.0 * noteHeight, dontSendNotification);
    vbar.setCurrentRange (scrollY, gridArea().getHeight(), dontSendNotification);
    repaint();
}

void PianoRoll::tick()
{
    auto* c = getClip();
    if (c == nullptr)
        return;

    const float x = ctx.engine.isPlaying() ? beatToX (ctx.engine.getPlayheadBeats() - c->start) : -100.0f;
    if (std::abs (x - lastPlayheadX) >= 0.5f)
    {
        repaint (roundToInt (lastPlayheadX) - 2, toolbarHeight, 5, getHeight());
        repaint (roundToInt (x) - 2, toolbarHeight, 5, getHeight());
        lastPlayheadX = x;
    }
}

void PianoRoll::scrollBarMoved (ScrollBar* bar, double start)
{
    if (bar == &hbar) scrollBeat = jmax (0.0, start);
    else              scrollY = (int) start;
    repaint();
}

//==============================================================================
Rectangle<int> PianoRoll::gridArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarHeight).withTrimmedLeft (keyboardWidth)
                           .withTrimmedRight (10).withTrimmedBottom (10 + velocityHeight);
}

Rectangle<int> PianoRoll::velocityArea() const
{
    auto g = gridArea();
    return { g.getX(), g.getBottom(), g.getWidth(), velocityHeight };
}

float PianoRoll::beatToX (double beat) const   { return (float) (gridArea().getX() + (beat - scrollBeat) * pxPerBeat); }
double PianoRoll::xToBeat (float x) const      { return scrollBeat + (x - gridArea().getX()) / pxPerBeat; }
int PianoRoll::pitchToY (int pitch) const      { return gridArea().getY() + (127 - pitch) * noteHeight - scrollY; }
int PianoRoll::yToPitch (int y) const          { return jlimit (0, 127, 127 - (y - gridArea().getY() + scrollY) / noteHeight); }

double PianoRoll::gridStep() const
{
    switch (gridBox.getSelectedId())
    {
        case 1: return 1.0;
        case 2: return 0.5;
        case 4: return 0.125;
        case 5: return 1.0 / 3.0;
        case 6: return 1.0 / 6.0;
        default: return 0.25;
    }
}

double PianoRoll::snap (double beat, bool bypass) const
{
    if (bypass)
        return beat;
    const double step = gridStep();
    return std::round (beat / step) * step;
}

int PianoRoll::noteAt (Point<int> p, bool& onRightEdge)
{
    onRightEdge = false;
    auto* c = getClip();
    if (c == nullptr)
        return -1;

    for (int i = (int) c->notes.size(); --i >= 0;)
    {
        auto& n = c->notes[(size_t) i];
        const float x1 = beatToX (n.start), x2 = beatToX (n.start + n.length);
        const int y = pitchToY (n.pitch);
        if (p.y >= y && p.y < y + noteHeight && p.x >= x1 && p.x <= x2)
        {
            onRightEdge = p.x > x2 - jmin (6.0f, (x2 - x1) * 0.4f);
            return i;
        }
    }
    return -1;
}

//==============================================================================
void PianoRoll::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::border);
    g.drawHorizontalLine (toolbarHeight - 1, 0.0f, (float) getWidth());

    auto* c = getClip();
    auto area = gridArea();
    if (c == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f));
        g.drawText ("Double-click a MIDI clip to edit its notes.", area, Justification::centred);
        return;
    }

    auto t = ctx.edit.findTrack (trackId);
    const auto colour = t != nullptr ? t->colour : theme::accent;

    // keyboard
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (0, area.getY(), keyboardWidth, area.getHeight());
        for (int p = 0; p < 128; ++p)
        {
            const int y = pitchToY (p);
            if (y > area.getBottom() || y + noteHeight < area.getY())
                continue;
            const bool black = MidiMessage::isMidiNoteBlack (p);
            g.setColour (black ? Colour (0xff1c1c1c) : Colour (0xffcfcfcf));
            g.fillRect (0, y, keyboardWidth - 1, noteHeight - 1);
            if (p % 12 == 0)
            {
                g.setColour (theme::textDark);
                g.setFont (theme::font (9.5f));
                g.drawText (MidiMessage::getMidiNoteName (p, true, true, 3), 2, y, keyboardWidth - 6, noteHeight, Justification::centredRight);
            }
        }
    }

    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (area);

        // rows
        for (int p = 0; p < 128; ++p)
        {
            const int y = pitchToY (p);
            if (y > area.getBottom() || y + noteHeight < area.getY())
                continue;
            g.setColour (MidiMessage::isMidiNoteBlack (p) ? theme::lane : theme::laneAlt);
            g.fillRect (area.getX(), y, area.getWidth(), noteHeight);
            if (p % 12 == 0)
            {
                g.setColour (theme::gridBar);
                g.drawHorizontalLine (y + noteHeight - 1, (float) area.getX(), (float) area.getRight());
            }
        }

        // grid
        const double step = gridStep();
        const double barLen = ctx.edit.beatsPerBar();
        for (double b = std::floor (scrollBeat / step) * step; b <= xToBeat ((float) area.getRight()); b += step)
        {
            const bool isBar = std::abs (std::remainder (b, barLen)) < 1.0e-6;
            const bool isBeat = std::abs (std::remainder (b, 1.0)) < 1.0e-6;
            g.setColour (isBar ? theme::gridBar : isBeat ? theme::gridBeat : theme::gridFine);
            g.drawVerticalLine (roundToInt (beatToX (b)), (float) area.getY(), (float) area.getBottom());
        }

        // outside the clip
        const float endX = beatToX (c->length);
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRect (Rectangle<float> (endX, (float) area.getY(), (float) area.getRight() - endX, (float) area.getHeight()));

        // notes
        for (int i = 0; i < (int) c->notes.size(); ++i)
        {
            auto& n = c->notes[(size_t) i];
            const float x1 = beatToX (n.start), x2 = beatToX (n.start + n.length);
            auto r = Rectangle<float> (x1, (float) pitchToY (n.pitch), jmax (3.0f, x2 - x1), (float) noteHeight - 1.0f);
            const bool sel = selected.count (i) > 0;
            g.setColour (colour.withMultipliedBrightness (0.55f + 0.6f * n.velocity));
            g.fillRect (r);
            g.setColour (sel ? theme::accent : theme::border);
            g.drawRect (r, sel ? 1.5f : 1.0f);
        }

        if (mode == Mode::marquee)
        {
            g.setColour (theme::accent.withAlpha (0.15f));
            g.fillRect (marquee);
            g.setColour (theme::accent);
            g.drawRect (marquee);
        }

        if (ctx.engine.isPlaying())
        {
            g.setColour (theme::accent);
            g.fillRect (Rectangle<float> (beatToX (ctx.engine.getPlayheadBeats() - c->start) - 0.5f, (float) area.getY(), 1.5f, (float) area.getHeight()));
        }
    }

    // velocity lane
    auto vel = velocityArea();
    g.setColour (theme::lane);
    g.fillRect (vel);
    g.setColour (theme::border);
    g.drawHorizontalLine (vel.getY(), (float) vel.getX(), (float) vel.getRight());
    g.setColour (theme::textDim);
    g.setFont (theme::font (10.5f));
    g.drawText ("Velocity", 0, vel.getY(), keyboardWidth - 4, 16, Justification::centredRight);
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (vel);
        for (int i = 0; i < (int) c->notes.size(); ++i)
        {
            auto& n = c->notes[(size_t) i];
            const float x = beatToX (n.start);
            const float h = (vel.getHeight() - 4) * n.velocity;
            g.setColour (selected.count (i) ? theme::accent : colour);
            g.fillRect (Rectangle<float> (x, (float) vel.getBottom() - h, 3.0f, h));
        }
    }
}

void PianoRoll::resized()
{
    auto top = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 3);
    closeButton.setBounds (top.removeFromRight (56));
    top.removeFromRight (8);
    quantizeButton.setBounds (top.removeFromRight (72));
    top.removeFromRight (6);
    gridBox.setBounds (top.removeFromRight (110));
    title.setBounds (top);

    auto area = gridArea();
    hbar.setBounds (area.getX(), getHeight() - 10, area.getWidth(), 10);
    vbar.setBounds (getWidth() - 10, area.getY(), 10, area.getHeight());
    refresh();
}

//==============================================================================
void PianoRoll::mouseMove (const MouseEvent& e)
{
    bool edge = false;
    const int i = gridArea().contains (e.getPosition()) ? noteAt (e.getPosition(), edge) : -1;
    setMouseCursor (i >= 0 ? (edge ? MouseCursor::LeftRightResizeCursor : MouseCursor::DraggingHandCursor) : MouseCursor::NormalCursor);
}

void PianoRoll::mouseDown (const MouseEvent& e)
{
    grabKeyboardFocus();
    auto* c = getClip();
    if (c == nullptr)
        return;

    dragStart = e.getPosition();
    dragChanged = false;
    dragOriginal = c->notes;

    if (velocityArea().contains (e.getPosition()))
    {
        mode = Mode::velocity;
        return;
    }

    if (! gridArea().contains (e.getPosition()))
    {
        mode = Mode::none;
        return;
    }

    bool edge = false;
    const int i = noteAt (e.getPosition(), edge);
    if (i < 0)
    {
        if (! e.mods.isShiftDown())
            selected.clear();
        mode = Mode::marquee;
        marquee = { e.x, e.y, 0, 0 };
        repaint();
        return;
    }

    if (! selected.count (i))
    {
        if (! e.mods.isShiftDown())
            selected.clear();
        selected.insert (i);
    }
    mode = edge ? Mode::resize : Mode::move;
    repaint();
}

void PianoRoll::mouseDrag (const MouseEvent& e)
{
    auto* c = getClip();
    if (c == nullptr || mode == Mode::none)
        return;

    auto beginChange = [&]
    {
        if (! dragChanged)
        {
            dragChanged = true;
            // Record the state from before the drag for undo.
            auto now = c->notes;
            c->notes = dragOriginal;
            ctx.edit.beginClipEdit();
            c->notes = now;
        }
    };

    const bool noSnap = e.mods.isAltDown();

    if (mode == Mode::marquee)
    {
        marquee = Rectangle<int> (dragStart, e.getPosition());
        std::set<int> hits;
        const double b1 = xToBeat ((float) marquee.getX()), b2 = xToBeat ((float) marquee.getRight());
        const int p1 = yToPitch (marquee.getBottom()), p2 = yToPitch (marquee.getY());
        for (int i = 0; i < (int) c->notes.size(); ++i)
        {
            auto& n = c->notes[(size_t) i];
            if (n.pitch >= p1 && n.pitch <= p2 && n.start + n.length > b1 && n.start < b2)
                hits.insert (i);
        }
        selected = hits;
    }
    else if (mode == Mode::velocity)
    {
        auto vel = velocityArea();
        const float v = jlimit (0.02f, 1.0f, (float) (vel.getBottom() - e.y) / (float) (vel.getHeight() - 4));
        // Edit the selected notes, or the note nearest the mouse.
        std::set<int> targets = selected;
        if (targets.empty())
        {
            double best = 1.0e9;
            int bestIndex = -1;
            for (int i = 0; i < (int) c->notes.size(); ++i)
            {
                const double d = std::abs (beatToX (c->notes[(size_t) i].start) - e.x);
                if (d < best && d < 8.0) { best = d; bestIndex = i; }
            }
            if (bestIndex >= 0) targets.insert (bestIndex);
        }
        if (! targets.empty())
        {
            beginChange();
            for (int i : targets)
                c->notes[(size_t) i].velocity = v;
        }
    }
    else if (e.getDistanceFromDragStart() > 2 || dragChanged)
    {
        beginChange();
        const double deltaBeats = (e.x - dragStart.x) / pxPerBeat;

        if (mode == Mode::move)
        {
            const int deltaPitch = (dragStart.y - e.y + noteHeight / 2) / noteHeight;
            // Snap the moved group by its first note so relative timing is kept.
            double anchor = 1.0e9;
            for (int i : selected)
                anchor = jmin (anchor, dragOriginal[(size_t) i].start);
            const double snappedDelta = snap (anchor + deltaBeats, noSnap) - anchor;

            for (int i : selected)
            {
                auto& o = dragOriginal[(size_t) i];
                auto& n = c->notes[(size_t) i];
                n.start = jmax (0.0, o.start + snappedDelta);
                n.pitch = jlimit (0, 127, o.pitch + deltaPitch);
            }
        }
        else if (mode == Mode::resize)
        {
            for (int i : selected)
            {
                auto& o = dragOriginal[(size_t) i];
                const double end = snap (o.start + o.length + deltaBeats, noSnap);
                c->notes[(size_t) i].length = jmax (noSnap ? 0.01 : gridStep() * 0.5, end - o.start);
            }
        }
    }

    repaint();
}

void PianoRoll::mouseUp (const MouseEvent&)
{
    if (dragChanged)
        ctx.edit.changed();
    mode = Mode::none;
    dragChanged = false;
    repaint();
}

void PianoRoll::mouseDoubleClick (const MouseEvent& e)
{
    auto* c = getClip();
    if (c == nullptr || ! gridArea().contains (e.getPosition()))
        return;

    bool edge = false;
    const int i = noteAt (e.getPosition(), edge);
    ctx.edit.beginClipEdit();

    if (i >= 0)
    {
        c->notes.erase (c->notes.begin() + i);
        selected.clear();
    }
    else
    {
        const double start = std::floor (xToBeat ((float) e.x) / gridStep()) * gridStep();
        if (start < 0.0 || start >= c->length)
            return;
        c->notes.push_back ({ yToPitch (e.y), start, gridStep(), 0.8f });
        selected = { (int) c->notes.size() - 1 };
    }

    mode = Mode::none;
    ctx.edit.changed();
    repaint();
}

void PianoRoll::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double anchor = xToBeat ((float) e.x);
        pxPerBeat = jlimit (8.0, 600.0, pxPerBeat * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
        scrollBeat = jmax (0.0, anchor - (e.x - gridArea().getX()) / pxPerBeat);
        refresh();
        return;
    }

    float dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown() && dx == 0.0f)
        std::swap (dx, dy);
    if (dx != 0.0f) hbar.setCurrentRangeStart (hbar.getCurrentRangeStart() - dx * 200.0 / pxPerBeat);
    if (dy != 0.0f) vbar.setCurrentRangeStart (vbar.getCurrentRangeStart() - dy * 200.0);
}

bool PianoRoll::keyPressed (const KeyPress& k)
{
    auto* c = getClip();
    if (c == nullptr)
        return false;

    if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey)
    {
        if (selected.empty())
            return true;
        ctx.edit.beginClipEdit();
        for (auto it = selected.rbegin(); it != selected.rend(); ++it)
            c->notes.erase (c->notes.begin() + *it);
        selected.clear();
        ctx.edit.changed();
        return true;
    }

    if (k == KeyPress ('a', ModifierKeys::commandModifier, 0))
    {
        selected.clear();
        for (int i = 0; i < (int) c->notes.size(); ++i)
            selected.insert (i);
        repaint();
        return true;
    }

    if ((k.getKeyCode() == KeyPress::upKey || k.getKeyCode() == KeyPress::downKey) && ! selected.empty())
    {
        const int delta = (k.getKeyCode() == KeyPress::upKey ? 1 : -1) * (k.getModifiers().isShiftDown() ? 12 : 1);
        ctx.edit.beginClipEdit();
        for (int i : selected)
            c->notes[(size_t) i].pitch = jlimit (0, 127, c->notes[(size_t) i].pitch + delta);
        ctx.edit.changed();
        return true;
    }

    if (k == KeyPress ('q'))
    {
        quantizeSelection();
        return true;
    }

    if (k == KeyPress::escapeKey)
    {
        if (onClose) onClose();
        return true;
    }

    return false;
}

void PianoRoll::quantizeSelection()
{
    auto* c = getClip();
    if (c == nullptr || c->notes.empty())
        return;

    ctx.edit.beginClipEdit();
    const double step = gridStep();
    for (int i = 0; i < (int) c->notes.size(); ++i)
    {
        if (! selected.empty() && ! selected.count (i))
            continue;
        auto& n = c->notes[(size_t) i];
        const double end = n.start + n.length;
        n.start = std::round (n.start / step) * step;
        n.length = jmax (step, std::round (end / step) * step - n.start);
    }
    ctx.edit.changed();
    ctx.showStatus ("Quantized " + String (selected.empty() ? "all notes" : String ((int) selected.size()) + " notes")
                    + " to " + gridBox.getText());
}
} // namespace lathe
