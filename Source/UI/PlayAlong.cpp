#include "PlayAlong.h"
#include "Theme.h"
#include "../Engine/Engine.h"

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
            b->onClick = [this] { saveChoice(); resized(); repaint(); };
            addAndMakeVisible (b);

            // What this diagram follows: the song's key, or a track with keys of its own.
            auto* f = follows.add (new ComboBox());
            f->setTooltip ("Whose key this shows: the song's, or a track with a key of its own "
                           "(Track menu > Own Key). Automatic picks a track named after the instrument.");
            f->onChange = [this, i] { followChosen (i); };
            addChildComponent (f);
        }
        rebuildFollowMenus();
        setSize (760, 520);
        startTimerHz (20);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12).removeFromTop (headerH).removeFromRight (380);
        for (auto* b : buttons)
            b->setBounds (r.removeFromLeft (90).reduced (2, 4));

        const auto slots = layout();
        const bool anyTrackKeys = std::any_of (ctx.edit.tracks.begin(), ctx.edit.tracks.end(), [] (auto& t) { return ! t->keys.empty(); });
        for (int i = 0; i < follows.size(); ++i)
        {
            auto it = slots.find (i);
            follows[i]->setVisible (anyTrackKeys && it != slots.end());
            if (it != slots.end())
                follows[i]->setBounds (it->second.withHeight (20).withTrimmedLeft (90).withWidth (190).toNearestInt());
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
        const bool anyTrackKeys = std::any_of (ctx.edit.tracks.begin(), ctx.edit.tracks.end(), [] (auto& t) { return ! t->keys.empty(); });
        if (! st.key && ctx.edit.keys.empty() && ! anyTrackKeys)
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

        // The song's key at the top.
        auto line = head.removeFromTop (head.getHeight() * 0.6f);
        if (st.key)
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
            g.drawText (ctx.edit.keys.empty() ? "No song key" : "No key yet", line, Justification::centredLeft);
        }
        g.setFont (theme::font (12.5f));
        g.setColour (st.nextFade > 0.0f ? theme::text : theme::textDim);
        String sub = st.key ? "From bar " + bar (st.key->beat) : String (ctx.edit.keys.empty() ? "Tracks with their own key show it below"
                                                                                                : "Before the first key change");
        if (st.next)
            sub << "   " << String::fromUTF8 ("\xc2\xb7") << "   Next: " << keys::keyName (*st.next) << " at bar " << bar (st.next->beat);
        g.drawText (sub, head, Justification::centredLeft);

        const auto slots = layout();
        if (slots.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText ("Pick an instrument above.", r.withTrimmedTop (10.0f).removeFromTop (30.0f), Justification::centredLeft);
            return;
        }
        for (auto& [i, slot] : slots)
        {
            const auto& inst = fretboard::instruments()[(size_t) i];
            const auto followed = followedTrack (i);
            const auto ds = stateAt (ctx.edit, beat, followed.get());
            // Following a track: say whose key, and what it is, beside the menu.
            if (followed != nullptr)
            {
                auto label = slot.withHeight (18.0f).withTrimmedLeft (follows[i]->isVisible() ? 290.0f : 90.0f);
                g.setFont (theme::font (12.0f, true));
                if (ds.key)
                {
                    g.setColour (theme::keyColour (ds.key->root));
                    g.drawText (keys::keyName (*ds.key) + (ds.next ? "   (next: " + keys::keyName (*ds.next) + ", bar " + bar (ds.next->beat) + ")" : String()),
                                label, Justification::centredLeft);
                }
            }
            drawInstrument (g, slot, inst, ds.key ? &*ds.key : nullptr, ds.next ? &*ds.next : nullptr, ds.nextFade);
        }
    }

private:
    /** Where each instrument that's on goes (by index), sharing the height below the header,
        each at most as tall as reads well. */
    std::map<int, Rectangle<float>> layout() const
    {
        std::map<int, Rectangle<float>> out;
        auto r = getLocalBounds().toFloat().reduced (12.0f);
        r.removeFromTop ((float) headerH + 10.0f);
        std::vector<int> on;
        for (int i = 0; i < buttons.size(); ++i)
            if (buttons[i]->getToggleState())
                on.push_back (i);
        if (on.empty())
            return out;
        const float each = r.getHeight() / (float) on.size();
        for (int i : on)
        {
            auto slot = r.removeFromTop (each);
            const auto& inst = fretboard::instruments()[(size_t) i];
            const float wanted = inst.strings.empty() ? 150.0f : 22.0f + 26.0f * (float) inst.strings.size() + 14.0f;
            out[i] = slot.withHeight (jmin (wanted, slot.getHeight() - 8.0f));
        }
        return out;
    }

    String followKey (int i) const   { return "playAlong.follow." + instrumentNames()[i]; }

    /** The track instrument i follows, or null for the song's key. The setting is "auto",
        "song" or a track's tag; a tag whose track is gone (or has no keys now) means auto. */
    std::shared_ptr<Track> followedTrack (int i) const
    {
        const auto choice = ctx.settings.getValue (followKey (i), "auto");
        if (choice == "song")
            return {};
        if (choice != "auto")
            if (auto t = ctx.edit.findTrackByUid (choice); t != nullptr && ! t->keys.empty())
                return t;
        return autoFollow (ctx.edit, instrumentNames()[i]);
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
            const auto automatic = autoFollow (ctx.edit, instrumentNames()[i]);
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
        repaint();
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

        // Repaint only when what's drawn would change: a key, the next one, or the fade's step.
        auto describe = [] (const State& st)
        {
            String sig;
            if (st.key) sig << keys::keyName (*st.key) << "@" << st.key->beat;
            if (st.next) sig << "|" << keys::keyName (*st.next) << "@" << st.next->beat;
            sig << "|" << roundToInt (st.nextFade * 20.0f) << ";";
            return sig;
        };
        String sig = describe (stateAt (ctx.edit, now));
        for (int i = 0; i < buttons.size(); ++i)
            if (buttons[i]->getToggleState())
                if (auto t = followedTrack (i))
                    sig << i << describe (stateAt (ctx.edit, now, t.get()));
        sig << (int) ctx.edit.keys.size();
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
    OwnedArray<ComboBox> follows;                    // one per instrument
    std::vector<std::shared_ptr<Track>> followable;  // the tracks the menus list
    double beat = 0.0;
    String shownSig, shownTracks;
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
