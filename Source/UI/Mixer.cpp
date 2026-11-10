#include "Mixer.h"
#include "EqEditor.h"

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
        : track (std::move (t)), ctx (c), meter (track), pan (true), eqThumb (c, track)
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
            midiOut.setTooltip ("Send this track's MIDI to a hardware port (e.g. your interface's MIDI OUT)");
            addAndMakeVisible (midiOut);
        }

        output.onChange = [this]
        {
            if (cue != nullptr && track->isMaster())
            {
                cue->channel = jmax (0, output.getSelectedId() - 1);
                ctx.edit.touched();
                return;
            }
            applyOutput (output, *track, ctx.edit);
            ctx.edit.changed();
        };
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
        pan.onValueChange = [this]
        {
            if (cueLevel != nullptr) cueLevel->pan = (float) pan.getValue();
            else                     track->pan = (float) pan.getValue();
            ctx.edit.markDirty();
        };
        addAndMakeVisible (pan);

        fader.onValueChange = [this]
        {
            const float db = theme::faderPosToDb ((float) fader.getValue());
            levelTarget() = db;
            dbLabel.setText (theme::formatDb (db), dontSendNotification);
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
        mute.onClick = [this]
        {
            auto& m = muteTarget();
            m = ! m.load();
            ctx.edit.touched();
        };
        addChildComponent (mute);
        if (! track->isMaster())
        {
            addAndMakeVisible (solo);
            mute.setVisible (true);
        }

        eqThumb.setCompact (true);
        addAndMakeVisible (eqThumb);

        update();
    }

    void update()
    {
        // Which mix the controls edit: the main mix, or a cue (monitor) mix.
        cue = ctx.edit.findCue (ctx.mixLayer);
        cueLevel = (cue != nullptr && ! track->isMaster()) ? ctx.edit.cueLevel (*cue, *track) : nullptr;
        const bool viaReturn = cue != nullptr && track->holdsClips() && track->output.kind == OutputRoute::Kind::bus;
        const bool cueMaster = cue != nullptr && track->isMaster();

        name.setText (cueMaster ? cue->name : track->name, dontSendNotification);
        fader.setValue (theme::dbToFaderPos (levelTarget().load()), dontSendNotification);
        dbLabel.setText (theme::formatDb (levelTarget().load()), dontSendNotification);
        pan.setValue (cueLevel != nullptr ? cueLevel->pan.load() : track->pan.load(), dontSendNotification);
        solo.setToggleState (track->solo.load(), dontSendNotification);
        mute.setToggleState (muteTarget().load(), dontSendNotification);

        fader.setEnabled (! viaReturn);
        pan.setEnabled (! viaReturn);
        solo.setEnabled (cue == nullptr);
        pan.setVisible (! cueMaster);
        // The master's inserts and EQ only shape the main mix, so they step aside in a cue mix.
        addInsert.setVisible (! cueMaster);
        eqThumb.setVisible (! cueMaster);
        if (track->isMaster())
            mute.setVisible (cueMaster);
        if (cue != nullptr) fader.setColour (Slider::thumbColourId, cue->colour.withMultipliedBrightness (0.8f));
        else                fader.removeColour (Slider::thumbColourId);
        dbLabel.setColour (Label::textColourId, cue != nullptr ? cue->colour : theme::textDim);
        fader.setTooltip (viaReturn ? "Heard in " + cue->name + " through the return it's routed to"
                                    : cue != nullptr ? (cueMaster ? cue->name + " volume" : "Level in " + cue->name) : String());
        if (cueMaster) meter.setSource (cue, &cue->meterL, &cue->meterR);
        else           meter.setSource (track, &track->meterL, &track->meterR);
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
        key << (cueMaster ? "cue" + String (cue->channel.load()) : String()) << "|";
        if (track->isAudio())
            key << audioInputKey (ctx.edit, *track, ctx.engine.getInputChannelNames().size());
        else if (track->isMidi())
            key << midiInputKey (ctx.edit, *track, ctx.engine.getMidiInputNames());
        key << track->input.channel << (track->input.stereo ? 1 : 0) << track->midiInput << track->midiOutput
            << (int) track->output.kind << track->output.busId << track->output.channel << (track->output.stereo ? 1 : 0);

        if (key != routingKey)
        {
            routingKey = key;
            if (track->isMidi())
            {
                fillMidiInputs (input, ctx.edit, *track, ctx.engine.getMidiInputNames());
                fillMidiOutputs (midiOut, *track, ctx.engine.getMidiOutputNames());
            }
            else if (track->isAudio())
            {
                fillAudioInputs (input, ctx.edit, *track, ctx.engine.getInputChannelNames().size());
            }
            if (cueMaster)
            {
                output.clear (dontSendNotification);
                const int numOuts = ctx.engine.getOutputChannelNames().size();
                for (int ch = 0; ch + 1 < jmax (2, numOuts); ch += 2)
                    output.addItem ("Out " + String (ch + 1) + "/" + String (ch + 2), ch + 1);
                output.setSelectedId (cue->channel.load() + 1, dontSendNotification);
                if (output.getSelectedId() == 0)
                    output.setText ("Out " + String (cue->channel.load() + 1) + "/" + String (cue->channel.load() + 2) + " (not available)", dontSendNotification);
            }
            else
            {
                fillOutputs (output, ctx.edit, *track, ctx.engine.getOutputChannelNames().size());
            }
            output.setTooltip (cueMaster ? "The outputs " + cue->name + " plays on (your headphone or in-ear feed)"
                                         : "Where this channel's audio goes");
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
                        // A track's first turn of a send knob creates the send. The audio engine
                        // only routes the sends it knew about when it last rebuilt, so a new one
                        // needs a rebuild or the return hears nothing. Later turns only change
                        // the level, which the engine reads live.
                        const bool isNew = track->getSend (busId, false) == nullptr;
                        track->getSend (busId, true)->levelDb = theme::faderPosToDb ((float) k->getValue());
                        if (isNew)
                            ctx.edit.changed();
                        else
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

    void refreshEq()   { eqThumb.repaint(); }

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
        if (cue != nullptr)
        {
            g.setColour (cue->colour);
            g.fillRect (0, getHeight() - 3, getWidth(), 3);
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

        const bool cueMaster = cue != nullptr && track->isMaster();
        r.removeFromTop (6);
        if (cueMaster) r.removeFromTop (13);
        else           sectionLabels.push_back ({ "Inserts", r.removeFromTop (13) });
        auto insertArea = r.removeFromTop (kInsertRows * 19 + 20);
        for (int i = 0; i < inserts.size(); ++i)
        {
            inserts[i]->setVisible (i < kInsertRows && ! cueMaster);
            if (i < kInsertRows)
                inserts[i]->setBounds (insertArea.removeFromTop (18).withTrimmedBottom (0));
            if (i < kInsertRows)
                insertArea.removeFromTop (1);
        }
        addInsert.setBounds (insertArea.removeFromTop (18));

        r.removeFromTop (4);
        if (cueMaster) r.removeFromTop (13);
        else           sectionLabels.push_back ({ "EQ", r.removeFromTop (13) });
        eqThumb.setBounds (r.removeFromTop (26));

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
        else
        {
            mute.setBounds (buttons.reduced (1, 0));   // a cue mix's own mute
        }

        r.removeFromBottom (3);
        dbLabel.setBounds (r.removeFromBottom (16));
        auto faderArea = r.reduced (0, 4);
        meter.setBounds (faderArea.removeFromRight (12).withTrimmedRight (4));
        fader.setBounds (faderArea);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (eqThumb.getBounds().contains (e.getPosition()) && ! e.mods.isPopupMenu())
        {
            ctx.selectTrack (track->id);
            if (ctx.showDevices)
                ctx.showDevices();
            return;
        }

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
    /** The level and mute the fader and M button control in the current mix. */
    std::atomic<float>& levelTarget()
    {
        if (cueLevel != nullptr) return cueLevel->levelDb;
        if (cue != nullptr && track->isMaster()) return cue->volumeDb;
        return track->volumeDb;
    }
    std::atomic<bool>& muteTarget()
    {
        if (cueLevel != nullptr) return cueLevel->mute;
        if (cue != nullptr && track->isMaster()) return cue->mute;
        return track->mute;
    }

    AppContext& ctx;
    std::shared_ptr<CueMix> cue;
    std::shared_ptr<CueLevel> cueLevel;
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
    EqEditor eqThumb;
    std::vector<std::pair<String, Rectangle<int>>> sectionLabels;
    String routingKey, insertKey, sendsKey;
};


//==============================================================================
/** Picks which mix the mixer edits: Main, or one of the cue (monitor) mixes.
    In a cue mix the faders, pans and mutes are that mix's own; the master strip
    becomes its volume and output pair. */
class MixBar : public Component
{
public:
    explicit MixBar (AppContext& c) : ctx (c)
    {
        addButton.setTooltip ("Add a monitor mix on its own outputs, e.g. for in-ear monitors or a headphone feed");
        addButton.setWantsKeyboardFocus (false);
        addButton.onClick = [this] { addCue(); };
        addAndMakeVisible (addButton);

        clickLabel.setFont (theme::font (11.5f));
        clickLabel.setColour (Label::textColourId, theme::textDim);
        clickLabel.setJustificationType (Justification::centredRight);
        addAndMakeVisible (clickLabel);

        click.setSliderStyle (Slider::LinearBar);
        click.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        click.setRange (0.0, 1.0);
        click.setDoubleClickReturnValue (true, theme::dbToFaderPos (0.0f));
        click.setPopupDisplayEnabled (true, true, nullptr);
        click.textFromValueFunction = [] (double v) { return "Click " + theme::formatDb (theme::faderPosToDb ((float) v)) + " dB"; };
        click.setTooltip ("Metronome level in this mix (when the metronome is on)");
        click.onValueChange = [this]
        {
            if (auto cue = ctx.edit.findCue (ctx.mixLayer))
            {
                cue->clickDb = theme::faderPosToDb ((float) click.getValue());
                ctx.edit.markDirty();
            }
        };
        addAndMakeVisible (click);

        styleToggle (clickInMain, "Click in main", theme::accent, "Play the metronome on the main outputs too. Turn off to hear the click only in your monitor mixes.");
        clickInMain.onClick = [this] { ctx.edit.clickInMain = ! ctx.edit.clickInMain.load(); ctx.edit.touched(); };
        addAndMakeVisible (clickInMain);
    }

    std::function<void()> onLayerChanged;

    void update()
    {
        if (ctx.mixLayer != -1 && ctx.edit.findCue (ctx.mixLayer) == nullptr)
            ctx.mixLayer = -1;

        String sig;
        for (auto& c : ctx.edit.cues)
            sig << String::toHexString ((pointer_sized_int) c.get()) << c->name << ",";
        if (sig != tabSignature)
        {
            tabSignature = sig;
            tabs.clear();
            tabs.add (new Tab (*this, -1, "Main", theme::accent));
            for (auto& c : ctx.edit.cues)
                tabs.add (new Tab (*this, c->id, c->name, c->colour));
            for (auto* t : tabs)
                addAndMakeVisible (t);
            resized();
        }

        auto cue = ctx.edit.findCue (ctx.mixLayer);
        click.setVisible (cue != nullptr);
        clickLabel.setVisible (cue != nullptr);
        clickInMain.setVisible (! ctx.edit.cues.empty());
        clickInMain.setToggleState (ctx.edit.clickInMain.load(), dontSendNotification);
        if (cue != nullptr)
        {
            click.setValue (theme::dbToFaderPos (cue->clickDb.load()), dontSendNotification);
            click.setColour (Slider::trackColourId, cue->colour);
        }
        clickLabel.setText (cue != nullptr ? "Click" : String(), dontSendNotification);
        repaint();
        for (auto* t : tabs)
            t->repaint();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText ("Mix", getLocalBounds().removeFromLeft (40).withTrimmedLeft (10), Justification::centredLeft);

        if (auto cue = ctx.edit.findCue (ctx.mixLayer))
        {
            auto hint = getLocalBounds().withLeft (addButton.getRight() + 14).withRight (clickLabel.getX() - 10);
            g.setColour (cue->colour);
            g.drawText ("Faders, pans and mutes now set " + cue->name + " (Out " + String (cue->channel.load() + 1) + "/"
                            + String (cue->channel.load() + 2) + "). The main mix is untouched.",
                        hint, Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 4);
        r.removeFromLeft (36);
        for (auto* t : tabs)
        {
            const int w = jmax (56, GlyphArrangement::getStringWidthInt (theme::font (12.0f), t->label) + 24);
            t->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (2);
        }
        r.removeFromLeft (4);
        addButton.setBounds (r.removeFromLeft (92));

        clickInMain.setBounds (r.removeFromRight (96));
        r.removeFromRight (12);
        click.setBounds (r.removeFromRight (110).reduced (0, 3));
        clickLabel.setBounds (r.removeFromRight (40));
    }

private:
    class Tab : public Component, public SettableTooltipClient
    {
    public:
        Tab (MixBar& b, int i, String l, Colour c) : bar (b), id (i), label (std::move (l)), colour (c)
        {
            setTooltip (id < 0 ? "The main mix" : "Monitor mix. Right-click to rename, copy the main mix or delete.");
        }

        void paint (Graphics& g) override
        {
            const bool on = bar.ctx.mixLayer == id;
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (on ? colour : theme::control);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (on ? theme::textDark : theme::text);
            g.setFont (theme::font (12.0f, on));
            g.drawText (label, getLocalBounds().reduced (6, 0), Justification::centred, true);
            if (! on && id >= 0)
            {
                g.setColour (colour);
                g.fillRect (r.removeFromBottom (2.0f).reduced (4.0f, 0.0f));
            }
        }

        void mouseUp (const MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && id >= 0)
                bar.showCueMenu (id, *this);
            else if (e.mouseWasClicked())
                bar.select (id);
        }

        void mouseDoubleClick (const MouseEvent&) override
        {
            if (id >= 0)
                bar.renameCue (id);
        }

        MixBar& bar;
        int id;
        String label;
        Colour colour;
    };

    void select (int id)
    {
        ctx.mixLayer = id;
        update();
        if (onLayerChanged)
            onLayerChanged();
    }

    void addCue()
    {
        // First stereo pair nobody else is using.
        std::set<int> used { ctx.edit.master->output.channel };
        for (auto& c : ctx.edit.cues)
            used.insert (c->channel.load());
        const int numOuts = jmax (4, ctx.engine.getOutputChannelNames().size());
        int channel = 2;
        for (int ch = 0; ch + 1 < numOuts; ch += 2)
            if (used.count (ch) == 0) { channel = ch; break; }

        const String name = ctx.edit.cues.empty() ? "In-ears" : "Cue " + String ((int) ctx.edit.cues.size() + 1);
        askForText ("New monitor mix", "Name (it starts as a copy of the main mix):", name, [this, channel] (const String& n)
        {
            auto cue = ctx.edit.addCue (n.trim(), channel);
            select (cue->id);
        });
    }

    void renameCue (int id)
    {
        if (auto cue = ctx.edit.findCue (id))
            askForText ("Rename monitor mix", "Name:", cue->name, [this, id] (const String& n)
            {
                if (auto c = ctx.edit.findCue (id); c != nullptr && n.trim().isNotEmpty())
                {
                    c->name = n.trim();
                    ctx.edit.touched();
                }
            });
    }

    void showCueMenu (int id, Component& anchor)
    {
        auto cue = ctx.edit.findCue (id);
        if (cue == nullptr)
            return;

        PopupMenu m;
        m.addItem ("Rename...", [this, id] { renameCue (id); });
        m.addItem ("Copy levels from the main mix", [this, id]
        {
            if (auto c = ctx.edit.findCue (id))
            {
                ctx.edit.copyMainMixToCue (*c);
                ctx.edit.touched();
            }
        });
        m.addSeparator();
        m.addItem ("Delete " + cue->name, [this, id]
        {
            if (ctx.mixLayer == id)
                ctx.mixLayer = -1;
            ctx.edit.removeCue (id);
            if (onLayerChanged)
                onLayerChanged();
        });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&anchor));
    }

    AppContext& ctx;
    OwnedArray<Tab> tabs;
    TextButton addButton { "+ Monitor mix" };
    Label clickLabel;
    Slider click;
    TextButton clickInMain;
    String tabSignature;
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

    mixBar = std::make_unique<MixBar> (ctx);
    mixBar->onLayerChanged = [this] { refresh(); };
    addAndMakeVisible (*mixBar);

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

    mixBar->update();
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
    if (mixBar != nullptr)
        mixBar->setBounds (r.removeFromTop (30));
    r.removeFromTop (4);
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
