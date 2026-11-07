#include "Mixer.h"

namespace lathe
{
namespace
{
constexpr int kInsertRows = 4;

/** One insert in a strip: click opens, right-click for bypass / move / remove. */
class InsertSlot : public Component, public SettableTooltipClient
{
public:
    InsertSlot (AppContext& c, std::shared_ptr<Track> t, std::shared_ptr<PluginSlot> s)
        : ctx (c), track (std::move (t)), slot (std::move (s))
    {
        setTooltip (slot->isMissing() ? slot->getName() + " (not installed)" : slot->getName());
    }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);

        const bool off = slot->bypassed.load();
        g.setColour (slot->isMissing() ? theme::record : off ? theme::textDim : theme::text);
        g.setFont (theme::font (11.5f));
        g.drawText (slot->getName(), getLocalBounds().reduced (5, 0), Justification::centredLeft, true);

        // little power indicator
        g.setColour (off || slot->isMissing() ? theme::control : theme::accent);
        g.fillRect (Rectangle<float> (3.0f, r.getHeight() * 0.5f - 0.5f, 2.0f, 2.0f).withX (r.getRight() - 6.0f));
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showPluginSlotMenu (ctx, *this, track, slot);
        else if (e.mouseWasClicked())
            openPluginEditor (ctx, slot, track->name);
    }

private:
    AppContext& ctx;
    std::shared_ptr<Track> track;
    std::shared_ptr<PluginSlot> slot;
};
} // namespace

//==============================================================================
class ChannelStrip : public Component
{
public:
    ChannelStrip (AppContext& c, std::shared_ptr<Track> t)
        : track (std::move (t)), ctx (c), meter (track), pan (true)
    {
        name.setFont (theme::font (12.5f, true));
        name.setJustificationType (Justification::centredLeft);
        name.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (name);

        if (track->holdsClips())
        {
            input.onChange = [this]
            {
                if (track->isMidi()) applyMidiInput (input, *track, ctx.engine.getMidiInputNames());
                else                 applyAudioInput (input, *track);
                ctx.edit.changed();
            };
            input.setTooltip (track->isMidi() ? "MIDI input" : "Audio input");
            addAndMakeVisible (input);

            for (auto* b : { &monAuto, &monIn, &monOff })
            {
                b->setClickingTogglesState (false);
                b->setWantsKeyboardFocus (false);
                b->setColour (TextButton::buttonOnColourId, theme::accent);
                addAndMakeVisible (b);
            }
            monAuto.setTooltip ("Monitor the input while the track is armed");
            monIn.setTooltip ("Always monitor the input");
            monOff.setTooltip ("Never monitor the input (use the interface's direct monitoring)");
            monAuto.onClick = [this] { track->monitor = (int) Monitor::automatic; ctx.edit.touched(); };
            monIn.onClick   = [this] { track->monitor = (int) Monitor::in;        ctx.edit.touched(); };
            monOff.onClick  = [this] { track->monitor = (int) Monitor::off;       ctx.edit.touched(); };

            styleToggle (arm, "", theme::record, "Arm for recording");
            arm.onClick = [this] { track->arm = ! track->arm.load(); ctx.edit.touched(); };
            addAndMakeVisible (arm);
        }

        if (track->isMidi())
        {
            midiOut.onChange = [this] { applyMidiOutput (midiOut, *track, ctx.engine.getMidiOutputNames()); ctx.edit.changed(); };
            midiOut.setTooltip ("Send this track's MIDI to a hardware port (e.g. the UMC's MIDI OUT)");
            addAndMakeVisible (midiOut);
        }

        output.onChange = [this] { applyOutput (output, *track, ctx.edit); ctx.edit.changed(); };
        output.setTooltip ("Where this channel's audio goes");
        addAndMakeVisible (output);

        addInsert.setTooltip ("Add a plug-in");
        addInsert.onClick = [this] { showAddPluginMenu (ctx, addInsert, track, -1); };
        addInsert.setWantsKeyboardFocus (false);
        addAndMakeVisible (addInsert);

        pan.setTooltip ("Pan");
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.textFromValueFunction = [] (double v)
        {
            const int p = roundToInt (v * 50.0);
            return p == 0 ? String ("C") : String (std::abs (p)) + (p < 0 ? "L" : "R");
        };
        pan.onValueChange = [this] { track->pan = (float) pan.getValue(); ctx.edit.markDirty(); };
        addAndMakeVisible (pan);

        fader.onValueChange = [this]
        {
            track->volumeDb = theme::faderPosToDb ((float) fader.getValue());
            dbLabel.setText (theme::formatDb (track->volumeDb.load()), dontSendNotification);
            ctx.edit.markDirty();
        };
        addAndMakeVisible (fader);
        addAndMakeVisible (meter);

        dbLabel.setFont (theme::font (11.5f));
        dbLabel.setJustificationType (Justification::centred);
        dbLabel.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (dbLabel);

        styleToggle (solo, "S", theme::solo, "Solo");
        styleToggle (mute, "M", theme::mute, "Mute");
        solo.onClick = [this] { toggleSolo (ctx, track.get(), nullptr); };
        mute.onClick = [this] { track->mute = ! track->mute.load(); ctx.edit.touched(); };
        if (! track->isMaster())
        {
            addAndMakeVisible (solo);
            addAndMakeVisible (mute);
        }

        update();
    }

    void update()
    {
        name.setText (track->name, dontSendNotification);
        fader.setValue (theme::dbToFaderPos (track->volumeDb.load()), dontSendNotification);
        dbLabel.setText (theme::formatDb (track->volumeDb.load()), dontSendNotification);
        pan.setValue (track->pan.load(), dontSendNotification);
        solo.setToggleState (track->solo.load(), dontSendNotification);
        mute.setToggleState (track->mute.load(), dontSendNotification);
        arm.setToggleState (track->arm.load(), dontSendNotification);

        const int mon = track->monitor.load();
        monAuto.setToggleState (mon == (int) Monitor::automatic, dontSendNotification);
        monIn.setToggleState (mon == (int) Monitor::in, dontSendNotification);
        monOff.setToggleState (mon == (int) Monitor::off, dontSendNotification);

        // Routing lists: refill only when something they depend on changed.
        String key;
        key << ctx.engine.getInputChannelNames().size() << "|" << ctx.engine.getOutputChannelNames().size() << "|"
            << ctx.engine.getMidiInputNames().joinIntoString (",") << "|" << ctx.engine.getMidiOutputNames().joinIntoString (",") << "|";
        for (auto& b : ctx.edit.buses)
            key << b->id << b->name << ",";
        key << track->input.channel << (track->input.stereo ? 1 : 0) << track->midiInput << track->midiOutput
            << (int) track->output.kind << track->output.busId << track->output.channel << (track->output.stereo ? 1 : 0);

        if (key != routingKey)
        {
            routingKey = key;
            if (track->isMidi())
            {
                fillMidiInputs (input, *track, ctx.engine.getMidiInputNames());
                fillMidiOutputs (midiOut, *track, ctx.engine.getMidiOutputNames());
            }
            else if (track->isAudio())
            {
                fillAudioInputs (input, *track, ctx.engine.getInputChannelNames().size());
            }
            fillOutputs (output, ctx.edit, *track, ctx.engine.getOutputChannelNames().size());
        }

        // Inserts
        String pluginKey;
        for (auto& p : track->plugins)
            pluginKey << String::toHexString ((pointer_sized_int) p.get()) << (p->bypassed.load() ? "b" : "") << ",";
        if (pluginKey != insertKey)
        {
            insertKey = pluginKey;
            inserts.clear();
            for (auto& p : track->plugins)
                addAndMakeVisible (inserts.add (new InsertSlot (ctx, track, p)));
        }

        // Sends: one knob per return track
        if (track->holdsClips())
        {
            String sendKey;
            for (auto& b : ctx.edit.buses)
                sendKey << b->id << ",";
            if (sendKey != sendsKey)
            {
                sendsKey = sendKey;
                sends.clear();
                sendLabels.clear();
                for (size_t i = 0; i < ctx.edit.buses.size(); ++i)
                {
                    auto bus = ctx.edit.buses[i];
                    auto* k = sends.add (new Knob (false));
                    k->setRange (0.0, 1.0);
                    k->setColour (Slider::trackColourId, bus->colour);
                    k->textFromValueFunction = [] (double v) { return theme::formatDb (theme::faderPosToDb ((float) v)) + " dB"; };
                    k->setDoubleClickReturnValue (true, 0.0);
                    k->setTooltip ("Send to " + bus->name);
                    const int busId = bus->id;
                    k->onValueChange = [this, k, busId]
                    {
                        track->getSend (busId, true)->levelDb = theme::faderPosToDb ((float) k->getValue());
                        ctx.edit.markDirty();
                    };
                    addAndMakeVisible (k);

                    auto* l = sendLabels.add (new Label ({}, String::charToString ((juce_wchar) ('A' + (int) i % 26))));
                    l->setFont (theme::font (10.5f));
                    l->setJustificationType (Justification::centred);
                    l->setColour (Label::textColourId, theme::textDim);
                    addAndMakeVisible (l);
                }
                resized();
            }

            for (int i = 0; i < sends.size() && (size_t) i < ctx.edit.buses.size(); ++i)
            {
                auto s = track->getSend (ctx.edit.buses[(size_t) i]->id, false);
                sends[i]->setValue (theme::dbToFaderPos (s != nullptr ? s->levelDb.load() : kSilenceDb), dontSendNotification);
            }
        }

        resized();
        repaint();
    }

    void tick()   { meter.tick(); }

    void paint (Graphics& g) override
    {
        const bool selected = ctx.isSelected (track->id);
        g.fillAll (selected ? theme::headerSel : theme::panel);
        g.setColour (track->colour);
        g.fillRect (0, 0, getWidth(), 4);

        // Group members carry the group's colour and name under the track colour.
        if (auto group = ctx.edit.findGroup (track->groupId))
        {
            g.setColour (group->colour.withAlpha (0.55f));
            g.fillRect (0, 4, getWidth(), 3);
            g.setColour (group->mute.load() ? theme::mute : group->solo.load() ? theme::solo : theme::textDim);
            g.setFont (theme::font (10.0f));
            g.drawText (group->name + (group->mute.load() ? " (muted)" : group->solo.load() ? " (solo)" : ""),
                        getLocalBounds().withTrimmedTop (8).withHeight (12).reduced (5, 0), Justification::centredRight, true);
        }
        g.setColour (theme::border);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        for (auto& [label, area] : sectionLabels)
            g.drawText (label, area, Justification::centredLeft);
    }

    void paintOverChildren (Graphics& g) override
    {
        if (arm.isVisible())
        {
            auto a = arm.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
            g.setColour (track->arm.load() ? theme::textDark : theme::record.withSaturation (0.45f));
            g.fillEllipse (a);
        }
    }

    void resized() override
    {
        sectionLabels.clear();
        auto r = getLocalBounds().reduced (5, 0).withTrimmedTop (6).withTrimmedBottom (6);

        r.removeFromTop (10);   // room for a group tag; kept on every strip so rows line up
        name.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        auto rowFor = [&] (Component& c, const String& label)
        {
            if (label.isNotEmpty())
                sectionLabels.push_back ({ label, r.removeFromTop (13) });
            c.setBounds (r.removeFromTop (20));
            r.removeFromTop (3);
        };

        // Fixed-height sections so strips line up side by side.
        const int ioHeight = 3 * (13 + 23) + 23;
        auto io = r.removeFromTop (ioHeight);
        std::swap (r, io);
        if (track->holdsClips())
            rowFor (input, "In");
        else
            r.removeFromTop (13 + 23);
        if (track->isMidi())
            rowFor (midiOut, "MIDI to");
        else
            r.removeFromTop (13 + 23);
        rowFor (output, "Out");
        if (track->holdsClips())
        {
            auto m = r.removeFromTop (20);
            const int w = m.getWidth() / 3;
            monAuto.setBounds (m.removeFromLeft (w));
            monIn.setBounds (m.removeFromLeft (w));
            monOff.setBounds (m);
        }
        std::swap (r, io);

        r.removeFromTop (6);
        sectionLabels.push_back ({ "Inserts", r.removeFromTop (13) });
        auto insertArea = r.removeFromTop (kInsertRows * 19 + 20);
        for (int i = 0; i < inserts.size(); ++i)
        {
            inserts[i]->setVisible (i < kInsertRows);
            if (i < kInsertRows)
                inserts[i]->setBounds (insertArea.removeFromTop (18).withTrimmedBottom (0));
            if (i < kInsertRows)
                insertArea.removeFromTop (1);
        }
        addInsert.setBounds (insertArea.removeFromTop (18));

        const int numBuses = track->holdsClips() ? (int) ctx.edit.buses.size() : 0;
        const int sendRows = (numBuses + 2) / 3;
        if (numBuses > 0)
        {
            r.removeFromTop (4);
            sectionLabels.push_back ({ "Sends", r.removeFromTop (13) });
            auto sendArea = r.removeFromTop (sendRows * 42);
            for (int i = 0; i < sends.size(); ++i)
            {
                const int row = i / 3, col = i % 3;
                const int w = sendArea.getWidth() / 3;
                auto cell = Rectangle<int> (sendArea.getX() + col * w, sendArea.getY() + row * 42, w, 42);
                sendLabels[i]->setBounds (cell.removeFromBottom (12));
                sends[i]->setBounds (cell.withSizeKeepingCentre (30, 30));
            }
        }

        r.removeFromTop (4);
        pan.setBounds (r.removeFromTop (34).withSizeKeepingCentre (34, 34));

        auto buttons = r.removeFromBottom (22);
        if (track->holdsClips())
        {
            const int w = buttons.getWidth() / 3;
            arm.setBounds (buttons.removeFromLeft (w).reduced (1, 0));
            solo.setBounds (buttons.removeFromLeft (w).reduced (1, 0));
            mute.setBounds (buttons.reduced (1, 0));
        }
        else if (! track->isMaster())
        {
            solo.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
            mute.setBounds (buttons.reduced (1, 0));
        }

        r.removeFromBottom (3);
        dbLabel.setBounds (r.removeFromBottom (16));
        auto faderArea = r.reduced (0, 4);
        meter.setBounds (faderArea.removeFromRight (12).withTrimmedRight (4));
        fader.setBounds (faderArea);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (! ctx.isSelected (track->id))
                ctx.selectTrack (track->id);
            showTrackMenu (ctx, *this, track);
        }
        else if (track->holdsClips() && e.mods.isShiftDown())
            ctx.extendSelection (track->id, true);
        else if (track->holdsClips() && e.mods.isCommandDown())
            ctx.extendSelection (track->id, false);
        else
            ctx.selectTrack (track->id);
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y < 26 && ! track->isMaster())
            renameTrack (ctx, track);
    }

    std::shared_ptr<Track> track;

private:
    AppContext& ctx;
    Label name;
    ComboBox input, midiOut, output;
    TextButton monAuto { "Auto" }, monIn { "In" }, monOff { "Off" };
    OwnedArray<InsertSlot> inserts;
    TextButton addInsert { "+" };
    OwnedArray<Knob> sends;
    OwnedArray<Label> sendLabels;
    LevelMeter meter;
    Knob pan;
    Fader fader;
    Label dbLabel;
    TextButton arm, solo, mute;
    std::vector<std::pair<String, Rectangle<int>>> sectionLabels;
    String routingKey, insertKey, sendsKey;
};

//==============================================================================
MixerView::MixerView (AppContext& c) : ctx (c)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    addReturnButton.setTooltip ("Add a return track for shared effects (reverb, delay)");
    addReturnButton.onClick = [this] { ctx.edit.addTrack (TrackKind::bus); };
    addReturnButton.setWantsKeyboardFocus (false);
    content.addAndMakeVisible (addReturnButton);

    refresh();
}

MixerView::~MixerView() = default;

String MixerView::structureSignature() const
{
    String s;
    // Object identity, not just ids: ids restart at 2 in every project, so a newly
    // loaded project or template can have the same ids as the one on screen.
    auto key = [] (const void* p) { return String::toHexString ((pointer_sized_int) p); };
    for (auto& t : ctx.edit.tracks) s << key (t.get()) << ":" << t->groupId << ",";
    s << "|";
    for (auto& t : ctx.edit.buses) s << key (t.get()) << ",";
    s << "|" << key (ctx.edit.master.get());
    return s;
}

void MixerView::refresh()
{
    const auto sig = structureSignature();
    if (sig != signature)
    {
        signature = sig;
        strips.clear();
        for (auto& t : ctx.edit.tracks) content.addAndMakeVisible (strips.add (new ChannelStrip (ctx, t)));
        for (auto& t : ctx.edit.buses)  content.addAndMakeVisible (strips.add (new ChannelStrip (ctx, t)));
        masterStrip = std::make_unique<ChannelStrip> (ctx, ctx.edit.master);
        addAndMakeVisible (*masterStrip);
        resized();
    }

    for (auto* s : strips)
        s->update();
    masterStrip->update();
    repaint();
}

void MixerView::tick()
{
    for (auto* s : strips)
        s->tick();
    if (masterStrip != nullptr)
        masterStrip->tick();
}

void MixerView::paint (Graphics& g)
{
    g.fillAll (theme::window);
}

void MixerView::resized()
{
    auto r = getLocalBounds();
    if (masterStrip != nullptr)
    {
        auto m = r.removeFromRight (theme::stripWidth + 12);
        masterStrip->setBounds (m.withTrimmedLeft (12).withTrimmedBottom (10));
    }

    viewport.setBounds (r);
    const int h = r.getHeight() - 10;
    int x = 0;
    const int numTracks = (int) ctx.edit.tracks.size();
    for (int i = 0; i < strips.size(); ++i)
    {
        if (i == numTracks)
            x += 8;   // gap between tracks and returns
        strips[i]->setBounds (x, 0, theme::stripWidth, h);
        x += theme::stripWidth;
    }

    addReturnButton.setBounds (x + 10, 10, 80, 24);
    content.setSize (jmax (r.getWidth(), x + 100), h);
}
} // namespace lathe
