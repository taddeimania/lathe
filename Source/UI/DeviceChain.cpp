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

    dropLine.setColour (Label::backgroundColourId, theme::accent);
    dropLine.setInterceptsMouseClicks (false, false);
    content.addChildComponent (dropLine);
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
        eqDevice->addMouseListener (this, false);   // dragging it by its name
        content.addAndMakeVisible (*eqDevice);
    }
    refresh();
}

void DeviceChainView::refresh()
{
    String sig;
    if (track != nullptr)
    {
        for (auto& p : track->plugins)
            sig << String::toHexString ((pointer_sized_int) p.get()) << ",";
        sig << "eq" << track->eqIndex();
    }

    if (sig != signature)
    {
        signature = sig;
        boxes.clear();
        if (track != nullptr)
            for (auto& p : track->plugins)
            {
                auto* box = boxes.add (new DeviceBox (ctx, track, p));
                box->addMouseListener (this, false);   // dragging it by its name
                content.addAndMakeVisible (box);
            }
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
    // "+ Add plug-in" sits where a new plug-in lands: after the last plug-in, so before an EQ
    // that's last (as it always was), or at the end when the EQ was moved in among them.
    const bool eqLast = track != nullptr && track->eqIndex() >= (int) track->plugins.size();
    auto placeAdd = [&]
    {
        addButton.setBounds (x, 2, 120, 26);
        browseButton.setBounds (x, 32, 120, 26);
        x += 130;
    };
    for (auto* c : chainCards())
    {
        if (c == eqDevice.get())
        {
            if (eqLast)
                placeAdd();
            c->setBounds (x, 2, 380, jmin (h, 150));
            x += 386;
        }
        else
        {
            c->setBounds (x, 2, 190, jmin (h, 120));
            x += 196;
        }
    }
    if (! eqLast || eqDevice == nullptr)
        placeAdd();
    content.setSize (jmax (r.getWidth(), x + 10), h + 4);
}

std::vector<Component*> DeviceChainView::chainCards() const
{
    std::vector<Component*> cards;
    if (track == nullptr)
        return cards;
    const int eq = track->eqIndex();
    for (int i = 0; i < boxes.size(); ++i)
    {
        if (i == eq && eqDevice != nullptr)
            cards.push_back (eqDevice.get());
        cards.push_back (boxes[i]);
    }
    if (eq >= boxes.size() && eqDevice != nullptr)
        cards.push_back (eqDevice.get());
    return cards;
}

int DeviceChainView::gapAt (int contentX) const
{
    const auto cards = chainCards();
    for (size_t i = 0; i < cards.size(); ++i)
        if (contentX < cards[i]->getBounds().getCentreX())
            return (int) i;
    return (int) cards.size();
}

void DeviceChainView::mouseDown (const MouseEvent& e)
{
    dragCard = nullptr;
    dragging = false;
    if (track == nullptr || ! e.mods.isLeftButtonDown())
        return;
    const auto cards = chainCards();
    for (size_t i = 0; i < cards.size(); ++i)
        if (e.eventComponent == cards[i] && e.getEventRelativeTo (cards[i]).y < 28)
        {
            dragCard = cards[i];
            dragFrom = (int) i;
        }
}

void DeviceChainView::mouseDrag (const MouseEvent& e)
{
    if (dragCard == nullptr || track == nullptr)
        return;
    if (! dragging)
    {
        if (e.getDistanceFromDragStart() < 5)
            return;
        dragging = true;
        dragCard->setAlpha (0.5f);
        setWantsKeyboardFocus (true);
        grabKeyboardFocus();   // for Esc
    }

    const auto here = e.getEventRelativeTo (this).position;
    const auto inContent = e.getEventRelativeTo (&content).position;
    viewport.autoScroll ((int) e.getEventRelativeTo (&viewport).position.x, (int) e.getEventRelativeTo (&viewport).position.y, 40, 12);

    dragTo = -1;
    if (getLocalBounds().toFloat().contains (here))
    {
        const int gap = gapAt ((int) inContent.x);
        int to = gap > dragFrom ? gap - 1 : gap;
        if (dragCard == eqDevice.get())
            to = jmax (to, track->eqEarliest());   // a MIDI track's EQ stays after the instrument
        if (to != dragFrom)
            dragTo = to;
    }

    const auto cards = chainCards();
    if (dragTo < 0 || cards.empty())
    {
        dropLine.setVisible (false);
        return;
    }
    const int gap = dragTo > dragFrom ? dragTo + 1 : dragTo;
    const int lineX = gap < (int) cards.size() ? cards[(size_t) gap]->getX() - 4 : cards.back()->getRight() + 2;
    dropLine.setBounds (lineX, 2, 3, cards.front()->getHeight());
    dropLine.setVisible (true);
    dropLine.toFront (false);
}

void DeviceChainView::mouseUp (const MouseEvent&)
{
    if (dragging)
        endDrag (true);
    dragCard = nullptr;
}

bool DeviceChainView::keyPressed (const KeyPress& key)
{
    if (dragging && key == KeyPress::escapeKey)
    {
        endDrag (false);
        return true;
    }
    return false;
}

void DeviceChainView::endDrag (bool drop)
{
    if (dragCard != nullptr)
        dragCard->setAlpha (1.0f);
    dropLine.setVisible (false);
    const int from = dragFrom, to = dragTo;
    dragging = false;
    dragCard = nullptr;
    dragTo = -1;
    setWantsKeyboardFocus (false);
    giveAwayKeyboardFocus();
    if (drop && to >= 0 && track != nullptr && moveInChain (*track, from, to))
        ctx.edit.changed();
}
} // namespace lathe
