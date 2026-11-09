#include "Arrangement.h"
#include "Notes.h"
#include "../Engine/ClipJoin.h"
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
        : track (std::move (t)), ctx (o.ctx), meter (track)
    {
        name.setFont (theme::font (13.0f));
        name.setColour (Label::textColourId, theme::text);
        // Double-click the name to rename it in place (Return to keep, Escape to cancel).
        name.setEditable (false, false, false);
        name.setInterceptsMouseClicks (false, false);
        name.onTextChange = [this]
        {
            if (name.getText().trim().isNotEmpty() && name.getText().trim() != track->name)
            {
                track->name = name.getText().trim();
                ctx.edit.changed();
            }
        };
        // Don't touch the text here: on Return, JUCE hides the editor before it reports
        // the new text, so resetting it now would throw the rename away.
        name.onEditorHide = [this] { name.setInterceptsMouseClicks (false, false); };
        addAndMakeVisible (name);

        styleToggle (arm, "", theme::record, "Arm for recording");
        styleToggle (solo, "S", theme::solo, "Solo");
        styleToggle (mute, "M", theme::mute, "Mute");
        arm.onClick = [this] { track->arm = ! track->arm.load(); ctx.edit.touched(); };
        solo.onClick = [this] { toggleSolo (ctx, track.get(), nullptr); };
        mute.onClick = [this] { track->mute = ! track->mute.load(); ctx.edit.touched(); };
        for (auto* b : { &arm, &solo, &mute })
            addAndMakeVisible (b);

        input.setTooltip (track->isMidi() ? "MIDI input" : "Audio input: your interface, or another track (to record an instrument with its effects)");
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
        volume.setScrollWheelEnabled (false);   // the wheel scrolls the track list
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
        updateMutedLook();
        resized();

        // Refill the input list only when the choices could have changed.
        const auto key = track->isMidi() ? midiInputKey (ctx.edit, *track, ctx.engine.getMidiInputNames())
                                         : audioInputKey (ctx.edit, *track, ctx.engine.getInputChannelNames().size());
        if (key != inputKey)
        {
            inputKey = key;
            if (track->isMidi())
                fillMidiInputs (input, ctx.edit, *track, ctx.engine.getMidiInputNames());
            else
                fillAudioInputs (input, ctx.edit, *track, ctx.engine.getInputChannelNames().size());
        }
        repaint();
    }

    void tick()   { meter.tick(); }

    int indent() const   { return ctx.edit.findGroup (track->groupId) != nullptr ? 8 : 0; }

    void paint (Graphics& g) override
    {
        g.fillAll (ctx.isSelected (track->id) ? theme::headerSel : theme::header);

        const int x = indent();
        if (auto group = ctx.edit.findGroup (track->groupId))
        {
            g.setColour (group->colour.withAlpha (0.55f));
            g.fillRect (0, 0, 4, getHeight());
        }
        g.setColour (track->colour);
        g.fillRect (x, 0, 5, getHeight());
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        // tiny kind tag so audio and MIDI tracks are told apart at a glance
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText (track->isMidi() ? "MIDI" : "AUDIO", x + 12, getHeight() - 17, 40, 12, Justification::centredLeft);
    }

    void paintOverChildren (Graphics& g) override
    {
        auto a = arm.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        g.setColour (track->arm.load() ? theme::textDark : theme::record.withSaturation (0.45f));
        g.fillEllipse (a);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (10 + indent()).withTrimmedRight (12).reduced (0, 5);
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
        if (e.mods.isPopupMenu())
        {
            if (! ctx.isSelected (track->id))
                ctx.selectTrack (track->id);
            showTrackMenu (ctx, *this, track);
        }
        else if (e.mods.isShiftDown())
            ctx.extendSelection (track->id, true);
        else if (e.mods.isCommandDown())
            ctx.extendSelection (track->id, false);
        else
            ctx.selectTrack (track->id);
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (name.getBounds().expanded (4).contains (e.getPosition()))
            MessageManager::callAsync ([safe = Component::SafePointer<Header> (this)] { if (safe != nullptr) safe->startRename(); });
    }

    void startRename()
    {
        name.setInterceptsMouseClicks (true, true);
        name.showEditor();
        if (auto* editor = name.getCurrentTextEditor())
        {
            editor->setFont (theme::font (13.0f));
            editor->selectAll();
        }
    }

    void updateMutedLook()
    {
        name.setColour (Label::textColourId, ctx.edit.isEffectivelyMuted (*track) ? theme::textDim : theme::text);
    }

    std::shared_ptr<Track> track;

private:
    AppContext& ctx;
    Label name;
    TextButton arm, solo, mute;
    ComboBox input;
    Slider volume;
    LevelMeter meter;
    String inputKey;
};

//==============================================================================
// Group header: collapse, name, and arm / solo / mute for every member
//==============================================================================
class ArrangementView::GroupHeader : public Component
{
public:
    GroupHeader (ArrangementView& o, std::shared_ptr<TrackGroup> g)
        : group (std::move (g)), ctx (o.ctx)
    {
        name.setFont (theme::font (13.0f, true));
        name.setEditable (false, false, false);
        name.setInterceptsMouseClicks (false, false);
        name.onTextChange = [this]
        {
            ctx.edit.renameGroup (group->id, name.getText());
        };
        // Don't touch the text here: on Return, JUCE hides the editor before it reports
        // the new text, so resetting it now would throw the rename away.
        name.onEditorHide = [this] { name.setInterceptsMouseClicks (false, false); };
        addAndMakeVisible (name);

        styleToggle (arm, "", theme::record, "Arm / disarm every track in the group");
        styleToggle (solo, "S", theme::solo, "Solo the whole group");
        styleToggle (mute, "M", theme::mute, "Mute the whole group");
        fx.setButtonText ("FX");
        fx.setWantsKeyboardFocus (false);
        fx.onClick = [this] { showGroupBus (ctx, group); };
        addAndMakeVisible (fx);
        arm.onClick = [this] { ctx.edit.toggleGroupArm (group->id); };
        solo.onClick = [this] { toggleSolo (ctx, nullptr, group.get()); };
        mute.onClick = [this] { group->mute = ! group->mute.load(); ctx.edit.touched(); };
        for (auto* b : { &arm, &solo, &mute })
            addAndMakeVisible (b);
        update();
    }

    void update()
    {
        name.setText (group->name, dontSendNotification);
        arm.setToggleState (ctx.edit.isGroupArmed (group->id), dontSendNotification);
        solo.setToggleState (group->solo.load(), dontSendNotification);
        mute.setToggleState (group->mute.load(), dontSendNotification);
        const auto bus = ctx.edit.findGroupBus (group->id);
        fx.setToggleState (bus != nullptr, dontSendNotification);
        fx.setTooltip (bus != nullptr ? "Show \"" + bus->name + "\": the group's plug-ins, EQ and fader"
                                      : String ("Add plug-ins to the whole group (makes a group bus every track plays through)"));
        repaint();
    }

    void paint (Graphics& g) override
    {
        bool allSelected = true;
        const auto members = ctx.edit.groupMembers (group->id);
        for (auto& t : members)
            allSelected = allSelected && ctx.isSelected (t->id);

        g.fillAll (allSelected && ! members.empty() ? theme::headerSel : theme::panelRaised);
        g.setColour (group->colour);
        g.fillRect (0, 0, 4, getHeight());
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        // disclosure triangle
        auto t = disclosureArea().toFloat().withSizeKeepingCentre (8.0f, 8.0f);
        Path p;
        if (group->collapsed)
            p.addTriangle (t.getX() + 1.0f, t.getY(), t.getX() + 1.0f, t.getBottom(), t.getRight(), t.getCentreY());
        else
            p.addTriangle (t.getX(), t.getY() + 1.0f, t.getRight(), t.getY() + 1.0f, t.getCentreX(), t.getBottom());
        g.setColour (theme::text);
        g.fillPath (p);

        // member count, shown in the gap between the name and the buttons
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText (String ((int) members.size()) + (members.size() == 1 ? " track" : " tracks"),
                    name.getBounds().withLeft (name.getX() + nameTextWidth() + 8).withRight (fx.getX() - 4),
                    Justification::centredLeft, true);
    }

    void paintOverChildren (Graphics& g) override
    {
        auto a = arm.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        g.setColour (arm.getToggleState() ? theme::textDark : theme::record.withSaturation (0.45f));
        g.fillEllipse (a);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (8).withTrimmedRight (12).reduced (0, 4);
        r.removeFromLeft (disclosureArea().getWidth());
        mute.setBounds (r.removeFromRight (22));
        r.removeFromRight (2);
        solo.setBounds (r.removeFromRight (22));
        r.removeFromRight (2);
        arm.setBounds (r.removeFromRight (22));
        r.removeFromRight (2);
        fx.setBounds (r.removeFromRight (28));
        r.removeFromRight (6);
        name.setBounds (r);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (disclosureArea().contains (e.getPosition()) && ! e.mods.isPopupMenu())
        {
            group->collapsed = ! group->collapsed;
            ctx.edit.changed();
            return;
        }

        // Clicking the group selects all of its tracks.
        const auto members = ctx.edit.groupMembers (group->id);
        if (! members.empty())
        {
            ctx.selectTrack (members.front()->id);
            for (auto& t : members)
                ctx.selection.trackIds.insert (t->id);
            if (ctx.selectionChanged)
                ctx.selectionChanged();
        }

        if (e.mods.isPopupMenu())
            showGroupMenu (ctx, *this, group);
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (name.getBounds().expanded (4).contains (e.getPosition()))
            MessageManager::callAsync ([safe = Component::SafePointer<GroupHeader> (this)]
            {
                if (safe == nullptr)
                    return;
                safe->name.setInterceptsMouseClicks (true, true);
                safe->name.showEditor();
                if (auto* editor = safe->name.getCurrentTextEditor())
                    editor->selectAll();
            });
    }

    std::shared_ptr<TrackGroup> group;

private:
    Rectangle<int> disclosureArea() const   { return { 8, 0, 18, getHeight() }; }
    int nameTextWidth() const               { return GlyphArrangement::getStringWidthInt (name.getFont(), group->name); }

    AppContext& ctx;
    Label name;
    TextButton arm, solo, mute, fx;
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
            g.drawVerticalLine (roundToInt (x), labelled ? (float) loopH + 2.0f : (float) getHeight() - 6.0f, (float) getHeight());
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
            const float mx = (float) e.x;
            if (std::abs (mx - x1) < 6.0f)       loopDrag = LoopDrag::start;
            else if (std::abs (mx - x2) < 6.0f)  loopDrag = LoopDrag::end;
            else if (mx > x1 && mx < x2)         loopDrag = LoopDrag::move;
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
class ArrangementView::Lanes : public Component, public TooltipClient, private ChangeListener
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
        int stripe = 0;
        for (auto& row : owner.rows)
        {
            const int y = row.y - owner.scrollY;
            if (y > clip.getBottom() || y + row.height < clip.getY())
            {
                ++stripe;
                continue;
            }

            if (row.group != nullptr)
            {
                // group row: a band in the group's colour
                g.setColour (theme::panel.interpolatedWith (row.group->colour, 0.10f));
                g.fillRect (clip.getX(), y, clip.getWidth(), row.height);
                continue;
            }

            g.setColour ((stripe++ % 2) ? theme::laneAlt : theme::lane);
            g.fillRect (clip.getX(), y, clip.getWidth(), row.height);
            if (ctx.isSelected (edit.tracks[(size_t) row.trackIndex]->id))
            {
                g.setColour (Colours::white.withAlpha (0.025f));
                g.fillRect (clip.getX(), y, clip.getWidth(), row.height);
            }
        }

        const int contentBottom = owner.contentBottom();

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

        g.setColour (theme::border);
        for (auto& row : owner.rows)
            g.drawHorizontalLine (row.y + row.height - owner.scrollY - 1, 0.0f, (float) getWidth());

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
            if (! owner.isTrackShown (i))
                continue;
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

        // what's being recorded right now: a red clip growing on each armed track
        const bool recording = ctx.engine.isRecording() && ctx.engine.isPlaying();
        if (recording)
        {
            // (The engine records straight through, ignoring the loop.)
            const double from = ctx.engine.recordStartBeats.load();
            const double to = ctx.engine.getPlayheadBeats();
            for (int i = 0; i < (int) edit.tracks.size(); ++i)
            {
                auto& t = *edit.tracks[(size_t) i];
                if (! owner.isTrackShown (i) || ! t.holdsClips() || ! t.arm.load() || to <= from)
                    continue;
                const float x1 = owner.beatToX (from), x2 = owner.beatToX (to);
                drawRecordingClip (g, { x1, (float) owner.trackTop (i) + 1.0f, jmax (2.0f, x2 - x1), (float) theme::trackHeight - 3.0f });
            }
        }

        // notes left on tracks
        for (auto& mark : noteMarks())
            drawNoteMark (g, mark);

        // box selection
        if (! marquee.isEmpty())
        {
            g.setColour (theme::accent.withAlpha (0.08f));
            g.fillRect (marquee);
            g.setColour (theme::accent.withAlpha (0.7f));
            g.drawRect (marquee, 1.0f);
        }

        // start marker and playhead
        g.setColour (theme::text.withAlpha (0.35f));
        g.drawVerticalLine (roundToInt (owner.beatToX (edit.insertMarker)), 0.0f, (float) contentBottom);

        if (ctx.engine.isPlaying())
        {
            const float w = recording ? 2.0f : 1.5f;
            g.setColour (recording ? theme::record : theme::accent);
            g.fillRect (Rectangle<float> (owner.beatToX (ctx.engine.getPlayheadBeats()) - w * 0.5f, 0.0f, w, (float) contentBottom));
        }
    }

    void drawRecordingClip (Graphics& g, Rectangle<float> r)
    {
        const auto outline = r;
        g.setColour (theme::record.withAlpha (0.16f));
        g.fillRect (r);
        auto titleBar = r.removeFromTop (14.0f);
        g.setColour (theme::record);
        g.fillRect (titleBar);
        g.setColour (Colours::white);
        g.setFont (theme::font (11.0f));
        auto text = titleBar.reduced (4.0f, 0.0f);
        if (text.getWidth() > 12.0f)
        {
            g.fillEllipse (text.removeFromLeft (7.0f).withSizeKeepingCentre (6.0f, 6.0f));
            text.removeFromLeft (4.0f);
            g.drawText ("Recording", text, Justification::centredLeft, true);
        }
        g.setColour (theme::record);
        g.drawRect (outline, 1.5f);
    }

    Rectangle<float> clipBounds (int trackIndex, const Clip& c) const
    {
        const float x1 = owner.beatToX (c.start), x2 = owner.beatToX (c.end());
        return { x1, (float) owner.trackTop (trackIndex) + 1.0f, jmax (2.0f, x2 - x1), (float) theme::trackHeight - 3.0f };
    }

    void drawClip (Graphics& g, Track& t, const Clip& c, Rectangle<float> r)
    {
        const bool selected = ctx.isClipSelected (c.id);
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
                const float beatWidth = (float) owner.pxPerBeat;
                g.setColour (colour.brighter (0.3f));
                for (auto& n : c.notes)
                {
                    if (n.start >= c.length)
                        continue;
                    const float x = body.getX() + (float) n.start * beatWidth;
                    const float w = jmax (1.5f, (float) jmin (n.length, c.length - n.start) * beatWidth - 1.0f);
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

    Hit hitTestClip (Point<int> p) const
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
                h.zone = (float) p.x < r.getX() + edge ? Hit::left : (float) p.x > r.getRight() - edge ? Hit::right : Hit::body;
                return h;
            }
        }
        return h;
    }

    void mouseMove (const MouseEvent& e) override
    {
        for (auto& mark : noteMarks())
            if (mark.r.contains (e.position))
            {
                setMouseCursor (MouseCursor::PointingHandCursor);
                return;
            }
        auto h = hitTestClip (e.getPosition());
        setMouseCursor (h.zone == Hit::left || h.zone == Hit::right ? MouseCursor::LeftRightResizeCursor
                        : h.zone == Hit::body ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        drag = {};
        clickBeat = jmax (0.0, owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown()));
        clickScreen = Rectangle<int> (e.getScreenX() - 2, e.getScreenY() - 2, 4, 4);

        // A note's marker: open its conversation.
        for (auto& mark : noteMarks())
            if (mark.r.contains (e.position))
            {
                if (ctx.notes != nullptr)
                    ctx.notes->showThread (mark.id, localAreaToGlobal (mark.r.toNearestInt()));
                return;
            }

        auto h = hitTestClip (e.getPosition());

        if (h.trackIndex < 0)
        {
            if (e.mods.isPopupMenu())
                showAddTrackMenu();
            return;
        }

        auto track = ctx.edit.tracks[(size_t) h.trackIndex];

        const bool adding = (e.mods.isCommandDown() || e.mods.isShiftDown()) && ! e.mods.isPopupMenu();

        if (h.clipId < 0)
        {
            // Click: set the start marker here (where Play starts and Paste goes).
            // Drag: draw a box to select clips; with Cmd/Ctrl or Shift it adds to the selection.
            ctx.selectTrack (track->id);
            drag.baseSelection = adding ? ctx.selection.clips : std::set<std::pair<int, int>>();
            if (! adding)
                ctx.clearClipSelection();
            ctx.edit.insertMarker = jmax (0.0, owner.snap (owner.xToBeat ((float) e.x), e.mods.isAltDown()));
            ctx.engine.seekTo (ctx.edit.insertMarker);
            ctx.edit.sendChangeMessage();
            if (e.mods.isPopupMenu())
                showLaneMenu (track);
            else
            {
                drag.mode = Drag::marquee;
                drag.anchor = e.position;
            }
            owner.repaint();
            return;
        }

        if (adding)
        {
            ctx.toggleClip (track->id, h.clipId);
            owner.repaint();
            return;
        }

        // Right-clicking one of several selected clips keeps them all (for Join, Copy...).
        if (! (e.mods.isPopupMenu() && ctx.isClipSelected (h.clipId) && ctx.selection.clips.size() > 1))
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

        if (drag.mode == Drag::marquee)
        {
            drag.started = true;
            marquee = Rectangle<float> (drag.anchor, e.position);
            auto picked = drag.baseSelection;
            for (int i = 0; i < (int) ctx.edit.tracks.size(); ++i)
            {
                if (! owner.isTrackShown (i))
                    continue;
                auto& t = *ctx.edit.tracks[(size_t) i];
                for (auto& c : t.clips)
                    if (clipBounds (i, c).intersects (marquee))
                        picked.insert ({ t.id, c.id });
            }
            ctx.setClipSelection (picked);
            repaint();
            return;
        }

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
        if (drag.mode == Drag::marquee)
        {
            marquee = {};
            drag = {};
            repaint();
            return;
        }

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
        auto h = hitTestClip (e.getPosition());
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

    // --- notes
    struct NoteMark { String id; Rectangle<float> r; int messages = 1; bool resolved = false, unread = false, pending = false; };

    std::vector<NoteMark> noteMarks() const
    {
        std::vector<NoteMark> out;
        if (ctx.notes == nullptr)
            return out;
        for (auto& n : ctx.notes->getNotes())
        {
            if (! ctx.notes->isShownOnTracks (n))
                continue;
            for (int i = 0; i < (int) ctx.edit.tracks.size(); ++i)
            {
                if (ctx.edit.tracks[(size_t) i]->id != n.track || ! owner.isTrackShown (i))
                    continue;
                NoteMark m;
                m.id = n.id;
                m.messages = 1 + (int) n.replies.size();
                m.resolved = n.resolved;
                m.pending = n.pending;
                m.unread = ctx.notes->isUnread (n);
                const float w = m.messages > 9 ? 26.0f : 20.0f;
                m.r = { owner.beatToX (n.beat), (float) owner.trackTop (i) + 3.0f, w, 15.0f };
                out.push_back (m);
            }
        }
        return out;
    }

    void drawNoteMark (Graphics& g, const NoteMark& m)
    {
        // A speech bubble whose tail points down at the moment the note is about.
        // Faded until the cloud has it (written offline, not sent yet).
        const auto fill = (m.resolved ? theme::control : theme::accent).withMultipliedAlpha (m.pending ? 0.45f : 1.0f);
        Path p;
        p.addRoundedRectangle (m.r, 3.0f);
        p.addTriangle (m.r.getX(), m.r.getBottom() - 2.0f, m.r.getX() + 6.0f, m.r.getBottom() - 2.0f, m.r.getX(), m.r.getBottom() + 5.0f);
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillPath (p, AffineTransform::translation (0.0f, 1.0f));
        g.setColour (fill);
        g.fillPath (p);
        g.setColour (fill.darker (0.5f));
        g.drawVerticalLine (roundToInt (m.r.getX()), m.r.getBottom() + 5.0f, m.r.getBottom() + 40.0f);
        g.setColour (m.resolved ? theme::textDim : theme::textDark);
        g.setFont (theme::font (11.0f, true));
        g.drawText (String (m.messages), m.r, Justification::centred, false);
        if (m.unread)
        {
            g.setColour (Colours::white);
            g.fillEllipse (m.r.getRight() - 4.0f, m.r.getY() - 3.0f, 7.0f, 7.0f);
        }
    }

    String getTooltip() override
    {
        if (ctx.notes == nullptr)
            return {};
        const auto pos = getMouseXYRelative().toFloat();
        for (auto& mark : noteMarks())
            if (mark.r.expanded (2.0f).contains (pos))
                if (auto* n = ctx.notes->find (mark.id))
                {
                    auto text = (n->author.isNotEmpty() ? n->author.upToFirstOccurrenceOf ("@", false, false) : String ("You")) + ": " + n->text;
                    if (text.length() > 160)
                        text = text.substring (0, 157) + "...";
                    if (! n->replies.empty())
                        text << "\n(" << (int) n->replies.size() << (n->replies.size() == 1 ? " reply" : " replies") << ". Click to read.)";
                    return text;
                }
        return {};
    }

    /** Right-click on empty lane space: a note here, or a new track. */
    void showLaneMenu (std::shared_ptr<Track> track)
    {
        PopupMenu m;
        if (ctx.notes != nullptr)
        {
            m.addItem (10, "Add Note Here...");
            m.addSeparator();
        }
        m.addItem (1, "Add audio track");
        m.addItem (2, "Add MIDI track");
        m.addItem (3, "Add return track");
        auto* context = &ctx;
        const auto beat = clickBeat;
        const auto area = clickScreen;
        const int trackId = track->id;
        m.showMenuAsync (PopupMenu::Options(), [context, beat, area, trackId] (int r)
        {
            if (r == 10 && context->notes != nullptr) context->notes->showCompose (trackId, beat, area);
            if (r == 1) context->edit.addTrack (TrackKind::audio);
            if (r == 2) context->edit.addTrack (TrackKind::midi);
            if (r == 3) context->edit.addTrack (TrackKind::bus);
        });
    }

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
        const int numSelected = owner.numSelectedClips();
        m.addItem (1, "Rename...", numSelected <= 1);
        m.addItem (20, numSelected > 1 ? "Copy " + String (numSelected) + " Clips" : String ("Copy"));
        m.addItem (21, "Paste at Start Marker", owner.canPaste());
        m.addItem (2, "Duplicate");
        m.addItem (22, "Join " + String (jmax (2, numSelected)) + " Clips", numSelected > 1);
        if (ctx.notes != nullptr)
            m.addItem (30, "Add Note Here...");
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
        const auto noteBeat = clickBeat;
        const auto noteArea = clickScreen;
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
            else if (r == 20) view->copySelectedClips();
            else if (r == 21) view->pasteClips();
            else if (r == 22) view->joinSelectedClips();
            else if (r == 30 && context->notes != nullptr) context->notes->showCompose (track->id, noteBeat, noteArea);
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
        enum Mode { none, move, trimStart, trimEnd, marquee } mode = none;
        int trackId = -1, clipId = -1;
        Clip original;
        double grabBeat = 0.0;
        bool started = false;
        Point<float> anchor;                              // marquee
        std::set<std::pair<int, int>> baseSelection;      // marquee with Cmd/Ctrl or Shift adds to this
    };
    Rectangle<float> marquee;
    double clickBeat = 0.0;               // where the last click was (for "Add Note Here")
    Rectangle<int> clickScreen;

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
        s << String::toHexString ((pointer_sized_int) t.get()) << ":" << t->groupId << ",";   // identity, not just id
    s << "|";
    for (auto& g : ctx.edit.groups)
        s << String::toHexString ((pointer_sized_int) g.get()) << (g->collapsed ? "c" : "o") << ",";
    return s;
}

void ArrangementView::layoutRows()
{
    auto& edit = ctx.edit;
    rows.clear();
    trackTops.assign (edit.tracks.size(), -1);
    std::set<int> placedGroups;
    int y = 0;

    // Display order follows edit.tracks; a group's header appears where its first
    // member is, followed by all its members.
    for (size_t i = 0; i < edit.tracks.size(); ++i)
    {
        auto& t = edit.tracks[i];
        auto group = edit.findGroup (t->groupId);

        if (group == nullptr)
        {
            rows.push_back ({ nullptr, (int) i, y, theme::trackHeight });
            trackTops[i] = y;
            y += theme::trackHeight;
            continue;
        }

        if (placedGroups.count (group->id))
            continue;
        placedGroups.insert (group->id);

        rows.push_back ({ group, -1, y, kGroupHeaderHeight });
        y += kGroupHeaderHeight;

        if (group->collapsed)
            continue;

        for (size_t j = i; j < edit.tracks.size(); ++j)
        {
            if (edit.tracks[j]->groupId != group->id)
                continue;
            rows.push_back ({ nullptr, (int) j, y, theme::trackHeight });
            trackTops[j] = y;
            y += theme::trackHeight;
        }
    }

    contentHeight = y;
}

void ArrangementView::refresh()
{
    const auto sig = structureSignature();
    if (sig != signature)
    {
        signature = sig;
        headers.clear();
        groupHeaders.clear();
        for (auto& t : ctx.edit.tracks)
            headerHolder.addChildComponent (headers.add (new Header (*this, t)));
        for (auto& g : ctx.edit.groups)
            headerHolder.addChildComponent (groupHeaders.add (new GroupHeader (*this, g)));
    }

    layoutRows();
    for (auto* h : headers)
        h->update();
    for (auto* h : groupHeaders)
        h->update();

    resized();
    repaint();
}

void ArrangementView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    // Over the track headers (or anywhere that doesn't use the wheel itself): scroll the list.
    float dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown() && dx == 0.0f)
        std::swap (dx, dy);

    if (dy != 0.0f)
        vbar.setCurrentRangeStart (vbar.getCurrentRangeStart() - dy * 300.0);
    if (dx != 0.0f)
        hbar.setCurrentRangeStart (hbar.getCurrentRangeStart() - dx * 300.0 / pxPerBeat);
}

void ArrangementView::tick()
{
    for (auto* h : headers)
        h->tick();

    // Recording started or stopped: redraw the lanes so the red take appears / goes.
    // While it runs, redraw the take itself as it grows (its label and outline move).
    const bool rec = ctx.engine.isRecording();
    if (rec != wasRecording)
    {
        wasRecording = rec;
        lanes->repaint();
    }
    else if (rec)
    {
        const int x1 = jmax (0, roundToInt (beatToX (ctx.engine.recordStartBeats.load())) - 2);
        const int x2 = roundToInt (beatToX (ctx.engine.getPlayheadBeats())) + 4;
        if (x2 > x1)
            lanes->repaint (x1, 0, x2 - x1, lanes->getHeight());
    }

    const float x = ctx.engine.isPlaying() ? beatToX (ctx.engine.getPlayheadBeats()) : -100.0f;
    if (std::abs (x - lastPlayheadX) >= 0.5f)
    {
        // Keep the playhead on screen while playing.
        if (ctx.engine.isPlaying() && (x > (float) lanes->getWidth() - 20.0f || x < 0.0f) && ! isMouseButtonDownAnywhere())
        {
            scrollToPlayhead();
            return;
        }

        lanes->repaint (roundToInt (lastPlayheadX) - 2, 0, 5, lanes->getHeight());
        lanes->repaint (roundToInt (x) - 2, 0, 5, lanes->getHeight());
        lastPlayheadX = x;
    }
}

void ArrangementView::reveal (int trackId, double beat)
{
    const double visibleBeats = lanes->getWidth() / pxPerBeat;
    if (beat < scrollBeats || beat > scrollBeats + visibleBeats * 0.85)
        hbar.setCurrentRangeStart (jmax (0.0, beat - visibleBeats * 0.25));
    for (size_t i = 0; i < ctx.edit.tracks.size() && i < trackTops.size(); ++i)
    {
        if (ctx.edit.tracks[i]->id != trackId || trackTops[i] < 0)
            continue;
        const int top = trackTops[i];
        if (top < scrollY || top + theme::trackHeight > scrollY + lanes->getHeight())
            vbar.setCurrentRangeStart (jmax (0, top - theme::trackHeight));
    }
    ctx.selectTrack (trackId);
    ctx.edit.insertMarker = beat;
    if (! ctx.engine.isPlaying())
        ctx.engine.seekTo (beat);
    ctx.edit.sendChangeMessage();
    repaint();
}

Rectangle<int> ArrangementView::screenAreaFor (int trackId, double beat) const
{
    int y = 0;
    for (int i = 0; i < (int) ctx.edit.tracks.size(); ++i)
        if (ctx.edit.tracks[(size_t) i]->id == trackId)
            y = jmax (0, trackTop (i)) + 10;
    const auto x = roundToInt (jlimit (0.0f, (float) lanes->getWidth(), beatToX (beat)));
    return lanes->localAreaToGlobal (Rectangle<int> (x - 2, y - 2, 4, 4));
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
    const int y = laneY + scrollY;
    for (auto& row : rows)
        if (y >= row.y && y < row.y + row.height)
            return row.trackIndex;   // -1 for group rows
    return -1;
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

    const int totalHeight = contentHeight + 120;
    vbar.setRangeLimits (0.0, jmax (totalHeight, lanes->getHeight()), dontSendNotification);
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
    // Clamp the scroll position to this content first: after switching to a shorter
    // project the old position would put every header out of sight.
    updateScrollBars();

    auto buttons = left.removeFromBottom (32).reduced (8, 5);
    addAudioButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
    buttons.removeFromLeft (4);
    addMidiButton.setBounds (buttons);

    headerHolder.setBounds (left);
    for (int i = 0; i < headers.size(); ++i)
    {
        headers[i]->setVisible (isTrackShown (i));
        if (isTrackShown (i))
            headers[i]->setBounds (0, trackTop (i), left.getWidth(), theme::trackHeight);
    }
    for (auto* gh : groupHeaders)
    {
        gh->setVisible (false);
        for (auto& row : rows)
            if (row.group == gh->group)
            {
                gh->setBounds (0, row.y - scrollY, left.getWidth(), row.height);
                gh->setVisible (true);
            }
    }

    updateScrollBars();
}

//==============================================================================
std::vector<ArrangementView::SelectedClip> ArrangementView::getSelectedClips() const
{
    auto wanted = ctx.selection.clips;
    if (ctx.selection.clipId >= 0)
        wanted.insert ({ ctx.selection.clipTrackId, ctx.selection.clipId });

    std::vector<SelectedClip> out;
    for (int i = 0; i < (int) ctx.edit.tracks.size(); ++i)
    {
        auto& t = ctx.edit.tracks[(size_t) i];
        std::vector<SelectedClip> onTrack;
        for (auto& c : t->clips)
            if (wanted.count ({ t->id, c.id }) > 0)
                onTrack.push_back ({ t, i, &c });
        std::sort (onTrack.begin(), onTrack.end(), [] (const SelectedClip& a, const SelectedClip& b) { return a.clip->start < b.clip->start; });
        out.insert (out.end(), onTrack.begin(), onTrack.end());
    }
    return out;
}

int ArrangementView::numSelectedClips() const
{
    return (int) getSelectedClips().size();
}

std::vector<ArrangementView::CopiedClip> ArrangementView::gatherSelection (double& blockStart, double& blockEnd, int& topTrack) const
{
    std::vector<CopiedClip> out;
    const auto selected = getSelectedClips();
    if (selected.empty())
        return out;

    blockStart = selected.front().clip->start;
    blockEnd = selected.front().clip->end();
    topTrack = selected.front().trackIndex;
    for (auto& s : selected)
    {
        blockStart = jmin (blockStart, s.clip->start);
        blockEnd = jmax (blockEnd, s.clip->end());
        topTrack = jmin (topTrack, s.trackIndex);
    }

    for (auto& s : selected)
    {
        CopiedClip c;
        c.clip = *s.clip;
        c.clip.start -= blockStart;
        c.trackOffset = s.trackIndex - topTrack;
        c.kind = s.track->kind;
        out.push_back (c);
    }
    return out;
}

int ArrangementView::placeClips (const std::vector<CopiedClip>& clips, double atBeat, int topTrackIndex)
{
    int skipped = 0;
    std::set<std::pair<int, int>> placed;
    for (auto& c : clips)
    {
        const int index = topTrackIndex + c.trackOffset;
        if (index < 0 || index >= (int) ctx.edit.tracks.size() || ctx.edit.tracks[(size_t) index]->kind != c.kind)
        {
            ++skipped;
            continue;
        }

        auto& track = *ctx.edit.tracks[(size_t) index];
        Clip copy = c.clip;
        copy.id = ctx.edit.newId();
        copy.start = atBeat + c.clip.start;
        ctx.edit.clearRange (track, copy.start, copy.end());
        track.clips.push_back (copy);
        placed.insert ({ track.id, copy.id });
    }

    if (! placed.empty())
    {
        ctx.selection.trackId = ctx.edit.tracks[(size_t) jlimit (0, (int) ctx.edit.tracks.size() - 1, topTrackIndex)]->id;
        ctx.setClipSelection (placed);
    }
    return skipped;
}

bool ArrangementView::deleteSelectedClip()
{
    const auto selected = getSelectedClips();
    if (selected.empty())
        return false;

    std::vector<std::pair<std::shared_ptr<Track>, int>> doomed;   // ids: erasing moves the clips
    for (auto& s : selected)
        doomed.push_back ({ s.track, s.clip->id });

    ctx.edit.beginClipEdit();
    for (auto& [track, id] : doomed)
    {
        const int index = track->indexOfClip (id);
        if (index >= 0)
            track->clips.erase (track->clips.begin() + index);
    }
    ctx.clearClipSelection();
    ctx.edit.changed();
    return true;
}

void ArrangementView::duplicateSelectedClip()
{
    double start = 0.0, end = 0.0;
    int top = 0;
    const auto block = gatherSelection (start, end, top);
    if (block.empty())
        return;

    ctx.edit.beginClipEdit();
    placeClips (block, end, top);
    ctx.edit.changed();
}

bool ArrangementView::copySelectedClips()
{
    double start = 0.0, end = 0.0;
    int top = 0;
    auto block = gatherSelection (start, end, top);
    if (block.empty())
        return false;

    clipboard = std::move (block);
    clipboardLength = end - start;
    ctx.showStatus (clipboard.size() == 1 ? String ("Copied 1 clip. Click where it should go and paste (Cmd/Ctrl+V).")
                                          : "Copied " + String ((int) clipboard.size()) + " clips. Click where they should go and paste (Cmd/Ctrl+V).");
    return true;
}

bool ArrangementView::cutSelectedClips()
{
    if (! copySelectedClips())
        return false;
    deleteSelectedClip();
    ctx.showStatus (clipboard.size() == 1 ? String ("Cut 1 clip.") : "Cut " + String ((int) clipboard.size()) + " clips.");
    return true;
}

void ArrangementView::pasteClips()
{
    if (clipboard.empty())
    {
        ctx.showStatus ("Nothing to paste. Select a clip and copy it first (Cmd/Ctrl+C).");
        return;
    }

    int top = -1;
    for (int i = 0; i < (int) ctx.edit.tracks.size(); ++i)
        if (ctx.edit.tracks[(size_t) i]->id == ctx.selection.trackId)
            top = i;
    if (top < 0)
    {
        ctx.showStatus ("Click a track to choose where to paste.");
        return;
    }

    int fits = 0;
    for (auto& c : clipboard)
    {
        const int index = top + c.trackOffset;
        fits += (index < (int) ctx.edit.tracks.size() && ctx.edit.tracks[(size_t) index]->kind == c.kind) ? 1 : 0;
    }
    if (fits == 0)
    {
        auto kind = clipboard.front().kind == TrackKind::midi ? "a MIDI" : "an audio";
        ctx.showStatus (clipboard.size() == 1 ? String ("That's ") + kind + " clip: paste it on " + kind + " track."
                                              : String ("The clips don't fit here: paste on tracks of the same kind (audio or MIDI)."));
        return;
    }

    ctx.edit.beginClipEdit();
    const double at = ctx.edit.insertMarker;
    const int skipped = placeClips (clipboard, at, top);

    // Move the start marker past what was pasted, so pasting again lines up after it.
    ctx.edit.insertMarker = at + clipboardLength;
    if (! ctx.engine.isPlaying())
        ctx.engine.seekTo (ctx.edit.insertMarker);
    ctx.edit.changed();

    if (skipped > 0)
        ctx.showStatus ("Pasted. " + String (skipped) + (skipped == 1 ? " clip didn't" : " clips didn't")
                        + " fit: there was no track of the same kind (audio or MIDI) for it.");
}

void ArrangementView::joinSelectedClips()
{
    const auto selected = getSelectedClips();
    if (selected.size() < 2)
    {
        ctx.showStatus ("Select two or more clips on a track to join them (Cmd/Ctrl- or Shift-click, or drag a box around them).");
        return;
    }

    // One join per track.
    std::map<int, std::vector<int>> byTrack;
    for (auto& s : selected)
        byTrack[s.track->id].push_back (s.clip->id);

    bool any = false;
    for (auto& [trackId, ids] : byTrack)
        any = any || ids.size() >= 2;
    if (! any)
    {
        ctx.showStatus ("Clips are joined per track: select two or more clips on the same track.");
        return;
    }

    for (auto& [trackId, ids] : byTrack)
    {
        auto track = ctx.edit.findTrack (trackId);
        if (track != nullptr && ids.size() >= 2 && track->isAudio())
            for (int id : ids)
                if (auto* c = track->findClip (id); c != nullptr && ! c->file.existsAsFile())
                {
                    ctx.showStatus ("Can't join: " + c->file.getFileName() + " is missing.");
                    return;
                }
    }

    ctx.edit.beginClipEdit();
    std::set<std::pair<int, int>> joined;
    String problem;
    for (auto& [trackId, ids] : byTrack)
    {
        auto track = ctx.edit.findTrack (trackId);
        if (track == nullptr || ids.size() < 2)
            continue;

        int newId = -1;
        auto r = joinClips (ctx.edit, ctx.pool, *track, ids, newId);
        if (r.failed())
            problem = r.getErrorMessage();
        else
            joined.insert ({ trackId, newId });
    }

    if (joined.empty())
    {
        ctx.edit.undo();
        ctx.showStatus (problem);
        return;
    }

    ctx.setClipSelection (joined);
    ctx.edit.changed();
    ctx.showStatus (problem.isNotEmpty() ? "Joined, except: " + problem
                                         : joined.size() == 1 ? String ("Joined into one clip.") : "Joined into one clip per track.");
}

void ArrangementView::selectAllClips()
{
    std::set<std::pair<int, int>> all;
    for (auto& t : ctx.edit.tracks)
        for (auto& c : t->clips)
            all.insert ({ t->id, c.id });
    ctx.setClipSelection (all);
    repaint();
}

void ArrangementView::splitSelectedClip()
{
    const double at = ctx.edit.insertMarker;
    std::vector<std::pair<std::shared_ptr<Track>, int>> targets;   // ids: splitting adds clips
    for (auto& s : getSelectedClips())
        if (at > s.clip->start && at < s.clip->end())
            targets.push_back ({ s.track, s.clip->id });

    if (targets.empty())
    {
        ctx.showStatus ("Place the start marker inside the selected clip to split it.");
        return;
    }

    ctx.edit.beginClipEdit();
    std::set<std::pair<int, int>> rights;
    for (auto& [track, id] : targets)
    {
        const int newId = ctx.edit.splitClip (*track, id, at);
        if (newId >= 0)
            rights.insert ({ track->id, newId });
    }
    if (! rights.empty())
        ctx.setClipSelection (rights);
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
