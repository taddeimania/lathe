#pragma once

#include "Widgets.h"
#include "../Engine/PluginStore.h"

namespace lathe
{
/** Your GitHub plug-ins (install / update / add in one click) plus everything
    else that's installed. Lives in its own window. */
class PluginBrowser : public Component, private Timer, private AsyncUpdater
{
public:
    PluginBrowser (AppContext&, PluginStore&);
    ~PluginBrowser() override;

    /** Rebuilds the list from the store and the scanned plug-ins. */
    void refresh();
    /** Use this from store notifications: a row may be in the middle of its own click handler. */
    void refreshSoon()   { triggerAsyncUpdate(); }
    void focusSearch()   { search.grabKeyboardFocus(); }

    void paint (Graphics&) override;
    void resized() override;

private:
    class Row;
    friend class Row;

    void timerCallback() override;
    void handleAsyncUpdate() override   { refresh(); }
    void select (const String& key);
    void showDetails();
    void addFromStore (String repo);
    void updateFromStore (String repo);
    void addLocal (const PluginDescription&);
    void layoutList();
    bool matches (const String& kind, const String& text) const;

    AppContext& ctx;
    PluginStore& store;

    TextButton filterAll { "All" }, filterEffects { "Effects" }, filterInstruments { "Instruments" }, filterMidi { "MIDI tools" };
    TextEditor search;
    TextButton checkButton { "Check for updates" }, addRepoButton { "Add repository..." };
    struct ListArea : public Component
    {
        std::function<void (Graphics&)> painter;
        void paint (Graphics& g) override   { if (painter) painter (g); }
    };
    Viewport viewport;
    ListArea list;
    OwnedArray<Row> rows;
    std::vector<std::pair<String, int>> headings;   // text, y in the list
    TextEditor notes;
    Label detailsTitle;
    HyperlinkButton releaseLink;
    Label footer;

    String filter = "All";
    String selectedKey;
};

class PluginBrowserWindow : public DocumentWindow
{
public:
    PluginBrowserWindow (AppContext&, PluginStore&);
    void closeButtonPressed() override   { setVisible (false); }
    void show();
    PluginBrowser& getBrowser()          { return *browser; }

private:
    PluginBrowser* browser;
};
} // namespace lathe
