#pragma once

#include "Widgets.h"

namespace lathe
{
/** Lathe's own instruments and effects, plus everything else that's installed,
    in one searchable list. Lives in its own window. */
class PluginBrowser : public Component, private AsyncUpdater
{
public:
    explicit PluginBrowser (AppContext&);
    ~PluginBrowser() override;

    /** Rebuilds the list from the built-in devices and the scanned plug-ins. */
    void refresh();
    /** Use this from notifications: a row may be in the middle of its own click handler. */
    void refreshSoon()   { triggerAsyncUpdate(); }
    void focusSearch()   { search.grabKeyboardFocus(); }

    void paint (Graphics&) override;
    void resized() override;

private:
    class Row;
    friend class Row;

    void handleAsyncUpdate() override   { refresh(); }
    void select (const String& key);
    void showDetails();
    void add (const PluginDescription&);
    void layoutList();
    bool matches (bool instrument, const String& text) const;

    AppContext& ctx;

    TextButton filterAll { "All" }, filterEffects { "Effects" }, filterInstruments { "Instruments" };
    TextEditor search;
    struct ListArea : public Component
    {
        std::function<void (Graphics&)> painter;
        void paint (Graphics& g) override   { if (painter) painter (g); }
    };
    Viewport viewport;
    ListArea list;
    OwnedArray<Row> rows;
    int numBuiltIn = 0;
    std::vector<std::pair<String, int>> headings;   // text, y in the list
    TextEditor details;
    Label detailsTitle;
    Label footer;

    String filter = "All";
    String selectedKey;
};

class PluginBrowserWindow : public DocumentWindow
{
public:
    explicit PluginBrowserWindow (AppContext&);
    void closeButtonPressed() override   { setVisible (false); }
    void show();
    PluginBrowser& getBrowser()          { return *browser; }

private:
    PluginBrowser* browser;
};
} // namespace lathe
