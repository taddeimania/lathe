#pragma once

#include "Arrangement.h"
#include "CloudUI.h"
#include "DeviceChain.h"
#include "Dialogs.h"
#include "Mixer.h"
#include "Notes.h"
#include "PianoRoll.h"
#include "PluginBrowser.h"
#include "TransportBar.h"
#include "UpdateNotice.h"

namespace lathe
{
class MainComponent : public Component,
                      public MenuBarModel,
                      private ChangeListener,
                      private Timer
{
public:
    /** forScreenshots: Lathe --screenshots. No audio device, no plug-in scan, no network;
        runScreenshots() then draws the demo song's windows to files and quits. */
    explicit MainComponent (PropertiesFile& settings, bool forScreenshots = false);
    ~MainComponent() override;

    /** Opens a file from the command line, the last project, or the default template. */
    void startup (const String& commandLine);

    /** Writes the demo song's screenshots (arrange, notes, mixer, history .jpg) into outDir,
        then quits Lathe (exit code 0, or 1 if something failed). */
    void runScreenshots (const File& outDir);

    void newProject (const File& templateFile);
    void openProject (const File&);
    void save (std::function<void (bool)> done = {});
    void saveAs (std::function<void (bool)> done = {});
    void saveAsTemplate();
    /** Runs `proceed` once unsaved changes are saved or discarded (not if the user cancels). */
    void confirmDiscard (std::function<void()> proceed);
    /** Quitting: warns if the cloud is still uploading or downloading, then confirmDiscard. */
    void confirmQuit (std::function<void()> proceed);

    // MenuBarModel
    StringArray getMenuBarNames() override;
    PopupMenu getMenuForIndex (int index, const String& name) override;
    void menuItemSelected (int itemId, int topLevelMenuIndex) override;

    bool keyPressed (const KeyPress&) override;
    void resized() override;
    void paint (Graphics&) override;

private:
    class Divider;

    void changeListenerCallback (ChangeBroadcaster*) override;
    void timerCallback() override;

    void refreshAll();
    void updateTitle();
    void setStatus (const String&);
    void setMixerShown (bool);
    void showDetail (bool pianoRoll);
    void loadProjectFile (const File& file, const File& readFrom);
    Edit::PluginFactory makePluginFactory (StringArray& missing);
    void reloadMissingPlugins();
    void ensureBuiltInTemplates();
    Array<File> templateFiles();
    void deleteTemplate (const File&);
    void importAudio();
    void openPluginBrowser();
    void openPatcher();
    void writeAutosave();
    File autosaveFileFor (const File& project) const   { return project.getSiblingFile (project.getFileName() + "-autosave"); }

    PropertiesFile& settings;
    const bool forScreenshots;
    PluginHost host;
    AudioPool pool;
    Edit edit;
    Engine engine;
    AppContext ctx;

    TransportBar transport;
    ArrangementView arrangement;
    MixerView mixer;
    DeviceChainView devices;
    PianoRoll pianoRoll;
    PluginStore store;
    std::unique_ptr<PluginBrowserWindow> browserWindow;
    std::unique_ptr<CloudController> cloud;
    std::unique_ptr<UpdateNotice> updateNotice;
    std::unique_ptr<NotesController> notes;
    std::shared_ptr<assistant::Service> aiAssistant;   // AI assistant (MCP) connection
    bool announcedUpdates = false;
    std::unique_ptr<Divider> divider;
    Label statusLeft, statusRight;
    TooltipWindow tooltips { this, 600 };

    RecentlyOpenedFilesList recentFiles;
    // The files behind the File menu's items, as they were when the menu was built. The Mac
    // keeps its menu bar between rebuilds, so a click must open what that item showed, not
    // whatever is at that position in the list now.
    Array<File> recentMenuFiles, templateMenuFiles;
    std::unique_ptr<FileChooser> chooser;

    bool mixerShown = false, pianoRollShown = false, detailShown = true;
    int detailHeight = 190;
    uint32 statusTime = 0;
    int ticks = 0;
    int64 lastAutosave = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace lathe
