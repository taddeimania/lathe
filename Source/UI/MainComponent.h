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
    /** The AI assistant demo's pictures (ai-1.jpg ... and ai-session.json), after the others. */
    void runAssistantShots (const File& outDir, const File& work, std::function<void (bool, StringArray)> done);
    /** The pictures for the site's DAW page (daw-*.jpg): the piano roll with a loop, Lathe's
        own devices, Compare and Undo History, from Night Drive with some built-ins added. */
    void runDawShots (const File& outDir, const File& work, const File& song, std::function<void (bool, StringArray)> done);
    /** The pictures in the user guide (getlathe.app/docs): parts of the window, a group with its bus,
        the channel EQ, the plug-in browser, the export dialog, and every built-in device, as docs-*.jpg. */
    void runDocShots (const File& outDir, const File& song, std::function<void (bool, StringArray)> done);

    void newProject (const File& templateFile, bool blank = false);   // blank: no tracks at all
    void openProject (const File&);
    /** Writes "Welcome to Lathe" to the projects folder (if it isn't there) and opens it. */
    void openWelcomeSong();
    void save (std::function<void (bool)> done = {});
    void saveAs (std::function<void (bool)> done = {});
    /** Save As to this file without asking: Name/Name.lathe in its own folder, audio copied in. */
    /** Saves into chosen's own folder, copying the project's audio there first. In the background
        (a progress bar with Cancel) unless `inBackground` is false; `done` gets whether it saved. */
    void saveTo (const File& chosen, std::function<void (bool)> done = {}, bool inBackground = true);
    /** Lets the AI assistant's tools make, open and save projects. */
    void connectAssistantToProjects();
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
    /** Cmd/Ctrl+S, Shift+S, O, N and Shift+E pressed in another of Lathe's windows (a plug-in's). */
    bool fileShortcutFromAnotherWindow (const KeyPress&);
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
    void loadProjectFile (const File& file, const File& readFrom);   // all at once: the assistant and screenshots wait for it
    /** The same, but the plug-ins load one at a time between events, with a progress window and
        Cancel (the open song stays until they're all loaded). `then` runs once it has opened. */
    void loadProjectFile (const File& file, const File& readFrom, std::function<void()> then);
    bool readProject (const File& readFrom, var& data);
    void matchProjectRate (const var& data);
    void finishLoading (const File& file, const var& data, const Edit::PluginFactory&, StringArray& missing);
    class ProjectOpener;
    std::unique_ptr<ProjectOpener> opener;   // while a project's plug-ins load
    bool stillOpening();
    Edit::PluginFactory makePluginFactory (StringArray& missing);
    void reloadMissingPlugins();
    String crashedPluginNote;   // a plug-in that crashed Lathe while loading, moved out (Engine::loadGuardFile)
    void ensureBuiltInTemplates();
    Array<File> templateFiles();
    void deleteTemplate (const File&);
    void importAudio();
    void importMidi();
    void importFolder();                       // File > Import Folder as New Project
    void newProjectFromFolder (const File& folder);
    void openPluginBrowser();
    void openPatcher();
    void writeAutosave();
    File autosaveFileFor (const File& project) const   { return Edit::autosaveFileFor (project); }

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
    std::unique_ptr<PluginBrowserWindow> browserWindow;
    std::unique_ptr<CloudController> cloud;
    std::unique_ptr<UpdateNotice> updateNotice;
    /** Over the arrangement on a song shared so you push only some tracks: which ones. */
    Label pushBanner;
    void showPushBanner();
    /** Under the transport while your changes are set aside to listen to a newer revision. */
    struct SetAsideBar : public Component
    {
        SetAsideBar();
        void resized() override;
        void paint (Graphics&) override;
        Label text;
        TextButton putBack { "Put My Changes Back" }, forget { "Forget Them..." };
    } setAsideBar;
    void showSetAsideBar();
    /** Under the transport while an AI app is working in the session: what it did last, and Pause
        (Continue while paused). Paused, the assistant is turned away at its next step. */
    struct AssistantBar : public Component
    {
        AssistantBar();
        void resized() override;
        void paint (Graphics&) override;
        Label text;
        TextButton pause { "Pause" };
        bool paused = false;
    } assistantBar;
    void showAssistantBar();
    std::unique_ptr<NotesController> notes;
    std::unique_ptr<Compare> compare;                  // View > Compare's versions
    std::shared_ptr<assistant::Service> aiAssistant;   // AI assistant (MCP) connection
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
    // The welcome song's "Press Space to play it": it comes back when another message times out,
    // until the song has played or another project is open (it's easy to miss behind the
    // first-launch question).
    String welcomeHint;
    String suggestedName;   // Save As's name for a project that's never been saved (the stems' folder)
    File welcomeSongFile;
    int ticks = 0;
    int64 lastAutosave = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace lathe
