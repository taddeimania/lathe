#include "Widgets.h"
#include "Dialogs.h"

namespace lathe
{
void styleToggle (TextButton& b, const String& text, Colour onColour, const String& tooltip)
{
    b.setButtonText (text);
    b.setClickingTogglesState (false);
    b.setColour (TextButton::buttonOnColourId, onColour);
    b.setColour (TextButton::textColourOnId, theme::textDark);
    b.setTooltip (tooltip);
    b.setWantsKeyboardFocus (false);
}

//==============================================================================
Fader::Fader() : Slider (Slider::LinearVertical, Slider::NoTextBox)
{
    setRange (0.0, 1.0);
    setDoubleClickReturnValue (true, theme::dbToFaderPos (0.0f));
    setPopupDisplayEnabled (true, true, nullptr);
    setVelocityModeParameters (0.6, 1, 0.0, true, ModifierKeys::shiftModifier);
    setWantsKeyboardFocus (false);
}

Knob::Knob (bool bipolar) : Slider (Slider::RotaryHorizontalVerticalDrag, Slider::NoTextBox)
{
    setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    setPopupDisplayEnabled (true, true, nullptr);
    setWantsKeyboardFocus (false);
    if (bipolar)
        setRange (-1.0, 1.0);
}

//==============================================================================
LevelMeter::LevelMeter (std::shared_ptr<Track> t)
{
    setInterceptsMouseClicks (true, false);
    if (t != nullptr)
        setSource (t, &t->meterL, &t->meterR);
}

void LevelMeter::setSource (std::shared_ptr<void> o, std::atomic<float>* l, std::atomic<float>* r)
{
    if (o == owner && l == sourceL)
        return;
    owner = std::move (o);
    sourceL = l;
    sourceR = r;
    left = right = 0.0f;
    clipped = false;
    repaint();
}

void LevelMeter::tick()
{
    if (sourceL == nullptr || sourceR == nullptr)
        return;
    const float l = sourceL->exchange (0.0f);
    const float r = sourceR->exchange (0.0f);
    if (l >= 1.0f || r >= 1.0f)
        clipped = true;

    const float newLeft = jmax (l, left * 0.82f);
    const float newRight = jmax (r, right * 0.82f);
    if (std::abs (newLeft - left) > 0.0005f || std::abs (newRight - right) > 0.0005f || newLeft > 0.0f)
    {
        left = newLeft < 0.0001f ? 0.0f : newLeft;
        right = newRight < 0.0001f ? 0.0f : newRight;
        repaint();
    }
}

void LevelMeter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto clipArea = r.removeFromTop (4.0f);
    g.setColour (clipped ? theme::meterHigh : theme::border);
    g.fillRect (clipArea.reduced (0.0f, 0.5f));

    g.setColour (theme::border);
    g.fillRect (r);

    auto drawBar = [&] (Rectangle<float> area, float gain)
    {
        const float pos = theme::dbToFaderPos (Decibels::gainToDecibels (gain, kSilenceDb));
        if (pos <= 0.0f)
            return;

        auto bar = area.withTop (area.getBottom() - area.getHeight() * pos);
        const float yellowLine = area.getBottom() - area.getHeight() * theme::dbToFaderPos (-12.0f);
        const float redLine = area.getBottom() - area.getHeight() * theme::dbToFaderPos (-3.0f);

        g.setColour (theme::meterLow);
        g.fillRect (bar.withTop (jmax (bar.getY(), yellowLine)));
        if (bar.getY() < yellowLine)
        {
            g.setColour (theme::meterMid);
            g.fillRect (bar.withTop (jmax (bar.getY(), redLine)).withBottom (yellowLine));
        }
        if (bar.getY() < redLine)
        {
            g.setColour (theme::meterHigh);
            g.fillRect (bar.withBottom (redLine));
        }
    };

    auto inner = r.reduced (1.0f);
    const float w = (inner.getWidth() - 1.0f) * 0.5f;
    drawBar (inner.removeFromLeft (w), left);
    inner.removeFromLeft (1.0f);
    drawBar (inner, right);
}

//==============================================================================
namespace
{
constexpr int kFromTrackBase = 1000000;   // + track id

const Track* findAnyTrack (const Edit& edit, int id)
{
    for (auto& t : edit.tracks)
        if (t->id == id)
            return t.get();
    return nullptr;
}

bool canFeed (const Track& source, const Track& to)
{
    // Audio tracks take any track's sound; MIDI tracks take another MIDI track's notes.
    return source.id != to.id && source.holdsClips() && (to.isAudio() || source.isMidi());
}
} // namespace

String describeInput (const Track& t, const Edit* edit)
{
    if (t.isMidi() && t.input.isTrack())
    {
        const auto* src = edit != nullptr ? findAnyTrack (*edit, t.input.track) : nullptr;
        return src != nullptr ? "From " + src->name : String ("From a track");
    }
    if (t.isMidi())
        return t.midiInput == "All" ? String ("All MIDI") : t.midiInput == "None" ? String ("No MIDI") : t.midiInput;

    if (t.input.isTrack())
    {
        const auto* src = edit != nullptr ? findAnyTrack (*edit, t.input.track) : nullptr;
        return src != nullptr ? "From " + src->name : String ("From a track");
    }
    if (t.input.channel < 0)
        return "No input";
    return t.input.stereo ? "In " + String (t.input.channel + 1) + "/" + String (t.input.channel + 2)
                          : "In " + String (t.input.channel + 1);
}

String audioInputKey (const Edit& edit, const Track& t, int numInputs)
{
    String key;
    key << numInputs << ":" << t.input.channel << (t.input.stereo ? "s" : "m") << t.input.track << "|";
    for (auto& o : edit.tracks)
        if (canFeed (*o, t))
            key << o->id << "=" << o->name << ",";
    return key;
}

void fillAudioInputs (ComboBox& box, const Edit& edit, const Track& t, int numInputs)
{
    box.clear (dontSendNotification);
    box.addItem ("No input", 1);

    // Another track's sound (an instrument with its effects, say), taken inside Lathe.
    bool anyTrack = false;
    for (auto& o : edit.tracks)
    {
        if (! canFeed (*o, t))
            continue;
        if (! anyTrack)
            box.addSectionHeading ("From a track");
        anyTrack = true;
        box.addItem ("From " + o->name, kFromTrackBase + o->id);
    }

    if (numInputs > 0)
    {
        box.addSectionHeading ("Mono");
        for (int c = 0; c < numInputs; ++c)
            box.addItem ("In " + String (c + 1), 100 + c);

        if (numInputs > 1)
        {
            box.addSectionHeading ("Stereo");
            for (int c = 0; c + 1 < numInputs; c += 2)
                box.addItem ("In " + String (c + 1) + "/" + String (c + 2), 1000 + c);
        }
    }

    int selected = 1;
    if (t.input.isTrack())
    {
        selected = kFromTrackBase + t.input.track;
        if (box.indexOfItemId (selected) < 0)
            box.addItem ("From a deleted track", selected);
    }
    else if (t.input.channel >= 0)
    {
        selected = (t.input.stereo ? 1000 : 100) + t.input.channel;
        if (box.indexOfItemId (selected) < 0)
            box.addItem (describeInput (t) + " (not available)", selected);
    }
    box.setSelectedId (selected, dontSendNotification);
}

void applyAudioInput (ComboBox& box, Track& t)
{
    const int id = box.getSelectedId();
    if (id >= kFromTrackBase) { t.input.track = id - kFromTrackBase; return; }   // keeps the interface input for later
    t.input.track = -1;
    if (id == 1)          { t.input.channel = -1; t.input.stereo = false; }
    else if (id >= 1000)  { t.input.channel = id - 1000; t.input.stereo = true; }
    else if (id >= 100)   { t.input.channel = id - 100; t.input.stereo = false; }
}

void fillOutputs (ComboBox& box, const Edit& edit, const Track& t, int numOutputs)
{
    box.clear (dontSendNotification);

    if (! t.isMaster())
    {
        box.addItem ("Master", 1);

        if (! t.isBus() && ! edit.buses.empty())
        {
            box.addSectionHeading ("Returns");
            for (size_t i = 0; i < edit.buses.size(); ++i)
                box.addItem (edit.buses[i]->name, 2000 + (int) i);
        }
    }

    if (numOutputs > 0)
    {
        box.addSectionHeading ("Interface outputs");
        for (int c = 0; c + 1 < numOutputs; c += 2)
            box.addItem ("Out " + String (c + 1) + "/" + String (c + 2), 3000 + c);
        for (int c = 0; c < numOutputs; ++c)
            box.addItem ("Out " + String (c + 1) + " (mono)", 4000 + c);
    }

    int selected = 1;
    switch (t.output.kind)
    {
        case OutputRoute::Kind::master:
            selected = 1;
            break;
        case OutputRoute::Kind::bus:
            for (size_t i = 0; i < edit.buses.size(); ++i)
                if (edit.buses[i]->id == t.output.busId)
                    selected = 2000 + (int) i;
            break;
        case OutputRoute::Kind::hardware:
            selected = (t.output.stereo ? 3000 : 4000) + t.output.channel;
            if (box.indexOfItemId (selected) < 0)
                box.addItem ((t.output.stereo ? "Out " + String (t.output.channel + 1) + "/" + String (t.output.channel + 2)
                                              : "Out " + String (t.output.channel + 1)) + " (not available)", selected);
            break;
    }
    box.setSelectedId (selected, dontSendNotification);
}

void applyOutput (ComboBox& box, Track& t, const Edit& edit)
{
    const int id = box.getSelectedId();
    if (id == 1)
    {
        t.output = {};
    }
    else if (id >= 4000)
    {
        t.output = { OutputRoute::Kind::hardware, -1, id - 4000, false };
    }
    else if (id >= 3000)
    {
        t.output = { OutputRoute::Kind::hardware, -1, id - 3000, true };
    }
    else if (id >= 2000)
    {
        const auto index = (size_t) (id - 2000);
        if (index < edit.buses.size())
            t.output = { OutputRoute::Kind::bus, edit.buses[index]->id, 0, true };
    }
}

String midiInputKey (const Edit& edit, const Track& t, const StringArray& names)
{
    String key = names.joinIntoString ("|") + ":" + t.midiInput + ":" + String (t.input.track) + "|";
    for (auto& o : edit.tracks)
        if (canFeed (*o, t))
            key << o->id << "=" << o->name << ",";
    return key;
}

void fillMidiInputs (ComboBox& box, const Edit& edit, const Track& t, const StringArray& names)
{
    box.clear (dontSendNotification);
    box.addItem ("All MIDI inputs", 1);
    box.addItem ("No MIDI input", 2);

    // Another MIDI track's notes: what it plays, or what its plug-ins (an arpeggiator,
    // a generator like Dicey) generate.
    bool anyTrack = false;
    for (auto& o : edit.tracks)
    {
        if (! canFeed (*o, t))
            continue;
        if (! anyTrack)
            box.addSectionHeading ("From a track");
        anyTrack = true;
        box.addItem ("From " + o->name, kFromTrackBase + o->id);
    }

    if (! names.isEmpty())
        box.addSectionHeading ("Devices");
    for (int i = 0; i < names.size(); ++i)
        box.addItem (names[i], 10 + i);

    int selected = t.midiInput == "All" ? 1 : t.midiInput == "None" ? 2 : 10 + names.indexOf (t.midiInput);
    if (t.input.isTrack())
    {
        selected = kFromTrackBase + t.input.track;
        if (box.indexOfItemId (selected) < 0)
            box.addItem ("From a deleted track", selected);
    }
    else if (selected < 1)
    {
        box.addItem (t.midiInput + " (not connected)", 9);
        selected = 9;
    }
    box.setSelectedId (selected, dontSendNotification);
}

void applyMidiInput (ComboBox& box, Track& t, const StringArray& names)
{
    const int id = box.getSelectedId();
    if (id >= kFromTrackBase) { t.input.track = id - kFromTrackBase; return; }   // keeps the device choice for later
    t.input.track = -1;
    if (id == 1)       t.midiInput = "All";
    else if (id == 2)  t.midiInput = "None";
    else if (id >= 10) t.midiInput = names[id - 10];
}

void fillMidiOutputs (ComboBox& box, const Track& t, const StringArray& names)
{
    box.clear (dontSendNotification);
    box.addItem ("No MIDI out", 1);
    if (! names.isEmpty())
        box.addSeparator();
    for (int i = 0; i < names.size(); ++i)
        box.addItem (names[i], 10 + i);

    int selected = t.midiOutput.isEmpty() ? 1 : 10 + names.indexOf (t.midiOutput);
    if (selected < 1)
    {
        box.addItem (t.midiOutput + " (not connected)", 9);
        selected = 9;
    }
    box.setSelectedId (selected, dontSendNotification);
}

void applyMidiOutput (ComboBox& box, Track& t, const StringArray& names)
{
    const int id = box.getSelectedId();
    if (id == 1)        t.midiOutput.clear();
    else if (id >= 10)  t.midiOutput = names[id - 10];
}

//==============================================================================
namespace
{
class PluginWindow : public DocumentWindow
{
public:
    PluginWindow (std::shared_ptr<PluginSlot> s, const String& trackName)
        : DocumentWindow (trackName + "  /  " + s->getName(), theme::panel, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          slot (s)
    {
        setUsingNativeTitleBar (true);

        AudioProcessorEditor* editor = s->instance->hasEditor() ? s->instance->createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new GenericAudioProcessorEditor (*s->instance);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        std::weak_ptr<PluginSlot> weak = slot;
        MessageManager::callAsync ([weak]
        {
            if (auto s = weak.lock())
                s->window.reset();
        });
    }

private:
    std::weak_ptr<PluginSlot> slot;
};
} // namespace

void openPluginEditor (AppContext& ctx, const std::shared_ptr<PluginSlot>& slot, const String& trackName)
{
    if (slot == nullptr)
        return;

    if (slot->instance == nullptr)
    {
        ctx.showStatus (slot->getName() + " isn't installed on this computer. Its settings are kept in the project.");
        return;
    }

    if (slot->window != nullptr)
    {
        slot->window->toFront (true);
        return;
    }

    slot->window = std::make_unique<PluginWindow> (slot, trackName);
}

void removePlugin (AppContext& ctx, Track& t, const std::shared_ptr<PluginSlot>& slot)
{
    slot->window.reset();
    t.plugins.erase (std::remove (t.plugins.begin(), t.plugins.end(), slot), t.plugins.end());
    ctx.edit.changed();
}

void movePlugin (AppContext& ctx, Track& t, const std::shared_ptr<PluginSlot>& slot, int delta)
{
    auto it = std::find (t.plugins.begin(), t.plugins.end(), slot);
    if (it == t.plugins.end())
        return;

    const auto index = (int) std::distance (t.plugins.begin(), it);
    const auto target = jlimit (0, (int) t.plugins.size() - 1, index + delta);
    if (target == index)
        return;

    std::swap (t.plugins[(size_t) index], t.plugins[(size_t) target]);
    ctx.edit.changed();
}

void showAddPluginMenu (AppContext& ctx, Component& anchor, std::shared_ptr<Track> track, int insertIndex)
{
    bool hasInstrument = false;
    for (auto& p : track->plugins)
        hasInstrument = hasInstrument || p->isInstrument();

    auto results = std::make_shared<std::vector<PluginDescription>>();
    auto menu = ctx.host.buildMenu (track->isMidi() && ! hasInstrument, *results);
    menu.addSeparator();
    menu.addItem (100001, "Browse your plug-ins on GitHub...");
    menu.addItem (100000, ctx.host.isScanning() ? "Scanning..." : "Scan for new plug-ins", ! ctx.host.isScanning());

    Component::SafePointer<Component> safeAnchor (&anchor);
    auto* context = &ctx;

    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&anchor),
                        [safeAnchor, context, track, insertIndex, results] (int result)
    {
        if (safeAnchor == nullptr || result == 0)
            return;

        if (result == 100000)
        {
            context->host.startScan (false);
            return;
        }

        if (result == 100001)
        {
            context->selectTrack (track->id);
            if (context->openPluginBrowser)
                context->openPluginBrowser();
            return;
        }

        if (result < 1 || (size_t) result > results->size())
            return;

        insertPlugin (*context, track, (*results)[(size_t) result - 1], insertIndex);
    });
}

std::shared_ptr<PluginSlot> insertPlugin (AppContext& ctx, std::shared_ptr<Track> track, const PluginDescription& desc, int insertIndex)
{
    if (track == nullptr)
        return {};

    ctx.showStatus ("Loading " + desc.name + "...");
    String error;
    auto slot = ctx.engine.makeSlot (desc, {}, error);
    if (slot->instance == nullptr)
    {
        ctx.showStatus ("Couldn't load " + desc.name + ": " + error);
        return {};
    }

    const auto index = (size_t) jlimit (0, (int) track->plugins.size(), insertIndex < 0 ? (int) track->plugins.size() : insertIndex);
    track->plugins.insert (track->plugins.begin() + (long) index, slot);
    ctx.edit.changed();
    ctx.showStatus (desc.name + " added to " + track->name);
    openPluginEditor (ctx, slot, track->name);
    return slot;
}

static bool hasInstrument (const Track& t)
{
    for (auto& p : t.plugins)
        if (p->isInstrument())
            return true;
    return false;
}

String describeAddTarget (AppContext& ctx, bool isInstrument, bool isMidiTool)
{
    auto t = ctx.edit.findTrack (ctx.selection.trackId);
    if (isInstrument || isMidiTool)
    {
        if (t != nullptr && t->isMidi() && (isMidiTool || ! hasInstrument (*t)))
            return "Add to " + t->name;
        return "Add on a new MIDI track";
    }
    return t != nullptr ? "Add to " + t->name : String ("Select a track first");
}

std::shared_ptr<Track> addPluginToSelection (AppContext& ctx, const PluginDescription& desc, bool isMidiTool)
{
    auto t = ctx.edit.findTrack (ctx.selection.trackId);
    const bool needsMidi = desc.isInstrument || isMidiTool;

    if (needsMidi)
    {
        if (t == nullptr || ! t->isMidi() || (! isMidiTool && hasInstrument (*t)))
        {
            t = ctx.edit.addTrack (TrackKind::midi, desc.name);
            ctx.selectTrack (t->id);
        }
        // MIDI tools (sequencers, chord generators) go before the instrument they drive.
        insertPlugin (ctx, t, desc, isMidiTool ? 0 : -1);
        return t;
    }

    if (t == nullptr)
    {
        ctx.showStatus ("Select a track (or the master) to add " + desc.name + " to.");
        return {};
    }

    insertPlugin (ctx, t, desc, -1);
    return t;
}

void showPluginSlotMenu (AppContext& ctx, Component& anchor, std::shared_ptr<Track> track, std::shared_ptr<PluginSlot> slot)
{
    PopupMenu m;
    m.addItem (1, "Open", slot->instance != nullptr);
    m.addItem (2, "Bypass", slot->instance != nullptr, slot->bypassed.load());
    m.addSeparator();
    m.addItem (3, "Move earlier");
    m.addItem (4, "Move later");
    m.addSeparator();
    m.addItem (5, "Remove");

    Component::SafePointer<Component> safeAnchor (&anchor);
    auto* context = &ctx;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&anchor), [=] (int r)
    {
        if (safeAnchor == nullptr)
            return;

        switch (r)
        {
            case 1: openPluginEditor (*context, slot, track->name); break;
            case 2: slot->bypassed = ! slot->bypassed.load(); context->edit.changed(); break;
            case 3: movePlugin (*context, *track, slot, -1); break;
            case 4: movePlugin (*context, *track, slot, 1); break;
            case 5: removePlugin (*context, *track, slot); break;
            default: break;
        }
    });
}

//==============================================================================
void askForText (const String& title, const String& prompt, const String& initial, std::function<void (const String&)> onOk)
{
    auto* w = new AlertWindow (title, prompt, MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", initial);
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    w->enterModalState (true, ModalCallbackFunction::create ([w, onOk] (int result)
    {
        const auto text = w->getTextEditorContents ("text").trim();
        if (result == 1 && text.isNotEmpty())
            onOk (text);
    }), true);

    // Ready to type straight away, with the old text selected so typing replaces it.
    auto focusEditor = [safe = Component::SafePointer<AlertWindow> (w)]
    {
        if (safe != nullptr)
            if (auto* editor = safe->getTextEditor ("text"))
            {
                editor->grabKeyboardFocus();
                editor->selectAll();
            }
    };
    focusEditor();
    MessageManager::callAsync (focusEditor);   // again once the window is actually on screen
}

void toggleSolo (AppContext& ctx, Track* track, TrackGroup* group)
{
    auto& edit = ctx.edit;
    const bool newState = track != nullptr ? ! track->solo.load() : (group != nullptr && ! group->solo.load());

    if (! ModifierKeys::currentModifiers.isCommandDown())
    {
        // Exclusive: clear everything at the same level (tracks + groups, or returns).
        if (track != nullptr && track->isBus())
        {
            for (auto& b : edit.buses) b->solo = false;
        }
        else
        {
            for (auto& t : edit.tracks) t->solo = false;
            for (auto& g : edit.groups) g->solo = false;
        }
    }

    if (track != nullptr) track->solo = newState;
    if (group != nullptr) group->solo = newState;
    edit.touched();
}

void groupSelectedTracks (AppContext& ctx, int fallbackTrackId)
{
    std::vector<int> ids (ctx.selection.trackIds.begin(), ctx.selection.trackIds.end());
    if (ids.size() < 2)
        ids = { fallbackTrackId >= 0 ? fallbackTrackId : ctx.selection.trackId };

    if (auto g = ctx.edit.createGroup (ids))
        ctx.showStatus ("Grouped " + String ((int) ctx.edit.groupMembers (g->id).size()) + " tracks as \"" + g->name
                        + "\". The group's arm, solo and mute act on all of them.");
    else
        ctx.showStatus ("Select audio or MIDI tracks to group (Cmd/Ctrl- or Shift-click the track names).");
}

void renameGroup (AppContext& ctx, std::shared_ptr<TrackGroup> group)
{
    auto* context = &ctx;
    askForText ("Rename group", {}, group->name, [context, group] (const String& name)
    {
        context->edit.renameGroup (group->id, name);
    });
}

void showGroupBus (AppContext& ctx, std::shared_ptr<TrackGroup> group)
{
    auto bus = ctx.edit.makeGroupBus (group->id);
    if (bus == nullptr)
        return;
    ctx.selectTrack (bus->id);
    if (ctx.showDevices)
        ctx.showDevices();
    ctx.showStatus ("\"" + bus->name + "\": every track in " + group->name
                    + " plays through it. Add plug-ins here to process the whole group.");
}

void showGroupMenu (AppContext& ctx, Component& anchor, std::shared_ptr<TrackGroup> group)
{
    static const char* colourNames[] = { "Clay", "Ochre", "Moss", "Jade", "Steel", "Iris", "Rose", "Stone" };
    PopupMenu colours;
    auto& palette = getTrackPalette();
    for (int i = 0; i < palette.size(); ++i)
        colours.addColouredItem (100 + i, colourNames[i % 8], palette[i], true, group->colour == palette[i]);

    PopupMenu m;
    m.addItem (1, "Rename group...");
    m.addSubMenu ("Colour", colours);
    m.addSeparator();
    m.addItem (2, ctx.edit.isGroupArmed (group->id) ? "Disarm all" : "Arm all");
    m.addItem (3, group->collapsed ? "Expand" : "Collapse");
    m.addSeparator();
    const auto bus = ctx.edit.findGroupBus (group->id);
    if (bus == nullptr)
    {
        m.addItem (5, "Add Plug-ins to the Whole Group (make a group bus)");
    }
    else
    {
        m.addItem (6, "Show \"" + bus->name + "\" (plug-ins, EQ, fader)");
        m.addItem (7, "Remove Group Bus (tracks go straight to the master)");
    }
    m.addSeparator();
    m.addItem (8, "Export This Group...");
    m.addItem (4, "Ungroup (keep the tracks)");

    Component::SafePointer<Component> safeAnchor (&anchor);
    auto* context = &ctx;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&anchor), [=] (int r)
    {
        if (safeAnchor == nullptr || r == 0)
            return;
        auto& edit = context->edit;
        if (r == 1)        renameGroup (*context, group);
        else if (r == 2)   edit.toggleGroupArm (group->id);
        else if (r == 3)   { group->collapsed = ! group->collapsed; edit.changed(); }
        else if (r == 4)   edit.ungroup (group->id);
        else if (r == 5 || r == 6)  showGroupBus (*context, group);
        else if (r == 7)   edit.removeGroupBus (group->id);
        else if (r == 8)
        {
            std::set<int> ids;
            for (auto& t : edit.tracks)
                if (t->groupId == group->id)
                    ids.insert (t->id);
            if (group->busId >= 0)
                ids.insert (group->busId);
            showExportDialog (*context, ids);
        }
        else if (r >= 100) { group->colour = getTrackPalette()[r - 100]; edit.touched(); }
    });
}

void renameTrack (AppContext& ctx, std::shared_ptr<Track> track)
{
    auto* context = &ctx;
    askForText ("Rename track", {}, track->name, [context, track] (const String& name)
    {
        track->name = name;
        context->edit.changed();
    });
}

void showTrackMenu (AppContext& ctx, Component& anchor, std::shared_ptr<Track> track)
{
    if (track->isMaster())
        return;

    static const char* colourNames[] = { "Clay", "Ochre", "Moss", "Jade", "Steel", "Iris", "Rose", "Stone" };
    PopupMenu colours;
    auto& palette = getTrackPalette();
    for (int i = 0; i < palette.size(); ++i)
        colours.addColouredItem (100 + i, colourNames[i % 8], palette[i], true, track->colour == palette[i]);

    PopupMenu groupMenu;
    if (track->holdsClips())
    {
        const auto selectedCount = (int) ctx.selection.trackIds.size();
        if (selectedCount > 1 && ctx.selection.trackIds.count (track->id))
            groupMenu.addItem (200, "Group the " + String (selectedCount) + " selected tracks");
        else
            groupMenu.addItem (200, "New group with this track");

        for (size_t i = 0; i < ctx.edit.groups.size(); ++i)
            if (ctx.edit.groups[i]->id != track->groupId)
                groupMenu.addItem (300 + (int) i, "Move to \"" + ctx.edit.groups[i]->name + "\"");

        if (track->groupId >= 0)
        {
            groupMenu.addSeparator();
            groupMenu.addItem (201, "Remove from group");
        }
    }

    PopupMenu m;
    m.addItem (1, "Rename...");
    m.addSubMenu ("Colour", colours);
    if (track->holdsClips())
        m.addSubMenu ("Group", groupMenu);
    m.addSeparator();
    m.addItem (2, "Move up");
    m.addItem (3, "Move down");
    m.addSeparator();

    // Export this track, or every selected track if it's one of them.
    std::set<int> toExport { track->id };
    if (ctx.selection.trackIds.count (track->id) > 0 && ctx.selection.trackIds.size() > 1)
        toExport = ctx.selection.trackIds;
    m.addItem (5, toExport.size() > 1 ? "Export " + String ((int) toExport.size()) + " Selected Tracks..."
                                      : String ("Export This Track..."));
    m.addSeparator();
    m.addItem (4, "Delete track");

    Component::SafePointer<Component> safeAnchor (&anchor);
    auto* context = &ctx;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&anchor), [=] (int r)
    {
        if (safeAnchor == nullptr || r == 0)
            return;

        auto& edit = context->edit;
        auto indexOf = [&]
        {
            auto& list = track->isBus() ? edit.buses : edit.tracks;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i] == track)
                    return (int) i;
            return -1;
        };

        if (r == 1)
        {
            renameTrack (*context, track);
        }
        else if (r == 5)
        {
            showExportDialog (*context, toExport);
        }
        else if (r == 200)
        {
            groupSelectedTracks (*context, track->id);
        }
        else if (r == 201)
        {
            edit.setTrackGroup (track->id, -1);
        }
        else if (r >= 300)
        {
            if ((size_t) (r - 300) < edit.groups.size())
                edit.setTrackGroup (track->id, edit.groups[(size_t) (r - 300)]->id);
        }
        else if (r >= 100)
        {
            track->colour = getTrackPalette()[r - 100];
            edit.touched();
        }
        else if (r == 2 || r == 3)
        {
            edit.moveTrack (track->id, indexOf() + (r == 2 ? -1 : 1));
        }
        else if (r == 4)
        {
            const auto id = track->id;
            if (track->clips.empty())
            {
                edit.removeTrack (id);
                return;
            }

            AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, "Delete " + track->name + "?",
                                          "This track has clips. Deleting it can't be undone.",
                                          "Delete", "Cancel", nullptr,
                                          ModalCallbackFunction::create ([context, id] (int ok)
                                          {
                                              if (ok == 1)
                                                  context->edit.removeTrack (id);
                                          }));
        }
    });
}
} // namespace lathe
