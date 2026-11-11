#include "DeviceChain.h"
#include "../Devices/Device.h"

namespace lathe
{
class DeviceChainView::DeviceBox : public Component
{
public:
    DeviceBox (AppContext& c, std::shared_ptr<Track> t, std::shared_ptr<PluginSlot> s)
        : ctx (c), track (std::move (t)), slot (std::move (s))
    {
        styleToggle (power, "", theme::accent, "On / bypass");
        power.onClick = [this]
        {
            slot->bypassed = ! slot->bypassed.load();
            ctx.edit.changed();
        };
        addAndMakeVisible (power);

        for (auto* b : { &openButton, &leftButton, &rightButton, &removeButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        openButton.onClick = [this] { openPluginEditor (ctx, slot, track->name); };
        leftButton.onClick = [this] { movePlugin (ctx, *track, slot, -1); };
        rightButton.onClick = [this] { movePlugin (ctx, *track, slot, 1); };
        removeButton.onClick = [this] { removePlugin (ctx, *track, slot); };
        leftButton.setTooltip ("Move earlier in the chain");
        rightButton.setTooltip ("Move later in the chain");
        removeButton.setTooltip ("Remove");
        openButton.setEnabled (! slot->isMissing());
        power.setEnabled (! slot->isMissing());
        update();
    }

    void update()
    {
        power.setToggleState (! slot->bypassed.load() && ! slot->isMissing(), dontSendNotification);
        repaint();
    }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        auto title = getLocalBounds().removeFromTop (26).withTrimmedLeft (30).withTrimmedRight (30);
        g.setColour (slot->isMissing() ? theme::record : slot->bypassed.load() ? theme::textDim : theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (slot->getName(), title, Justification::centredLeft, true);

        auto info = getLocalBounds().withTrimmedTop (30).reduced (10, 0).removeFromTop (40);
        g.setFont (theme::font (11.5f));
        g.setColour (theme::textDim);
        String line1 = slot->description.manufacturerName;
        String line2;
        if (slot->isMissing())
            line2 = "Not installed. Settings are kept.";
        else
        {
            const int latency = slot->instance->getLatencySamples();
            line2 = devices::kindName (slot->description) + String (latency > 0 ? ",  " + String (latency) + " smp latency" : "");
        }
        g.drawText (line1, info.removeFromTop (16), Justification::centredLeft, true);
        g.drawText (line2, info.removeFromTop (16), Justification::centredLeft, true);
    }

    void paintOverChildren (Graphics& g) override
    {
        // power symbol on the bypass toggle
        auto p = power.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        g.setColour (power.getToggleState() ? theme::textDark : theme::textDim);
        g.drawEllipse (p, 1.5f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (5);
        auto top = r.removeFromTop (18);
        power.setBounds (top.removeFromLeft (20));
        removeButton.setBounds (top.removeFromRight (20));

        auto bottom = r.removeFromBottom (22);
        leftButton.setBounds (bottom.removeFromLeft (24));
        bottom.removeFromLeft (2);
        rightButton.setBounds (bottom.removeFromLeft (24));
        bottom.removeFromLeft (6);
        openButton.setBounds (bottom);
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showPluginSlotMenu (ctx, *this, track, slot);
    }

    void mouseDoubleClick (const MouseEvent&) override
    {
        openPluginEditor (ctx, slot, track->name);
    }

private:
    AppContext& ctx;
    std::shared_ptr<Track> track;
    std::shared_ptr<PluginSlot> slot;
    TextButton power, openButton { "Open" }, leftButton { "<" }, rightButton { ">" }, removeButton { "x" };
};

//==============================================================================
DeviceChainView::DeviceChainView (AppContext& c) : ctx (c)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this]
    {
        if (track != nullptr)
            showAddPluginMenu (ctx, addButton, track, -1);
    };
    content.addAndMakeVisible (addButton);

    browseButton.setWantsKeyboardFocus (false);
    browseButton.setTooltip ("Your plug-ins on GitHub, and everything installed  (Cmd/Ctrl+B)");
    browseButton.onClick = [this] { if (ctx.openPluginBrowser) ctx.openPluginBrowser(); };
    content.addAndMakeVisible (browseButton);
}

DeviceChainView::~DeviceChainView() = default;

void DeviceChainView::setTrack (std::shared_ptr<Track> t)
{
    if (t == track)
        return;
    track = std::move (t);
    signature = "-";
    eqDevice.reset();
    if (track != nullptr)
    {
        eqDevice = std::make_unique<EqDevice> (ctx, track);
        content.addAndMakeVisible (*eqDevice);
    }
    refresh();
}

void DeviceChainView::refresh()
{
    String sig;
    if (track != nullptr)
        for (auto& p : track->plugins)
            sig << String::toHexString ((pointer_sized_int) p.get()) << ",";

    if (sig != signature)
    {
        signature = sig;
        boxes.clear();
        if (track != nullptr)
            for (auto& p : track->plugins)
                content.addAndMakeVisible (boxes.add (new DeviceBox (ctx, track, p)));
        resized();
    }

    for (auto* b : boxes)
        b->update();
    if (eqDevice != nullptr)
        eqDevice->update();

    addButton.setVisible (track != nullptr);
    browseButton.setVisible (track != nullptr);
    repaint();
}

void DeviceChainView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    auto title = getLocalBounds().removeFromTop (24).reduced (10, 0);
    g.setFont (theme::font (12.0f, true));
    if (track == nullptr)
    {
        g.setColour (theme::textDim);
        g.drawText ("Select a track to see its plug-ins.", title, Justification::centredLeft);
        return;
    }

    g.setColour (track->colour);
    g.fillRect (title.removeFromLeft (4).reduced (0, 6));
    title.removeFromLeft (8);
    g.setColour (theme::text);
    g.drawText (track->name + "  -  plug-ins and EQ in signal order", title, Justification::centredLeft);

    if (track->plugins.empty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        auto hint = Rectangle<int> (addButton.getX(), 24 + browseButton.getBottom() + 6, addButton.getWidth(), 40);
        g.drawFittedText (track->isMidi() ? "Add an instrument to hear this MIDI track."
                                          : "No plug-ins yet.",
                          hint, Justification::topLeft, 3);
    }
}

void DeviceChainView::resized()
{
    auto r = getLocalBounds().withTrimmedTop (24);
    viewport.setBounds (r);
    const int h = jmax (100, r.getHeight() - 12);
    int x = 10;
    for (auto* b : boxes)
    {
        b->setBounds (x, 2, 190, jmin (h, 120));
        x += 196;
    }
    addButton.setBounds (x, 2, 120, 26);
    browseButton.setBounds (x, 32, 120, 26);
    x += 130;
    if (eqDevice != nullptr)
    {
        eqDevice->setBounds (x, 2, 380, jmin (h, 150));
        x += 386;
    }
    content.setSize (jmax (r.getWidth(), x + 10), h + 4);
}
} // namespace lathe
