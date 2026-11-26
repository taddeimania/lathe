#include "PianoRoll.h"

namespace lathe
{
namespace
{
    // The grid menu's ids. 1-6 are the steps the piano roll always had.
    enum GridId { quarter = 1, eighth, sixteenth, thirtySecond, eighthTriplet, sixteenthTriplet,
                  adaptive = 10, bar, half, sixtyFourth, off };

    bool isLetter (const KeyPress& k, juce_wchar c)
    {
        const int code = k.getKeyCode();
        return CharacterFunctions::toLowerCase (k.getTextCharacter()) == c
            || (code > 0 && code < 256 && CharacterFunctions::toLowerCase ((juce_wchar) code) == c);
    }

    String plural (int n, const String& thing)   { return String (n) + " " + thing + (n == 1 ? "" : "s"); }
}

PianoRoll::PianoRoll (AppContext& c) : ctx (c)
{
    setWantsKeyboardFocus (true);

    title.setFont (theme::font (12.5f, true));
    addAndMakeVisible (title);
    info.setFont (theme::font (11.5f));
    info.setColour (Label::textColourId, theme::textDim);
    addAndMakeVisible (info);

    gridBox.addItem ("Adaptive", adaptive);
    gridBox.addItem ("1 bar", bar);
    gridBox.addItem ("1/2", half);
    gridBox.addItem ("1/4", quarter);
    gridBox.addItem ("1/8", eighth);
    gridBox.addItem ("1/16", sixteenth);
    gridBox.addItem ("1/32", thirtySecond);
    gridBox.addItem ("1/64", sixtyFourth);
    gridBox.addItem ("1/8 triplet", eighthTriplet);
    gridBox.addItem ("1/16 triplet", sixteenthTriplet);
    gridBox.addItem ("Off", off);
    gridBox.setSelectedId (ctx.settings.getIntValue ("pianoRollGrid", sixteenth), dontSendNotification);
    if (gridBox.getSelectedId() == 0)
        gridBox.setSelectedId (sixteenth, dontSendNotification);
    gridBox.setTooltip ("Grid for drawing, moving and quantizing (hold Cmd/Ctrl to ignore it). Adaptive follows the zoom.");
    gridBox.onChange = [this]
    {
        ctx.settings.setValue ("pianoRollGrid", gridBox.getSelectedId());
        repaint();
    };
    gridBox.setWantsKeyboardFocus (false);
    addAndMakeVisible (gridBox);

    quantizeButton.setTooltip ("Snap the selected notes (or all notes) to the grid  (Q)");
    quantizeButton.onClick = [this] { quantizeSelection(); };
    quantizeMenuButton.setTooltip ("Quantize strength, swing, and whether note ends move too");
    quantizeMenuButton.onClick = [this] { showQuantizeOptions(); };
    closeButton.onClick = [this] { if (onClose) onClose(); };
    for (auto* b : { &quantizeButton, &quantizeMenuButton, &closeButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    for (auto* b : { &hearButton, &drawButton })
    {
        b->setClickingTogglesState (true);
        b->setColour (TextButton::buttonOnColourId, theme::accent);
        b->setColour (TextButton::textColourOnId, theme::textDark);
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    hearButton.setToggleState (ctx.settings.getBoolValue ("pianoRollHearNotes", true), dontSendNotification);
    hearButton.setTooltip ("Play notes through the track's instrument as you click, drag and draw them");
    hearButton.onClick = [this]
    {
        ctx.settings.setValue ("pianoRollHearNotes", hearButton.getToggleState());
        if (! hearButton.getToggleState())
            stopHearing();
    };
    drawButton.setTooltip ("Draw: click or drag on empty space to add notes (B). Without it, double-click (and drag) adds one.");
    drawButton.onClick = [this] { repaint(); };

    hbar.addListener (this);
    vbar.addListener (this);
    hbar.setAutoHide (false);
    vbar.setAutoHide (false);
    addAndMakeVisible (hbar);
    addAndMakeVisible (vbar);
}

PianoRoll::~PianoRoll()
{
    stopHearing();
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
    if (changedClip)
        stopHearing();
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

void PianoRoll::zoomToFit()
{
    auto* c = getClip();
    if (c == nullptr)
        return;
    auto area = gridArea();

    double lo = 0.0, hi = c->length;
    int pLo = 55, pHi = 67;
    if (! c->notes.empty())
    {
        std::tie (lo, hi) = noteedit::span (c->notes, selected);
        pLo = 127; pHi = 0;
        const auto sel = selected.empty() ? noteedit::Selection() : selected;
        for (int i = 0; i < (int) c->notes.size(); ++i)
            if (sel.empty() || sel.count (i))
            {
                pLo = jmin (pLo, c->notes[(size_t) i].pitch);
                pHi = jmax (pHi, c->notes[(size_t) i].pitch);
            }
    }
    const double margin = jmax (0.25, (hi - lo) * 0.04);
    pxPerBeat = jlimit (8.0, 600.0, area.getWidth() / jmax (0.25, hi - lo + 2.0 * margin));
    scrollBeat = jmax (0.0, lo - margin);

    const int rows = pHi - pLo + 3;
    noteHeight = jlimit (6, 28, area.getHeight() / jmax (1, rows));
    scrollY = jmax (0, (127 - pHi - 1) * noteHeight - (area.getHeight() - rows * noteHeight) / 2);
    refresh();
}

void PianoRoll::zoomTime (double factor, float anchorX)
{
    const double anchor = xToBeat (anchorX);
    pxPerBeat = jlimit (8.0, 600.0, pxPerBeat * factor);
    scrollBeat = jmax (0.0, anchor - (anchorX - gridArea().getX()) / pxPerBeat);
    refresh();
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
Rectangle<int> PianoRoll::rulerArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarHeight).withHeight (rulerHeight)
                           .withTrimmedLeft (keyboardWidth).withTrimmedRight (10);
}

Rectangle<int> PianoRoll::gridArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarHeight + rulerHeight).withTrimmedLeft (keyboardWidth)
                           .withTrimmedRight (10).withTrimmedBottom (10 + velocityHeight);
}

Rectangle<int> PianoRoll::velocityArea() const
{
    auto g = gridArea();
    return { g.getX(), g.getBottom(), g.getWidth(), velocityHeight };
}

float PianoRoll::beatToX (double beat) const   { return (float) (gridArea().getX() + (beat - scrollBeat) * pxPerBeat); }
double PianoRoll::xToBeat (float x) const      { return scrollBeat + (x - (float) gridArea().getX()) / pxPerBeat; }
int PianoRoll::pitchToY (int pitch) const      { return gridArea().getY() + (127 - pitch) * noteHeight - scrollY; }
int PianoRoll::yToPitch (int y) const          { return jlimit (0, 127, 127 - (y - gridArea().getY() + scrollY) / noteHeight); }

double PianoRoll::gridStep() const
{
    switch (gridBox.getSelectedId())
    {
        case quarter:          return 1.0;
        case eighth:           return 0.5;
        case thirtySecond:     return 0.125;
        case sixtyFourth:      return 0.0625;
        case eighthTriplet:    return 1.0 / 3.0;
        case sixteenthTriplet: return 1.0 / 6.0;
        case half:             return 2.0;
        case bar:              return ctx.edit.beatsPerBar();
        case adaptive:
        case off:
        {
            // The finest step whose lines are at least 16 px apart.
            for (double s : { 0.0625, 0.125, 0.25, 0.5, 1.0 })
                if (s * pxPerBeat >= 16.0)
                    return s;
            return ctx.edit.beatsPerBar();
        }
        default:               return 0.25;
    }
}

bool PianoRoll::snapping() const   { return gridBox.getSelectedId() != off; }

double PianoRoll::snap (double beat, bool bypass) const
{
    if (bypass || ! snapping())
        return beat;
    const double step = gridStep();
    return std::round (beat / step) * step;
}

double PianoRoll::snapDown (double beat, bool bypass) const
{
    if (bypass || ! snapping())
        return beat;
    const double step = gridStep();
    return std::floor (beat / step + 1.0e-6) * step;
}

int PianoRoll::noteAt (Point<int> p, Zone& zone) const
{
    zone = Zone::body;
    auto* c = getClip();
    if (c == nullptr)
        return -1;

    for (int i = (int) c->notes.size(); --i >= 0;)
    {
        auto& n = c->notes[(size_t) i];
        const float x1 = beatToX (n.start), x2 = jmax (x1 + 3.0f, beatToX (n.start + n.length));
        const int y = pitchToY (n.pitch);
        const float px = (float) p.x;
        if (p.y >= y && p.y < y + noteHeight && px >= x1 && px <= x2)
        {
            const float edge = jmin (6.0f, (x2 - x1) * 0.3f);
            zone = px > x2 - edge ? Zone::right : px < x1 + edge ? Zone::left : Zone::body;
            return i;
        }
    }
    return -1;
}

int PianoRoll::velocityStemAt (Point<int> p) const
{
    auto* c = getClip();
    if (c == nullptr)
        return -1;
    int best = -1;
    float bestDistance = 6.0f;
    for (int i = 0; i < (int) c->notes.size(); ++i)
    {
        const float d = std::abs (beatToX (c->notes[(size_t) i].start) + 1.0f - (float) p.x);
        // Ties (a chord) go to the selected note, then the louder.
        if (d < bestDistance || (best >= 0 && std::abs (d - bestDistance) < 0.01f
                                  && (selected.count (i) > selected.count (best)
                                      || c->notes[(size_t) i].velocity > c->notes[(size_t) best].velocity)))
        {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

float PianoRoll::velocityAtY (int y) const
{
    auto vel = velocityArea();
    return jlimit (0.02f, 1.0f, (float) (vel.getBottom() - y) / (float) (vel.getHeight() - 6));
}

double PianoRoll::startMarkerBeat() const
{
    auto* c = getClip();
    return c != nullptr ? ctx.edit.insertMarker - c->start : 0.0;
}

//==============================================================================
String PianoRoll::positionName (double clipBeat) const
{
    auto* c = getClip();
    const double beat = (c != nullptr ? c->start : 0.0) + clipBeat;
    const double perBar = ctx.edit.beatsPerBar();
    const double barIndex = std::floor (beat / perBar + 1.0e-9);
    const double within = beat - barIndex * perBar;
    const double beatIndex = std::floor (within + 1.0e-9);
    const int sixteenth = (int) std::floor ((within - beatIndex) * 4.0 + 1.0e-6) + 1;
    return String ((int) barIndex + 1) + "." + String ((int) beatIndex + 1) + "." + String (sixteenth);
}

String PianoRoll::lengthName (double beats)
{
    // As a note value: 1/8, 3/16, 1/8T (a triplet eighth), else in beats.
    const double whole = beats / 4.0;
    for (int d : { 1, 2, 4, 8, 16, 32, 64, 3, 6, 12, 24, 48 })
    {
        const double n = whole * d;
        const int rounded = roundToInt (n);
        if (rounded >= 1 && std::abs (n - rounded) < 1.0e-4)
        {
            int num = rounded, den = d;
            const int g = std::gcd (num, den);
            num /= g;
            den /= g;
            if (den % 3 == 0)
                return String (num) + "/" + String (den * 2 / 3) + "T";
            return String (num) + "/" + String (den);
        }
    }
    return String (beats, 2) + " beats";
}

String PianoRoll::noteInfo (const Note& n) const
{
    return MidiMessage::getMidiNoteName (n.pitch, true, true, 3) + "    " + positionName (n.start)
           + "    " + lengthName (n.length) + "    velocity " + String (jlimit (1, 127, roundToInt (n.velocity * 127.0f)));
}

void PianoRoll::showInfo (const String& s)
{
    if (info.getText() != s)
        info.setText (s, dontSendNotification);
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
    const double perBar = ctx.edit.beatsPerBar();
    const float endX = beatToX (c->length);

    // ruler: the song's bars, the start marker and the clip's end
    {
        auto ruler = rulerArea();
        g.setColour (theme::header);
        g.fillRect (ruler);
        g.setColour (theme::border);
        g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());

        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (ruler);
        // Label every bar, or every 2nd, 4th... when they're close together.
        int every = 1;
        while (perBar * every * pxPerBeat < 28.0)
            every *= 2;
        const double from = c->start + xToBeat ((float) ruler.getX()), to = c->start + xToBeat ((float) ruler.getRight());
        g.setFont (theme::font (10.5f));
        for (long long b = (long long) std::floor (from / perBar); b * perBar <= to; ++b)
        {
            const float x = beatToX (b * perBar - c->start);
            const bool labelled = b % every == 0;
            g.setColour (labelled ? theme::textDim : theme::gridBar);
            g.drawVerticalLine (roundToInt (x), (float) ruler.getY() + (labelled ? 3.0f : 10.0f), (float) ruler.getBottom());
            if (labelled)
                g.drawText (String (b + 1), roundToInt (x) + 3, ruler.getY(), 40, ruler.getHeight(), Justification::centredLeft);
            if (pxPerBeat >= 14.0)
                for (int beat = 1; beat < (int) std::ceil (perBar); ++beat)
                {
                    g.setColour (theme::gridBar);
                    g.drawVerticalLine (roundToInt (beatToX (b * perBar + beat - c->start)), (float) ruler.getBottom() - 4.0f, (float) ruler.getBottom());
                }
        }

        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRect (Rectangle<float> (endX, (float) ruler.getY(), (float) ruler.getRight() - endX, (float) ruler.getHeight()));
        // the clip's end, a handle to drag
        g.setColour (colour);
        g.fillRect (Rectangle<float> (endX - 3.0f, (float) ruler.getY() + 2.0f, 3.0f, (float) ruler.getHeight() - 4.0f));

        const float mx = beatToX (startMarkerBeat());
        Path tri;
        tri.addTriangle (mx - 5.0f, (float) ruler.getY() + 3.0f, mx + 5.0f, (float) ruler.getY() + 3.0f, mx, (float) ruler.getY() + 11.0f);
        g.setColour (theme::accent);
        g.fillPath (tri);
    }

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
            // Name every C, and every white key when the rows are tall enough to read.
            if (p % 12 == 0 || (! black && noteHeight >= 14))
            {
                g.setColour (p % 12 == 0 ? theme::textDark : theme::textDark.withAlpha (0.55f));
                g.setFont (theme::font (jmin (11.0f, noteHeight - 1.5f)));
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
        }

        // The key the track plays in: rows out of the key darker, the root's rows tinted.
        const auto keyList = ctx.edit.keysFor (t.get());
        if (! keyList.empty())
        {
            double b = xToBeat ((float) area.getX());
            const double bEnd = xToBeat ((float) area.getRight());
            while (b < bEnd)
            {
                const auto* k = keys::at (keyList, c->start + b);
                const auto* next = keys::after (keyList, c->start + b);
                const double segEnd = next != nullptr ? jmin (bEnd, next->beat - c->start) : bEnd;
                if (k != nullptr)
                {
                    const float x1 = beatToX (b), x2 = beatToX (segEnd);
                    for (int p = 0; p < 128; ++p)
                    {
                        const int y = pitchToY (p);
                        if (y > area.getBottom() || y + noteHeight < area.getY())
                            continue;
                        if (! keys::contains (k->root, k->scale, p))
                            g.setColour (Colours::black.withAlpha (0.28f));
                        else if (keys::isRoot (k->root, p))
                            g.setColour (theme::keyColour (k->root).withAlpha (0.12f));
                        else
                            g.setColour (Colours::white.withAlpha (0.025f));
                        g.fillRect (Rectangle<float> (x1, (float) y, x2 - x1, (float) noteHeight));
                    }
                }
                if (next == nullptr || segEnd <= b)
                    break;
                b = segEnd;
            }
        }

        for (int p = 0; p < 128; p += 12)
        {
            g.setColour (theme::gridBar);
            g.drawHorizontalLine (pitchToY (p) + noteHeight - 1, (float) area.getX(), (float) area.getRight());
        }

        // grid: bars and beats counted from the song's start
        double step = gridStep();
        while (step * pxPerBeat < 4.0)
            step *= 2.0;
        for (double b = std::floor (scrollBeat / step) * step; b <= xToBeat ((float) area.getRight()); b += step)
        {
            const double songBeat = c->start + b;
            const bool isBar = std::abs (std::remainder (songBeat, perBar)) < 1.0e-6;
            const bool isBeat = std::abs (std::remainder (songBeat, 1.0)) < 1.0e-6;
            g.setColour (isBar ? theme::gridBar : isBeat ? theme::gridBeat : theme::gridFine);
            g.drawVerticalLine (roundToInt (beatToX (b)), (float) area.getY(), (float) area.getBottom());
        }

        // outside the clip
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRect (Rectangle<float> (endX, (float) area.getY(), (float) area.getRight() - endX, (float) area.getHeight()));

        // the start marker
        g.setColour (theme::accent.withAlpha (0.35f));
        g.drawVerticalLine (roundToInt (beatToX (startMarkerBeat())), (float) area.getY(), (float) area.getBottom());

        // notes
        g.setFont (theme::font (jlimit (8.0f, 11.0f, noteHeight - 2.0f)));
        for (int i = 0; i < (int) c->notes.size(); ++i)
        {
            auto& n = c->notes[(size_t) i];
            const float x1 = beatToX (n.start), x2 = beatToX (n.start + n.length);
            if (x2 < (float) area.getX() - 2.0f || x1 > (float) area.getRight())
                continue;
            auto r = Rectangle<float> (x1, (float) pitchToY (n.pitch), jmax (3.0f, x2 - x1), (float) noteHeight - 1.0f);
            const bool sel = selected.count (i) > 0;
            // Selected notes lit up with a light outline, whatever the track's colour.
            auto fill = colour.withMultipliedBrightness (0.55f + 0.6f * n.velocity);
            if (sel)
                fill = fill.brighter (0.35f);
            else if (! selected.empty())
                fill = fill.withMultipliedSaturation (0.75f).withMultipliedBrightness (0.85f);
            g.setColour (fill);
            g.fillRect (r);
            g.setColour (sel ? theme::text : theme::border);
            g.drawRect (r, sel ? 1.5f : 1.0f);
            if (r.getWidth() >= 26.0f && noteHeight >= 10)
            {
                g.setColour (theme::textOn (fill).withAlpha (0.8f));
                g.drawText (MidiMessage::getMidiNoteName (n.pitch, true, true, 3), r.reduced (3.0f, 0.0f), Justification::centredLeft, false);
            }
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
        g.setColour (theme::gridFine);
        g.drawHorizontalLine (vel.getBottom() - (vel.getHeight() - 6) / 2, (float) vel.getX(), (float) vel.getRight());
        for (int pass = 0; pass < 2; ++pass)   // the selected notes' stems on top
            for (int i = 0; i < (int) c->notes.size(); ++i)
            {
                const bool sel = selected.count (i) > 0;
                if (sel != (pass == 1))
                    continue;
                auto& n = c->notes[(size_t) i];
                const float x = beatToX (n.start);
                const float h = (float) (vel.getHeight() - 6) * n.velocity;
                g.setColour (sel ? theme::text : colour);
                g.fillRect (Rectangle<float> (x, (float) vel.getBottom() - h, 2.0f, h));
                g.fillEllipse (x - 2.0f, (float) vel.getBottom() - h - 3.0f, 6.0f, 6.0f);
            }

        if (mode == Mode::velocityLine)
        {
            g.setColour (theme::text);
            g.drawLine (Line<float> (dragStart.toFloat(), dragNow.toFloat()), 1.5f);
        }
    }
}

void PianoRoll::resized()
{
    auto top = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 3);
    closeButton.setBounds (top.removeFromRight (56));
    top.removeFromRight (8);
    quantizeMenuButton.setBounds (top.removeFromRight (24));
    top.removeFromRight (2);
    quantizeButton.setBounds (top.removeFromRight (72));
    top.removeFromRight (6);
    hearButton.setBounds (top.removeFromRight (84));
    top.removeFromRight (6);
    drawButton.setBounds (top.removeFromRight (50));
    top.removeFromRight (6);
    gridBox.setBounds (top.removeFromRight (110));
    top.removeFromRight (8);
    title.setBounds (top.removeFromLeft (jmin (240, top.getWidth() / 2)));
    info.setBounds (top);

    auto area = gridArea();
    hbar.setBounds (area.getX(), getHeight() - 10, area.getWidth(), 10);
    vbar.setBounds (getWidth() - 10, area.getY(), 10, area.getHeight());
    refresh();
}

//==============================================================================
void PianoRoll::mouseMove (const MouseEvent& e)
{
    auto* c = getClip();
    if (c == nullptr)
        return;
    const auto p = e.getPosition();
    auto cursor = MouseCursor::NormalCursor;

    if (gridArea().contains (p))
    {
        Zone zone;
        const int i = noteAt (p, zone);
        if (i >= 0)
        {
            cursor = zone == Zone::body ? MouseCursor::DraggingHandCursor : MouseCursor::LeftRightResizeCursor;
            showInfo (noteInfo (c->notes[(size_t) i]));
        }
        else
        {
            if (drawButton.getToggleState())
                cursor = MouseCursor::CrosshairCursor;
            showInfo (MidiMessage::getMidiNoteName (yToPitch (p.y), true, true, 3) + "    " + positionName (snapDown (xToBeat ((float) p.x), false)));
        }
    }
    else if (rulerArea().contains (p))
    {
        if (std::abs ((float) p.x - beatToX (c->length)) <= 5.0f)
        {
            cursor = MouseCursor::LeftRightResizeCursor;
            showInfo ("Drag to change the clip's length (" + lengthName (c->length) + ")");
        }
        else
        {
            showInfo ("Click to move the start marker to " + positionName (snap (xToBeat ((float) p.x), false)));
        }
    }
    else if (velocityArea().contains (p))
    {
        const int i = velocityStemAt (p);
        cursor = i >= 0 ? MouseCursor::UpDownResizeCursor : MouseCursor::NormalCursor;
        showInfo (i >= 0 ? noteInfo (c->notes[(size_t) i]) : String ("Drag across the lane to draw a velocity ramp"));
    }
    else if (onKeyboard (p))
    {
        showInfo (MidiMessage::getMidiNoteName (yToPitch (p.y), true, true, 3) + ": click to hear it and select its notes");
    }
    setMouseCursor (cursor);
}

void PianoRoll::mouseExit (const MouseEvent&)
{
    if (mode == Mode::none)
        showInfo ({});
}

void PianoRoll::mouseDown (const MouseEvent& e)
{
    grabKeyboardFocus();
    auto* c = getClip();
    if (c == nullptr)
        return;

    const auto p = e.getPosition();
    dragStart = dragNow = p;
    dragChanged = false;
    copyOnDrag = false;
    dragOriginal = c->notes;
    dragOriginalLength = c->length;
    dragNote = -1;
    mode = Mode::none;

    if (e.mods.isPopupMenu())
    {
        Zone zone;
        if (gridArea().contains (p))
            if (const int i = noteAt (p, zone); i >= 0 && ! selected.count (i))
                selected = { i };
        repaint();
        showContextMenu();
        return;
    }

    if (rulerArea().contains (p))
    {
        mode = std::abs ((float) p.x - beatToX (c->length)) <= 5.0f ? Mode::clipEnd : Mode::marker;
        mouseDrag (e);
        return;
    }

    if (velocityArea().contains (p))
    {
        if (const int i = velocityStemAt (p); i >= 0)
        {
            if (! selected.count (i))
                selected = { i };
            velocityTargets = selected;
            mode = Mode::velocity;
            showInfo (noteInfo (c->notes[(size_t) i]));
        }
        else
        {
            // A ramp across the selected notes, or across every note when none are.
            velocityTargets = selected;
            if (selected.empty())
                for (int n = 0; n < (int) c->notes.size(); ++n)
                    velocityTargets.insert (n);
            mode = Mode::velocityLine;
        }
        repaint();
        return;
    }

    if (onKeyboard (p))
    {
        mode = Mode::keyboard;
        const int pitch = yToPitch (e.y);
        hear (pitch, 0.8f);
        const auto ofPitch = noteedit::notesOfPitch (c->notes, pitch);
        if (! e.mods.isShiftDown())
            selected.clear();
        selected.insert (ofPitch.begin(), ofPitch.end());
        if (! ofPitch.empty())
            showInfo (plural ((int) ofPitch.size(), MidiMessage::getMidiNoteName (pitch, true, true, 3) + " note") + " selected");
        repaint();
        return;
    }

    if (! gridArea().contains (p))
        return;

    const bool noSnap = e.mods.isCommandDown();
    Zone zone;
    const int i = noteAt (p, zone);

    if (i >= 0 && e.getNumberOfClicks() >= 2)
    {
        selected.clear();
        editNotes ({}, [i] (Clip& clip) { noteedit::remove (clip.notes, { i }); });
        return;
    }

    if (i < 0)
    {
        if (drawButton.getToggleState() || e.getNumberOfClicks() >= 2)
        {
            // Draw: a note of the last length, and dragging sets how long it is.
            const double start = snapDown (xToBeat ((float) e.x), noSnap);
            if (start < 0.0 || start >= c->length)
                return;
            const int pitch = yToPitch (e.y);
            ctx.edit.beginClipEdit();
            c->notes.push_back ({ pitch, start, lastLength > 0.0 ? lastLength : gridStep(), 0.8f });
            dragNote = (int) c->notes.size() - 1;
            selected = { dragNote };
            dragOriginal = c->notes;
            dragChanged = true;
            mode = Mode::draw;
            hear (pitch, 0.8f);
            showInfo (noteInfo (c->notes.back()));
            repaint();
            return;
        }

        if (! e.mods.isShiftDown())
            selected.clear();
        mode = Mode::marquee;
        marquee = { e.x, e.y, 0, 0 };
        repaint();
        return;
    }

    if (e.mods.isShiftDown() && selected.count (i))
    {
        selected.erase (i);
        repaint();
        return;
    }
    narrowOnRelease = selected.count (i) > 0 && selected.size() > 1 && ! e.mods.isShiftDown() && ! e.mods.isAltDown();
    if (! selected.count (i))
    {
        if (! e.mods.isShiftDown())
            selected.clear();
        selected.insert (i);
    }
    mode = zone == Zone::left ? Mode::resizeStart : zone == Zone::right ? Mode::resizeEnd : Mode::move;
    copyOnDrag = mode == Mode::move && e.mods.isAltDown();
    dragNote = i;
    if (mode == Mode::move)
        hear (c->notes[(size_t) i].pitch, c->notes[(size_t) i].velocity);
    showInfo (noteInfo (c->notes[(size_t) i]));
    repaint();
}

void PianoRoll::mouseDrag (const MouseEvent& e)
{
    auto* c = getClip();
    if (c == nullptr || mode == Mode::none)
        return;
    dragNow = e.getPosition();

    if (mode == Mode::keyboard)
    {
        if (const int p = yToPitch (e.y); p != heardPitch)
            hear (p, 0.8f);
        return;
    }

    auto beginChange = [&]
    {
        if (! dragChanged)
        {
            dragChanged = true;
            // Record the state from before the drag for undo.
            auto now = c->notes;
            const double nowLength = c->length;
            c->notes = dragOriginal;
            c->length = dragOriginalLength;
            ctx.edit.beginClipEdit();
            c->notes = now;
            c->length = nowLength;
        }
    };

    const bool noSnap = e.mods.isCommandDown();
    const double minLength = noSnap || ! snapping() ? 0.01 : gridStep() * 0.5;

    if (mode == Mode::marker)
    {
        const double beat = jmax (-c->start, snap (xToBeat ((float) e.x), noSnap));
        ctx.edit.insertMarker = c->start + beat;
        ctx.engine.seekTo (ctx.edit.insertMarker);
        ctx.edit.sendChangeMessage();
        showInfo ("Start marker at " + positionName (beat));
    }
    else if (mode == Mode::clipEnd)
    {
        // As long as there's room before the track's next clip.
        double maxLength = 100000.0;
        if (auto t = ctx.edit.findTrack (trackId))
            for (auto& other : t->clips)
                if (other.id != c->id && other.start >= c->start + 1.0e-9)
                    maxLength = jmin (maxLength, other.start - c->start);
        const double length = jlimit (jmin (gridStep(), maxLength), maxLength, snap (xToBeat ((float) e.x), noSnap));
        if (std::abs (length - c->length) > 1.0e-9)
        {
            beginChange();
            c->length = length;
            refresh();
        }
        // In bars and beats: "2 bars", "1 bar 3 beats", "3.5 beats".
        const double perBar = ctx.edit.beatsPerBar();
        const int wholeBars = (int) std::floor (c->length / perBar + 1.0e-6);
        const double beats = c->length - wholeBars * perBar;
        String text = wholeBars > 0 ? plural (wholeBars, "bar") : String();
        if (beats > 1.0e-6)
            text << (text.isEmpty() ? "" : " ")
                 << (std::abs (beats - std::round (beats)) < 1.0e-6 ? plural (roundToInt (beats), "beat") : String (beats, 2) + " beats");
        showInfo ("Clip length " + text);
    }
    else if (mode == Mode::marquee)
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
        if (e.mods.isShiftDown())
            hits.insert (selected.begin(), selected.end());
        selected = hits;
        showInfo (plural ((int) selected.size(), "note") + " selected");
    }
    else if (mode == Mode::velocity)
    {
        const float amount = velocityAtY (e.y) - velocityAtY (dragStart.y);
        beginChange();
        noteedit::offsetVelocity (c->notes, dragOriginal, velocityTargets, amount);
        const int shown = velocityTargets.empty() ? -1 : *velocityTargets.begin();
        if (shown >= 0 && shown < (int) c->notes.size())
            showInfo ("Velocity " + String (roundToInt (c->notes[(size_t) shown].velocity * 127.0f))
                      + (velocityTargets.size() > 1 ? "  (" + String (amount >= 0 ? "+" : "") + String (roundToInt (amount * 127.0f))
                                                          + " on " + plural ((int) velocityTargets.size(), "note") + ")"
                                                    : String()));
    }
    else if (mode == Mode::velocityLine)
    {
        beginChange();
        c->notes = dragOriginal;
        const float v1 = velocityAtY (dragStart.y), v2 = velocityAtY (e.y);
        const int n = noteedit::velocityLine (c->notes, velocityTargets, xToBeat ((float) dragStart.x), v1,
                                              xToBeat ((float) e.x), v2);
        showInfo ("Velocity " + String (roundToInt (v1 * 127.0f)) + " to " + String (roundToInt (v2 * 127.0f))
                  + " on " + plural (n, "note"));
    }
    else if (mode == Mode::draw)
    {
        if (e.getDistanceFromDragStart() > 2 && dragNote >= 0 && dragNote < (int) c->notes.size())
        {
            auto& n = c->notes[(size_t) dragNote];
            const double end = snap (xToBeat ((float) e.x), noSnap);
            n.length = jmax (noSnap || ! snapping() ? 0.01 : gridStep(), end - n.start);
            showInfo (noteInfo (n));
        }
    }
    else if (e.getDistanceFromDragStart() > 2 || dragChanged)
    {
        if (! dragChanged)
        {
            beginChange();
            if (copyOnDrag)
            {
                // Alt-drag: leave the notes where they are and drag copies of them.
                std::set<int> copies;
                for (int i : selected)
                {
                    c->notes.push_back (c->notes[(size_t) i]);
                    const int copy = (int) c->notes.size() - 1;
                    if (i == dragNote)
                        dragNote = copy;
                    copies.insert (copy);
                }
                selected = copies;
                dragOriginal = c->notes;
            }
        }
        const double deltaBeats = (e.x - dragStart.x) / pxPerBeat;

        if (mode == Mode::move)
        {
            const int deltaPitch = (dragStart.y - e.y + (dragStart.y > e.y ? noteHeight / 2 : -noteHeight / 2)) / noteHeight;
            // Snap the moved group by its first note so relative timing is kept.
            double anchor = 1.0e9;
            for (int i : selected)
                anchor = jmin (anchor, dragOriginal[(size_t) i].start);
            const double snappedDelta = jmax (-anchor, snap (anchor + deltaBeats, noSnap) - anchor);

            for (int i : selected)
            {
                auto& o = dragOriginal[(size_t) i];
                auto& n = c->notes[(size_t) i];
                n.start = o.start + snappedDelta;
                n.pitch = jlimit (0, 127, o.pitch + deltaPitch);
            }

            // Follow the grabbed note: sound its new pitch when it changes.
            if (dragNote >= 0 && dragNote < (int) c->notes.size() && heardPitch >= 0
                 && c->notes[(size_t) dragNote].pitch != heardPitch)
                hear (c->notes[(size_t) dragNote].pitch, c->notes[(size_t) dragNote].velocity);
        }
        else if (mode == Mode::resizeEnd)
        {
            for (int i : selected)
            {
                auto& o = dragOriginal[(size_t) i];
                const double end = snap (o.start + o.length + deltaBeats, noSnap);
                c->notes[(size_t) i].length = jmax (minLength, end - o.start);
            }
        }
        else if (mode == Mode::resizeStart)
        {
            for (int i : selected)
            {
                auto n = dragOriginal[(size_t) i];
                noteedit::moveStart (n, snap (n.start + deltaBeats, noSnap), minLength);
                c->notes[(size_t) i] = n;
            }
        }

        if (dragNote >= 0 && dragNote < (int) c->notes.size())
            showInfo (noteInfo (c->notes[(size_t) dragNote]));
    }

    repaint();
}

void PianoRoll::mouseUp (const MouseEvent&)
{
    auto* c = getClip();
    const bool validNote = c != nullptr && dragNote >= 0 && dragNote < (int) c->notes.size();
    // New notes take the length of the last one drawn or resized.
    if (validNote && (mode == Mode::draw || ((mode == Mode::resizeEnd || mode == Mode::resizeStart) && dragChanged)))
        lastLength = c->notes[(size_t) dragNote].length;

    // A click (no drag) on a note of a selection selects just that note.
    if (narrowOnRelease && ! dragChanged && validNote)
        selected = { dragNote };
    narrowOnRelease = false;

    // A held note stops on release; a blip from a double-click stops on its own timer.
    if (mode == Mode::keyboard || mode == Mode::move || mode == Mode::draw)
        stopHearing();
    dragNote = -1;
    if (dragChanged)
        ctx.edit.changed();
    mode = Mode::none;
    dragChanged = false;
    copyOnDrag = false;
    repaint();
}

void PianoRoll::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomTime (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, (float) e.x);
        return;
    }

    if (e.mods.isAltDown())
    {
        // Taller or shorter rows, keeping the pitch under the mouse where it is.
        const float d = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
        if (d == 0.0f)
            return;
        const int pitch = yToPitch (e.y);
        const int oldY = pitchToY (pitch);
        noteHeight = jlimit (6, 28, noteHeight + (d > 0 ? 1 : -1));
        scrollY = jlimit (0, jmax (0, 128 * noteHeight - gridArea().getHeight()),
                          gridArea().getY() + (127 - pitch) * noteHeight - oldY);
        refresh();
        return;
    }

    float dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown() && dx == 0.0f)
        std::swap (dx, dy);
    if (dx != 0.0f) hbar.setCurrentRangeStart (hbar.getCurrentRangeStart() - dx * 200.0 / pxPerBeat);
    if (dy != 0.0f) vbar.setCurrentRangeStart (vbar.getCurrentRangeStart() - dy * 200.0);
}

void PianoRoll::mouseMagnify (const MouseEvent& e, float scaleFactor)
{
    zoomTime (scaleFactor, (float) e.x);
}

//==============================================================================
void PianoRoll::editNotes (const String& status, const std::function<void (Clip&)>& fn)
{
    auto* c = getClip();
    if (c == nullptr)
        return;
    ctx.edit.beginClipEdit();
    fn (*c);
    ctx.edit.changed();
    if (status.isNotEmpty())
        ctx.showStatus (status);
    refresh();
}

std::vector<Note>& PianoRoll::clipboard()
{
    static std::vector<Note> notes;
    return notes;
}

bool PianoRoll::editsNotes() const
{
    return isShowing() && hasKeyboardFocus (true) && hasClip();
}

void PianoRoll::copyNotes()
{
    auto* c = getClip();
    if (c == nullptr || selected.empty())
    {
        ctx.showStatus ("Select some notes to copy.");
        return;
    }
    clipboard() = noteedit::copy (c->notes, selected);
    ctx.showStatus ("Copied " + plural ((int) selected.size(), "note") + ".");
}

void PianoRoll::cutNotes()
{
    if (selected.empty())
    {
        ctx.showStatus ("Select some notes to cut.");
        return;
    }
    copyNotes();
    const auto sel = selected;
    selected.clear();
    editNotes ("Cut " + plural ((int) sel.size(), "note") + ".", [sel] (Clip& c) { noteedit::remove (c.notes, sel); });
}

void PianoRoll::pasteNotes()
{
    auto* c = getClip();
    if (c == nullptr || clipboard().empty())
        return;

    // At the start marker when it's in the clip; otherwise after the selection, or at the start.
    double at = startMarkerBeat();
    if (at < 0.0 || at >= c->length)
        at = selected.empty() ? 0.0 : snap (noteedit::span (c->notes, selected).second, false);

    // Nothing would land inside the clip: say so rather than make an empty undo step.
    auto trial = c->notes;
    if (noteedit::paste (trial, clipboard(), at, c->length).empty())
    {
        ctx.showStatus ("Nothing to paste there: it's past the clip's end. Click the ruler to move the start marker.");
        return;
    }

    noteedit::Selection added;
    editNotes ({}, [&] (Clip& clip) { added = noteedit::paste (clip.notes, clipboard(), at, clip.length); });
    const int left = (int) clipboard().size() - (int) added.size();
    selected = added;
    ctx.showStatus ("Pasted " + plural ((int) added.size(), "note") + " at " + positionName (at)
                    + (left > 0 ? " (" + String (left) + " past the clip's end left out)" : String()) + ".");

    // Move the marker on past them, so pasting again adds the next copy.
    if (! ctx.engine.isPlaying())
    {
        double length = 0.0;
        for (auto& n : clipboard())
            length = jmax (length, n.start + n.length);
        const double step = gridStep();
        ctx.edit.insertMarker = c->start + at + jmax (step, std::ceil (length / step - 1.0e-6) * step);
        ctx.engine.seekTo (ctx.edit.insertMarker);
        ctx.edit.sendChangeMessage();
    }
    repaint();
}

void PianoRoll::duplicateNotes()
{
    if (selected.empty())
    {
        ctx.showStatus ("Select some notes to duplicate.");
        return;
    }
    const auto sel = selected;
    noteedit::Selection added;
    editNotes ({}, [&] (Clip& c) { added = noteedit::duplicate (c.notes, sel, gridStep(), c.length); });
    if (added.empty())
    {
        ctx.showStatus ("No room to duplicate those notes before the clip's end.");
        return;
    }
    selected = added;
    ctx.showStatus ("Duplicated " + plural ((int) added.size(), "note") + ".");
    repaint();
}

void PianoRoll::deleteNotes()
{
    if (selected.empty())
        return;
    const auto sel = selected;
    selected.clear();
    editNotes ({}, [sel] (Clip& c) { noteedit::remove (c.notes, sel); });
}

void PianoRoll::selectAllNotes()
{
    if (auto* c = getClip())
    {
        selected.clear();
        for (int i = 0; i < (int) c->notes.size(); ++i)
            selected.insert (i);
        repaint();
    }
}

void PianoRoll::splitNotes()
{
    if (selected.empty())
    {
        ctx.showStatus ("Select the notes to split.");
        return;
    }
    const auto sel = selected;
    editNotes ({}, [&] (Clip& c) { selected = noteedit::splitAtGrid (c.notes, sel, gridStep()); });
    ctx.showStatus ("Split " + plural ((int) sel.size(), "note") + " at every " + lengthName (gridStep()) + ".");
}

void PianoRoll::glueNotes()
{
    if (selected.size() < 2)
    {
        ctx.showStatus ("Select two or more notes of a pitch to glue them.");
        return;
    }
    const auto sel = selected;
    editNotes ({}, [&] (Clip& c) { selected = noteedit::glue (c.notes, sel); });
    ctx.showStatus ("Glued the selected notes of each pitch.");
}

//==============================================================================
noteedit::QuantizeOptions PianoRoll::quantizeOptions() const
{
    noteedit::QuantizeOptions o;
    o.gridStep = gridStep();
    o.strength = (float) ctx.settings.getIntValue ("pianoRollQuantizeStrength", 100) / 100.0f;
    o.swing = (float) ctx.settings.getIntValue ("pianoRollQuantizeSwing", 0) / 100.0f;
    o.ends = ctx.settings.getBoolValue ("pianoRollQuantizeEnds", true);
    return o;
}

void PianoRoll::quantizeSelection()
{
    auto* c = getClip();
    if (c == nullptr || c->notes.empty())
        return;

    const auto o = quantizeOptions();
    const auto sel = selected;
    String how = lengthName (o.gridStep);
    if (o.strength < 0.999f) how << ", " << roundToInt (o.strength * 100.0f) << "%";
    if (o.swing > 0.001f)    how << ", swing " << roundToInt (o.swing * 100.0f) << "%";
    if (! o.ends)            how << ", starts only";
    editNotes ("Quantized " + (sel.empty() ? String ("all notes") : plural ((int) sel.size(), "note")) + " to " + how,
               [&] (Clip& clip) { noteedit::quantize (clip.notes, sel, o); });
}

void PianoRoll::showQuantizeOptions()
{
    const auto o = quantizeOptions();
    PopupMenu strength, swing, m;
    for (int s : { 100, 75, 50, 25 })
        strength.addItem (100 + s, String (s) + "%", true, roundToInt (o.strength * 100.0f) == s);
    for (int s : { 0, 25, 50, 75, 100 })
        swing.addItem (300 + s, s == 0 ? String ("None") : s == 100 ? String ("100% (triplet feel)") : String (s) + "%",
                       true, roundToInt (o.swing * 100.0f) == s);
    m.addSectionHeader ("Quantize");
    m.addSubMenu ("Strength", strength);
    m.addSubMenu ("Swing", swing);
    m.addItem (1, "Move note ends too", true, o.ends);

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&quantizeMenuButton),
                     [safe = Component::SafePointer<PianoRoll> (this)] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& s = safe->ctx.settings;
        if (r == 1)                    s.setValue ("pianoRollQuantizeEnds", ! s.getBoolValue ("pianoRollQuantizeEnds", true));
        else if (r >= 100 && r < 300)  s.setValue ("pianoRollQuantizeStrength", r - 100);
        else if (r >= 300)             s.setValue ("pianoRollQuantizeSwing", r - 300);
    });
}

void PianoRoll::showContextMenu()
{
    auto* c = getClip();
    if (c == nullptr)
        return;

    const String cmd =
       #if JUCE_MAC
        "Cmd+";
       #else
        "Ctrl+";
       #endif
    const bool any = ! selected.empty();
    auto item = [] (PopupMenu& m, int id, const String& text, const String& keys, bool enabled = true)
    {
        PopupMenu::Item i (text);
        i.itemID = id;
        i.isEnabled = enabled;
        i.shortcutKeyDescription = keys;
        m.addItem (i);
    };

    // The pitch right-clicked, to select all its notes.
    const int pitch = yToPitch (dragStart.y);
    const String pitchName = MidiMessage::getMidiNoteName (pitch, true, true, 3);

    PopupMenu m;
    item (m, 1, "Cut", cmd + "X", any);
    item (m, 2, "Copy", cmd + "C", any);
    item (m, 3, "Paste", cmd + "V", canPasteNotes());
    item (m, 4, "Duplicate", cmd + "D", any);
    item (m, 5, "Delete", "Delete", any);
    m.addSeparator();
    item (m, 6, "Select All", cmd + "A", ! c->notes.empty());
    item (m, 7, "Select All " + pitchName + " Notes", {}, ! noteedit::notesOfPitch (c->notes, pitch).empty());
    m.addSeparator();
    item (m, 10, any ? "Quantize Selection" : "Quantize All", "Q", ! c->notes.empty());
    item (m, 11, "Legato", {}, selected.size() > 1);
    item (m, 12, "Split at Grid", cmd + "E", any);
    item (m, 13, "Glue", cmd + "J", selected.size() > 1);
    item (m, 14, "Reverse", {}, selected.size() > 1);
    item (m, 15, "Invert", {}, selected.size() > 1);
    m.addSeparator();
    item (m, 20, "Zoom to Fit", "Z");

    m.showMenuAsync (PopupMenu::Options().withMousePosition(), [safe = Component::SafePointer<PianoRoll> (this), pitch] (int r)
    {
        if (safe == nullptr || safe->getClip() == nullptr)
            return;
        auto& self = *safe;
        const auto sel = self.selected;
        const int n = (int) sel.size();
        switch (r)
        {
            case 1:  self.cutNotes(); break;
            case 2:  self.copyNotes(); break;
            case 3:  self.pasteNotes(); break;
            case 4:  self.duplicateNotes(); break;
            case 5:  self.deleteNotes(); break;
            case 6:  self.selectAllNotes(); break;
            case 7:  self.selected = noteedit::notesOfPitch (self.getClip()->notes, pitch); self.repaint(); break;
            case 10: self.quantizeSelection(); break;
            case 11: self.editNotes ("Legato on " + plural (n, "note") + ".", [sel] (Clip& clip) { noteedit::legato (clip.notes, sel); }); break;
            case 12: self.splitNotes(); break;
            case 13: self.glueNotes(); break;
            case 14: self.editNotes ("Reversed " + plural (n, "note") + ".", [sel] (Clip& clip) { noteedit::reverse (clip.notes, sel); }); break;
            case 15: self.editNotes ("Inverted " + plural (n, "note") + ".", [sel] (Clip& clip) { noteedit::invert (clip.notes, sel); }); break;
            case 20: self.zoomToFit(); break;
            default: break;
        }
    });
}

bool PianoRoll::keyPressed (const KeyPress& k)
{
    auto* c = getClip();
    if (c == nullptr)
        return false;
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();

    if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey)
    {
        deleteNotes();
        return true;
    }

    if (mods.isCommandDown() && ! mods.isAltDown())
    {
        if (isLetter (k, 'a') && ! mods.isShiftDown()) { selectAllNotes(); return true; }
        if (isLetter (k, 'c')) { copyNotes(); return true; }
        if (isLetter (k, 'x')) { cutNotes(); return true; }
        if (isLetter (k, 'v')) { pasteNotes(); return true; }
        if (isLetter (k, 'd')) { duplicateNotes(); return true; }
        if (isLetter (k, 'e') && ! mods.isShiftDown()) { splitNotes(); return true; }
        if (isLetter (k, 'j')) { glueNotes(); return true; }
        return false;
    }

    if ((code == KeyPress::upKey || code == KeyPress::downKey) && ! selected.empty() && ! mods.isCommandDown())
    {
        const int delta = (code == KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? 12 : 1);
        const auto sel = selected;
        editNotes ({}, [sel, delta] (Clip& clip) { noteedit::transpose (clip.notes, sel, delta); });
        const auto& first = c->notes[(size_t) *selected.begin()];
        blip (first.pitch, first.velocity);
        showInfo (noteInfo (first));
        return true;
    }

    if ((code == KeyPress::leftKey || code == KeyPress::rightKey) && ! selected.empty() && ! mods.isCommandDown())
    {
        const double sign = code == KeyPress::rightKey ? 1.0 : -1.0;
        const auto sel = selected;
        if (mods.isShiftDown())
        {
            // Longer or shorter by a grid step (Alt: a 1/128 note).
            const double step = mods.isAltDown() ? 1.0 / 32.0 : gridStep();
            editNotes ({}, [sel, sign, step] (Clip& clip) { noteedit::lengthen (clip.notes, sel, sign * step, step); });
        }
        else
        {
            // Earlier or later by a grid step (Alt: a 1/128 note, off the grid).
            const double step = mods.isAltDown() ? 1.0 / 32.0 : gridStep();
            editNotes ({}, [sel, sign, step] (Clip& clip) { noteedit::nudge (clip.notes, sel, sign * step); });
        }
        showInfo (noteInfo (c->notes[(size_t) *selected.begin()]));
        return true;
    }

    if (mods.isAnyModifierKeyDown())
        return false;

    if (isLetter (k, 'q')) { quantizeSelection(); return true; }
    if (isLetter (k, 'z')) { zoomToFit(); return true; }
    if (isLetter (k, 'b'))
    {
        drawButton.setToggleState (! drawButton.getToggleState(), sendNotification);
        ctx.showStatus (drawButton.getToggleState() ? "Draw: click or drag on empty space to add notes." : "Draw off: drag on empty space to select.");
        return true;
    }

    if (k == KeyPress::escapeKey)
    {
        if (onClose) onClose();
        return true;
    }

    return false;
}

//==============================================================================
bool PianoRoll::onKeyboard (Point<int> p) const
{
    const auto g = gridArea();
    return p.x >= 0 && p.x < keyboardWidth && p.y >= g.getY() && p.y < g.getBottom();
}

bool PianoRoll::hearNotes() const
{
    return hearButton.getToggleState() && trackId >= 0;
}

void PianoRoll::hear (int pitch, float velocity)
{
    if (! hearNotes())
        return;
    stopHearing();
    heardPitch = jlimit (0, 127, pitch);
    heardTrack = trackId;
    ctx.engine.previewNote (heardTrack, heardPitch, jlimit (0.05f, 1.0f, velocity));
}

void PianoRoll::stopHearing()
{
    if (heardPitch >= 0 && heardTrack >= 0)
        ctx.engine.previewNote (heardTrack, heardPitch, 0.0f);
    heardPitch = -1;
    heardTrack = -1;
}

void PianoRoll::blip (int pitch, float velocity)
{
    if (! hearNotes())
        return;
    hear (pitch, velocity);
    const int p = heardPitch, track = heardTrack;
    Timer::callAfterDelay (250, [safe = Component::SafePointer<PianoRoll> (this), p, track]
    {
        // Stop this note only if nothing else has taken over since.
        if (safe != nullptr && safe->heardPitch == p && safe->heardTrack == track && safe->mode == Mode::none)
            safe->stopHearing();
    });
}
} // namespace lathe
