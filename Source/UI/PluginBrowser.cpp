#include "PluginBrowser.h"
#include "../Devices/Device.h"

namespace lathe
{
namespace
{
constexpr int kRowHeight = 50;
constexpr int kHeadingHeight = 28;

Colour kindColour (bool instrument)
{
    auto& p = getTrackPalette();
    return instrument ? p[1] : p[4];
}
} // namespace

//==============================================================================
class PluginBrowser::Row : public Component
{
public:
    Row (PluginBrowser& o, const PluginDescription& d)
        : desc (d), key (d.createIdentifierString()), builtIn (devices::isBuiltIn (d)), owner (o)
    {
        addButton.setWantsKeyboardFocus (false);
        addButton.onClick = [this] { owner.select (key); owner.add (desc); };
        addAndMakeVisible (addButton);
        update();
    }

    void update()
    {
        addButton.setTooltip (describeAddTarget (owner.ctx, desc.isInstrument, devices::isNoteGenerator (desc)));
        repaint();
    }

    void paint (Graphics& g) override
    {
        const bool selected = owner.selectedKey == key;
        g.fillAll (selected ? theme::headerSel : (isMouseOver (true) ? theme::panelRaised : theme::panel));
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        g.setColour (kindColour (desc.isInstrument));
        g.fillRect (0, 6, 3, getHeight() - 12);

        auto text = getLocalBounds().withTrimmedLeft (14).withRight (getWidth() - 140).reduced (0, 7);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        auto top = text.removeFromTop (18);
        g.drawText (desc.name, top, Justification::centredLeft, true);

        const int nameWidth = GlyphArrangement::getStringWidthInt (theme::font (14.0f, true), desc.name);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText (devices::kindName (desc), top.withTrimmedLeft (nameWidth + 10), Justification::centredLeft, true);

        g.setFont (theme::font (12.5f));
        const auto line = builtIn ? desc.descriptiveName
                                  : desc.manufacturerName + (desc.version.isNotEmpty() ? "  /  " + desc.version : String());
        g.drawText (line, text, Justification::centredLeft, true);
    }

    void resized() override
    {
        addButton.setBounds (getLocalBounds().removeFromRight (124).reduced (12, 12));
    }

    void mouseEnter (const MouseEvent&) override   { repaint(); }
    void mouseExit (const MouseEvent&) override    { repaint(); }
    void mouseDown (const MouseEvent& e) override
    {
        owner.select (key);
        if (e.mods.isPopupMenu() && ! builtIn)
        {
            PopupMenu m;
            const File bundle (desc.fileOrIdentifier);
            m.addItem (1, "Show installed plug-in", File::isAbsolutePath (desc.fileOrIdentifier) && bundle.exists());
            m.showMenuAsync (PopupMenu::Options(), [bundle] (int r)
            {
                if (r == 1)
                    bundle.revealToUser();
            });
        }
    }
    void mouseDoubleClick (const MouseEvent&) override   { addButton.triggerClick(); }

    const PluginDescription desc;
    const String key;
    const bool builtIn;

private:
    PluginBrowser& owner;
    TextButton addButton { "Add" };
};

//==============================================================================
PluginBrowser::PluginBrowser (AppContext& c) : ctx (c)
{
    for (auto* b : { &filterAll, &filterEffects, &filterInstruments })
    {
        b->setClickingTogglesState (false);
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, b] { filter = b->getButtonText(); refresh(); };
        addAndMakeVisible (b);
    }

    search.setTextToShowWhenEmpty ("Search", theme::textDim);
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { search.clear(); refresh(); };
    addAndMakeVisible (search);

    list.painter = [this] (Graphics& g)
    {
        g.fillAll (theme::panel);
        g.setFont (theme::font (12.5f, true));
        for (auto& [text, y] : headings)
        {
            g.setColour (theme::window);
            g.fillRect (0, y, list.getWidth(), kHeadingHeight);
            g.setColour (theme::textDim);
            g.drawText (text, 14, y, list.getWidth() - 20, kHeadingHeight, Justification::centredLeft);
        }
    };
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    detailsTitle.setFont (theme::font (13.0f, true));
    addAndMakeVisible (detailsTitle);

    details.setMultiLine (true);
    details.setReadOnly (true);
    details.setScrollbarsShown (true);
    details.setCaretVisible (false);
    details.setFont (theme::font (12.5f));
    details.setColour (TextEditor::backgroundColourId, theme::lane);
    details.setColour (TextEditor::textColourId, theme::text);
    addAndMakeVisible (details);

    footer.setFont (theme::font (12.0f));
    footer.setColour (Label::textColourId, theme::textDim);
    addAndMakeVisible (footer);

    refresh();
}

PluginBrowser::~PluginBrowser() = default;

bool PluginBrowser::matches (bool instrument, const String& text) const
{
    if (filter == "Effects" && instrument)
        return false;
    if (filter == "Instruments" && ! instrument)
        return false;
    const auto q = search.getText().trim();
    return q.isEmpty() || text.containsIgnoreCase (q);
}

void PluginBrowser::refresh()
{
    filterAll.setToggleState (filter == "All", dontSendNotification);
    filterEffects.setToggleState (filter == "Effects", dontSendNotification);
    filterInstruments.setToggleState (filter == "Instruments", dontSendNotification);

    rows.clear();
    headings.clear();

    // Lathe's own, in catalogue order: instruments first.
    for (const bool instruments : { true, false })
        for (auto& info : devices::all())
            if ((info.kind != devices::Kind::effect) == instruments   // generators go with the instruments
                && matches (instruments, info.name + " " + info.blurb + " Lathe"))
                list.addAndMakeVisible (rows.add (new Row (*this, devices::describe (info))));
    numBuiltIn = rows.size();

    // Everything else that's installed.
    auto types = ctx.host.known.getTypes();
    std::sort (types.begin(), types.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    for (auto& d : types)
        if (matches (d.isInstrument, d.name + " " + d.manufacturerName + " " + d.category))
            list.addAndMakeVisible (rows.add (new Row (*this, d)));

    layoutList();
    showDetails();

    const auto target = ctx.edit.findTrack (ctx.selection.trackId);
    footer.setText ("Effects go on the selected track" + (target != nullptr ? " (" + target->name + ")" : String())
                    + "; instruments on a MIDI track (a new one if needed). Scan for newly installed plug-ins in Options.",
                    dontSendNotification);
}

void PluginBrowser::layoutList()
{
    const int width = viewport.getMaximumVisibleWidth();
    int y = 0;
    std::vector<std::pair<String, int>> placed;

    placed.push_back ({ "Built into Lathe", y });
    y += kHeadingHeight;
    if (numBuiltIn == 0)
    {
        placed.push_back ({ "No matches", y });
        y += kHeadingHeight;
    }

    for (int i = 0; i < rows.size(); ++i)
    {
        if (i == numBuiltIn)
        {
            y += 10;
            placed.push_back ({ "Installed plug-ins", y });
            y += kHeadingHeight;
        }
        rows[i]->setBounds (0, y, width, kRowHeight);
        y += kRowHeight;
    }
    if (rows.size() == numBuiltIn)
    {
        y += 10;
        placed.push_back ({ ctx.host.known.getNumTypes() == 0 ? "Installed plug-ins: none found yet (Options > Scan for New Plug-ins)"
                                                              : "Installed plug-ins: no matches", y });
        y += kHeadingHeight;
    }

    headings = placed;
    list.setSize (width, y + 8);
    list.repaint();
    repaint();
}

void PluginBrowser::select (const String& key)
{
    selectedKey = key;
    for (auto* r : rows)
        r->repaint();
    showDetails();
}

void PluginBrowser::showDetails()
{
    Row* row = nullptr;
    for (auto* r : rows)
        if (r->key == selectedKey)
            row = r;

    if (row == nullptr)
    {
        detailsTitle.setText ("Select a plug-in to see what it is", dontSendNotification);
        details.setText ({}, false);
        return;
    }

    auto& d = row->desc;
    detailsTitle.setText (d.name, dontSendNotification);
    if (row->builtIn)
        details.setText (d.descriptiveName + "\n\nBuilt into Lathe: nothing to install, and anyone you share the project with has it too.", false);
    else
        details.setText (d.manufacturerName + "\n" + d.pluginFormatName + "  /  " + devices::kindName (d)
                         + (d.version.isNotEmpty() ? "  /  " + d.version : String()) + "\n" + d.fileOrIdentifier, false);
}

void PluginBrowser::add (const PluginDescription& d)
{
    addPluginToSelection (ctx, d, false);
    for (auto* r : rows)
        r->update();
}

//==============================================================================
void PluginBrowser::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::border);
    g.drawHorizontalLine (44, 0.0f, (float) getWidth());
}

void PluginBrowser::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (44).reduced (10, 9);
    for (auto* b : { &filterAll, &filterEffects, &filterInstruments })
        b->setBounds (top.removeFromLeft (b == &filterAll ? 44 : 92));
    top.removeFromLeft (12);
    search.setBounds (top);

    footer.setBounds (r.removeFromBottom (26).reduced (10, 0));
    auto info = r.removeFromBottom (130).reduced (10, 6);
    detailsTitle.setBounds (info.removeFromTop (22));
    details.setBounds (info);

    viewport.setBounds (r);
    layoutList();
}

//==============================================================================
PluginBrowserWindow::PluginBrowserWindow (AppContext& ctx)
    : DocumentWindow ("Plug-ins", theme::panel, DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    browser = new PluginBrowser (ctx);
    setContentOwned (browser, false);
    setResizable (true, false);
    setResizeLimits (560, 420, 4000, 4000);
    centreWithSize (760, 620);
}

void PluginBrowserWindow::show()
{
    setVisible (true);
    toFront (true);
    browser->refresh();
    browser->focusSearch();
}
} // namespace lathe
