#include "PlayAlong.h"
#include "Theme.h"
#include "../Engine/Engine.h"

namespace lathe
{
Component::SafePointer<PlayAlongWindow> PlayAlongWindow::current;

PlayAlongWindow::State PlayAlongWindow::stateAt (const Edit& edit, double beat)
{
    State s;
    s.key = edit.keyAt (beat);
    s.next = keys::after (edit.keys, beat);
    if (s.next != nullptr)
    {
        const double bar = edit.beatsPerBar();
        const double until = s.next->beat - beat;
        if (until <= bar)
            s.nextFade = (float) jlimit (0.0, 1.0, 1.0 - until / bar);
    }
    return s;
}

namespace
{
    const StringArray& instrumentNames()
    {
        static const StringArray names = []
        {
            StringArray n;
            for (auto& i : fretboard::instruments())
                n.add (i.name);
            return n;
        }();
        return names;
    }

    /** One instrument, drawn for a key (and, fading in, the next one). */
    void drawInstrument (Graphics& g, Rectangle<float> area, const fretboard::Instrument& inst,
                         const KeyChange* key, const KeyChange* next, float fade)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f, true));
        g.drawText (inst.name, area.removeFromTop (18.0f), Justification::centredLeft);
        area.removeFromTop (2.0f);

        const bool flats = key != nullptr && keys::usesFlats (key->root, key->scale);
        const bool nextFlats = next != nullptr && keys::usesFlats (next->root, next->scale);
        const auto keyColour = key != nullptr ? theme::keyColour (key->root) : theme::textDim;
        const auto nextColour = next != nullptr ? theme::keyColour (next->root) : theme::textDim;

        // A note: the root filled with the key's colour, the other notes of the key a light dot
        // ringed with it. Notes only in the next key are rings that strengthen as it nears.
        auto drawNote = [&] (Point<float> c, float radius, int pitch)
        {
            const bool now = key != nullptr && keys::contains (key->root, key->scale, pitch);
            const bool later = next != nullptr && fade > 0.0f && keys::contains (next->root, next->scale, pitch);
            if (! now && ! later)
                return;
            auto circle = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
            String label;
            if (now)
            {
                const bool root = keys::isRoot (key->root, pitch);
                // A note leaving with the key change dims as it goes.
                const float alpha = later || fade <= 0.0f ? 1.0f : 1.0f - 0.6f * fade;
                g.setColour ((root ? keyColour : theme::text).withAlpha (alpha));
                g.fillEllipse (circle);
                g.setColour (keyColour.withAlpha (alpha));
                g.drawEllipse (circle.reduced (0.75f), 1.5f);
                g.setColour (theme::textDark.withAlpha (alpha));
                label = keys::noteName (pitch, flats);
            }
            else
            {
                const bool root = keys::isRoot (next->root, pitch);
                g.setColour (nextColour.withAlpha (0.2f + 0.7f * fade));
                g.drawEllipse (circle.reduced (1.0f), root ? 2.5f : 1.75f);
                g.setColour (theme::text.withAlpha (0.25f + 0.6f * fade));
                label = keys::noteName (pitch, nextFlats);
            }
            g.setFont (theme::font (jmin (12.0f, radius * 1.05f), true));
            g.drawText (label, circle, Justification::centred, false);
        };

        if (inst.strings.empty())
        {
            // Keyboard: two octaves of white keys, the black keys on top, a dot on each note.
            const int whiteCount = 14;
            const float w = area.getWidth() / (float) whiteCount;
            const float h = jmin (area.getHeight(), w * 4.2f);
            auto keysArea = area.withHeight (h);
            static const bool black[] { false, true, false, true, false, false, true, false, true, false, true, false };
            int white = 0;
            std::vector<std::pair<int, Rectangle<float>>> whites, blacks;
            for (int p = fretboard::keyboardLow; p <= fretboard::keyboardHigh; ++p)
            {
                if (! black[p % 12])
                    whites.push_back ({ p, { keysArea.getX() + white++ * w, keysArea.getY(), w, h } });
                else
                    blacks.push_back ({ p, { keysArea.getX() + white * w - w * 0.3f, keysArea.getY(), w * 0.6f, h * 0.6f } });
            }
            for (auto& [p, r] : whites)
            {
                g.setColour (Colour (0xffe8e8e8));
                g.fillRect (r.reduced (0.5f, 0.0f));
                g.setColour (theme::border);
                g.drawRect (r, 1.0f);
            }
            for (auto& [p, r] : blacks)
            {
                g.setColour (Colour (0xff1a1a1a));
                g.fillRoundedRectangle (r, 2.0f);
            }
            const float radius = jmin (w * 0.36f, 11.0f);
            for (auto& [p, r] : whites)
                drawNote ({ r.getCentreX(), r.getBottom() - radius - 5.0f }, radius, p);
            for (auto& [p, r] : blacks)
                drawNote ({ r.getCentreX(), r.getBottom() - radius - 3.0f }, jmin (radius, r.getWidth() * 0.48f), p);
            return;
        }

        // A neck seen from the front, the nut on the left, the thickest string at the bottom
        // (like a chord chart turned on its side, or tab).
        const int strings = (int) inst.strings.size();
        const float openW = 26.0f;
        auto neck = area.withTrimmedLeft (openW);
        const float fretW = neck.getWidth() / (float) inst.frets;
        const float gap = neck.getHeight() / (float) strings;
        auto stringY = [&] (int s) { return neck.getBottom() - gap * ((float) s + 0.5f); };

        g.setColour (Colour (0xff3a3027));
        g.fillRect (neck.withTop (stringY (strings - 1) - gap * 0.35f).withBottom (stringY (0) + gap * 0.35f));

        // Inlays at 3, 5, 7, 9, 12 (two), 15.
        g.setColour (Colour (0xff6b5e50));
        for (int f : { 3, 5, 7, 9, 12, 15 })
        {
            if (f > inst.frets)
                continue;
            const float x = neck.getX() + fretW * ((float) f - 0.5f);
            const float cy = neck.getCentreY();
            const float r = jmin (4.5f, gap * 0.22f);
            if (f == 12 && strings > 2)
            {
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, stringY (1) - gap * 0.5f }));
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, stringY (strings - 2) + gap * 0.5f }));
            }
            else
                g.fillEllipse (Rectangle<float> (r * 2, r * 2).withCentre ({ x, cy }));
        }

        const float top = stringY (strings - 1) - gap * 0.35f, bottom = stringY (0) + gap * 0.35f;
        for (int f = 0; f <= inst.frets; ++f)
        {
            const float x = neck.getX() + fretW * (float) f;
            g.setColour (f == 0 ? Colour (0xffd8d0c0) : Colour (0xff8a8a8a));
            g.fillRect (x - (f == 0 ? 2.0f : 0.75f), top, f == 0 ? 4.0f : 1.5f, bottom - top);
        }
        g.setFont (theme::font (10.0f));
        g.setColour (theme::textDim);
        for (int f : { 3, 5, 7, 9, 12, 15 })
            if (f <= inst.frets)
                g.drawText (String (f), Rectangle<float> (neck.getX() + fretW * ((float) f - 1.0f), bottom + 1.0f, fretW, 12.0f),
                            Justification::centred);

        for (int s = 0; s < strings; ++s)
        {
            g.setColour (Colour (0xffb8b0a0));
            g.fillRect (neck.getX(), stringY (s) - 0.5f - (float) (strings - 1 - s) * 0.12f,
                        neck.getWidth(), 1.0f + (float) (strings - 1 - s) * 0.25f);
        }

        const float radius = jmin (gap * 0.42f, fretW * 0.38f, 11.0f);
        for (auto& spot : fretboard::spots (inst, key != nullptr ? key->root : 0, key != nullptr ? key->scale : Scale::major))
        {
            const float x = spot.fret == 0 ? area.getX() + openW * 0.5f - 2.0f
                                           : neck.getX() + fretW * ((float) spot.fret - 0.5f);
            drawNote ({ x, stringY (spot.string) }, radius, spot.pitch);
        }
    }
}

//==============================================================================
class PlayAlongWindow::Content : public Component, private Timer
{
public:
    explicit Content (AppContext& c) : ctx (c)
    {
        auto shown = StringArray::fromTokens (ctx.settings.getValue ("playAlong.instruments", "Guitar"), ",", "");
        for (int i = 0; i < instrumentNames().size(); ++i)
        {
            auto* b = buttons.add (new TextButton (instrumentNames()[i]));
            b->setClickingTogglesState (true);
            b->setToggleState (shown.contains (instrumentNames()[i]), dontSendNotification);
            b->setColour (TextButton::buttonOnColourId, theme::accent);
            b->setColour (TextButton::textColourOnId, theme::textDark);
            b->onClick = [this] { saveChoice(); repaint(); };
            addAndMakeVisible (b);
        }
        setSize (760, 520);
        startTimerHz (20);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12).removeFromTop (headerH).removeFromRight (380);
        for (auto* b : buttons)
        {
            b->setBounds (r.removeFromLeft (90).reduced (2, 4));
        }
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);
        auto r = getLocalBounds().toFloat().reduced (12.0f);
        auto head = r.removeFromTop ((float) headerH);
        head.removeFromRight (390.0f);

        const auto st = stateAt (ctx.edit, beat);
        const double barLen = ctx.edit.beatsPerBar();
        auto bar = [barLen] (double b) { return String (b / barLen + 1.0, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };
        if (st.key == nullptr && ctx.edit.keys.empty())
        {
            g.setColour (theme::text);
            g.setFont (theme::font (15.0f, true));
            g.drawText ("No key set", head, Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawFittedText ("Click the Key lane under the bar numbers to set the song's key, or ask the AI assistant "
                              "to work the keys out. The notes of the key show up here, on every instrument you pick.",
                              r.reduced (0.0f, 10.0f).removeFromTop (60.0f).toNearestInt(), Justification::topLeft, 3);
            return;
        }

        auto line = head.removeFromTop (head.getHeight() * 0.6f);
        if (st.key != nullptr)
        {
            g.setColour (theme::keyColour (st.key->root));
            g.fillRoundedRectangle (line.removeFromLeft (8.0f).reduced (0.0f, 4.0f), 2.0f);
            line.removeFromLeft (8.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (19.0f, true));
            g.drawText (keys::keyName (*st.key), line, Justification::centredLeft);
        }
        else
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (17.0f, true));
            g.drawText ("No key yet", line, Justification::centredLeft);
        }
        g.setFont (theme::font (12.5f));
        g.setColour (st.nextFade > 0.0f ? theme::text : theme::textDim);
        String sub = st.key != nullptr ? "From bar " + bar (st.key->beat) : String ("Before the first key change");
        if (st.next != nullptr)
            sub << "   " << String::fromUTF8 ("\xc2\xb7") << "   Next: " << keys::keyName (*st.next) << " at bar " << bar (st.next->beat);
        g.drawText (sub, head, Justification::centredLeft);

        r.removeFromTop (10.0f);
        std::vector<const fretboard::Instrument*> chosen;
        for (int i = 0; i < buttons.size(); ++i)
            if (buttons[i]->getToggleState())
                chosen.push_back (&fretboard::instruments()[(size_t) i]);
        if (chosen.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText ("Pick an instrument above.", r.removeFromTop (30.0f), Justification::centredLeft);
            return;
        }

        // Share the height, each instrument at most as tall as reads well.
        const float each = r.getHeight() / (float) chosen.size();
        for (auto* inst : chosen)
        {
            auto slot = r.removeFromTop (each);
            const float wanted = inst->strings.empty() ? 150.0f : 22.0f + 26.0f * (float) inst->strings.size() + 14.0f;
            drawInstrument (g, slot.withHeight (jmin (wanted, slot.getHeight() - 8.0f)), *inst, st.key, st.next, st.nextFade);
        }
    }

private:
    void timerCallback() override
    {
        auto& engine = ctx.engine;
        const double now = engine.isPlaying() ? engine.getPlayheadBeats() : ctx.edit.insertMarker;
        const auto st = stateAt (ctx.edit, now);
        // Repaint only when what's drawn would change: the key, the next one, or the fade's step.
        String sig;
        if (st.key != nullptr) sig << keys::keyName (*st.key) << "@" << st.key->beat;
        if (st.next != nullptr) sig << "|" << keys::keyName (*st.next) << "@" << st.next->beat;
        sig << "|" << roundToInt (st.nextFade * 20.0f) << "|" << (int) ctx.edit.keys.size();
        beat = now;
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

    static constexpr int headerH = 46;
    AppContext& ctx;
    OwnedArray<TextButton> buttons;
    double beat = 0.0;
    String shownSig;
};

//==============================================================================
PlayAlongWindow::PlayAlongWindow (AppContext& c)
    : DocumentWindow ("Play Along", theme::panel, DocumentWindow::closeButton), ctx (c)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (c), true);
    setResizable (true, true);
    setResizeLimits (520, 260, 1800, 1400);
    const auto saved = ctx.settings.getValue ("playAlong.window");
    if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
        centreWithSize (760, 520);
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
