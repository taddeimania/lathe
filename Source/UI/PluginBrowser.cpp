#include "PluginBrowser.h"

namespace lathe
{
namespace
{
constexpr int kRowHeight = 50;
constexpr int kHeadingHeight = 28;

Colour kindColour (const String& kind)
{
    auto& p = getTrackPalette();
    if (kind == "Instrument") return p[1];
    if (kind == "MIDI")       return p[2];
    return p[4];
}

/** Release notes are Markdown; show them as plain, readable text. */
String plainNotes (const String& markdown)
{
    StringArray out;
    for (auto line : StringArray::fromLines (markdown))
    {
        line = line.trimEnd();
        if (line.startsWith ("#"))
            line = line.trimCharactersAtStart ("# ").toUpperCase();
        line = line.replace ("**", "").replace ("`", "");
        if (line.startsWith ("- ") || line.startsWith ("* "))
            line = "  -  " + line.substring (2);
        out.add (line);
    }
    return out.joinIntoString ("\n").trim();
}
} // namespace

//==============================================================================
class PluginBrowser::Row : public Component
{
public:
    // A plug-in from GitHub
    Row (PluginBrowser& o, const String& repoName) : repo (repoName), key ("gh:" + repoName), owner (o)
    {
        setup();
    }

    // Anything else installed
    Row (PluginBrowser& o, const PluginDescription& d) : local (d), key ("local:" + d.createIdentifierString()), owner (o)
    {
        setup();
    }

    StoreEntry* entry() const   { return repo.isNotEmpty() ? owner.store.find (repo) : nullptr; }
    bool isLocal() const        { return repo.isEmpty(); }

    void update()
    {
        auto* e = entry();
        if (e == nullptr && ! isLocal())
            return;

        if (isLocal())
        {
            addButton.setButtonText ("Add");
            addButton.setEnabled (true);
            addButton.setTooltip (describeAddTarget (owner.ctx, local.isInstrument, false));
            updateButton.setVisible (false);
            progress.setVisible (false);
            repaint();
            return;
        }

        const bool busy = e->busy;
        progress.setVisible (busy);
        addButton.setVisible (! busy);
        updateButton.setVisible (! busy && e->updateAvailable() && ! e->pendingRestart);

        const bool installed = e->isInstalled();
        if (e->pendingRestart)
        {
            addButton.setButtonText ("Restart to finish");
            addButton.setEnabled (false);
        }
        else if (installed)
        {
            addButton.setButtonText ("Add");
            addButton.setEnabled (true);
        }
        else
        {
            addButton.setButtonText ("Get & add");
            addButton.setEnabled (e->hasRelease());
        }

        const bool midiTool = e->kind == "MIDI";
        addButton.setTooltip ((installed ? String() : "Downloads " + e->assetName + " from GitHub, installs it, then: ")
                              + describeAddTarget (owner.ctx, e->kind == "Instrument", midiTool));
        updateButton.setButtonText ("Update to " + e->version);
        updateButton.setTooltip ("Download and install " + e->name + " " + e->version);
        repaint();
    }

    void tickProgress()
    {
        if (auto* e = entry(); e != nullptr && e->busy && std::abs (shownProgress - e->progress) > 0.001)
        {
            shownProgress = e->progress;
            progress.repaint();
        }
    }

    void paint (Graphics& g) override
    {
        const bool selected = owner.selectedKey == key;
        g.fillAll (selected ? theme::headerSel : (isMouseOver (true) ? theme::panelRaised : theme::panel));
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        auto* e = entry();
        const auto kind = isLocal() ? (local.isInstrument ? String ("Instrument") : String ("Effect")) : e->kind;
        g.setColour (kindColour (kind));
        g.fillRect (0, 6, 3, getHeight() - 12);

        auto text = getLocalBounds().withTrimmedLeft (14).withRight (statusLeft()).reduced (0, 7);
        const auto name = isLocal() ? local.name : e->name;
        const auto description = isLocal() ? local.manufacturerName + (local.category.isNotEmpty() ? "  /  " + local.category : String())
                                           : e->description;

        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        auto top = text.removeFromTop (18);
        g.drawText (name, top, Justification::centredLeft, true);

        const int nameWidth = GlyphArrangement::getStringWidthInt (theme::font (14.0f, true), name);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText (kind.toUpperCase(), top.withTrimmedLeft (nameWidth + 10), Justification::centredLeft, true);

        g.setFont (theme::font (12.5f));
        g.drawText (description, text, Justification::centredLeft, true);

        // status column
        auto status = getLocalBounds().withLeft (statusLeft()).withRight (buttonsLeft() - 10);
        String line1, line2;
        Colour c1 = theme::textDim;
        if (isLocal())
        {
            line1 = local.version.isNotEmpty() ? "Installed " + local.version : "Installed";
        }
        else if (e->pendingRestart)
        {
            line1 = "Updated to " + e->installedVersion;
            line2 = "Restart Lathe to finish";
            c1 = theme::accent;
        }
        else if (e->isInstalled())
        {
            line1 = "Installed " + (e->installedVersion == "?" ? String() : e->installedVersion);
            if (e->updateAvailable())
            {
                line2 = e->version + " available";
                c1 = theme::accent;
            }
        }
        else if (e->hasRelease())
        {
            line1 = e->version + " on GitHub";
            line2 = String (e->assetSize / (1024.0 * 1024.0), 1) + " MB";
        }
        else
        {
            line1 = e->fetchError.isNotEmpty() ? e->fetchError : String ("Not checked yet");
        }

        g.setFont (theme::font (12.0f));
        g.setColour (c1);
        g.drawFittedText (line1, status.removeFromTop (getHeight() / 2).withTrimmedTop (6), Justification::bottomLeft, 1);
        g.setColour (theme::textDim);
        g.drawFittedText (line2, status.withTrimmedBottom (6), Justification::topLeft, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().withLeft (buttonsLeft()).reduced (0, 12).withTrimmedRight (12);
        addButton.setBounds (r.removeFromRight (112));
        progress.setBounds (addButton.getBounds());
        r.removeFromRight (6);
        updateButton.setBounds (r.removeFromRight (110));
    }

    void mouseEnter (const MouseEvent&) override   { repaint(); }
    void mouseExit (const MouseEvent&) override    { repaint(); }

    void mouseDown (const MouseEvent& e) override
    {
        owner.select (key);
        if (e.mods.isPopupMenu())
            showMenu();
    }

    void mouseDoubleClick (const MouseEvent&) override
    {
        addButton.triggerClick();
    }

    const String repo;
    const PluginDescription local;
    const String key;

private:
    void setup()
    {
        addButton.setWantsKeyboardFocus (false);
        updateButton.setColour (TextButton::buttonColourId, theme::accent);
        updateButton.setColour (TextButton::textColourOffId, theme::textDark);
        addButton.onClick = [this]
        {
            owner.select (key);
            if (isLocal()) owner.addLocal (local);
            else           owner.addFromStore (String (repo));
        };
        addAndMakeVisible (addButton);

        updateButton.setWantsKeyboardFocus (false);
        updateButton.onClick = [this] { owner.select (key); owner.updateFromStore (String (repo)); };
        addChildComponent (updateButton);
        addChildComponent (progress);
        update();
    }

    int statusLeft() const    { return jmax (240, getWidth() - 420); }
    int buttonsLeft() const   { return getWidth() - 250; }

    void showMenu()
    {
        PopupMenu m;
        auto* e = entry();
        if (e != nullptr)
        {
            m.addItem (1, e->isInstalled() ? "Reinstall " + e->version : "Install only (don't add)", e->hasRelease() && ! e->busy);
            m.addItem (2, "Open release page", e->releaseUrl.isNotEmpty());
            m.addItem (3, "Open repository");
        }
        const File bundle = e != nullptr ? e->installedBundle : File (local.fileOrIdentifier);
        m.addItem (4, "Show installed plug-in", bundle.exists());

        Component::SafePointer<Row> safe (this);
        m.showMenuAsync (PopupMenu::Options(), [safe, bundle] (int r)
        {
            if (safe == nullptr || r == 0)
                return;
            auto* self = safe.getComponent();
            auto* entry = self->entry();
            if (r == 1 && entry != nullptr)
            {
                auto* browser = &self->owner;
                const auto name = entry->name;
                browser->store.install (entry->repo, [browser, name] (const Result& res, const PluginDescription*)
                {
                    browser->ctx.showStatus (res.wasOk() ? name + " installed." : res.getErrorMessage());
                });
            }
            if (r == 2 && entry != nullptr) URL (entry->releaseUrl).launchInDefaultBrowser();
            if (r == 3 && entry != nullptr) URL ("https://github.com/" + entry->repo).launchInDefaultBrowser();
            if (r == 4) bundle.revealToUser();
        });
    }

    PluginBrowser& owner;
    TextButton addButton, updateButton;
    double shownProgress = 0.0;
    ProgressBar progress { shownProgress };
};

//==============================================================================
PluginBrowser::PluginBrowser (AppContext& c, PluginStore& s) : ctx (c), store (s)
{
    for (auto* b : { &filterAll, &filterEffects, &filterInstruments, &filterMidi })
    {
        b->setClickingTogglesState (false);
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, b] { filter = b->getButtonText() == "MIDI tools" ? "MIDI" : b->getButtonText(); refresh(); };
        addAndMakeVisible (b);
    }

    search.setTextToShowWhenEmpty ("Search", theme::textDim);
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { search.clear(); refresh(); };
    addAndMakeVisible (search);

    checkButton.setTooltip ("Ask GitHub for the latest releases now");
    checkButton.onClick = [this] { store.refresh (true); };
    addRepoButton.setTooltip ("Add another GitHub repository that publishes VST3 releases");
    addRepoButton.onClick = [this]
    {
        askForText ("Add a repository", "GitHub repository (owner/name or URL). Its releases need a "
                    + PluginStore::platformTag() + " .zip with a .vst3 inside, like the others.", {},
                    [this] (const String& text) { store.addRepository (text); });
    };
    for (auto* b : { &checkButton, &addRepoButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    list.painter = [this] (Graphics& g)
    {
        g.fillAll (theme::panel);
        g.setFont (theme::font (11.5f, true));
        for (auto& [text, y] : headings)
        {
            g.setColour (theme::window);
            g.fillRect (0, y, list.getWidth(), kHeadingHeight);
            g.setColour (theme::textDim);
            g.drawText (text.toUpperCase(), 14, y, list.getWidth() - 20, kHeadingHeight, Justification::centredLeft);
        }
    };
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    detailsTitle.setFont (theme::font (13.0f, true));
    addAndMakeVisible (detailsTitle);
    releaseLink.setFont (theme::font (12.5f), false, Justification::centredRight);
    releaseLink.setColour (HyperlinkButton::textColourId, theme::accent);
    addAndMakeVisible (releaseLink);

    notes.setMultiLine (true);
    notes.setReadOnly (true);
    notes.setScrollbarsShown (true);
    notes.setCaretVisible (false);
    notes.setFont (theme::font (12.5f));
    notes.setColour (TextEditor::backgroundColourId, theme::lane);
    notes.setColour (TextEditor::textColourId, theme::text);
    addAndMakeVisible (notes);

    footer.setFont (theme::font (12.0f));
    footer.setColour (Label::textColourId, theme::textDim);
    addAndMakeVisible (footer);

    refresh();
    startTimerHz (15);
}

PluginBrowser::~PluginBrowser() = default;

bool PluginBrowser::matches (const String& kind, const String& text) const
{
    if (filter != "All" && kind != (filter == "Effects" ? "Effect" : filter == "Instruments" ? "Instrument" : "MIDI"))
        return false;
    const auto q = search.getText().trim();
    return q.isEmpty() || text.containsIgnoreCase (q);
}

void PluginBrowser::refresh()
{
    filterAll.setToggleState (filter == "All", dontSendNotification);
    filterEffects.setToggleState (filter == "Effects", dontSendNotification);
    filterInstruments.setToggleState (filter == "Instruments", dontSendNotification);
    filterMidi.setToggleState (filter == "MIDI", dontSendNotification);
    checkButton.setEnabled (! store.isRefreshing());
    checkButton.setButtonText (store.isRefreshing() ? "Checking..." : "Check for updates");

    rows.clear();
    headings.clear();

    // Your plug-ins, grouped by kind.
    StringArray yourBundles;
    for (const auto* kind : { "Effect", "Instrument", "MIDI" })
    {
        for (auto& e : store.entries)
        {
            yourBundles.add (e.name + ".vst3");
            if (e.kind == kind && matches (e.kind, e.name + " " + e.description + " " + e.repo))
                list.addAndMakeVisible (rows.add (new Row (*this, e.repo)));
        }
    }

    const int numYours = rows.size();

    // Everything else that's installed.
    auto types = ctx.host.known.getTypes();
    std::sort (types.begin(), types.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    for (auto& d : types)
    {
        const auto bundleName = File::createFileWithoutCheckingPath (d.fileOrIdentifier).getFileName();
        if (yourBundles.contains (bundleName, true))
            continue;
        if (matches (d.isInstrument ? "Instrument" : "Effect", d.name + " " + d.manufacturerName + " " + d.category))
            list.addAndMakeVisible (rows.add (new Row (*this, d)));
    }

    ignoreUnused (numYours);
    layoutList();
    showDetails();

    const auto target = ctx.edit.findTrack (ctx.selection.trackId);
    footer.setText (store.getStatus() + "   Effects go on the selected track"
                    + (target != nullptr ? " (" + target->name + ")" : String()) + "; instruments and MIDI tools on a MIDI track.  "
                    + "Installs to " + PluginStore::getUserVst3Dir().getFullPathName(), dontSendNotification);
}

void PluginBrowser::layoutList()
{
    const int width = viewport.getMaximumVisibleWidth();
    int y = 0;
    int numYours = 0;
    for (auto* r : rows)
        if (! r->isLocal())
            ++numYours;

    std::vector<std::pair<String, int>> placed;
    placed.push_back ({ "Your plug-ins on GitHub", y });
    y += kHeadingHeight;

    if (numYours == 0)
    {
        placed.push_back ({ store.isRefreshing() ? "Checking GitHub..." : "No matches", y });
        y += kHeadingHeight;
    }

    for (int i = 0; i < rows.size(); ++i)
    {
        if (i == numYours)
        {
            y += 10;
            placed.push_back ({ "Other installed plug-ins", y });
            y += kHeadingHeight;
        }
        rows[i]->setBounds (0, y, width, kRowHeight);
        y += kRowHeight;
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

    releaseLink.setVisible (false);
    if (row == nullptr)
    {
        detailsTitle.setText ("Select a plug-in to see its release notes", dontSendNotification);
        notes.setText ({}, false);
        return;
    }

    if (row->isLocal())
    {
        auto& d = row->local;
        detailsTitle.setText (d.name + "  " + d.version, dontSendNotification);
        notes.setText (d.manufacturerName + "\n" + d.pluginFormatName + "  /  " + (d.isInstrument ? "Instrument" : "Effect")
                       + "\n" + d.fileOrIdentifier, false);
        return;
    }

    if (auto* e = row->entry())
    {
        detailsTitle.setText (e->name + (e->version.isNotEmpty() ? "  " + e->version : String())
                              + (e->publishedAt.isNotEmpty() ? "   released " + e->publishedAt : String()), dontSendNotification);
        notes.setText (e->notes.isNotEmpty() ? plainNotes (e->notes) : (e->fetchError.isNotEmpty() ? e->fetchError : e->description), false);
        if (e->releaseUrl.isNotEmpty())
        {
            releaseLink.setButtonText ("Release page");
            releaseLink.setURL (URL (e->releaseUrl));
            releaseLink.setVisible (true);
        }
    }
}

void PluginBrowser::timerCallback()
{
    for (auto* r : rows)
        r->tickProgress();
}

//==============================================================================
void PluginBrowser::addLocal (const PluginDescription& d)
{
    addPluginToSelection (ctx, d, false);
}

void PluginBrowser::addFromStore (String repo)
{
    auto* e = store.find (repo);
    if (e == nullptr)
        return;

    const bool midiTool = e->kind == "MIDI";

    if (e->isInstalled())
    {
        auto desc = store.findDescription (*e);
        if (! desc.has_value() && e->installedBundle.exists())
        {
            // Installed but not scanned yet (e.g. installed by hand): scan it now.
            OwnedArray<PluginDescription> found;
            if (ctx.host.scanFile (e->installedBundle, found))
                desc = *found[0];
            store.updateInstalledState();
        }

        if (desc.has_value())
            addPluginToSelection (ctx, *desc, midiTool);
        else
            ctx.showStatus ("Couldn't load " + e->name + " from " + e->installedBundle.getFullPathName());
        return;
    }

    ctx.showStatus ("Downloading " + e->name + " " + e->version + " from GitHub...");
    auto* context = &ctx;
    const auto name = e->name;
    Component::SafePointer<PluginBrowser> safe (this);
    store.install (repo, [context, name, midiTool, safe] (const Result& r, const PluginDescription* desc)
    {
        if (r.failed())
        {
            context->showStatus ("Couldn't install " + name + ": " + r.getErrorMessage());
            return;
        }
        if (desc == nullptr)
        {
            context->showStatus (name + " was installed but couldn't be loaded. Try Options > Scan for New Plug-ins.");
            return;
        }
        context->showStatus (name + " installed.");
        addPluginToSelection (*context, *desc, midiTool);
        if (safe != nullptr)
            safe->refreshSoon();
    });
}

void PluginBrowser::updateFromStore (String repo)
{
    auto* e = store.find (repo);
    if (e == nullptr)
        return;

    // A plug-in that's in use can't be swapped out from under the running session.
    bool inUse = false;
    for (auto* l : { &ctx.edit.tracks, &ctx.edit.buses })
        for (auto& t : *l)
            for (auto& p : t->plugins)
                if (p->instance != nullptr && p->description.name == e->name)
                    inUse = true;

    ctx.showStatus ("Updating " + e->name + " to " + e->version + "...");
    auto* context = &ctx;
    const auto name = e->name, version = e->version;
    store.install (repo, [context, name, version, inUse] (const Result& r, const PluginDescription*)
    {
        if (r.failed())
            context->showStatus ("Couldn't update " + name + ": " + r.getErrorMessage());
        else
            context->showStatus (name + " updated to " + version
                                 + (inUse ? ". Save and restart Lathe to use the new version in this project." : "."));
    });
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
    for (auto* b : { &filterAll, &filterEffects, &filterInstruments, &filterMidi })
        b->setBounds (top.removeFromLeft (b == &filterAll ? 44 : b == &filterMidi ? 82 : 86));
    top.removeFromLeft (12);
    addRepoButton.setBounds (top.removeFromRight (128));
    top.removeFromRight (6);
    checkButton.setBounds (top.removeFromRight (128));
    top.removeFromRight (12);
    search.setBounds (top);

    footer.setBounds (r.removeFromBottom (26).reduced (10, 0));
    auto details = r.removeFromBottom (170).reduced (10, 6);
    auto titleRow = details.removeFromTop (22);
    releaseLink.setBounds (titleRow.removeFromRight (110));
    detailsTitle.setBounds (titleRow);
    notes.setBounds (details);

    viewport.setBounds (r);
    layoutList();
}

//==============================================================================
PluginBrowserWindow::PluginBrowserWindow (AppContext& ctx, PluginStore& store)
    : DocumentWindow ("Plug-ins", theme::panel, DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    browser = new PluginBrowser (ctx, store);
    setContentOwned (browser, false);
    setResizable (true, false);
    setResizeLimits (640, 420, 4000, 4000);
    centreWithSize (860, 640);
}

void PluginBrowserWindow::show()
{
    setVisible (true);
    toFront (true);
    browser->refresh();
    browser->focusSearch();
}
} // namespace lathe
