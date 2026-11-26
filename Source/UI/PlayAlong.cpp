#include "PlayAlong.h"
#include "Theme.h"
#include "../Engine/Engine.h"
#include "../Model/Chords.h"

namespace lathe
{
Component::SafePointer<PlayAlongWindow> PlayAlongWindow::current;

PlayAlongWindow::State PlayAlongWindow::stateAt (const Edit& edit, double beat, const Track* track)
{
    State s;
    const auto list = edit.keysFor (track);
    if (auto* k = keys::at (list, beat))    s.key = *k;
    if (auto* k = keys::after (list, beat)) s.next = *k;
    if (s.next)
    {
        const double bar = edit.beatsPerBar();
        const double until = s.next->beat - beat;
        if (until <= bar)
            s.nextFade = (float) jlimit (0.0, 1.0, 1.0 - until / bar);
    }
    return s;
}

std::shared_ptr<Track> PlayAlongWindow::autoFollow (const Edit& edit, const String& instrument)
{
    static const std::map<String, StringArray> words {
        { "Guitar",   { "guitar", "gtr", "git" } },
        { "Bass",     { "bass" } },
        { "Ukulele",  { "ukulele", "uke" } },
        { "Keyboard", { "keyboard", "keys", "key", "piano", "rhodes", "organ", "synth" } },
    };
    auto it = words.find (instrument);
    if (it == words.end())
        return {};
    for (auto& t : edit.tracks)
    {
        if (t->keys.empty())
            continue;
        const auto name = t->name.toLowerCase();
        // Whole words or the start of one: "Bass DI" and "Gtr 2" fit; "Bassoon" doesn't fit Bass.
        auto tokens = StringArray::fromTokens (name.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 "), " ", "");
        for (auto& w : it->second)
            for (auto& tok : tokens)
                if (tok == w || (tok.startsWith (w) && tok.substring (w.length()).containsOnly ("0123456789")))
                    return t;
    }
    return {};
}

namespace
{
    // The window's sections: the four instruments, then the chord sheet.
    constexpr int chordsSection = 4;

    const StringArray& sectionNames()
    {
        static const StringArray names = []
        {
            StringArray n;
            for (auto& i : fretboard::instruments())
                n.add (i.name);
            n.add ("Chords");
            return n;
        }();
        return names;
    }


    String barText (double beat, double barLen)
    {
        return String (beat / barLen + 1.0, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }

    String tuningText (const fretboard::Instrument& inst)
    {
        if (inst.strings.empty())
            return "C3 to B4";
        String t;
        for (auto p : inst.strings)
            t << (t.isEmpty() ? "" : " ") << keys::noteName (p, false);
        return t + ", frets 0 to " + String (inst.frets);
    }

    float textWidth (const Font& f, const String& s)   { return GlyphArrangement::getStringWidth (f, s); }

    /** A section of the window: an inset panel, as Compare and Undo History draw theirs. */
    void drawCard (Graphics& g, Rectangle<float> r)
    {
        g.setColour (theme::lane);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

    /** A small tag, like Undo History's "You": a flat rounded box with bold text. */
    void drawTag (Graphics& g, Rectangle<float> r, const String& text, Colour fill, Colour ink, float size = 12.0f)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (ink);
        g.setFont (theme::font (size, true));
        g.drawText (text, r, Justification::centred);
    }

    /** A note: a dot, a ring and its name. */
    void drawDot (Graphics& g, Point<float> c, float radius, const String& label, Colour fill, Colour ring,
                  float ringWidth, Colour textColour, float alpha, float fontScale = 1.05f)
    {
        auto circle = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
        g.setColour (fill.withMultipliedAlpha (alpha));
        g.fillEllipse (circle);
        if (ringWidth > 0.0f)
        {
            g.setColour (ring.withMultipliedAlpha (alpha));
            g.drawEllipse (circle.reduced (ringWidth * 0.5f), ringWidth);
        }
        if (label.isNotEmpty())
        {
            g.setColour (textColour.withMultipliedAlpha (alpha));
            g.setFont (theme::font (jmin (12.0f, radius * fontScale), true));
            g.drawText (label, circle.expanded (4.0f, 0.0f), Justification::centred, false);
        }
    }

    // The instruments' few colours of their own, flat as the rest of Lathe.
    const Colour noteFill  { 0xffe8e8e8 };   // a note of the key (as the keyboard's white keys)
    const Colour wood      { 0xff3a3027 };
    const Colour inlay     { 0xff6b5e50 };
    const Colour fretWire  { 0xff8a8a8a };
    const Colour nut       { 0xffd8d0c0 };
    const Colour stringCol { 0xffb8b0a0 };

    /** One instrument, drawn for a key (and, fading in, the next one). On the guitar, a chord
        picked on the chord sheet stands out, the rest of the key dimmed behind it. */
    void drawInstrument (Graphics& g, Rectangle<float> area, const fretboard::Instrument& inst,
                         const KeyChange* key, const KeyChange* next, float fade, const chords::Voicing* chord)
    {
        const auto keyColour = key != nullptr ? theme::keyColour (key->root) : theme::textDim;
        const auto nextColour = next != nullptr ? theme::keyColour (next->root) : theme::textDim;

        // A note: the root filled with the key's colour, the other notes of the key a light dot
        // ringed with it. Notes only in the next key are rings that strengthen as it nears.
        // In the picked chord: a bright halo; the key's other notes step back.
        auto drawNote = [&] (Point<float> c, float radius, int pitch, bool inChord)
        {
            const bool now = key != nullptr && keys::contains (key->root, key->scale, pitch);
            const bool later = next != nullptr && fade > 0.0f && keys::contains (next->root, next->scale, pitch);
            if (! now && ! later && ! inChord)
                return;
            const float chordDim = chord != nullptr && ! inChord ? 0.32f : 1.0f;
            if (inChord)
            {
                auto halo = Rectangle<float> (radius * 2.0f + 7.0f, radius * 2.0f + 7.0f).withCentre (c);
                g.setColour (theme::text);
                g.drawEllipse (halo, 2.0f);
            }
            if (now || inChord)
            {
                const bool root = key != nullptr && keys::isRoot (key->root, pitch);
                // A note leaving with the key change dims as it goes.
                const float alpha = (inChord || later || fade <= 0.0f ? 1.0f : 1.0f - 0.6f * fade) * chordDim;
                const auto fill = root ? keyColour : noteFill;
                drawDot (g, c, radius, keys::spell (key->root, key->scale, pitch), fill, keyColour, root ? 0.0f : 2.0f,
                         root ? theme::textOn (keyColour) : theme::textDark, alpha);
            }
            else
            {
                const bool root = keys::isRoot (next->root, pitch);
                auto circle = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
                g.setColour (nextColour.withAlpha ((0.25f + 0.1f * fade) * fade * chordDim));
                g.fillEllipse (circle);
                g.setColour (nextColour.withAlpha ((0.2f + 0.75f * fade) * chordDim));
                g.drawEllipse (circle.reduced (1.0f), root ? 2.75f : 1.75f);
                g.setColour (theme::text.withAlpha ((0.25f + 0.7f * fade) * chordDim));
                g.setFont (theme::font (jmin (12.0f, radius * 1.05f), true));
                g.drawText (keys::spell (next->root, next->scale, pitch), circle.expanded (4.0f, 0.0f), Justification::centred, false);
            }
        };

        if (inst.strings.empty())
        {
            // Keyboard: two octaves of white keys, the black keys on top, a dot on each note.
            const int whiteCount = 14;
            const float w = area.getWidth() / (float) whiteCount;
            const float h = jmin (area.getHeight() - 14.0f, w * 4.4f);
            auto keysArea = area.withHeight (h);
            static const bool black[] { false, true, false, true, false, false, true, false, true, false, true, false };
            int white = 0;
            std::vector<std::pair<int, Rectangle<float>>> whites, blacks;
            for (int p = fretboard::keyboardLow; p <= fretboard::keyboardHigh; ++p)
            {
                if (! black[p % 12])
                    whites.push_back ({ p, { keysArea.getX() + (float) white++ * w, keysArea.getY(), w, h } });
                else
                    blacks.push_back ({ p, { keysArea.getX() + (float) white * w - w * 0.31f, keysArea.getY(), w * 0.62f, h * 0.6f } });
            }
            for (auto& [p, r] : whites)
            {
                g.setColour (noteFill);
                g.fillRect (r.reduced (0.5f, 0.0f));
                g.setColour (theme::border);
                g.drawRect (r, 1.0f);
                if (p % 12 == 0)
                {
                    g.setColour (theme::textDim);
                    g.setFont (theme::font (11.0f));
                    g.drawText ("C" + String (p / 12 - 1), r.withY (r.getBottom() + 1.0f).withHeight (13.0f), Justification::centred);
                }
            }
            for (auto& [p, r] : blacks)
            {
                g.setColour (Colour (0xff1a1a1a));
                g.fillRoundedRectangle (r, 2.0f);
            }
            const float radius = jmin (w * 0.36f, 11.0f);
            for (auto& [p, r] : whites)
                drawNote ({ r.getCentreX(), r.getBottom() - radius - 7.0f }, radius, p, false);
            for (auto& [p, r] : blacks)
                drawNote ({ r.getCentreX(), r.getBottom() - radius - 4.0f }, jmin (radius, r.getWidth() * 0.46f), p, false);
            return;
        }

        // A neck seen from the front, the nut on the left, the thickest string at the bottom
        // (like a chord chart turned on its side, or tab).
        const int strings = (int) inst.strings.size();
        const float openW = 30.0f;
        auto neckArea = area.withTrimmedLeft (openW).withTrimmedBottom (16.0f);
        const float fretW = neckArea.getWidth() / (float) inst.frets;
        const float gap = neckArea.getHeight() / (float) strings;
        auto stringY = [&] (int s) { return neckArea.getBottom() - gap * ((float) s + 0.5f); };
        auto fretX = [&] (int f) { return neckArea.getX() + fretW * (float) f; };
        const float top = stringY (strings - 1) - gap * 0.42f, bottom = stringY (0) + gap * 0.42f;
        auto board = Rectangle<float> (neckArea.getX(), top, neckArea.getWidth(), bottom - top);

        g.setColour (wood);
        g.fillRect (board);

        // Inlays at 3, 5, 7, 9, 12 (two), 15.
        g.setColour (inlay);
        for (int f : { 3, 5, 7, 9, 12, 15 })
        {
            if (f > inst.frets)
                continue;
            const float x = fretX (f) - fretW * 0.5f;
            const float r = jmin (4.5f, gap * 0.22f);
            if (f == 12 && strings > 2)
            {
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, stringY (1) - gap * 0.5f }));
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, stringY (strings - 2) + gap * 0.5f }));
            }
            else
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, board.getCentreY() }));
        }

        for (int f = 0; f <= inst.frets; ++f)
        {
            const float x = fretX (f);
            g.setColour (f == 0 ? nut : fretWire);
            g.fillRect (x - (f == 0 ? 2.0f : 0.75f), top, f == 0 ? 4.0f : 1.5f, bottom - top);
        }

        // Fret numbers at the inlays.
        g.setFont (theme::font (11.0f));
        g.setColour (theme::textDim);
        for (int f : { 3, 5, 7, 9, 12, 15 })
            if (f <= inst.frets)
                g.drawText (String (f), Rectangle<float> (fretX (f - 1), bottom + 3.0f, fretW, 13.0f), Justification::centred);

        for (int s = 0; s < strings; ++s)
        {
            const float thick = 1.0f + (float) (strings - 1 - s) * 0.25f;
            g.setColour (stringCol);
            g.fillRect (neckArea.getX(), stringY (s) - thick * 0.5f, neckArea.getWidth(), thick);
        }

        // The string names, left of the open notes.
        g.setFont (theme::font (11.0f));
        for (int s = 0; s < strings; ++s)
        {
            g.setColour (theme::textDim);
            g.drawText (keys::noteName (inst.strings[(size_t) s], false), Rectangle<float> (area.getX() - 14.0f, stringY (s) - 6.0f, 10.0f, 12.0f),
                        Justification::centredRight);
        }

        const float radius = jmin (gap * 0.42f, fretW * 0.38f, 11.0f);
        const bool showChord = chord != nullptr && strings == 6;
        for (auto& spot : fretboard::spots (inst, key != nullptr ? key->root : 0, key != nullptr ? key->scale : Scale::major))
        {
            const float x = spot.fret == 0 ? area.getX() + openW * 0.5f - 2.0f : fretX (spot.fret) - fretW * 0.5f;
            const bool inChord = showChord && chord->frets[(size_t) spot.string] == spot.fret;
            if ((key == nullptr && ! inChord) || (showChord && spot.fret == 0 && chord->frets[(size_t) spot.string] < 0))
                continue;
            drawNote ({ x, stringY (spot.string) }, radius, spot.pitch, inChord);
        }
        // Strings the chord leaves out: a cross by the nut.
        if (showChord)
            for (int s = 0; s < strings; ++s)
                if (chord->frets[(size_t) s] < 0)
                {
                    const auto c = Point<float> (area.getX() + openW * 0.5f - 2.0f, stringY (s));
                    const float d = jmin (5.0f, radius * 0.5f);
                    g.setColour (theme::text);
                    g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 2.0f);
                    g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, 2.0f);
                }
    }

    /** A chord diagram: strings upright (low E on the left), the nut at the top, the chord's
        tones named in the dots, crosses over strings left out and rings over open ones. */
    void drawChordDiagram (Graphics& g, Rectangle<float> area, const chords::Voicing& v, const chords::Chord& chord, Colour colour)
    {
        static const int open[] { 40, 45, 50, 55, 59, 64 };
        auto grid = area.withTrimmedTop (14.0f).withTrimmedLeft (20.0f).withTrimmedRight (8.0f).withTrimmedBottom (4.0f);
        const int base = v.highestFret() <= 4 || v.usesOpenStrings() ? 1 : v.lowestFret();
        const int rows = 5;
        const float sx = grid.getWidth() / 5.0f, fy = grid.getHeight() / (float) rows;
        auto xOf = [&] (int s) { return grid.getX() + sx * (float) s; };
        auto yOf = [&] (int fret) { return grid.getY() + fy * ((float) (fret - base) + 0.5f); };

        for (int r = 0; r <= rows; ++r)
        {
            const float y = grid.getY() + fy * (float) r;
            if (r == 0 && base == 1)
            {
                g.setColour (nut);
                g.fillRect (xOf (0) - 1.0f, y - 2.0f, xOf (5) - xOf (0) + 2.0f, 4.0f);
            }
            else
            {
                g.setColour (fretWire);
                g.fillRect (xOf (0), y - 0.5f, xOf (5) - xOf (0), 1.0f);
            }
        }
        for (int s = 0; s < 6; ++s)
        {
            g.setColour (stringCol);
            const float w = 1.0f + (float) (5 - s) * 0.15f;
            g.fillRect (xOf (s) - w * 0.5f, grid.getY(), w, grid.getHeight());
        }
        if (base > 1)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            const float labelRight = grid.getX() - jmin (sx, fy) * 0.44f - 3.0f;
            g.drawText (String (base), Rectangle<float> (area.getX() - 6.0f, yOf (base) - 7.0f, labelRight - area.getX() + 6.0f, 14.0f),
                        Justification::centredRight);
        }

        const float radius = jmin (sx, fy) * 0.44f;
        // Over each string: a cross (left out) or a ring (open).
        for (int s = 0; s < 6; ++s)
        {
            const int f = v.frets[(size_t) s];
            const auto c = Point<float> (xOf (s), area.getY() + 6.0f);
            if (f < 0)
            {
                g.setColour (theme::textDim);
                g.drawLine (c.x - 3.0f, c.y - 3.0f, c.x + 3.0f, c.y + 3.0f, 1.5f);
                g.drawLine (c.x - 3.0f, c.y + 3.0f, c.x + 3.0f, c.y - 3.0f, 1.5f);
            }
            else if (f == 0)
            {
                const bool root = open[s] % 12 == chord.root;
                g.setColour (root ? colour : theme::textDim);
                g.drawEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (c), root ? 2.0f : 1.4f);
            }
        }

        if (v.barreFret > 0)
        {
            const float y = yOf (v.barreFret);
            auto bar = Rectangle<float> (xOf (v.barreFrom) - radius, y - radius * 0.8f, xOf (v.barreTo) - xOf (v.barreFrom) + radius * 2.0f, radius * 1.6f);
            g.setColour (noteFill);
            g.fillRoundedRectangle (bar, radius * 0.8f);
            g.setColour (colour);
            g.drawRoundedRectangle (bar.reduced (0.75f), radius * 0.8f, 1.5f);
        }

        for (int s = 0; s < 6; ++s)
        {
            const int f = v.frets[(size_t) s];
            if (f <= 0)
                continue;
            const int interval = ((open[s] + f - chord.root) % 12 + 12) % 12;
            const bool root = interval == 0;
            drawDot (g, { xOf (s), yOf (f) }, radius, chords::toneLabel (chord.quality, interval),
                     root ? colour : noteFill, colour, root ? 0.0f : 1.5f, root ? theme::textOn (colour) : theme::textDark, 1.0f, 0.95f);
        }
    }
}

//==============================================================================
/** The chord sheet: the key's chords degree by degree, the variants of the picked one, and
    ways to play it on a guitar. What's picked is kept by degree and variant, so "the V7" stays
    picked when the key changes. */
class ChordSheet : public Component, private ScrollBar::Listener
{
public:
    explicit ChordSheet (PropertiesFile& s) : settings (s)
    {
        auto saved = StringArray::fromTokens (settings.getValue ("playAlong.chord", "0,0,0"), ",", "");
        degreePick = jlimit (0, 6, saved[0].getIntValue());
        variantPick = (chords::Variant) jlimit (0, (int) chords::Variant::power, saved[1].getIntValue());
        voicingPick = jmax (0, saved[2].getIntValue());
        setRepaintsOnMouseActivity (false);
        hbar.setAutoHide (false);
        hbar.addListener (this);
        addChildComponent (hbar);
    }

    std::function<void()> onPick;      // a chord or voicing was picked
    std::function<void()> onResize;    // the sheet wants a different height

    /** The key the sheet shows (and the next one, fading in). Rebuilds if it changed. */
    void setKey (const std::optional<KeyChange>& k, const std::optional<KeyChange>& n, float f, const String& nextWhen)
    {
        const bool keyChanged = ! sameKey (k, key);
        const bool nextChanged = ! sameKey (n, next);
        const int fadeStep = roundToInt (f * 20.0f);
        if (! keyChanged && ! nextChanged && fadeStep == shownFade && nextWhen == when)
            return;
        key = k;
        next = n;
        fade = f;
        shownFade = fadeStep;
        when = nextWhen;
        if (keyChanged)
            rebuild (true);
        if (nextChanged)
            nextDegrees = next ? chords::diatonic (next->root, next->scale) : std::vector<chords::Degree>();
        repaint();
    }

    const std::optional<KeyChange>& shownKey() const   { return key; }
    const chords::Chord* pickedChord() const            { return chord ? &*chord : nullptr; }
    const chords::Voicing* pickedVoicing() const
    {
        return chord && voicingPick < (int) voicings.size() ? &voicings[(size_t) voicingPick] : nullptr;
    }

    /** How tall the sheet would like to be at a width. */
    int wantedHeight (int width) const
    {
        ignoreUnused (width);
        return (int) (pad + titleH + 8 + chipH + 10 + variantH + 14 + sectionH + cellH + barH + pad);
    }

    void resized() override   { layoutHits(); }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        drawCard (g, r);
        r = r.reduced (pad);

        auto title = r.removeFromTop (titleH);
        g.setFont (theme::font (14.0f, true));
        g.setColour (theme::text);
        const String head = "Chords";
        g.drawText (head, title, Justification::centredLeft);
        float x = title.getX() + textWidth (theme::font (14.0f, true), head) + 10.0f;
        if (key)
        {
            const auto name = keys::keyName (*key);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText ("in", Rectangle<float> (x, title.getY(), 20.0f, title.getHeight()), Justification::centredLeft);
            x += 18.0f;
            drawKeyPill (g, { x, title.getCentreY() - 10.0f }, *key, 12.0f);
            x += pillWidth (name, 12.0f) + 12.0f;
            if (key->scale == Scale::majorPentatonic || key->scale == Scale::minorPentatonic || key->scale == Scale::blues)
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.5f));
                const String parent = key->scale == Scale::majorPentatonic ? "from its major scale" : "from its minor scale";
                g.drawText (key->scale == Scale::blues ? parent + ", with the blues 7ths" : parent,
                            Rectangle<float> (x, title.getY(), 260.0f, title.getHeight()), Justification::centredLeft);
            }
        }
        if (! key || degrees.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText ("Set a key on the Key lane, and its chords show up here.", r.withTrimmedTop (10.0f).removeFromTop (24.0f),
                        Justification::centredLeft);
            return;
        }

        // The degrees, each marked in its root's colour; the picked one outlined in it, as
        // Compare outlines the version you hear.
        for (auto& hit : chipHits)
        {
            const auto& deg = degrees[(size_t) hit.index];
            const auto& c = chordFor (deg);
            const auto colour = theme::keyColour (deg.root);
            const bool picked = hit.index == degreePick;
            const bool hover = hover_ == Hover { 0, hit.index };
            auto chip = hit.area;
            g.setColour (picked ? theme::headerSel : hover ? theme::headerSel.darker (0.08f) : theme::header);
            g.fillRoundedRectangle (chip, 6.0f);
            g.setColour (picked ? colour : theme::border);
            g.drawRoundedRectangle (chip.reduced (picked ? 1.0f : 0.5f), 6.0f, picked ? 2.0f : 1.0f);
            if (! picked)
            {
                g.setColour (colour);
                g.fillRect (chip.withWidth (4.0f).reduced (0.0f, 6.0f).translated (1.0f, 0.0f));
            }
            auto inner = chip.reduced (8.0f, 6.0f);
            g.setColour (colour);
            g.setFont (theme::font (12.5f, true));
            g.drawText (c.numeral, inner.removeFromTop (16.0f).withTrimmedTop (2.0f), Justification::centred);
            g.setColour (theme::text);
            g.setFont (theme::font (jmin (17.0f, inner.getWidth() / (float) jmax (3, c.name.length()) * 1.7f), true));
            g.drawFittedText (c.name, inner.removeFromTop (22.0f).toNearestInt(), Justification::centred, 1, 0.7f);

            // The same step in the next key, fading in with it.
            if (fade > 0.0f && hit.index < (int) nextDegrees.size())
            {
                const auto& nd = nextDegrees[(size_t) hit.index];
                const auto& nc = chordFor (nd);
                g.setColour (theme::keyColour (nd.root).withAlpha (0.25f + 0.75f * fade));
                g.setFont (theme::font (12.0f));
                g.drawText (String::fromUTF8 ("\xe2\x86\x92 ") + nc.name, inner, Justification::centred);
            }
        }

        // The variants of the picked degree, and the picked chord's notes.
        const auto& deg = degrees[(size_t) degreePick];
        const auto colour = theme::keyColour (deg.root);
        for (auto& hit : variantHits)
        {
            const auto& c = deg.chords[(size_t) hit.index];
            const bool picked = chord && c.name == chord->name;
            const bool hover = hover_ == Hover { 1, hit.index };
            drawTag (g, hit.area, c.name, picked ? colour : hover ? theme::controlHi : theme::control,
                     picked ? theme::textOn (colour) : theme::text, 12.5f);
        }
        if (chord)
        {
            // The chord's notes, as the scale's pills: the root filled.
            const auto steps = chords::intervals (chord->quality);
            const auto names = chords::toneNames (*chord, keys::usesFlats (key->root, key->scale));
            float tx = toneArea.getRight();
            for (int i = (int) steps.size() - 1; i >= 0; --i)
            {
                const int interval = steps[(size_t) i];
                const auto noteName = names[(size_t) i];
                const auto label = chords::toneLabel (chord->quality, interval);
                const float nameW = textWidth (theme::font (12.5f, true), noteName);
                const float labelW = textWidth (theme::font (11.5f), label);
                const float w = nameW + labelW + 26.0f;
                auto tag = Rectangle<float> (tx - w, toneArea.getY(), w, toneArea.getHeight());
                tx -= w + 6.0f;
                const bool root = interval == 0;
                g.setColour (root ? colour : theme::control);
                g.fillRoundedRectangle (tag, 4.0f);
                const auto ink = root ? theme::textOn (colour) : theme::text;
                auto inner = tag.reduced (9.0f, 0.0f);
                g.setColour (ink);
                g.setFont (theme::font (12.5f, true));
                g.drawText (noteName, inner, Justification::centredLeft);
                // What the note is in the chord: "b3".
                g.setColour (root ? ink.withAlpha (0.7f) : theme::textDim);
                g.setFont (theme::font (11.5f));
                g.drawText (label, inner, Justification::centredRight);
            }
        }

        // The voicings, in a row that scrolls sideways when it's longer than the sheet.
        Graphics::ScopedSaveState clipped (g);
        g.reduceClipRegion (voicingArea.toNearestInt());
        const auto shift = AffineTransform::translation (-scroll, 0.0f);
        g.setFont (theme::font (12.5f));
        g.setColour (theme::textDim);
        for (auto& [label, area] : sectionLabels)
            g.drawText (label, area.transformedBy (shift), Justification::centredLeft);
        g.setColour (theme::border);
        for (auto& d : dividers)
            g.fillRect (d.transformedBy (shift));
        for (auto& hit : voicingHits)
        {
            const auto& v = voicings[(size_t) hit.index];
            const bool picked = hit.index == voicingPick;
            const bool hover = hover_ == Hover { 2, hit.index };
            auto cell = hit.area.transformedBy (shift);
            if (! cell.intersects (voicingArea))
                continue;
            if (picked || hover)
            {
                g.setColour (picked ? theme::headerSel : theme::header);
                g.fillRoundedRectangle (cell, 6.0f);
            }
            if (picked)
            {
                g.setColour (colour);
                g.drawRoundedRectangle (cell.reduced (1.0f), 6.0f, 2.0f);
            }
            auto inner = cell.reduced (6.0f, 6.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f, true));
            g.drawFittedText (v.name, inner.removeFromTop (15.0f).toNearestInt(), Justification::centred, 1, 0.75f);
            auto detail = inner.removeFromBottom (26.0f);
            drawChordDiagram (g, inner.reduced (0.0f, 2.0f), v, *chord, colour);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (v.detail, detail.toNearestInt(), Justification::centred, 2, 0.85f);
        }
    }

    void mouseMove (const MouseEvent& e) override
    {
        const auto h = hitAt (e.position);
        setMouseCursor (h.kind >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
        if (h != hover_)
        {
            hover_ = h;
            repaint();
        }
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel) override
    {
        if (! hbar.isVisible() || ! voicingArea.contains (e.position))
        {
            Component::mouseWheelMove (e, wheel);
            return;
        }
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        hbar.setCurrentRangeStart (hbar.getCurrentRangeStart() - delta * 260.0);
    }

    void mouseExit (const MouseEvent&) override
    {
        hover_ = {};
        repaint();
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto h = hitAt (e.position);
        if (h.kind == 0)
            degreePick = h.index;
        else if (h.kind == 1)
            variantPick = degrees[(size_t) degreePick].chords[(size_t) h.index].variant;
        else if (h.kind == 2)
            voicingPick = h.index;
        else
            return;
        if (h.kind == 2)
            save();
        else
            rebuild (false);
        repaint();
        if (onPick)
            onPick();
    }

private:
    struct Hit { Rectangle<float> area; int index = 0; };
    struct Hover
    {
        int kind = -1, index = -1;   // 0 a degree, 1 a variant, 2 a voicing
        bool operator== (const Hover& o) const   { return kind == o.kind && index == o.index; }
        bool operator!= (const Hover& o) const   { return ! operator== (o); }
    };

    static bool sameKey (const std::optional<KeyChange>& a, const std::optional<KeyChange>& b)
    {
        if (a.has_value() != b.has_value())
            return false;
        return ! a || (a->root == b->root && a->scale == b->scale && exactlyEqual (a->beat, b->beat));
    }

    /** The degree's chord in the picked variant (or its triad, if it has none). */
    const chords::Chord& chordFor (const chords::Degree& d) const
    {
        for (auto& c : d.chords)
            if (c.variant == variantPick)
                return c;
        return d.chords.front();
    }

    /** New chords (a new key, or a new pick): keeps the voicing nearest the one played, in the
        same group, so you stay in your position on the neck. */
    void rebuild (bool fromKey)
    {
        const auto old = pickedVoicing() ? std::optional<chords::Voicing> (*pickedVoicing()) : std::nullopt;
        const auto oldName = chord ? chord->name : String();
        degrees = key ? chords::diatonic (key->root, key->scale) : std::vector<chords::Degree>();
        chord.reset();
        voicings.clear();
        if (! degrees.empty())
        {
            degreePick = jlimit (0, (int) degrees.size() - 1, degreePick);
            chord = chordFor (degrees[(size_t) degreePick]);
            voicings = chords::voicings (*chord);
            if (old && chord->name != oldName)
            {
                int best = 0, bestScore = 1 << 20;
                for (int i = 0; i < (int) voicings.size(); ++i)
                {
                    const auto& v = voicings[(size_t) i];
                    const int score = (v.group != old->group ? 100 : 0) + std::abs (v.lowestFret() - old->lowestFret());
                    if (score < bestScore)
                    {
                        bestScore = score;
                        best = i;
                    }
                }
                voicingPick = best;
            }
            voicingPick = jlimit (0, jmax (0, (int) voicings.size() - 1), voicingPick);
        }
        if (! fromKey)
            save();
        layoutHits();
        if (onResize)
            onResize();
    }

    void save()
    {
        settings.setValue ("playAlong.chord", String (degreePick) + "," + String ((int) variantPick) + "," + String (voicingPick));
    }

    void layoutHits()
    {
        chipHits.clear();
        variantHits.clear();
        voicingHits.clear();
        sectionLabels.clear();
        dividers.clear();
        auto r = getLocalBounds().toFloat().reduced (pad);
        r.removeFromTop (titleH + 8.0f);
        if (degrees.empty())
            return;

        auto chips = r.removeFromTop (chipH);
        const float gap = 6.0f;
        const float w = jmin (170.0f, (chips.getWidth() - gap * (float) (degrees.size() - 1)) / (float) degrees.size());
        for (int i = 0; i < (int) degrees.size(); ++i)
            chipHits.push_back ({ Rectangle<float> (chips.getX() + (float) i * (w + gap), chips.getY(), w, chipH), i });
        r.removeFromTop (10.0f);

        auto row = r.removeFromTop (variantH);
        toneArea = row;
        float x = row.getX();
        const auto& deg = degrees[(size_t) degreePick];
        for (int i = 0; i < (int) deg.chords.size(); ++i)
        {
            const float vw = textWidth (theme::font (12.5f, true), deg.chords[(size_t) i].name) + 26.0f;
            variantHits.push_back ({ Rectangle<float> (x, row.getY(), vw, variantH), i });
            x += vw + 6.0f;
        }
        toneArea = row.withTrimmedLeft (x - row.getX() + 16.0f);
        r.removeFromTop (14.0f);

        voicingArea = r.removeFromTop (sectionH + cellH);
        auto f = flow (voicingArea);
        voicingHits = std::move (f.cells);
        sectionLabels = std::move (f.labels);
        dividers = std::move (f.dividers);
        contentWidth = f.width;
        const bool scrolls = contentWidth > voicingArea.getWidth() + 0.5f;
        hbar.setVisible (scrolls);
        hbar.setBounds (voicingArea.withY (voicingArea.getBottom() + 3.0f).withHeight (barH - 3.0f).toNearestInt());
        hbar.setRangeLimits (0.0, jmax ((double) voicingArea.getWidth(), (double) contentWidth), dontSendNotification);
        hbar.setCurrentRange (scrolls ? (double) scroll : 0.0, voicingArea.getWidth(), dontSendNotification);
        scroll = (float) hbar.getCurrentRangeStart();
        // Keep the picked voicing in view.
        if (scrolls && voicingPick < (int) voicingHits.size())
        {
            const auto cell = voicingHits[(size_t) voicingPick].area;
            if (cell.getX() - scroll < voicingArea.getX())
                hbar.setCurrentRangeStart (cell.getX() - voicingArea.getX() - cellGap, dontSendNotification);
            else if (cell.getRight() - scroll > voicingArea.getRight())
                hbar.setCurrentRangeStart (cell.getRight() - voicingArea.getRight() + cellGap, dontSendNotification);
            scroll = (float) hbar.getCurrentRangeStart();
        }
    }

    void scrollBarMoved (ScrollBar*, double start) override
    {
        scroll = (float) start;
        repaint();
    }

    /** The voicings' cells, in one row: the open and barre shapes, then a divider and the ones
        up the neck, each group labelled where it starts. */
    struct Flow
    {
        std::vector<Hit> cells;
        std::vector<std::pair<String, Rectangle<float>>> labels;
        std::vector<Rectangle<float>> dividers;
        float width = 0.0f;
    };
    Flow flow (Rectangle<float> r) const
    {
        Flow f;
        float x = r.getX();
        const float y = r.getY() + sectionH;
        int lastGroup = -1;
        for (int i = 0; i < (int) voicings.size(); ++i)
        {
            const auto group = (int) voicings[(size_t) i].group;
            if (group != lastGroup)
            {
                if (lastGroup >= 0)
                {
                    x += groupGap;
                    f.dividers.push_back ({ x - groupGap * 0.5f - cellGap * 0.5f, y + 8.0f, 1.0f, cellH - 16.0f });
                }
                f.labels.push_back ({ group == chords::Voicing::shape ? "Open and barre" : "Up the neck",
                                      Rectangle<float> (x + 4.0f, y - sectionH, 200.0f, sectionH - 2.0f) });
                lastGroup = group;
            }
            f.cells.push_back ({ Rectangle<float> (x, y, cellW, cellH), i });
            x += cellW + cellGap;
        }
        f.width = x - cellGap - r.getX();
        return f;
    }

    Hover hitAt (Point<float> p) const
    {
        for (auto& h : chipHits)    if (h.area.contains (p)) return { 0, h.index };
        for (auto& h : variantHits) if (h.area.contains (p)) return { 1, h.index };
        if (voicingArea.contains (p))
            for (auto& h : voicingHits) if (h.area.translated (-scroll, 0.0f).contains (p)) return { 2, h.index };
        return {};
    }

    static float pillWidth (const String& text, float size)   { return textWidth (theme::font (size, true), text) + 20.0f; }

public:
    /** A key as a tag in its colour, as the key lane draws its blocks: "A minor". */
    static void drawKeyPill (Graphics& g, Point<float> topLeft, const KeyChange& k, float size)
    {
        const auto name = keys::keyName (k);
        const auto colour = theme::keyColour (k.root);
        drawTag (g, { topLeft.x, topLeft.y, pillWidth (name, size), size + 8.0f }, name, colour, theme::textOn (colour), size);
    }
    static float keyPillWidth (const KeyChange& k, float size)   { return pillWidth (keys::keyName (k), size); }

private:
    static constexpr float pad = 14.0f, titleH = 24.0f, chipH = 62.0f, variantH = 26.0f, sectionH = 18.0f;
    static constexpr float cellW = 100.0f, cellH = 142.0f, cellGap = 8.0f, groupGap = 18.0f, barH = 12.0f;

    PropertiesFile& settings;
    std::optional<KeyChange> key, next;
    float fade = 0.0f;
    int shownFade = -1;
    String when;
    std::vector<chords::Degree> degrees, nextDegrees;
    std::optional<chords::Chord> chord;
    std::vector<chords::Voicing> voicings;
    int degreePick = 0, voicingPick = 0;
    chords::Variant variantPick = chords::Variant::triad;

    std::vector<Hit> chipHits, variantHits, voicingHits;
    std::vector<std::pair<String, Rectangle<float>>> sectionLabels;
    Rectangle<float> toneArea;
    std::vector<Rectangle<float>> dividers;
    Rectangle<float> voicingArea;
    float contentWidth = 0.0f, scroll = 0.0f;
    ScrollBar hbar { false };
    Hover hover_;
};

//==============================================================================
class PlayAlongView : public Component, private Timer
{
public:
    using State = PlayAlongWindow::State;

    explicit PlayAlongView (AppContext& c) : ctx (c), sheet (c.settings)
    {
        addChildComponent (sheet);
        sheet.onPick = [this] { repaint(); };
        sheet.onResize = [this] { if (getWidth() > 0) resized(); };

        // One instrument at a time (the first one saved, if an older Lathe saved more), and Chords.
        auto shown = StringArray::fromTokens (ctx.settings.getValue ("playAlong.instruments", "Guitar"), ",", "");
        bool instrumentShown = false;
        for (int i = 0; i < sectionNames().size(); ++i)
        {
            auto* b = buttons.add (new TextButton (sectionNames()[i]));
            b->setClickingTogglesState (true);
            const bool on = shown.contains (sectionNames()[i]) && (i == chordsSection || ! instrumentShown);
            instrumentShown = instrumentShown || (on && i != chordsSection);
            b->setToggleState (on, dontSendNotification);
            b->setColour (TextButton::buttonOnColourId, theme::accent);
            b->setColour (TextButton::textColourOnId, theme::textDark);
            b->setColour (TextButton::textColourOffId, theme::text);
            if (i < chordsSection)
                b->setConnectedEdges ((i > 0 ? Button::ConnectedOnLeft : 0) | (i < chordsSection - 1 ? Button::ConnectedOnRight : 0));
            b->onClick = [this, i]
            {
                // Picking an instrument puts the others away; clicking the one shown hides it.
                if (i != chordsSection && buttons[i]->getToggleState())
                    for (int k = 0; k < chordsSection; ++k)
                        if (k != i)
                            buttons[k]->setToggleState (false, dontSendNotification);
                saveChoice();
                if (onLayoutChanged) onLayoutChanged();
                resized();
                repaint();
            };
            addAndMakeVisible (b);

            // What this diagram follows: the song's key, or a track with keys of its own.
            auto* f = follows.add (new ComboBox());
            f->setTooltip (i == chordsSection
                               ? "Whose key the chords are in: the song's, or a track with a key of its own "
                                 "(Track menu > Own Key). Automatic picks a guitar track."
                               : "Whose key this shows: the song's, or a track with a key of its own "
                                 "(Track menu > Own Key). Automatic picks a track named after the instrument.");
            f->onChange = [this, i] { followChosen (i); };
            addChildComponent (f);
        }
        rebuildFollowMenus();
        setSize (980, 820);
        timerCallback();
        startTimerHz (20);
    }

    std::function<void()> onLayoutChanged;   // the sections shown changed: the height needed too

    /** How tall the view needs to be to show every section that's on at full size. */
    int neededHeight() const
    {
        float h = pad * 2.0f + (float) toolbarH + 12.0f + headerH + 12.0f;
        for (int i = 0; i < chordsSection; ++i)
            if (buttons[i]->getToggleState())
                h += wantedHeight (i) + gap;
        if (buttons[chordsSection]->getToggleState())
            h += (float) sheet.wantedHeight (getWidth());
        return (int) std::ceil (h);
    }

    void resized() override
    {
        auto bar = getLocalBounds().reduced ((int) pad).removeFromTop (toolbarH);
        auto chordsButton = bar.removeFromRight (86);
        buttons[chordsSection]->setBounds (chordsButton.reduced (0, 2));
        bar.removeFromRight (10);
        for (int i = chordsSection - 1; i >= 0; --i)
            buttons[i]->setBounds (bar.removeFromRight (84).reduced (0, 2));

        const auto slots = layout();
        const bool anyTrackKeys = std::any_of (ctx.edit.tracks.begin(), ctx.edit.tracks.end(), [] (auto& t) { return ! t->keys.empty(); });
        for (int i = 0; i < follows.size(); ++i)
        {
            auto it = slots.find (i);
            follows[i]->setVisible (anyTrackKeys && it != slots.end());
            if (it != slots.end())
                follows[i]->setBounds (it->second.reduced (cardPad, 0.0f).withTrimmedTop (i == chordsSection ? 13.0f : 12.0f)
                                         .withHeight (22.0f).removeFromRight (210.0f).toNearestInt());
        }
        auto it = slots.find (chordsSection);
        sheet.setVisible (it != slots.end());
        if (it != slots.end())
            sheet.setBounds (it->second.toNearestInt());
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);
        auto r = getLocalBounds().toFloat().reduced (pad);
        auto toolbar = r.removeFromTop ((float) toolbarH);
        r.removeFromTop (12.0f);
        auto head = r.removeFromTop (headerH);

        const auto st = PlayAlongWindow::stateAt (ctx.edit, beat);
        const double barLen = ctx.edit.beatsPerBar();
        const bool anyTrackKeys = std::any_of (ctx.edit.tracks.begin(), ctx.edit.tracks.end(), [] (auto& t) { return ! t->keys.empty(); });

        // Where in the song: the playhead, or the start marker.
        g.setFont (theme::font (12.5f));
        g.setColour (theme::textDim);
        g.drawText ("Bar " + barText (std::floor (beat * 4.0) / 4.0, barLen) + (ctx.engine.isPlaying() ? ", playing" : ", at the start marker"),
                    toolbar, Justification::centredLeft);

        if (! st.key && ctx.edit.keys.empty() && ! anyTrackKeys)
        {
            drawCard (g, head);
            auto in = head.reduced (20.0f, 16.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (19.0f, true));
            g.drawText ("No key set", in.removeFromTop (26.0f), Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawFittedText ("Click the Key lane under the bar numbers to set the song's key, or ask the AI assistant "
                              "to work the keys out. The notes of the key show up here, on every instrument you pick.",
                              in.toNearestInt(), Justification::topLeft, 3);
        }
        else
        {
            auto nextCard = head.removeFromRight (jmin (300.0f, head.getWidth() * 0.38f));
            head.removeFromRight (12.0f);
            drawNowCard (g, head, st, barLen);
            drawNextCard (g, nextCard, st, barLen);
        }

        const auto slots = layout();
        if (slots.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText ("Pick an instrument, or Chords, at the top right.", r.withTrimmedTop (16.0f).removeFromTop (30.0f), Justification::centred);
            return;
        }
        const auto* voicing = sheet.isVisible() ? sheet.pickedVoicing() : nullptr;
        for (auto& [i, slot] : slots)
        {
            if (i == chordsSection)
                continue;
            const auto& inst = fretboard::instruments()[(size_t) i];
            const auto followed = followedTrack (i);
            const auto ds = PlayAlongWindow::stateAt (ctx.edit, beat, followed.get());
            drawCard (g, slot);
            auto in = slot.reduced (cardPad, 10.0f);
            auto title = in.removeFromTop (26.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (14.0f, true));
            g.drawText (inst.name, title, Justification::centredLeft);
            const float nameW = textWidth (theme::font (14.0f, true), inst.name);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (tuningText (inst), title.withTrimmedLeft (nameW + 12.0f), Justification::centredLeft);

            // Following a track: its key as a pill, beside the menu.
            if (followed != nullptr && ds.key)
            {
                const float w = ChordSheet::keyPillWidth (*ds.key, 11.5f);
                const float right = title.getRight() - (follows[i]->isVisible() ? 220.0f : 0.0f);
                ChordSheet::drawKeyPill (g, { right - w, title.getCentreY() - 9.75f }, *ds.key, 11.5f);
            }
            in.removeFromTop (8.0f);
            // The chord picked on the sheet, if the guitar shows the sheet's key.
            const chords::Voicing* chordHere = nullptr;
            if (voicing != nullptr && inst.strings.size() == 6 && ds.key && sheet.shownKey()
                && ds.key->root == sheet.shownKey()->root && ds.key->scale == sheet.shownKey()->scale)
                chordHere = voicing;
            drawInstrument (g, in.withTrimmedLeft (inst.strings.empty() ? 0.0f : 14.0f), inst,
                            ds.key ? &*ds.key : nullptr, ds.next ? &*ds.next : nullptr, ds.nextFade, chordHere);
        }
    }

private:
    void drawNowCard (Graphics& g, Rectangle<float> card, const State& st, double barLen)
    {
        drawCard (g, card);
        auto in = card.reduced (16.0f, 12.0f);
        if (! st.key)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (19.0f, true));
            g.drawText (ctx.edit.keys.empty() ? "No song key" : "No key yet", in.removeFromTop (28.0f), Justification::centredLeft);
            g.setFont (theme::font (12.5f));
            g.drawText (ctx.edit.keys.empty() ? "Tracks with a key of their own show it below" : "Before the first key change",
                        in.removeFromTop (20.0f), Justification::centredLeft);
            return;
        }
        // The key's colour as a bar down the left, as the device panel marks a track.
        const auto colour = theme::keyColour (st.key->root);
        g.setColour (colour);
        g.fillRect (card.withWidth (4.0f).reduced (0.0f, 10.0f).translated (8.0f, 0.0f));
        in.removeFromLeft (8.0f);

        g.setColour (theme::text);
        g.setFont (theme::font (22.0f, true));
        g.drawText (keys::keyName (*st.key), in.removeFromTop (28.0f), Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText ("The key now, from bar " + barText (st.key->beat, barLen), in.removeFromTop (20.0f), Justification::centredLeft);
        in.removeFromTop (8.0f);

        // The notes of the key, as tags: the root in the key's colour.
        auto tags = in.removeFromTop (22.0f);
        float x = tags.getX();
        for (auto step : keys::intervals (st.key->scale))
        {
            const auto name = keys::spell (st.key->root, st.key->scale, st.key->root + step);
            const float w = jmax (28.0f, textWidth (theme::font (12.0f, true), name) + 16.0f);
            auto t = Rectangle<float> (x, tags.getY(), w, tags.getHeight());
            if (t.getRight() > tags.getRight())
                break;
            x += w + 4.0f;
            drawTag (g, t, name, step == 0 ? colour : theme::control, step == 0 ? theme::textOn (colour) : theme::text);
        }
    }

    void drawNextCard (Graphics& g, Rectangle<float> card, const State& st, double barLen)
    {
        drawCard (g, card);
        auto in = card.reduced (16.0f, 12.0f);
        if (! st.next)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (19.0f, true));
            g.drawText ("Next", in.removeFromTop (28.0f), Justification::centredLeft);
            g.setFont (theme::font (12.5f));
            g.drawText ("No key change ahead", in.removeFromTop (20.0f), Justification::centredLeft);
            return;
        }
        // In the bar before it, outlined in its colour, as Compare outlines the version playing.
        const auto colour = theme::keyColour (st.next->root);
        if (st.nextFade > 0.0f)
        {
            g.setColour (colour);
            g.drawRoundedRectangle (card.reduced (1.0f), 6.0f, 2.0f);
        }
        g.setColour (colour);
        g.setFont (theme::font (19.0f, true));
        g.drawFittedText (keys::keyName (*st.next), in.removeFromTop (28.0f).toNearestInt(), Justification::centredLeft, 1, 0.8f);

        const double until = st.next->beat - beat;
        String howFar;
        if (until <= barLen)
        {
            const int beatsLeft = jmax (1, (int) std::ceil (until - 1.0e-6));
            howFar = String (beatsLeft) + (beatsLeft == 1 ? " beat" : " beats");
        }
        else
        {
            const int barsLeft = (int) std::ceil (until / barLen - 1.0e-6);
            howFar = String (barsLeft) + (barsLeft == 1 ? " bar" : " bars");
        }
        g.setColour (st.nextFade > 0.0f ? theme::text : theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText ("Next, at bar " + barText (st.next->beat, barLen) + ": in " + howFar, in.removeFromTop (20.0f), Justification::centredLeft);
        in.removeFromTop (10.0f);

        // Over the last bar, a bar filling up as its notes fade in (as Compare's progress).
        auto track = in.removeFromTop (8.0f);
        g.setColour (theme::control);
        g.fillRoundedRectangle (track, 4.0f);
        if (st.nextFade > 0.0f)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (track.withWidth (jmax (8.0f, track.getWidth() * st.nextFade)), 4.0f);
        }
    }

    static float wantedHeight (int instrument)
    {
        const auto& inst = fretboard::instruments()[(size_t) instrument];
        return inst.strings.empty() ? 210.0f : 20.0f + 34.0f + 27.0f * (float) inst.strings.size() + 16.0f;
    }

    /** Where each section that's on goes (by index): the instruments from the top, each as tall
        as reads well, and the chord sheet right below them. (The window scrolls if they don't
        fit; if the view is still short of room, the instruments share it.) */
    std::map<int, Rectangle<float>> layout() const
    {
        std::map<int, Rectangle<float>> out;
        auto r = getLocalBounds().toFloat().reduced (pad);
        r.removeFromTop ((float) toolbarH + 12.0f + headerH + 12.0f);
        const bool chordsOn = buttons[chordsSection]->getToggleState();
        const float sheetH = chordsOn ? (float) sheet.wantedHeight (getWidth()) : 0.0f;
        std::vector<int> on;
        float total = 0.0f;
        for (int i = 0; i < chordsSection; ++i)
            if (buttons[i]->getToggleState())
            {
                on.push_back (i);
                total += wantedHeight (i);
            }
        const float avail = r.getHeight() - gap * (float) on.size() - sheetH;
        const float scale = total > avail ? jmax (0.5f, avail / total) : 1.0f;
        for (int i : on)
        {
            out[i] = r.removeFromTop (wantedHeight (i) * scale);
            r.removeFromTop (gap);
        }
        if (chordsOn)
            out[chordsSection] = r.removeFromTop (sheetH);
        return out;
    }

    String followKey (int i) const   { return "playAlong.follow." + sectionNames()[i]; }
    static String followsLike (int i)   { return i == chordsSection ? String ("Guitar") : sectionNames()[i]; }

    /** The track section i follows, or null for the song's key. The setting is "auto", "song"
        or a track's tag; a tag whose track is gone (or has no keys now) means auto. The chord
        sheet follows a guitar track by itself. */
    std::shared_ptr<Track> followedTrack (int i) const
    {
        const auto choice = ctx.settings.getValue (followKey (i), "auto");
        if (choice == "song")
            return {};
        if (choice != "auto")
            if (auto t = ctx.edit.findTrackByUid (choice); t != nullptr && ! t->keys.empty())
                return t;
        return PlayAlongWindow::autoFollow (ctx.edit, followsLike (i));
    }

    // Menu ids: 1 automatic, 2 the song's key, 100 + index into the tracks with keys.
    void rebuildFollowMenus()
    {
        followable.clear();
        for (auto& t : ctx.edit.tracks)
            if (! t->keys.empty())
                followable.push_back (t);
        for (int i = 0; i < follows.size(); ++i)
        {
            auto* f = follows[i];
            f->clear (dontSendNotification);
            const auto automatic = PlayAlongWindow::autoFollow (ctx.edit, followsLike (i));
            f->addItem ("Automatic" + (automatic != nullptr ? " (" + automatic->name + ")" : String (" (song)")), 1);
            f->addItem ("Song's key", 2);
            for (size_t k = 0; k < followable.size(); ++k)
                f->addItem ("\"" + followable[k]->name + "\"'s key", 100 + (int) k);

            const auto choice = ctx.settings.getValue (followKey (i), "auto");
            int id = choice == "song" ? 2 : 1;
            for (size_t k = 0; k < followable.size(); ++k)
                if (followable[k]->uid == choice)
                    id = 100 + (int) k;
            f->setSelectedId (id, dontSendNotification);
        }
    }

    void followChosen (int i)
    {
        const int id = follows[i]->getSelectedId();
        String value = "auto";
        if (id == 2)
            value = "song";
        else if (id >= 100 && (size_t) (id - 100) < followable.size())
            value = followable[(size_t) (id - 100)]->uid;
        ctx.settings.setValue (followKey (i), value);
        shownSig = {};
        timerCallback();
    }

    String nextWhen (const State& st) const
    {
        return st.next ? String (std::ceil (st.next->beat - beat)) : String();
    }

    void timerCallback() override
    {
        auto& engine = ctx.engine;
        const double now = engine.isPlaying() ? engine.getPlayheadBeats() : ctx.edit.insertMarker;
        beat = now;

        // The tracks with keys of their own (and their names) changed: new menus.
        String tracksSig;
        for (auto& t : ctx.edit.tracks)
            if (! t->keys.empty())
                tracksSig << t->uid << ":" << t->name << ";";
        if (tracksSig != shownTracks)
        {
            shownTracks = tracksSig;
            rebuildFollowMenus();
            resized();
            shownSig = {};
        }

        // The chord sheet: the key of the track it follows.
        const auto sheetState = PlayAlongWindow::stateAt (ctx.edit, now, followedTrack (chordsSection).get());
        sheet.setKey (sheetState.key, sheetState.next, sheetState.nextFade, nextWhen (sheetState));

        // Repaint only when what's drawn would change: a key, the next one, the fade's step,
        // the beats to go, or the bar shown.
        auto describe = [this] (const State& st)
        {
            String sig;
            if (st.key) sig << keys::keyName (*st.key) << "@" << st.key->beat;
            if (st.next) sig << "|" << keys::keyName (*st.next) << "@" << st.next->beat << "~" << nextWhen (st);
            sig << "|" << roundToInt (st.nextFade * 20.0f) << ";";
            return sig;
        };
        String sig = describe (PlayAlongWindow::stateAt (ctx.edit, now));
        for (int i = 0; i < chordsSection; ++i)
            if (buttons[i]->getToggleState())
                if (auto t = followedTrack (i))
                    sig << i << describe (PlayAlongWindow::stateAt (ctx.edit, now, t.get()));
        sig << (int) ctx.edit.keys.size() << (engine.isPlaying() ? "p" : "s") << std::floor (now * 4.0);
        if (sig != shownSig)
        {
            shownSig = sig;
            repaint();
        }
    }

    void saveChoice()
    {
        StringArray on;
        for (auto* b : buttons)
            if (b->getToggleState())
                on.add (b->getButtonText());
        ctx.settings.setValue ("playAlong.instruments", on.joinIntoString (","));
    }

    static constexpr int toolbarH = 30;
    static constexpr float pad = 16.0f, headerH = 104.0f, cardPad = 16.0f, gap = 10.0f;
    AppContext& ctx;
    ChordSheet sheet;
    OwnedArray<TextButton> buttons;                  // the instruments, then Chords
    OwnedArray<ComboBox> follows;                    // one per section
    std::vector<std::shared_ptr<Track>> followable;  // the tracks the menus list
    double beat = 0.0;
    String shownSig, shownTracks;
};

/** The window's content: the view, in a viewport that scrolls when the window is shorter than
    the sections that are on. */
class PlayAlongWindow::Content : public Component
{
public:
    explicit Content (AppContext& c) : view (c)
    {
        viewport.setViewedComponent (&view, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        view.onLayoutChanged = [this] { resized(); };
        setSize (980, 820);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const int needed = view.neededHeight();
        const int bar = needed > getHeight() ? viewport.getScrollBarThickness() : 0;
        view.setSize (getWidth() - bar, jmax (getHeight(), needed));
    }

private:
    Viewport viewport;
    PlayAlongView view;
};

//==============================================================================
PlayAlongWindow::PlayAlongWindow (AppContext& c)
    : DocumentWindow ("Play Along", theme::panel, DocumentWindow::closeButton), ctx (c)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (c), true);
    setResizable (true, true);
    setResizeLimits (640, 360, 2200, 1600);
    const auto saved = ctx.settings.getValue ("playAlong.window");
    if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
        centreWithSize (980, 820);
}

PlayAlongWindow::~PlayAlongWindow()
{
    ctx.settings.setValue ("playAlong.window", getWindowStateAsString());
}

void PlayAlongWindow::closeButtonPressed()   { delete this; }

void PlayAlongWindow::closeIfOpen()
{
    if (auto* w = current.getComponent())
        delete w;
}

void PlayAlongWindow::show (AppContext& ctx)
{
    if (auto* w = current.getComponent())
    {
        w->toFront (true);
        return;
    }
    auto* w = new PlayAlongWindow (ctx);
    current = w;
    w->setVisible (true);
}
} // namespace lathe
