#include "Arrangement.h"
#include <set>

namespace lathe
{
//==============================================================================
// Track header (left column)
//==============================================================================
class ArrangementView::Header : public Component
{
public:
    Header (ArrangementView& o, std::shared_ptr<Track> t)
        : track (std::move (t)), owner (o), ctx (o.ctx), meter (track)
    {
        name.setFont (theme::font (13.0f));
        name.setColour (Label::textColourId, theme::text);
        name.setEditable (false, true, false);
        name.setInterceptsMouseClicks (false, false);
        name.onTextChange = [this]
        {
            if (name.getText().trim().isNotEmpty())
            {
                track->name = name.getText().trim();
                ctx.edit.changed();
            }
        };
        addAndMakeVisible (name);

        styleToggle (arm, "", theme::record, "Arm for recording");
        styleToggle (solo, "S", theme::solo, "Solo");
        styleToggle (mute, "M", theme::mute, "Mute");
        arm.onClick = [this] { track->arm = ! track->arm.load(); ctx.edit.touched(); };
        solo.onClick = [this]
        {
            // Plain click solos exclusively; Cmd/Ctrl-click adds to the solo set.
            const bool newState = ! track->solo.load();
            if (! ModifierKeys::currentModifiers.isCommandDown())
                for (auto& other : ctx.edit.tracks)
                    other->solo = false;
            track->solo = newState;
            ctx.edit.touched();
        };
        mute.onClick = [this] { track->mute = ! track->mute.load(); ctx.edit.touched(); };
        for (auto* b : { &arm, &solo, &mute })
            addAndMakeVisible (b);

        input.setTooltip (track->isMidi() ? "MIDI input" : "Audio input from your interface");
        input.onChange = [this]
        {
            if (track->isMidi())
                applyMidiInput (input, *track, ctx.engine.getMidiInputNames());
            else
                applyAudioInput (input, *track);
            ctx.edit.changed();
        };
        addAndMakeVisible (input);

        volume.setSliderStyle (Slider::LinearHorizontal);
        volume.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.0);
        volume.setDoubleClickReturnValue (true, theme::dbToFaderPos (0.0f));
        volume.setPopupDisplayEnabled (true, true, nullptr);
        volume.setWantsKeyboardFocus (false);
        volume.textFromValueFunction = [] (double v) { return theme::formatDb (theme::faderPosToDb ((float) v)) + " dB"; };
        volume.setColour (Slider::trackColourId, theme::textDim);
        volume.onValueChange = [this]
        {
            track->volumeDb = theme::faderPosToDb ((float) volume.getValue());
            ctx.edit.markDirty();
        };
        volume.setTooltip ("Volume");
        addAndMakeVisible (volume);

        addAndMakeVisible (meter);
        update();
    }

    void update()
    {
        name.setText (track->name, dontSendNotification);
        arm.setToggleState (track->arm.load(), dontSendNotification);
        solo.setToggleState (track->solo.load(), dontSendNotification);
        mute.setToggleState (track->mute.load(), dontSendNotification);
        volume.setValue (theme::dbToFaderPos (track->volumeDb.load()), dontSendNotification);

        // Refill the input list only when the choices could have changed.
        const auto key = track->isMidi() ? ctx.engine.getMidiInputNames().joinIntoString ("|") + track->midiInput
                                         : String (ctx.engine.getInputChannelNames().size()) + ":" + String (track->input.channel) + (track->input.stereo ? "s" : "m");
        if (key != inputKey)
        {
            inputKey = key;
            if (track->isMidi())
                fillMidiInputs (input, *track, ctx.engine.getMidiInputNames());
            else
                fillAudioInputs (input, *track, ctx.engine.getInputChannelNames().size());
        }
        repaint();
    }

    void tick()   { meter.tick(); }

    void paint (Graphics& g) override
    {
        const bool selected = ctx.selection.trackId == track->id;
        g.fillAll (selected ? theme::headerSel : theme::header);
        g.setColour (track->colour);
        g.fillRect (0, 0, 5, getHeight());
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        // tiny kind tag so audio and MIDI tracks are told apart at a glance
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText (track->isMidi() ? "MIDI" : "AUDIO", 12, getHeight() - 17, 40, 12, Justification::centredLeft);
    }

    void paintOverChildren (Graphics& g) override
    {
        auto a = arm.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        g.setColour (track->arm.load() ? theme::textDark : theme::record.withSaturation (0.45f));
        g.fillEllipse (a);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (10).withTrimmedRight (12).reduced (0, 5);
        meter.setBounds (getWidth() - 10, 5, 6, getHeight() - 11);

        auto row1 = r.removeFromTop (22);
        mute.setBounds (row1.removeFromRight (22));
        row1.removeFromRight (2);
        solo.setBounds (row1.removeFromRight (22));
        row1.removeFromRight (2);
        arm.setBounds (row1.removeFromRight (22));
        row1.removeFromRight (6);
        name.setBounds (row1);

        r.removeFromTop (5);
        auto row2 = r.removeFromTop (20).withTrimmedLeft (40);
        volume.setBounds (row2.removeFromRight (70));
        row2.removeFromRight (6);
        input.setBounds (row2);
    }

    void mouseDown (const MouseEvent& e) override
    {
        ctx.selectTrack (track->id);
        if (e.mods.isPopupMenu())
            showTrackMenu (ctx, *this, track);
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (name.getBounds().contains (e.getPosition()))
            name.showEditor();
    }

    std::shared_ptr<Track> track;

private:
    ArrangementView& owner;
    AppContext& ctx;
    Label name;
    TextButton arm, solo, mute;
    ComboBox input;
    Slider volume;
    LevelMeter meter;
    String inputKey;
};

//==============================================================================
// Ruler: bar numbers, loop brace, click to place the start marker
//==============================================================================
class ArrangementView::Ruler : public Component
{
public:
    explicit Ruler (ArrangementView& o) : owner (o) {}

    void paint (Graphics& g) override
    {
        auto& edit = owner.ctx.edit;
        g.fillAll (theme::panel);

        const int loopH = 9;
        // loop brace: a solid bar when on, a thin bracket when off
        {
            const float x1 = owner.beatToX (edit.loopStart), x2 = owner.beatToX (edit.loopEnd);
            if (edit.loopEnabled)
            {
                g.setColour (theme::accent.withAlpha (0.85f));
                g.fillRect (Rectangle<float> (x1, 1.0f, x2 - x1, (float) loopH - 2.0f));
            }
            else
            {
                g.setColour (theme::textDim.withAlpha (0.6f));
                g.fillRect (Rectangle<float> (x1, 2.0f, x2 - x1, 1.0f));
                g.fillRect (Rectangle<float> (x1, 2.0f, 1.0f, (float) loopH - 4.0f));
                g.fillRect (Rectangle<float> (x2 - 1.0f, 2.0f, 1.0f, (float) loopH - 4.0f));
            }
        }

        const double barLen = edit.beatsPerBar();
        const double firstBeat = owner.xToBeat (0.0f);
        const double lastBeat = owner.xToBeat ((float) getWidth());

        // Label every bar, or every 2/4/8... when zoomed out.
        int barStep = 1;
        while (barStep * barLen * owner.pxPerBeat < 44.0)
            barStep *= 2;

        g.setFont (theme::font (11.5f));
        const int firstBar = jmax (0, (int) std::floor (firstBeat / barLen));
        for (int bar = firstBar; bar * barLen <= lastBeat; ++bar)
        {
            const float x = owner.beatToX (bar * barLen);
            const bool labelled = (bar % barStep) == 0;
            g.setColour (labelled ? theme::textDim : theme::gridBar);
            g.drawVerticalLine (roundToInt (x), labelled ? (float) loopH + 2.0f : getHeight() - 6.0f, (float) getHeight());
            if (labelled)
            {
                g.setColour (theme::text);
                g.drawText (String (bar + 1), roundToInt (x) + 3, loopH, 40, getHeight() - loopH, Justification::centredLeft);
            }
        }

        // start marker
        const float mx = owner.beatToX (edit.insertMarker);
        Path tri;
        tri.addTriangle (mx - 5.0f, (float) getHeight() - 8.0f, mx + 5.0f, (float) getHeight() - 8.0f, mx, (float) getHeight());
        g.setColour (theme::text);
        g.fillPath (tri);

        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto& edit = owner.ctx.edit;
        const double beat = owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown());

        if (e.y < 10)
        {
            const float x1 = owner.beatToX (edit.loopStart), x2 = owner.beatToX (edit.loopEnd);
            if (std::abs (e.x - x1) < 6.0f)       loopDrag = LoopDrag::start;
            else if (std::abs (e.x - x2) < 6.0f)  loopDrag = LoopDrag::end;
            else if (e.x > x1 && e.x < x2)        loopDrag = LoopDrag::move;
            else
            {
                loopDrag = LoopDrag::end;
                edit.loopStart = edit.loopEnd = beat;
            }
            grabStart = edit.loopStart;
            grabEnd = edit.loopEnd;
            grabBeat = beat;
            return;
        }

        loopDrag = LoopDrag::none;
        placeMarker (e);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        auto& edit = owner.ctx.edit;
        const double beat = owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown());
        const double minLen = owner.gridStep();

        switch (loopDrag)
        {
            case LoopDrag::start: edit.loopStart = jlimit (0.0, edit.loopEnd - minLen, beat); break;
            case LoopDrag::end:
                if (beat >= grabStart) { edit.loopStart = grabStart; edit.loopEnd = jmax (grabStart + minLen, beat); }
                else                   { edit.loopStart = jmax (0.0, beat); edit.loopEnd = grabStart + minLen; }
                break;
            case LoopDrag::move:
            {
                const double delta = jmax (-grabStart, beat - grabBeat);
                edit.loopStart = grabStart + delta;
                edit.loopEnd = grabEnd + delta;
                break;
            }
            case LoopDrag::none:
                placeMarker (e);
                return;
        }
        owner.repaint();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (loopDrag != LoopDrag::none)
        {
            auto& edit = owner.ctx.edit;
            if (edit.loopEnd - edit.loopStart < 0.01)
                edit.loopEnd = edit.loopStart + edit.beatsPerBar();
            edit.loopEnabled = true;
            edit.changed();
        }
        loopDrag = LoopDrag::none;
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y < 10)
        {
            owner.ctx.edit.loopEnabled = ! owner.ctx.edit.loopEnabled;
            owner.ctx.edit.changed();
        }
    }

private:
    void placeMarker (const MouseEvent& e)
    {
        auto& edit = owner.ctx.edit;
        edit.insertMarker = jmax (0.0, owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown()));
        owner.ctx.engine.seekTo (edit.insertMarker);
        edit.sendChangeMessage();
        owner.repaint();
    }

    enum class LoopDrag { none, start, end, move };
    ArrangementView& owner;
    LoopDrag loopDrag = LoopDrag::none;
    double grabStart = 0, grabEnd = 0, grabBeat = 0;
};

//==============================================================================
// Lanes: grid, clips, playhead
//==============================================================================
class ArrangementView::Lanes : public Component, private ChangeListener
{
public:
    explicit Lanes (ArrangementView& o) : owner (o), ctx (o.ctx)
    {
        setWantsKeyboardFocus (true);
    }

    ~Lanes() override
    {
        for (auto* t : watchedThumbs)
            t->removeChangeListener (this);
    }

    void paint (Graphics& g) override
    {
        auto& edit = ctx.edit;
        const auto clip = g.getClipBounds();
        const double step = owner.gridStep();
        const double barLen = edit.beatsPerBar();
        const double first = std::floor (owner.xToBeat ((float) clip.getX()) / step) * step;
        const double last = owner.xToBeat ((float) clip.getRight());

        g.fillAll (theme::lane);

        // rows
        for (int i = 0; i < (int) edit.tracks.size(); ++i)
        {
            const int y = owner.trackTop (i);
            if (y > clip.getBottom() || y + theme::trackHeight < clip.getY())
                continue;
            g.setColour ((i % 2) ? theme::laneAlt : theme::lane);
            g.fillRect (clip.getX(), y, clip.getWidth(), theme::trackHeight);
            if (ctx.selection.trackId == edit.tracks[(size_t) i]->id)
            {
                g.setColour (Colours::white.withAlpha (0.025f));
                g.fillRect (clip.getX(), y, clip.getWidth(), theme::trackHeight);
            }
        }

        const int contentBottom = owner.trackTop ((int) edit.tracks.size());

        // loop region shading
        if (edit.loopEnabled)
        {
            const float x1 = owner.beatToX (edit.loopStart), x2 = owner.beatToX (edit.loopEnd);
            g.setColour (theme::accent.withAlpha (0.05f));
            g.fillRect (Rectangle<float> (x1, 0.0f, x2 - x1, (float) contentBottom));
        }

        // grid
        for (double b = first; b <= last; b += step)
        {
            const float x = owner.beatToX (b);
            const bool isBar = std::abs (std::remainder (b, barLen)) < 1.0e-6;
            const bool isBeat = std::abs (std::remainder (b, 1.0)) < 1.0e-6;
            g.setColour (isBar ? theme::gridBar : isBeat ? theme::gridBeat : theme::gridFine);
            g.drawVerticalLine (roundToInt (x), 0.0f, (float) contentBottom);
        }

        for (int i = 0; i <= (int) edit.tracks.size(); ++i)
        {
            g.setColour (theme::border);
            g.drawHorizontalLine (owner.trackTop (i) - 1, 0.0f, (float) getWidth());
        }

        // empty area below the tracks
        if (contentBottom < getHeight())
        {
            g.setColour (theme::window);
            g.fillRect (0, contentBottom, getWidth(), getHeight() - contentBottom);
            if (edit.tracks.empty())
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (14.0f));
                g.drawText ("Add a track with the buttons on the left, or drop audio files here.",
                            getLocalBounds().withTop (contentBottom).withHeight (80), Justification::centred);
            }
        }

        // clips
        for (int i = 0; i < (int) edit.tracks.size(); ++i)
        {
            auto& t = *edit.tracks[(size_t) i];
            const int y = owner.trackTop (i);
            if (y > clip.getBottom() || y + theme::trackHeight < clip.getY())
                continue;

            for (auto& c : t.clips)
            {
                auto r = clipBounds (i, c);
                if (r.getRight() < (float) clip.getX() || r.getX() > (float) clip.getRight())
                    continue;
                drawClip (g, t, c, r);
            }
        }

        // start marker and playhead
        g.setColour (theme::text.withAlpha (0.35f));
        g.drawVerticalLine (roundToInt (owner.beatToX (edit.insertMarker)), 0.0f, (float) contentBottom);

        if (ctx.engine.isPlaying())
        {
            g.setColour (ctx.engine.isRecording() ? theme::record : theme::accent);
            g.fillRect (Rectangle<float> (owner.beatToX (ctx.engine.getPlayheadBeats()) - 0.5f, 0.0f, 1.5f, (float) contentBottom));
        }
    }

    Rectangle<float> clipBounds (int trackIndex, const Clip& c) const
    {
        const float x1 = owner.beatToX (c.start), x2 = owner.beatToX (c.end());
        return { x1, (float) owner.trackTop (trackIndex) + 1.0f, jmax (2.0f, x2 - x1), (float) theme::trackHeight - 3.0f };
    }

    void drawClip (Graphics& g, Track& t, const Clip& c, Rectangle<float> r)
    {
        const bool selected = ctx.selection.clipId == c.id;
        const auto colour = t.colour;
        const auto outline = r;

        g.setColour (colour.darker (1.6f));
        g.fillRect (r);

        auto titleBar = r.removeFromTop (14.0f);
        g.setColour (colour);
        g.fillRect (titleBar);
        g.setColour (theme::textDark);
        g.setFont (theme::font (11.0f));
        g.drawText (c.name, titleBar.reduced (4.0f, 0.0f), Justification::centredLeft, true);

        auto body = r.reduced (0.0f, 2.0f);
        if (c.kind == ClipKind::audio)
        {
            auto& thumb = ctx.pool.getThumbnail (c.file);
            if (watchedThumbs.insert (&thumb).second)
                thumb.addChangeListener (this);

            auto src = ctx.pool.get (c.file);
            const double sr = src != nullptr ? src->sampleRate : ctx.edit.sampleRate;
            const double startSec = (double) c.sourceOffset / jmax (1.0, sr);
            const double lenSec = c.length * 60.0 / ctx.edit.tempo;

            g.setColour (colour.brighter (0.2f));
            if (thumb.getTotalLength() > 0.0)
            {
                const float gain = Decibels::decibelsToGain (c.gainDb);
                if (thumb.getNumChannels() == 1)
                    thumb.drawChannel (g, body.toNearestInt(), startSec, startSec + lenSec, 0, gain);
                else
                    thumb.drawChannels (g, body.toNearestInt(), startSec, startSec + lenSec, gain);
            }
            else if (! c.file.existsAsFile())
            {
                g.setColour (theme::record);
                g.drawText ("Missing: " + c.file.getFileName(), body.reduced (4.0f), Justification::topLeft, true);
            }
        }
        else
        {
            int lo = 127, hi = 0;
            for (auto& n : c.notes)
            {
                lo = jmin (lo, n.pitch);
                hi = jmax (hi, n.pitch);
            }
            if (lo <= hi)
            {
                lo -= 2;
                hi += 2;
                const float rowH = body.getHeight() / (float) (hi - lo + 1);
                const float pxPerBeat = (float) owner.pxPerBeat;
                g.setColour (colour.brighter (0.3f));
                for (auto& n : c.notes)
                {
                    if (n.start >= c.length)
                        continue;
                    const float x = body.getX() + (float) n.start * pxPerBeat;
                    const float w = jmax (1.5f, (float) jmin (n.length, c.length - n.start) * pxPerBeat - 1.0f);
                    const float y = body.getBottom() - (float) (n.pitch - lo + 1) * rowH;
                    g.fillRect (x, y, w, jmax (1.5f, rowH - 1.0f));
                }
            }
        }

        g.setColour (selected ? theme::accent : theme::border);
        g.drawRect (outline, selected ? 1.5f : 1.0f);
    }

    // --- hit testing
    struct Hit { int trackIndex = -1; int clipId = -1; enum Zone { none, body, left, right } zone = none; };

    Hit hitTest (Point<int> p) const
    {
        Hit h;
        h.trackIndex = owner.trackIndexAtY (p.y);
        if (h.trackIndex < 0)
            return h;

        auto& t = *ctx.edit.tracks[(size_t) h.trackIndex];
        // later clips are drawn on top, so search backwards
        for (int i = (int) t.clips.size(); --i >= 0;)
        {
            auto& c = t.clips[(size_t) i];
            auto r = clipBounds (h.trackIndex, c);
            if (r.contains (p.toFloat()))
            {
                h.clipId = c.id;
                const float edge = jmin (7.0f, r.getWidth() / 3.0f);
                h.zone = p.x < r.getX() + edge ? Hit::left : p.x > r.getRight() - edge ? Hit::right : Hit::body;
                return h;
            }
        }
        return h;
    }

    void mouseMove (const MouseEvent& e) override
    {
        auto h = hitTest (e.getPosition());
        setMouseCursor (h.zone == Hit::left || h.zone == Hit::right ? MouseCursor::LeftRightResizeCursor
                        : h.zone == Hit::body ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto h = hitTest (e.getPosition());
        drag = {};

        if (h.trackIndex < 0)
        {
            if (e.mods.isPopupMenu())
                showAddTrackMenu();
            return;
        }

        auto track = ctx.edit.tracks[(size_t) h.trackIndex];

        if (h.clipId < 0)
        {
            ctx.selectTrack (track->id);
            ctx.clearClipSelection();
            ctx.edit.insertMarker = jmax (0.0, owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown()));
            ctx.engine.seekTo (ctx.edit.insertMarker);
            ctx.edit.sendChangeMessage();
            if (e.mods.isPopupMenu())
                showAddTrackMenu();
            owner.repaint();
            return;
        }

        ctx.selectClip (track->id, h.clipId);

        if (e.mods.isPopupMenu())
        {
            showClipMenu (track, h.clipId);
            return;
        }

        drag.mode = h.zone == Hit::left ? Drag::trimStart : h.zone == Hit::right ? Drag::trimEnd : Drag::move;
        drag.trackId = track->id;
        drag.clipId = h.clipId;
        drag.original = *track->findClip (h.clipId);
        drag.grabBeat = owner.xToBeat ((float) e.x);
        owner.repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (drag.mode == Drag::none || (e.getDistanceFromDragStart() < 3 && ! drag.started))
            return;

        auto track = ctx.edit.findTrack (drag.trackId);
        if (track == nullptr)
            return;
        auto* c = track->findClip (drag.clipId);
        if (c == nullptr)
            return;

        if (! drag.started)
        {
            drag.started = true;
            ctx.edit.beginClipEdit();
        }

        const bool noSnap = e.mods.isAltDown();
        const double beat = owner.xToBeat ((float) e.x);
        const auto& o = drag.original;

        if (drag.mode == Drag::move)
        {
            double newStart = jmax (0.0, o.start + (beat - drag.grabBeat));
            if (! noSnap)
                newStart = jmax (0.0, owner.snap (newStart, false));
            c->start = newStart;

            // Drag onto another track of the same kind.
            const int over = owner.trackIndexAtY (e.y);
            if (over >= 0)
            {
                auto target = ctx.edit.tracks[(size_t) over];
                if (target != track && target->kind == track->kind)
                {
                    Clip moved = *c;
                    track->clips.erase (track->clips.begin() + track->indexOfClip (c->id));
                    target->clips.push_back (moved);
                    drag.trackId = target->id;
                    ctx.selectClip (target->id, moved.id);
                }
            }
        }
        else if (drag.mode == Drag::trimEnd)
        {
            double end = noSnap ? beat : owner.snap (beat, false);
            double maxLength = 100000.0;
            if (o.kind == ClipKind::audio)
                if (auto src = ctx.pool.get (o.file))
                    maxLength = (double) (src->lengthInSamples - o.sourceOffset) / ctx.edit.samplesPerBeat (src->sampleRate);
            c->length = jlimit (owner.gridStep() * 0.5, maxLength, end - o.start);
        }
        else if (drag.mode == Drag::trimStart)
        {
            double start = noSnap ? beat : owner.snap (beat, false);
            start = jlimit (0.0, o.end() - owner.gridStep() * 0.5, start);
            Clip copy = o;
            ctx.edit.trimClipStart (copy, start);
            *c = copy;
        }

        owner.repaint();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (drag.mode != Drag::none && drag.started)
        {
            if (auto track = ctx.edit.findTrack (drag.trackId))
                if (auto* c = track->findClip (drag.clipId))
                {
                    const auto start = c->start, end = c->end();
                    ctx.edit.clearRange (*track, start, end, drag.clipId);
                }
            ctx.edit.changed();
        }
        drag = {};
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        auto h = hitTest (e.getPosition());
        if (h.trackIndex < 0)
            return;

        auto track = ctx.edit.tracks[(size_t) h.trackIndex];
        if (h.clipId >= 0)
        {
            if (track->isMidi() && ctx.openClipEditor)
                ctx.openClipEditor (track->id, h.clipId);
            return;
        }

        if (track->isMidi())
        {
            // New one-bar MIDI clip at the bar under the mouse.
            const double barLen = ctx.edit.beatsPerBar();
            ctx.edit.beginClipEdit();
            Clip c;
            c.id = ctx.edit.newId();
            c.kind = ClipKind::midi;
            c.name = track->name;
            c.start = std::floor (owner.xToBeat ((float) e.x) / barLen) * barLen;
            c.length = barLen;
            ctx.edit.clearRange (*track, c.start, c.end());
            track->clips.push_back (c);
            ctx.edit.changed();
            ctx.selectClip (track->id, c.id);
            if (ctx.openClipEditor)
                ctx.openClipEditor (track->id, c.id);
        }
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            owner.zoom (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, (float) e.x);
            return;
        }

        float dx = w.deltaX, dy = w.deltaY;
        if (e.mods.isShiftDown() && dx == 0.0f)
            std::swap (dx, dy);

        if (dx != 0.0f)
            owner.hbar.setCurrentRangeStart (owner.hbar.getCurrentRangeStart() - dx * 300.0 / owner.pxPerBeat);
        if (dy != 0.0f)
            owner.vbar.setCurrentRangeStart (owner.vbar.getCurrentRangeStart() - dy * 300.0);
    }

private:
    void changeListenerCallback (ChangeBroadcaster*) override   { repaint(); }

    void showAddTrackMenu()
    {
        PopupMenu m;
        m.addItem (1, "Add audio track");
        m.addItem (2, "Add MIDI track");
        m.addItem (3, "Add return track");
        auto* context = &ctx;
        m.showMenuAsync (PopupMenu::Options(), [context] (int r)
        {
            if (r == 1) context->edit.addTrack (TrackKind::audio);
            if (r == 2) context->edit.addTrack (TrackKind::midi);
            if (r == 3) context->edit.addTrack (TrackKind::bus);
        });
    }

    void showClipMenu (std::shared_ptr<Track> track, int clipId)
    {
        auto* c = track->findClip (clipId);
        if (c == nullptr)
            return;

        PopupMenu m;
        if (c->kind == ClipKind::midi)
            m.addItem (10, "Edit notes...");
        m.addItem (1, "Rename...");
        m.addItem (2, "Duplicate");
        m.addItem (3, "Split at start marker", ctx.edit.insertMarker > c->start && ctx.edit.insertMarker < c->end());
        if (c->kind == ClipKind::audio)
        {
            PopupMenu gain;
            for (float db : { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f })
                gain.addItem (100 + (int) db, theme::formatDb (db) + " dB", true, std::abs (c->gainDb - db) < 0.01f);
            m.addSubMenu ("Clip gain", gain);
            m.addItem (11, "Show file");
        }
        m.addSeparator();
        m.addItem (4, "Delete");

        auto* context = &ctx;
        auto* view = &owner;
        Component::SafePointer<Component> safe (this);
        m.showMenuAsync (PopupMenu::Options(), [=] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            auto* clip = track->findClip (clipId);
            if (clip == nullptr)
                return;

            if (r == 10 && context->openClipEditor)
                context->openClipEditor (track->id, clipId);
            else if (r == 1)
                askForText ("Rename clip", {}, clip->name, [context, track, clipId] (const String& name)
                {
                    if (auto* cc = track->findClip (clipId)) { cc->name = name; context->edit.changed(); }
                });
            else if (r == 2) view->duplicateSelectedClip();
            else if (r == 3) view->splitSelectedClip();
            else if (r == 4) view->deleteSelectedClip();
            else if (r == 11) clip->file.revealToUser();
            else if (r >= 90 && r <= 110)
            {
                context->edit.beginClipEdit();
                clip->gainDb = (float) (r - 100);
                context->edit.changed();
            }
        });
    }

    struct Drag
    {
        enum Mode { none, move, trimStart, trimEnd } mode = none;
        int trackId = -1, clipId = -1;
        Clip original;
        double grabBeat = 0.0;
        bool started = false;
    };

    ArrangementView& owner;
    AppContext& ctx;
    Drag drag;
    std::set<AudioThumbnail*> watchedThumbs;
};

//==============================================================================
ArrangementView::ArrangementView (AppContext& c) : ctx (c)
{
    ruler = std::make_unique<Ruler> (*this);
    lanes = std::make_unique<Lanes> (*this);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*lanes);
    addAndMakeVisible (headerHolder);

    hbar.addListener (this);
    vbar.addListener (this);
    hbar.setAutoHide (false);
    vbar.setAutoHide (false);
    addAndMakeVisible (hbar);
    addAndMakeVisible (vbar);

    addAudioButton.setTooltip ("Add an audio track  (Ctrl/Cmd+T)");
    addMidiButton.setTooltip ("Add a MIDI track  (Ctrl/Cmd+Shift+T)");
    addAudioButton.onClick = [this] { ctx.edit.addTrack (TrackKind::audio); };
    addMidiButton.onClick = [this] { ctx.edit.addTrack (TrackKind::midi); };
    for (auto* b : { &addAudioButton, &addMidiButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    refresh();
}

ArrangementView::~ArrangementView()
{
    hbar.removeListener (this);
    vbar.removeListener (this);
}

String ArrangementView::structureSignature() const
{
    String s;
    for (auto& t : ctx.edit.tracks)
        s << t->id << ",";
    return s;
}

void ArrangementView::refresh()
{
    const auto sig = structureSignature();
    if (sig != signature)
    {
        signature = sig;
        headers.clear();
        for (auto& t : ctx.edit.tracks)
            headerHolder.addAndMakeVisible (headers.add (new Header (*this, t)));
        resized();
    }

    for (auto* h : headers)
        h->update();

    updateScrollBars();
    repaint();
}

void ArrangementView::tick()
{
    for (auto* h : headers)
        h->tick();

    const float x = ctx.engine.isPlaying() ? beatToX (ctx.engine.getPlayheadBeats()) : -100.0f;
    if (std::abs (x - lastPlayheadX) >= 0.5f)
    {
        // Keep the playhead on screen while playing.
        if (ctx.engine.isPlaying() && (x > lanes->getWidth() - 20 || x < 0.0f) && ! isMouseButtonDownAnywhere())
        {
            scrollToPlayhead();
            return;
        }

        lanes->repaint (roundToInt (lastPlayheadX) - 2, 0, 5, lanes->getHeight());
        lanes->repaint (roundToInt (x) - 2, 0, 5, lanes->getHeight());
        lastPlayheadX = x;
    }
}

void ArrangementView::scrollToPlayhead()
{
    const double visibleBeats = lanes->getWidth() / pxPerBeat;
    hbar.setCurrentRangeStart (jmax (0.0, ctx.engine.getPlayheadBeats() - visibleBeats * 0.1));
}

double ArrangementView::gridStep() const
{
    const double barLen = ctx.edit.beatsPerBar();
    for (double step : { 0.125, 0.25, 0.5, 1.0 })
        if (step * pxPerBeat >= 14.0)
            return step;

    double step = barLen;
    while (step * pxPerBeat < 14.0)
        step *= 2.0;
    return step;
}

double ArrangementView::snap (double beat, bool bypass) const
{
    if (bypass)
        return beat;
    const double step = gridStep();
    return std::round (beat / step) * step;
}

int ArrangementView::trackIndexAtY (int laneY) const
{
    const int index = (laneY + scrollY) / theme::trackHeight;
    return (laneY + scrollY >= 0 && index < (int) ctx.edit.tracks.size()) ? index : -1;
}

void ArrangementView::zoom (double factor, float anchorX)
{
    const double anchorBeat = xToBeat (anchorX);
    pxPerBeat = jlimit (1.0, 400.0, pxPerBeat * factor);
    scrollBeats = jmax (0.0, anchorBeat - anchorX / pxPerBeat);
    updateScrollBars();
    repaint();
}

void ArrangementView::updateScrollBars()
{
    const double visibleBeats = jmax (1.0, lanes->getWidth() / pxPerBeat);
    const double total = jmax (ctx.edit.getEndBeat() + ctx.edit.beatsPerBar() * 16.0, scrollBeats + visibleBeats);
    hbar.setRangeLimits (0.0, total, dontSendNotification);
    hbar.setCurrentRange (scrollBeats, visibleBeats, dontSendNotification);

    const int contentHeight = (int) ctx.edit.tracks.size() * theme::trackHeight + 120;
    vbar.setRangeLimits (0.0, jmax (contentHeight, lanes->getHeight()), dontSendNotification);
    vbar.setCurrentRange (scrollY, lanes->getHeight(), dontSendNotification);
    scrollY = (int) vbar.getCurrentRangeStart();
}

void ArrangementView::scrollBarMoved (ScrollBar* bar, double start)
{
    if (bar == &hbar)
        scrollBeats = jmax (0.0, start);
    else
    {
        scrollY = (int) start;
        resized();
    }
    repaint();
}

void ArrangementView::paint (Graphics& g)
{
    g.fillAll (theme::window);
    g.setColour (theme::panel);
    g.fillRect (0, 0, theme::headerWidth, theme::rulerHeight);
    g.setColour (theme::border);
    g.drawHorizontalLine (theme::rulerHeight - 1, 0.0f, (float) theme::headerWidth);
}

void ArrangementView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (theme::rulerHeight);
    top.removeFromLeft (theme::headerWidth);
    top.removeFromRight (vbar.getWidth() > 0 ? 10 : 0);
    ruler->setBounds (top);

    auto bottom = r.removeFromBottom (10);
    hbar.setBounds (bottom.withTrimmedLeft (theme::headerWidth).withTrimmedRight (10));

    vbar.setBounds (r.removeFromRight (10));
    auto left = r.removeFromLeft (theme::headerWidth);
    lanes->setBounds (r);

    auto buttons = left.removeFromBottom (32).reduced (8, 5);
    addAudioButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
    buttons.removeFromLeft (4);
    addMidiButton.setBounds (buttons);

    headerHolder.setBounds (left);
    for (int i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, trackTop (i), left.getWidth(), theme::trackHeight);

    updateScrollBars();
}

//==============================================================================
bool ArrangementView::deleteSelectedClip()
{
    auto track = ctx.edit.findTrack (ctx.selection.clipTrackId);
    if (track == nullptr)
        return false;

    const int index = track->indexOfClip (ctx.selection.clipId);
    if (index < 0)
        return false;

    ctx.edit.beginClipEdit();
    track->clips.erase (track->clips.begin() + index);
    ctx.clearClipSelection();
    ctx.edit.changed();
    return true;
}

void ArrangementView::duplicateSelectedClip()
{
    auto track = ctx.edit.findTrack (ctx.selection.clipTrackId);
    if (track == nullptr)
        return;
    auto* c = track->findClip (ctx.selection.clipId);
    if (c == nullptr)
        return;

    ctx.edit.beginClipEdit();
    Clip copy = *c;
    copy.id = ctx.edit.newId();
    copy.start = c->end();
    ctx.edit.clearRange (*track, copy.start, copy.end());
    track->clips.push_back (copy);
    ctx.selectClip (track->id, copy.id);
    ctx.edit.changed();
}

void ArrangementView::splitSelectedClip()
{
    auto track = ctx.edit.findTrack (ctx.selection.clipTrackId);
    if (track == nullptr)
        return;
    auto* c = track->findClip (ctx.selection.clipId);
    if (c == nullptr || ctx.edit.insertMarker <= c->start || ctx.edit.insertMarker >= c->end())
    {
        ctx.showStatus ("Place the start marker inside the selected clip to split it.");
        return;
    }

    ctx.edit.beginClipEdit();
    const int newId = ctx.edit.splitClip (*track, c->id, ctx.edit.insertMarker);
    if (newId >= 0)
        ctx.selectClip (track->id, newId);
    ctx.edit.changed();
}

bool ArrangementView::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files)
        if (File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
            return true;
    return false;
}

void ArrangementView::filesDropped (const StringArray& files, int x, int y)
{
    auto p = lanes->getLocalPoint (this, Point<int> (x, y));
    Array<File> list;
    for (auto& f : files)
        if (File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
            list.add (File (f));

    importFiles (list, trackIndexAtY (p.y), jmax (0.0, snap (xToBeat ((float) p.x), false)));
}

void ArrangementView::importFiles (const Array<File>& files, int trackIndex, double atBeat)
{
    if (files.isEmpty())
        return;

    auto& edit = ctx.edit;
    edit.ensureWorkDir();
    edit.beginClipEdit();
    MouseCursor::showWaitCursor();

    int imported = 0;
    for (int i = 0; i < files.size(); ++i)
    {
        // First file goes on the audio track under the mouse; the rest get new tracks.
        std::shared_ptr<Track> track;
        if (i == 0 && trackIndex >= 0 && trackIndex < (int) edit.tracks.size() && edit.tracks[(size_t) trackIndex]->isAudio())
            track = edit.tracks[(size_t) trackIndex];
        else
        {
            track = edit.addTrack (TrackKind::audio, files[i].getFileNameWithoutExtension());
            track->input.channel = -1;
        }

        String error;
        const auto file = ctx.pool.importFile (files[i], edit.getAudioDir(), ctx.engine.getSampleRate() > 0 ? ctx.engine.getSampleRate() : edit.sampleRate, error);
        auto src = file.existsAsFile() ? ctx.pool.get (file) : nullptr;
        if (src == nullptr)
        {
            ctx.showStatus (error.isNotEmpty() ? error : "Couldn't import " + files[i].getFileName());
            continue;
        }

        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::audio;
        c.name = files[i].getFileNameWithoutExtension();
        c.file = file;
        c.start = atBeat;
        c.length = (double) src->lengthInSamples / edit.samplesPerBeat (src->sampleRate);
        edit.clearRange (*track, c.start, c.end());
        track->clips.push_back (c);
        ++imported;
    }

    MouseCursor::hideWaitCursor();
    edit.changed();
    if (imported > 0)
        ctx.showStatus ("Imported " + String (imported) + (imported == 1 ? " file" : " files") + " into the project's Audio folder.");
}
} // namespace lathe
