#include "MainComponent.h"
#include "../Assistant/AssistantService.h"
#include "GlobalKeys.h"
#include "UndoHistory.h"
#include "CompareWindow.h"
#include "../Compare/Compare.h"
#include "../Assistant/AssistantShared.h"
#include "../Devices/Device.h"
#include "../Demo/DemoSong.h"
#include "../Demo/Welcome.h"
#include "../Cloud/Milestones.h"
#include "../Cloud/CrashReports.h"

#include <iostream>

namespace lathe
{
namespace menu
{
    enum
    {
        newProject = 1, open, save, saveAs, saveTemplate, importAudio, exportAudio, exportTracks, showFolder, quit, openWelcome,
        undo = 100, redo, undoHistory, compareMixes, deleteClip, duplicate, split, cutClips, copyClips, pasteClips, joinClips, selectAllClips,
        addAudio = 200, addMidi, addReturn, deleteTrack, groupTracks, ungroupTracks,
        toggleView = 300, toggleDetail, zoomIn, zoomOut, showNotes, notesAll, notesUnresolved, notesNone, addNote, addSongNote,
        settings = 400, scan, metronome, browser, checkUpdates, autoUpdates, assistant,
        templateBase = 1000, deleteTemplateBase = 1500, restoreTemplates = 1999, recentBase = 2000
    };
}

//==============================================================================
/** Drag bar between the main view and the bottom panel. */
class MainComponent::Divider : public Component
{
public:
    explicit Divider (MainComponent& o) : owner (o)   { setMouseCursor (MouseCursor::UpDownResizeCursor); }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::border);
        g.setColour (theme::control);
        g.fillRect (getWidth() / 2 - 12, 2, 24, 1);
    }

    void mouseDown (const MouseEvent&) override   { startHeight = owner.detailHeight; }
    void mouseDrag (const MouseEvent& e) override
    {
        owner.detailHeight = jlimit (90, jmax (120, owner.getHeight() - 260), startHeight - e.getDistanceFromDragStartY());
        owner.resized();
    }
    void mouseDoubleClick (const MouseEvent&) override
    {
        owner.detailShown = ! owner.detailShown;
        owner.resized();
    }

private:
    MainComponent& owner;
    int startHeight = 0;
};

//==============================================================================
MainComponent::MainComponent (PropertiesFile& s, bool screenshots)
    : settings (s),
      forScreenshots (screenshots),
      host (s),
      engine (host, pool, s),
      ctx { edit, engine, host, pool, s, {}, {}, {}, {}, {}, {} },
      transport (ctx),
      arrangement (ctx),
      mixer (ctx),
      devices (ctx),
      pianoRoll (ctx)
{
    setWantsKeyboardFocus (true);
    divider = std::make_unique<Divider> (*this);

    ctx.selectionChanged = [this]
    {
        devices.setTrack (edit.findTrack (ctx.selection.trackId));
        if (browserWindow != nullptr && browserWindow->isVisible())
            browserWindow->getBrowser().refreshSoon();
        arrangement.repaint();
        mixer.repaint();
        for (auto* c : mixer.getChildren())
            c->repaint();
    };
    ctx.openClipEditor = [this] (int trackId, int clipId)
    {
        pianoRoll.setClip (trackId, clipId);
        showDetail (true);
    };
    ctx.showStatus = [this] (const String& m) { setStatus (m); };
    ctx.openPluginBrowser = [this] { openPluginBrowser(); };
    ctx.showDevices = [this] { if (! detailShown || pianoRollShown) showDetail (false); };

    pianoRoll.onClose = [this] { showDetail (false); };
    transport.onViewChange = [this] (bool m) { setMixerShown (m); };
    transport.onOpenSettings = [this] { showSettingsDialog (ctx); };

    engine.onStatus = [this] (const String& m) { setStatus (m); };

    // Crash-loop guard (Engine::loadGuardFile): a plug-in that crashed Lathe while loading runs
    // separately from now on, so the song opens next time instead of crashing again.
    if (! forScreenshots)
    {
        engine.loadGuardFile = settings.getFile().getSiblingFile ("Loading plug-in.txt");
        for (auto& id : StringArray::fromLines (settings.getValue ("runSeparately")))
            if (id.isNotEmpty())
                engine.runSeparately.insert (id);
        if (const auto crashed = engine.takeCrashedLoad(); crashed.isNotEmpty())
        {
            const auto id = crashed.upToFirstOccurrenceOf ("\n", false, false);
            const auto name = crashed.fromFirstOccurrenceOf ("\n", false, false);
            engine.runSeparately.insert (id);
            StringArray ids;
            for (auto& i : engine.runSeparately)
                ids.add (i);
            settings.setValue ("runSeparately", ids.joinIntoString ("\n"));
            settings.saveIfNeeded();
            crashedPluginNote = name + " crashed Lathe while it was loading last time, so it now runs separately: "
                              "if it crashes again, only it stops. (Right-click it to change this.)";
        }
    }
    engine.onDeviceChanged = [this] { refreshAll(); };
    engine.onRecordingFinished = [this] { refreshAll(); milestones::reach (settings, "recorded"); };

    host.onScanProgress = [this] { statusRight.setText (host.getScanStatus(), dontSendNotification); };
    host.onScanFinished = [this]
    {
        statusRight.setText ({}, dontSendNotification);
        const auto skipped = host.getSkippedPlugins();
        setStatus ("Plug-in scan finished: " + String (host.known.getNumTypes()) + " plug-ins available."
                   + (skipped.isEmpty() ? String()
                                        : "  Skipped " + String (skipped.size()) + " that failed to load safely: "
                                          + skipped.joinIntoString (", ") + ". See Settings."));
        reloadMissingPlugins();
        if (browserWindow != nullptr && browserWindow->isVisible())
            browserWindow->getBrowser().refreshSoon();
    };

    addAndMakeVisible (transport);
    addAndMakeVisible (arrangement);
    addChildComponent (mixer);
    addAndMakeVisible (devices);
    addChildComponent (pianoRoll);
    addAndMakeVisible (*divider);

    for (auto* l : { &statusLeft, &statusRight })
    {
        l->setFont (theme::font (12.0f));
        l->setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (l);
    }
    statusRight.setJustificationType (Justification::centredRight);

    recentFiles.setMaxNumberOfItems (10);
    recentFiles.restoreFromString (settings.getValue ("recentProjects"));
    detailHeight = settings.getIntValue ("detailHeight", 190);
    engine.metronomeOn = settings.getBoolValue ("metronome", false);

    edit.addChangeListener (this);
    engine.setEdit (&edit);
    engine.setProcessingThreads (settings.getIntValue ("processingThreads", 0));   // 0 = auto (Settings > Processing)
    if (! forScreenshots)
    {
        engine.openDevice();
        ensureBuiltInTemplates();
    }
    else
    {
        // A pretend interface (nothing plays), so tracks show their inputs.
        auto& dm = engine.deviceManager;
        dm.addAudioDeviceType (demo::makeDeviceType());
        dm.setCurrentAudioDeviceType ("Demo", true);
        AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = "Audio Interface";
        setup.sampleRate = 48000.0;
        setup.bufferSize = 128;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange (0, 8, true);
        setup.outputChannels.setRange (0, 8, true);
        dm.setAudioDeviceSetup (setup, false);
        dm.addAudioCallback (&engine);
    }

    if (host.known.getNumTypes() == 0 && ! forScreenshots)
    {
        setStatus ("First run: scanning your VST3 plug-ins in the background...");
        host.startScan (false);
    }

    CloudController::Hooks hooks;
    hooks.saveProject = [this] (std::function<void (bool)> done) { save (std::move (done)); };
    hooks.openProject = [this] (const File& f)
    {
        autosaveFileFor (f).deleteFile();
        loadProjectFile (f, f);
    };
    hooks.confirmDiscard = [this] (std::function<void()> proceed) { confirmDiscard (std::move (proceed)); };
    hooks.setStatus = [this] (const String& m) { setStatus (m); };
    hooks.projectsDir = [] { return getProjectsDir(); };
    hooks.menuChanged = [this] { menuItemsChanged(); };
    hooks.setAsideChanged = [this] { showSetAsideBar(); };
    cloud = std::make_unique<CloudController> (ctx, settings, hooks);
    cloud->screenshotMode = forScreenshots;
    addAndMakeVisible (cloud->getStatusComponent());

    notes = std::make_unique<NotesController> (ctx, *cloud);
    ctx.notes = notes.get();
    notes->reveal = [this] (int trackId, double beat)
    {
        if (mixerShown)
            setMixerShown (false);
        arrangement.reveal (trackId, beat);
    };
    notes->addChangeListener (this);

    compare = std::make_unique<Compare> (engine, edit, settings);
    ctx.compare = compare.get();

    // AI assistant (MCP) connection: off unless you turned it on (Options > AI Assistant).
    if (! forScreenshots)
    {
        aiAssistant = std::make_shared<assistant::Service> (ctx);
        connectAssistantToProjects();
        aiAssistant->onActivity = [this] { showAssistantBar(); };
    }

    updateNotice = std::make_unique<UpdateNotice> (settings, [this] { openPatcher(); });
    updateNotice->onShowOrHide = [this] { resized(); };
    addChildComponent (*updateNotice);

    // A song shared so you push only some tracks: say which, dim the rest, outline yours.
    pushBanner.setColour (Label::backgroundColourId, theme::accent);
    pushBanner.setColour (Label::textColourId, theme::textDark);
    pushBanner.setFont (theme::font (13.0f));
    pushBanner.setJustificationType (Justification::centredLeft);
    pushBanner.setBorderSize ({ 0, 12, 0, 12 });
    addChildComponent (pushBanner);
    ctx.pushTracksChanged = [this] { showPushBanner(); };

    // Changes set aside while you listen to a newer revision: put them back from here.
    setAsideBar.putBack.onClick = [this] { if (cloud != nullptr) cloud->putBackChanges(); };
    setAsideBar.forget.onClick = [this] { if (cloud != nullptr) cloud->forgetSetAside(); };
    addChildComponent (setAsideBar);

    // While an AI app works in the session: what it's doing, and a way to make it stop and wait.
    assistantBar.pause.onClick = [this]
    {
        if (aiAssistant != nullptr)
            aiAssistant->setPaused (! aiAssistant->isPaused());
    };
    addChildComponent (assistantBar);

    // Space starts and stops from anywhere in Lathe: the mixer, a focused button, a plug-in window.
    if (! forScreenshots)
        globalkeys::install ([this]
        {
            if (! CompareWindow::takeSpace())   // Compare in front: Space plays the comparison
            {
                if (compare != nullptr && compare->isPlaying())
                    compare->stop();
                engine.togglePlay();
            }
        });

    setSize (1360, 860);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    UndoHistoryWindow::closeIfOpen();
    CompareWindow::closeIfOpen();
    globalkeys::uninstall();
    stopTimer();
    if (aiAssistant != nullptr)
        aiAssistant->onActivity = nullptr;   // a dialog may keep it alive a moment longer
    aiAssistant.reset();   // before the project and engine it reads
    ctx.compare = nullptr;
    compare.reset();
    notes->removeChangeListener (this);
    ctx.notes = nullptr;
    notes.reset();
    cloud.reset();
    browserWindow.reset();
    settings.setValue ("detailHeight", detailHeight);
    settings.setValue ("recentProjects", recentFiles.toString());
    engine.saveDeviceSettings();
    edit.removeChangeListener (this);

#if JUCE_MAC
    MenuBarModel::setMacMainMenu (nullptr);
#endif
}

//==============================================================================
void MainComponent::startup (const String& commandLine)
{
    if (crashedPluginNote.isNotEmpty())
    {
        Component::SafePointer<MainComponent> safe (this);
        Timer::callAfterDelay (2500, [safe] { if (safe != nullptr) safe->setStatus (safe->crashedPluginNote); });
    }
    // Anonymous milestones: asked once, on a brand-new install only (Main.cpp sets askMilestones
    // when there was no settings file), after the window is up. Never after an update.
    // Nothing is sent unless they say yes (Cloud/Milestones.h).
    if (! forScreenshots)
    {
        milestones::startup (settings, settings.getBoolValue ("askMilestones", false));
        if (milestones::shouldAsk (settings))
        {
            Component::SafePointer<MainComponent> safe (this);
            Timer::callAfterDelay (1200, [safe]
            {
                if (safe != nullptr && milestones::shouldAsk (safe->settings))
                    showMilestonesDialog (safe->settings);
            });
        }
        else
        {
            // Crashed last time? Ask before sending the report (or follow Settings > Privacy).
            Component::SafePointer<MainComponent> safe (this);
            Timer::callAfterDelay (1500, [safe]
            {
                if (safe != nullptr)
                    handleCrashReports (safe->settings);
            });
        }
    }

    const auto arg = commandLine.unquoted().trim();
    if (arg.isNotEmpty() && File::isAbsolutePath (arg) && File (arg).existsAsFile())
    {
        openProject (File (arg));
        return;
    }

    const auto last = File (settings.getValue ("lastProject"));

    // A new install: open a finished-sounding song, made with Lathe's own instruments, so the
    // first thing anyone does is press play. Once only; File > Open the Welcome Song brings it back.
    if (! settings.containsKey ("welcomeSongShown"))
    {
        settings.setValue ("welcomeSongShown", true);
        if (! last.existsAsFile())
        {
            openWelcomeSong();
            return;
        }
    }

    if (settings.getValue ("startup", "last") == "last" && last.existsAsFile())
    {
        openProject (last);
        return;
    }

    newProject (getTemplatesDir().getChildFile (settings.getValue ("defaultTemplate", "Basic") + kProjectExtension));
}

void MainComponent::openWelcomeSong()
{
    File song;
    if (const auto r = welcome::write (getProjectsDir(), song); r.failed())
    {
        setStatus ("Couldn't open the welcome song: " + r.getErrorMessage());
        return;
    }
    openProject (song);
    setStatus ("This song was made in Lathe with its built-in instruments. Press Space to play it, "
               "then change anything: it's yours.");
}

Array<File> MainComponent::templateFiles()
{
    auto files = getTemplatesDir().findChildFiles (File::findFiles, false, "*" + String (kProjectExtension));
    files.sort();
    return files;
}

void MainComponent::deleteTemplate (const File& f)
{
    const auto name = f.getFileNameWithoutExtension();
    Component::SafePointer<MainComponent> safe (this);
    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::NoIcon)
                                .withTitle ("Delete the template \"" + name + "\"?")
                                .withMessage (String ("It goes to the ")
                                             #if JUCE_WINDOWS
                                              + "Recycle Bin"
                                             #else
                                              + "Trash"
                                             #endif
                                              + ". Projects you made from it aren't affected.")
                                .withButton ("Delete")
                                .withButton ("Cancel"),
                            [safe, f, name] (int button)
    {
        if (safe == nullptr || button != 1)
            return;
        auto& props = safe->settings;
        if (! f.moveToTrash() && ! f.deleteFile())
        {
            safe->setStatus ("Couldn't delete " + f.getFullPathName());
            return;
        }
        // Keep a deleted built-in from coming back next time Lathe starts.
        if (name == "Basic" || name == "8-Input Tracking")
        {
            auto hidden = StringArray::fromLines (props.getValue ("hiddenTemplates"));
            hidden.addIfNotAlreadyThere (name);
            hidden.removeEmptyStrings();
            props.setValue ("hiddenTemplates", hidden.joinIntoString ("\n"));
        }
        // New Project used it: fall back to another template (or a blank project).
        if (props.getValue ("defaultTemplate", "Basic") == name)
        {
            const auto rest = safe->templateFiles();
            props.setValue ("defaultTemplate", rest.isEmpty() ? String() : rest[0].getFileNameWithoutExtension());
        }
        safe->menuItemsChanged();
        safe->setStatus ("Deleted the template \"" + name + "\".");
    });
}

void MainComponent::ensureBuiltInTemplates()
{
    getTemplatesDir().createDirectory();

    // Built-in templates you deleted stay deleted (File > New from Template > Restore brings them back).
    const auto hidden = StringArray::fromLines (settings.getValue ("hiddenTemplates"));
    auto writeTemplate = [hidden] (const String& name, std::function<void (Edit&)> build)
    {
        auto f = getTemplatesDir().getChildFile (name + kProjectExtension);
        if (f.existsAsFile() || hidden.contains (name))
            return;
        Edit e;
        build (e);
        f.replaceWithText (JSON::toString (e.toVar (true)));
    };

    writeTemplate ("Basic", [] (Edit& e)
    {
        auto a1 = e.addTrack (TrackKind::audio, "Audio 1");
        a1->input = { 0, false };
        auto a2 = e.addTrack (TrackKind::audio, "Audio 2");
        a2->input = { 1, false };
        e.addTrack (TrackKind::midi, "MIDI 1");
        e.addTrack (TrackKind::bus, "A Reverb");
        e.addTrack (TrackKind::bus, "B Delay");
    });

    // Eight inputs on their own tracks, monitored through the interface's own
    // direct monitoring (zero latency) rather than through Lathe.
    writeTemplate ("8-Input Tracking", [] (Edit& e)
    {
        for (int i = 0; i < 8; ++i)
        {
            auto t = e.addTrack (TrackKind::audio, "Input " + String (i + 1));
            t->input = { i, false };
            t->monitor = (int) Monitor::off;
        }
        e.addTrack (TrackKind::midi, "MIDI 1");
        e.addTrack (TrackKind::bus, "A Reverb");
        e.addTrack (TrackKind::bus, "B Delay");
    });
}

Edit::PluginFactory MainComponent::makePluginFactory (StringArray& missing)
{
    return [this, &missing] (const PluginDescription& desc, const MemoryBlock& state, bool separate)
    {
        String error;
        auto slot = engine.makeSlot (desc, state, error, separate);
        if (slot->instance == nullptr)
            missing.addIfNotAlreadyThere (desc.name);
        return slot;
    };
}

void MainComponent::newProject (const File& templateFile)
{
    if (engine.isPlaying())
        engine.stop();

    edit.reset();
    StringArray missing;

    if (templateFile.existsAsFile())
    {
        const auto data = JSON::parse (templateFile);
        const auto result = edit.loadFrom (data, File(), makePluginFactory (missing));
        if (result.failed())
            setStatus ("Couldn't read template " + templateFile.getFileName() + ": " + result.getErrorMessage());
        edit.projectFile = File();
        edit.workDir = File();
        for (auto& t : edit.tracks)
            t->clips.clear();
    }
    else
    {
        edit.addTrack (TrackKind::audio)->input = { 0, false };
        edit.addTrack (TrackKind::midi);
    }

    // New projects record at the chosen rate (Settings; 48 kHz unless changed). An open
    // project always runs at its own rate, so this only applies here.
    const double newRate = settings.getDoubleValue ("newProjectRate", 48000.0);
    String rateNote;
    if (newRate > 0 && ! engine.setDeviceRate (newRate) && engine.getSampleRate() > 0)
        rateNote = " (your interface can't run at " + String (newRate / 1000.0, 1) + " kHz, so it uses "
                   + String (engine.getSampleRate() / 1000.0, 1) + " kHz)";

    edit.sampleRate = engine.getSampleRate() > 0 ? engine.getSampleRate() : 48000.0;
    ctx.selection = {};
    if (! edit.tracks.empty())
        ctx.selection.trackId = edit.tracks.front()->id;
    showDetail (false);

    engine.rebuild();
    edit.needsRebuild = false;
    edit.dirty = false;
    edit.clearUndo();   // setting the project up isn't something to undo
    if (compare != nullptr)
        compare->projectChanged();   // Before and Now were the old song's
    refreshAll();

    if (cloud != nullptr)
        cloud->projectChanged();

    setStatus ((missing.isEmpty() ? (templateFile.existsAsFile() ? "New project from template \"" + templateFile.getFileNameWithoutExtension() + "\"" : String ("New project"))
                                  : "Missing plug-ins: " + missing.joinIntoString (", ")) + rateNote);
}

void MainComponent::openProject (const File& file)
{
    if (! file.existsAsFile())
    {
        setStatus ("Can't find " + file.getFullPathName());
        return;
    }

    const auto autosave = autosaveFileFor (file);
    if (autosave.existsAsFile() && autosave.getLastModificationTime() > file.getLastModificationTime())
    {
        auto options = MessageBoxOptions()
                           .withIconType (MessageBoxIconType::QuestionIcon)
                           .withTitle ("Recover unsaved changes?")
                           .withMessage ("Lathe found autosaved changes to \"" + file.getFileNameWithoutExtension()
                                         + "\" that are newer than the saved project (it may have closed unexpectedly).")
                           .withButton ("Recover")
                           .withButton ("Open saved version");
        AlertWindow::showAsync (options, [this, file, autosave] (int button)
        {
            loadProjectFile (file, button == 1 ? autosave : file);
            if (button == 1)
                edit.dirty = true;
            else
                autosave.deleteFile();
            updateTitle();
        });
        return;
    }

    loadProjectFile (file, file);
}

void MainComponent::loadProjectFile (const File& file, const File& readFrom)
{
    const auto data = JSON::parse (readFrom);
    if (! data.isObject())
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Can't open project",
                                          readFrom.getFileName() + " isn't a readable Lathe project.");
        return;
    }

    if (engine.isPlaying())
        engine.stop();

    // Match the device to the project's sample rate, if the interface can do it.
    const double projectRate = data.hasProperty ("sampleRate") ? (double) data["sampleRate"] : 0.0;
    if (projectRate > 0)
        engine.setDeviceRate (projectRate);

    edit.reset();
    edit.projectFile = file;
    edit.workDir = file.getParentDirectory();

    StringArray missing;
    const auto result = edit.loadFrom (data, edit.workDir, makePluginFactory (missing));
    if (result.failed())
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Can't open project", result.getErrorMessage());
        newProject ({});
        return;
    }

    ctx.selection = {};
    if (! edit.tracks.empty())
        ctx.selection.trackId = edit.tracks.front()->id;
    showDetail (false);
    engine.seekTo (edit.insertMarker);

    engine.rebuild();
    edit.needsRebuild = false;
    edit.dirty = false;
    edit.clearUndo();   // setting the project up isn't something to undo
    if (compare != nullptr)
        compare->projectChanged();   // Before and Now were the old song's
    refreshAll();

    settings.setValue ("lastProject", file.getFullPathName());
    recentFiles.addFile (file);
    settings.setValue ("recentProjects", recentFiles.toString());
    menuItemsChanged();   // the Mac's menu bar rebuilds only when told
    if (cloud != nullptr)
        cloud->projectChanged();

    int missingAudio = 0;
    for (auto& t : edit.tracks)
        for (auto& c : t->clips)
            if (c.kind == ClipKind::audio && ! c.file.existsAsFile())
                ++missingAudio;

    StringArray problems;
    if (! missing.isEmpty())
        problems.add ("missing plug-ins: " + missing.joinIntoString (", "));
    if (missingAudio > 0)
        problems.add (String (missingAudio) + " audio file(s) not found");
    if (engine.getSampleRate() > 0 && std::abs (engine.getSampleRate() - edit.sampleRate) > 0.5)
        problems.add ("project is " + String (edit.sampleRate / 1000.0, 1) + " kHz but the device runs at "
                      + String (engine.getSampleRate() / 1000.0, 1) + " kHz");

    setStatus (problems.isEmpty() ? "Opened " + file.getFileNameWithoutExtension()
                                  : "Opened with problems: " + problems.joinIntoString ("; "));
}

void MainComponent::save (std::function<void (bool)> done)
{
    if (edit.projectFile == File())
    {
        saveAs (done);
        return;
    }

    const auto file = edit.projectFile;
    const auto text = JSON::toString (edit.toVar (false));

    // Keep the previous version as a .bak, then replace atomically.
    if (file.existsAsFile())
        file.copyFileTo (file.getSiblingFile (file.getFileName() + ".bak"));

    TemporaryFile temp (file);
    const bool ok = temp.getFile().replaceWithText (text) && temp.overwriteTargetFileWithTemporary();

    if (ok)
    {
        edit.dirty = false;
        autosaveFileFor (file).deleteFile();
        settings.setValue ("lastProject", file.getFullPathName());
        recentFiles.addFile (file);
        settings.setValue ("recentProjects", recentFiles.toString());
        menuItemsChanged();
        setStatus ("Saved " + file.getFullPathName());
        if (cloud != nullptr)
            cloud->projectChanged (false);
    }
    else
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save failed",
                                          "Couldn't write " + file.getFullPathName());
    }

    updateTitle();
    if (done)
        done (ok);
}

void MainComponent::saveAs (std::function<void (bool)> done)
{
    getProjectsDir().createDirectory();
    chooser = std::make_unique<FileChooser> ("Save project as", getProjectsDir().getChildFile (edit.getName()), "*.lathe");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles,
                          [this, done] (const FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == File())
        {
            if (done) done (false);
            return;
        }
        saveTo (chosen, done);
    });
}

void MainComponent::saveTo (const File& chosen, std::function<void (bool)> done)
{
    // Every project gets its own folder: Name/Name.lathe, Name/Audio, Name/Exports.
    const auto name = chosen.getFileNameWithoutExtension();
    auto folder = chosen.getParentDirectory();
    if (folder.getFileName() != name)
        folder = folder.getChildFile (name);
    folder.createDirectory();
    const auto newFile = folder.getChildFile (name + kProjectExtension);
    const auto audioDir = folder.getChildFile ("Audio");

    // Bring all audio into the new folder so the project is self-contained.
    std::map<String, File> copied;
    int failures = 0;
    for (auto& t : edit.tracks)
    {
        for (auto& c : t->clips)
        {
            if (c.kind != ClipKind::audio || ! c.file.existsAsFile() || c.file.isAChildOf (folder))
                continue;

            const auto key = c.file.getFullPathName();
            if (copied.find (key) == copied.end())
            {
                audioDir.createDirectory();
                auto dest = audioDir.getChildFile (c.file.getFileName());
                if (dest.existsAsFile())
                    dest = audioDir.getNonexistentChildFile (c.file.getFileNameWithoutExtension(), c.file.getFileExtension(), false);
                if (c.file.copyFileTo (dest))
                    copied[key] = dest;
                else
                {
                    ++failures;
                    copied[key] = c.file;
                }
            }
            c.file = copied[key];
        }
    }

    edit.projectFile = newFile;
    edit.workDir = folder;
    if (failures > 0)
        setStatus (String (failures) + " audio file(s) couldn't be copied into the project folder.");

    engine.rebuild();
    save (done);
}

void MainComponent::connectAssistantToProjects()
{
    // File > New, Open and Save for the AI assistant's tools: the same as the menu, without
    // dialogs (the tools have already settled unsaved changes, and say what went wrong).
    auto& hooks = aiAssistant->projectHooks();
    hooks.newProject = [this] (const File& templateFile) -> String
    {
        newProject (templateFile);
        updateTitle();
        return {};
    };
    hooks.openProject = [this] (const File& file) -> String
    {
        const auto autosave = autosaveFileFor (file);
        if (autosave.existsAsFile() && autosave.getLastModificationTime() > file.getLastModificationTime())
            return "Lathe has autosaved changes to it that are newer than the saved project (it may have closed unexpectedly). "
                   "Open it with File > Open, which asks whether to recover them.";
        if (! JSON::parse (file).isObject())
            return file.getFileName() + " isn't a readable Lathe project.";
        loadProjectFile (file, file);
        updateTitle();
        return edit.projectFile == file ? String() : "Lathe couldn't read it.";
    };
    hooks.save = [this] (const File& saveAs) -> String
    {
        bool ok = false;
        if (saveAs == File())
        {
            if (edit.projectFile == File())
                return "it has never been saved, so it needs a name.";
            save ([&ok] (bool r) { ok = r; });
        }
        else
        {
            saveTo (saveAs, [&ok] (bool r) { ok = r; });
        }
        return ok ? String() : "Lathe couldn't write " + (saveAs == File() ? edit.projectFile : saveAs).getFullPathName() + ".";
    };
}

void MainComponent::saveAsTemplate()
{
    askForText ("Save as template", "Name for the template (tracks, routing and plug-ins are saved; clips are not):",
                edit.getName(), [this] (const String& name)
    {
        getTemplatesDir().createDirectory();
        const auto f = getTemplatesDir().getChildFile (File::createLegalFileName (name) + kProjectExtension);
        if (f.replaceWithText (JSON::toString (edit.toVar (true))))
        {
            menuItemsChanged();
            setStatus ("Saved template \"" + name + "\". Choose it in File > New from Template, or make it the default in Settings.");
        }
        else
            setStatus ("Couldn't write " + f.getFullPathName());
    });
}

void MainComponent::confirmDiscard (std::function<void()> proceed)
{
    if (! edit.dirty)
    {
        proceed();
        return;
    }

    AlertWindow::showYesNoCancelBox (MessageBoxIconType::QuestionIcon, "Save changes?",
                                     "Save the changes to \"" + edit.getName() + "\" first?",
                                     "Save", "Don't save", "Cancel", nullptr,
                                     ModalCallbackFunction::create ([this, proceed] (int result)
    {
        if (result == 1)
            save ([proceed] (bool ok) { if (ok) proceed(); });
        else if (result == 2)
            proceed();
    }));
}

void MainComponent::writeAutosave()
{
    if (! edit.dirty || ! edit.projectFile.existsAsFile() || engine.isRecording())
        return;
    autosaveFileFor (edit.projectFile).replaceWithText (JSON::toString (edit.toVar (false)));
}

void MainComponent::reloadMissingPlugins()
{
    int reloaded = 0;
    std::vector<std::shared_ptr<Track>> master { edit.master };
    for (auto* list : { &edit.tracks, &edit.buses, &master })
    {
        for (auto& t : *list)
        {
            if (t == nullptr)
                continue;
            for (auto& slot : t->plugins)
            {
                if (slot->instance != nullptr)
                    continue;
                String error;
                auto fresh = engine.makeSlot (slot->description, slot->savedState, error, slot->separate);
                if (fresh->instance != nullptr)
                {
                    fresh->bypassed = slot->bypassed.load();
                    slot = fresh;
                    ++reloaded;
                }
            }
        }
    }
    if (reloaded > 0)
    {
        edit.changed();
        setStatus ("Loaded " + String (reloaded) + " plug-in(s) that were missing before the scan.");
    }
}

void MainComponent::openPluginBrowser()
{
    if (browserWindow == nullptr)
        browserWindow = std::make_unique<PluginBrowserWindow> (ctx);
    browserWindow->show();
}

void MainComponent::importAudio()
{
    chooser = std::make_unique<FileChooser> ("Import audio", File::getSpecialLocation (File::userMusicDirectory),
                                             "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                          [this] (const FileChooser& fc)
    {
        int trackIndex = -1;
        for (size_t i = 0; i < edit.tracks.size(); ++i)
            if (edit.tracks[i]->id == ctx.selection.trackId)
                trackIndex = (int) i;
        arrangement.importFiles (fc.getResults(), trackIndex, edit.insertMarker);
    });
}

//==============================================================================
void MainComponent::changeListenerCallback (ChangeBroadcaster* source)
{
    if (notes != nullptr && source == notes.get())
    {
        arrangement.repaint();   // note markers
        return;
    }
    if (edit.needsRebuild)
    {
        edit.needsRebuild = false;
        engine.rebuild();
    }
    refreshAll();
}

void MainComponent::refreshAll()
{
    transport.refresh();
    arrangement.refresh();
    mixer.refresh();
    devices.setTrack (edit.findTrack (ctx.selection.trackId));
    devices.refresh();
    if (pianoRollShown)
    {
        if (pianoRoll.hasClip())
            pianoRoll.refresh();
        else
            showDetail (false);
    }
    updateTitle();
}

void MainComponent::updateTitle()
{
    if (auto* w = findParentComponentOfClass<DocumentWindow>())
        w->setName (edit.getName() + (edit.dirty ? "*" : "") + "  -  Lathe");
}

void MainComponent::setStatus (const String& message)
{
    statusLeft.setText (message, dontSendNotification);
    statusTime = Time::getMillisecondCounter();
}

void MainComponent::timerCallback()
{
    transport.refresh();
    if (mixerShown) mixer.tick();
    else            arrangement.tick();
    if (pianoRollShown)
        pianoRoll.tick();

    if (++ticks % 30 == 0)
    {
        if (milestones::choice (settings) == milestones::Choice::yes)
        {
            if (engine.isPlaying())
                milestones::reach (settings, "played");
            if (auto* d = engine.deviceManager.getCurrentAudioDevice(); d != nullptr && d->getActiveInputChannels().countNumberOfSetBits() > 0)
                milestones::reach (settings, "interface");
        }
        showAssistantBar();   // goes away a little after the assistant's last step
        if (statusTime != 0 && Time::getMillisecondCounter() - statusTime > 12000)
        {
            statusLeft.setText ({}, dontSendNotification);
            statusTime = 0;
        }
        updateTitle();

        if (++lastAutosave >= 60)
        {
            lastAutosave = 0;
            writeAutosave();
        }
    }
}

void MainComponent::setMixerShown (bool m)
{
    mixerShown = m;
    transport.setMixerShown (m);
    arrangement.setVisible (! m);
    mixer.setVisible (m);
    if (m)
        mixer.refresh();
    else
        arrangement.refresh();
}

void MainComponent::showDetail (bool roll)
{
    pianoRollShown = roll;
    detailShown = true;
    pianoRoll.setVisible (roll);
    devices.setVisible (! roll);
    if (roll && detailHeight < 240)
        detailHeight = 280;
    resized();
    if (roll)
        pianoRoll.grabKeyboardFocus();
}

//==============================================================================
void MainComponent::paint (Graphics& g)
{
    g.fillAll (theme::window);
    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromBottom (theme::statusHeight));
}

void MainComponent::showPushBanner()
{
    const bool limited = ctx.isPushLimited();
    if (limited)
    {
        auto name = cloud != nullptr && cloud->getStore() != nullptr ? cloud->getStore()->getCloudName() : String();
        if (name.isEmpty())
            name = edit.projectFile.getFileNameWithoutExtension();
        pushBanner.setText ((name.isNotEmpty() ? "In \"" + name + "\" you push " : String ("In this song you push "))
                                + ctx.pushTrackNames + ". Everything else you change stays on this computer.",
                            dontSendNotification);
    }
    pushBanner.setVisible (limited);
    resized();
    arrangement.refresh();
    mixer.refresh();
    // Every header, lane and strip draws the difference.
    std::function<void (Component&)> repaintAll = [&] (Component& c)
    {
        c.repaint();
        for (auto* child : c.getChildren())
            repaintAll (*child);
    };
    repaintAll (arrangement);
    repaintAll (mixer);
}

MainComponent::SetAsideBar::SetAsideBar()
{
    text.setFont (theme::font (13.0f));
    text.setColour (Label::textColourId, theme::textDark);
    text.setBorderSize ({ 0, 12, 0, 12 });
    addAndMakeVisible (text);
    putBack.setColour (TextButton::buttonColourId, theme::textDark);
    putBack.setColour (TextButton::textColourOffId, theme::accent);
    addAndMakeVisible (putBack);
    addAndMakeVisible (forget);
}

void MainComponent::SetAsideBar::resized()
{
    auto r = getLocalBounds().reduced (6, 3);
    forget.setBounds (r.removeFromRight (110));
    r.removeFromRight (6);
    putBack.setBounds (r.removeFromRight (170));
    text.setBounds (r);
}

void MainComponent::SetAsideBar::paint (Graphics& g)
{
    g.fillAll (theme::accent);
}

void MainComponent::showSetAsideBar()
{
    const bool aside = cloud != nullptr && cloud->hasSetAside();
    if (aside)
    {
        String on;
        if (auto* store = cloud->getStore())
            if (auto rev = store->getRevision (store->getBase()))
                on = rev->label();
        setAsideBar.text.setText ("Your changes are set aside" + (on.isNotEmpty() ? " while you listen to " + on : String())
                                      + ". Put them back on top when you're ready, then push.",
                                  dontSendNotification);
    }
    if (aside != setAsideBar.isVisible())
    {
        setAsideBar.setVisible (aside);
        resized();
    }
}

MainComponent::AssistantBar::AssistantBar()
{
    text.setFont (theme::font (13.0f));
    text.setColour (Label::textColourId, theme::text);
    text.setBorderSize ({ 0, 12, 0, 12 });
    addAndMakeVisible (text);
    addAndMakeVisible (pause);
}

void MainComponent::AssistantBar::resized()
{
    auto r = getLocalBounds().reduced (6, 3);
    pause.setBounds (r.removeFromRight (110));
    text.setBounds (r);
}

void MainComponent::AssistantBar::paint (Graphics& g)
{
    g.fillAll (paused ? theme::accent.interpolatedWith (theme::panel, 0.7f) : theme::panelRaised);
    g.setColour (theme::accent);
    g.fillRect (0, 0, 3, getHeight());   // the assistant's mark, at the left edge
}

void MainComponent::showAssistantBar()
{
    if (aiAssistant == nullptr)
        return;
    const bool paused = aiAssistant->isPaused();
    const bool show = paused || (aiAssistant->isOn() && aiAssistant->isWorking());
    if (show)
    {
        const auto last = aiAssistant->lastAction();
        assistantBar.paused = paused;
        assistantBar.text.setText (paused ? String ("Your assistant is paused: it stops at its next step. Work as you like, "
                                                    "then press Continue and tell it to carry on.")
                                          : "Your assistant is working" + (last.isNotEmpty() ? ": " + last : String())
                                                + ". You can keep working: it's told what you change before it changes anything.",
                                   dontSendNotification);
        assistantBar.pause.setButtonText (paused ? "Continue" : "Pause");
        assistantBar.pause.setTooltip (paused ? "Let the assistant carry on when you tell it to"
                                              : "Stop the assistant at its next step, until you press Continue");
        assistantBar.repaint();
    }
    if (show != assistantBar.isVisible())
    {
        assistantBar.setVisible (show);
        resized();
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transport.setBounds (r.removeFromTop (theme::transportHeight));
    if (pushBanner.isVisible())
        pushBanner.setBounds (r.removeFromTop (28));
    if (setAsideBar.isVisible())
        setAsideBar.setBounds (r.removeFromTop (34));
    if (assistantBar.isVisible())
        assistantBar.setBounds (r.removeFromTop (34));

    auto status = r.removeFromBottom (theme::statusHeight).reduced (10, 0);
    if (cloud != nullptr)
        cloud->getStatusComponent().setBounds (status.removeFromRight (300));
    if (updateNotice != nullptr && updateNotice->isVisible())
        updateNotice->setBounds (status.removeFromRight (230));
    statusRight.setBounds (status.removeFromRight (360));
    statusLeft.setBounds (status);

    if (detailShown)
    {
        auto detail = r.removeFromBottom (jmin (detailHeight, jmax (90, r.getHeight() - 200)));
        devices.setBounds (detail);
        pianoRoll.setBounds (detail);
        divider->setBounds (r.removeFromBottom (5));
    }
    else
    {
        devices.setBounds ({});
        pianoRoll.setBounds ({});
        divider->setBounds (r.removeFromBottom (5));
    }

    arrangement.setBounds (r);
    mixer.setBounds (r);
}

//==============================================================================
StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "View", "Options", "Cloud" };
}

PopupMenu MainComponent::getMenuForIndex (int index, const String&)
{
    PopupMenu m;
   #if JUCE_MAC
    const String cmd = "Cmd+";
   #else
    const String cmd = "Ctrl+";
   #endif

    auto item = [&] (int id, const String& text, const String& shortcut = {}, bool enabled = true, bool ticked = false)
    {
        PopupMenu::Item i (text);
        i.itemID = id;
        i.isEnabled = enabled;
        i.isTicked = ticked;
        i.shortcutKeyDescription = shortcut;
        m.addItem (i);
    };

    if (index == 0)
    {
        item (menu::newProject, "New Project", cmd + "N");

        PopupMenu templates, deleteTemplates;
        int i = 0;
        templateMenuFiles = templateFiles();
        for (auto& f : templateMenuFiles)
        {
            templates.addItem (menu::templateBase + i, f.getFileNameWithoutExtension());
            deleteTemplates.addItem (menu::deleteTemplateBase + i, f.getFileNameWithoutExtension());
            ++i;
        }
        if (i > 0)
            templates.addSeparator();
        templates.addSubMenu ("Delete Template", deleteTemplates, i > 0);
        templates.addItem (menu::restoreTemplates, "Restore Built-in Templates", settings.getValue ("hiddenTemplates").isNotEmpty());
        m.addSubMenu ("New from Template", templates);

        item (menu::open, "Open...", cmd + "O");
        PopupMenu recent;
        recentMenuFiles.clear();
        for (int r = 0; r < recentFiles.getNumFiles(); ++r)
        {
            const auto f = recentFiles.getFile (r);
            if (! f.existsAsFile())
                continue;
            recent.addItem (menu::recentBase + recentMenuFiles.size(), f.getFullPathName());
            recentMenuFiles.add (f);
        }
        m.addSubMenu ("Open Recent", recent, recent.getNumItems() > 0);
        item (menu::openWelcome, "Open the Welcome Song");
        m.addSeparator();
        item (menu::save, "Save", cmd + "S");
        item (menu::saveAs, "Save As...", cmd + "Shift+S");
        item (menu::saveTemplate, "Save as Template...");
        m.addSeparator();
        item (menu::importAudio, "Import Audio...", cmd + "I");
        item (menu::exportAudio, "Export Mix / Stems...", cmd + "Shift+E");
        item (menu::exportTracks, ctx.selection.trackIds.size() > 1 ? "Export Selected Tracks..." : "Export Selected Track...", {},
              ! ctx.selection.trackIds.empty() || ctx.selection.trackId >= 0);
        m.addSeparator();
        item (menu::showFolder, "Show Project Folder", {}, edit.workDir.isDirectory());
       #if ! JUCE_MAC
        m.addSeparator();
        item (menu::quit, "Quit", cmd + "Q");
       #endif
    }
    else if (index == 1)
    {
        item (menu::undo, "Undo", cmd + "Z", edit.canUndo() && ! engine.isRecording());
        item (menu::redo, "Redo", cmd + "Shift+Z", edit.canRedo() && ! engine.isRecording());
        item (menu::undoHistory, "Undo History...", cmd + "Alt+Z");
        m.addSeparator();
        const int numClips = arrangement.numSelectedClips();
        const bool hasClip = numClips > 0;
        const String clips = numClips > 1 ? "Clips" : "Clip";
        item (menu::cutClips, "Cut " + clips, cmd + "X", hasClip);
        item (menu::copyClips, "Copy " + clips, cmd + "C", hasClip);
        item (menu::pasteClips, "Paste at Start Marker", cmd + "V", arrangement.canPaste());
        item (menu::duplicate, "Duplicate " + clips, cmd + "D", hasClip);
        m.addSeparator();
        item (menu::joinClips, "Join Clips", cmd + "J", numClips > 1);
        item (menu::split, "Split " + clips + " at Start Marker", cmd + "E", hasClip);
        item (menu::deleteClip, "Delete " + clips, "Delete", hasClip);
        m.addSeparator();
        item (menu::selectAllClips, "Select All Clips", cmd + "A", true);
    }
    else if (index == 2)
    {
        item (menu::addAudio, "Add Audio Track", cmd + "T");
        item (menu::addMidi, "Add MIDI Track", cmd + "Shift+T");
        item (menu::addReturn, "Add Return Track", cmd + "Alt+T");
        m.addSeparator();
        auto t = edit.findTrack (ctx.selection.trackId);
        m.addSeparator();
        item (menu::groupTracks, ctx.selection.trackIds.size() > 1 ? "Group Selected Tracks" : "Group Track",
              cmd + "G", t != nullptr && t->holdsClips());
        item (menu::ungroupTracks, "Ungroup", cmd + "Shift+G", t != nullptr && t->groupId >= 0);
        m.addSeparator();
        item (menu::deleteTrack, "Delete Selected Track", {}, t != nullptr && ! t->isMaster());
        m.addSeparator();
        item (menu::addNote, "Add Note at Start Marker...", cmd + "Alt+N", t != nullptr && t->holdsClips());
        item (menu::addSongNote, "Add Song Note...", {});
    }
    else if (index == 3)
    {
        item (menu::toggleView, mixerShown ? "Show Arrangement" : "Show Mixer", "Tab");
        item (menu::toggleDetail, "Show Bottom Panel", {}, true, detailShown);
        m.addSeparator();
        item (menu::compareMixes, "Compare...", cmd + "K");
        item (menu::showNotes, "Notes...", cmd + "Shift+N");
        PopupMenu showOn;
        const auto show = notes->getShow();
        showOn.addItem (menu::notesAll, "Every Note", true, show == NotesController::Show::all);
        showOn.addItem (menu::notesUnresolved, "Unresolved Notes Only", true, show == NotesController::Show::unresolved);
        showOn.addItem (menu::notesNone, "No Notes", true, show == NotesController::Show::none);
        m.addSubMenu ("Show on Tracks", showOn);
        m.addSeparator();
        item (menu::zoomIn, "Zoom In", "+");
        item (menu::zoomOut, "Zoom Out", "-");
    }
    else if (index == 4)
    {
        item (menu::browser, "Plug-in Browser...", cmd + "B");
        item (menu::settings, "Audio / MIDI Settings...", cmd + ",");
        item (menu::scan, host.isScanning() ? "Scanning Plug-ins..." : "Scan for New Plug-ins", {}, ! host.isScanning());
        item (menu::metronome, "Metronome", "C", true, engine.metronomeOn.load());
        m.addSeparator();
        item (menu::assistant, "AI Assistant...", {}, true, aiAssistant != nullptr && aiAssistant->isOn());
        m.addSeparator();
        item (menu::checkUpdates, "Check for Updates...", {});
        item (menu::autoUpdates, "Check for Updates Automatically", {}, true,
              updateNotice != nullptr && updateNotice->checksAutomatically());
    }
    else if (index == 5 && cloud != nullptr)
    {
        cloud->addMenuItems (m);
    }
    return m;
}

void MainComponent::menuItemSelected (int id, int)
{
    if (cloud != nullptr && id >= CloudController::menuBase && id < CloudController::menuEnd)
    {
        cloud->handleMenuItem (id);
        return;
    }

    if (id >= menu::recentBase)
    {
        const int index = id - menu::recentBase;
        if (! isPositiveAndBelow (index, recentMenuFiles.size()))
            return;
        const auto f = recentMenuFiles[index];
        confirmDiscard ([this, f] { openProject (f); });
        return;
    }

    if (id == menu::restoreTemplates)
    {
        settings.removeValue ("hiddenTemplates");
        ensureBuiltInTemplates();
        menuItemsChanged();
        setStatus ("Restored the built-in templates.");
        return;
    }

    if (id >= menu::deleteTemplateBase)
    {
        const int index = id - menu::deleteTemplateBase;
        if (isPositiveAndBelow (index, templateMenuFiles.size()))
            deleteTemplate (templateMenuFiles[index]);
        return;
    }

    if (id >= menu::templateBase)
    {
        const int index = id - menu::templateBase;
        if (isPositiveAndBelow (index, templateMenuFiles.size()))
        {
            const auto f = templateMenuFiles[index];
            confirmDiscard ([this, f] { newProject (f); });
        }
        return;
    }

    switch (id)
    {
        case menu::newProject:
            confirmDiscard ([this]
            {
                newProject (getTemplatesDir().getChildFile (settings.getValue ("defaultTemplate", "Basic") + kProjectExtension));
            });
            break;
        case menu::openWelcome:
            confirmDiscard ([this] { openWelcomeSong(); });
            break;
        case menu::open:
            confirmDiscard ([this]
            {
                chooser = std::make_unique<FileChooser> ("Open project", getProjectsDir(), "*.lathe");
                chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                      [this] (const FileChooser& fc)
                {
                    if (fc.getResult().existsAsFile())
                        openProject (fc.getResult());
                });
            });
            break;
        case menu::save:          save(); break;
        case menu::saveAs:        saveAs(); break;
        case menu::saveTemplate:  saveAsTemplate(); break;
        case menu::importAudio:   importAudio(); break;
        case menu::exportAudio:   showExportDialog (ctx); break;
        case menu::exportTracks:
        {
            auto ids = ctx.selection.trackIds;
            if (ids.empty() && ctx.selection.trackId >= 0)
                ids.insert (ctx.selection.trackId);
            showExportDialog (ctx, ids);
            break;
        }
        case menu::showFolder:    edit.workDir.revealToUser(); break;
        case menu::quit:          JUCEApplication::getInstance()->systemRequestedQuit(); break;

        case menu::undo:
            if (engine.isRecording()) setStatus ("Stop recording first, then undo.");
            else if (! edit.undo())   setStatus ("Nothing to undo.");
            else                      setStatus ("Undid: " + edit.getUndoneSteps().front().label);
            break;
        case menu::redo:
            if (engine.isRecording())  setStatus ("Stop recording first, then redo.");
            else if (edit.redo())      setStatus ("Redid: " + edit.getDoneSteps().back().label);
            break;
        case menu::compareMixes:
            if (compare != nullptr)
                CompareWindow::show (*compare, [this] (const String& st) { setStatus (st); });
            break;
        case menu::undoHistory:
            UndoHistoryWindow::show (edit,
                                     [this]
                                     {
                                         if (! engine.isRecording())
                                             return true;
                                         setStatus ("Stop recording first, then undo.");
                                         return false;
                                     },
                                     [this] (const String& st) { setStatus (st); });
            break;
        case menu::deleteClip:    arrangement.deleteSelectedClip(); break;
        case menu::duplicate:     arrangement.duplicateSelectedClip(); break;
        case menu::split:         arrangement.splitSelectedClip(); break;
        case menu::cutClips:      arrangement.cutSelectedClips(); break;
        case menu::copyClips:     arrangement.copySelectedClips(); break;
        case menu::pasteClips:    arrangement.pasteClips(); break;
        case menu::joinClips:     arrangement.joinSelectedClips(); break;
        case menu::selectAllClips: arrangement.selectAllClips(); break;

        case menu::addAudio:      ctx.selectTrack (edit.addTrack (TrackKind::audio)->id); break;
        case menu::addMidi:       ctx.selectTrack (edit.addTrack (TrackKind::midi)->id); break;
        case menu::addReturn:     ctx.selectTrack (edit.addTrack (TrackKind::bus)->id); break;
        case menu::groupTracks:   groupSelectedTracks (ctx, ctx.selection.trackId); break;
        case menu::ungroupTracks:
            if (auto t = edit.findTrack (ctx.selection.trackId); t != nullptr && t->groupId >= 0)
                edit.ungroup (t->groupId);
            break;
        case menu::deleteTrack:
            if (auto t = edit.findTrack (ctx.selection.trackId); t != nullptr && ! t->isMaster())
                showTrackMenu (ctx, *this, t);
            break;

        case menu::toggleView:    setMixerShown (! mixerShown); break;
        case menu::toggleDetail:  detailShown = ! detailShown; resized(); break;
        case menu::showNotes:     notes->showWindow(); break;
        case menu::notesAll:        notes->setShow (NotesController::Show::all); break;
        case menu::notesUnresolved: notes->setShow (NotesController::Show::unresolved); break;
        case menu::notesNone:       notes->setShow (NotesController::Show::none); break;
        case menu::addNote:
            if (auto t = edit.findTrack (ctx.selection.trackId); t != nullptr && t->holdsClips())
            {
                if (mixerShown)
                    setMixerShown (false);
                arrangement.reveal (t->id, edit.insertMarker);
                notes->showCompose (t->id, edit.insertMarker, arrangement.screenAreaFor (t->id, edit.insertMarker));
            }
            break;
        case menu::addSongNote:
            notes->showCompose (-1, 0.0, transport.getScreenBounds().withSizeKeepingCentre (10, 10));
            break;
        case menu::zoomIn:        arrangement.zoom (1.25, 0.0f); break;
        case menu::zoomOut:       arrangement.zoom (0.8, 0.0f); break;

        case menu::settings:      showSettingsDialog (ctx); break;
        case menu::assistant:     showAssistantDialog (ctx, aiAssistant); break;
        case menu::scan:          host.startScan (false); break;
        case menu::browser:       openPluginBrowser(); break;
        case menu::checkUpdates:  openPatcher(); break;
        case menu::autoUpdates:
            if (updateNotice != nullptr)
                updateNotice->setChecksAutomatically (! updateNotice->checksAutomatically());
            break;
        case menu::metronome:     engine.metronomeOn = ! engine.metronomeOn.load(); break;
        default: break;
    }
}

bool MainComponent::keyPressed (const KeyPress& k)
{
    const auto mods = k.getModifiers();
    const bool cmd = mods.isCommandDown();
    const bool shift = mods.isShiftDown();
    const int code = k.getKeyCode();
    const auto ch = CharacterFunctions::toLowerCase (k.getTextCharacter());

    // Letter key codes arrive upper- or lower-case depending on the platform, and
    // with Ctrl held the text character can be a control code, so check both.
    auto is = [&] (juce_wchar c)
    {
        return ch == c || (code > 0 && code < 256 && CharacterFunctions::toLowerCase ((juce_wchar) code) == c);
    };

    if (code == KeyPress::spaceKey && ! cmd && ! shift && ! mods.isAltDown())
        return globalkeys::handleSpace();   // where there's no app-wide hook (Linux); see GlobalKeys.h
    if (code == KeyPress::homeKey)             { edit.insertMarker = 0.0; engine.seekTo (0.0); edit.sendChangeMessage(); return true; }
    if (code == KeyPress::tabKey && ! cmd)     { setMixerShown (! mixerShown); return true; }

    if (cmd)
    {
        if (is ('z')) { menuItemSelected (k.getModifiers().isAltDown() ? menu::undoHistory : shift ? menu::redo : menu::undo, 0); return true; }
        if (is ('y')) { menuItemSelected (menu::redo, 0); return true; }
        if (is ('s')) { menuItemSelected (shift ? menu::saveAs : menu::save, 0); return true; }
        if (is ('o')) { menuItemSelected (menu::open, 0); return true; }
        if (is ('n'))
        {
            menuItemSelected (shift ? menu::showNotes : k.getModifiers().isAltDown() ? menu::addNote : menu::newProject, 0);
            return true;
        }
        if (is ('i')) { menuItemSelected (menu::importAudio, 0); return true; }
        if (is ('e')) { menuItemSelected (shift ? menu::exportAudio : menu::split, 0); return true; }
        if (is ('d')) { menuItemSelected (menu::duplicate, 0); return true; }
        if (is ('c')) { menuItemSelected (menu::copyClips, 0); return true; }
        if (is ('x')) { menuItemSelected (menu::cutClips, 0); return true; }
        if (is ('v')) { menuItemSelected (menu::pasteClips, 0); return true; }
        if (is ('j')) { menuItemSelected (menu::joinClips, 0); return true; }
        if (is ('a')) { menuItemSelected (menu::selectAllClips, 0); return true; }
        if (is ('t')) { menuItemSelected (mods.isAltDown() ? menu::addReturn : shift ? menu::addMidi : menu::addAudio, 0); return true; }
        if (ch == ',') { menuItemSelected (menu::settings, 0); return true; }
        if (is ('b')) { openPluginBrowser(); return true; }
        if (is ('k')) { menuItemSelected (menu::compareMixes, 0); return true; }
        if (is ('g')) { menuItemSelected (shift ? menu::ungroupTracks : menu::groupTracks, 0); return true; }
        if (is ('q')) { JUCEApplication::getInstance()->systemRequestedQuit(); return true; }
        return false;
    }

    if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey)
        return arrangement.deleteSelectedClip();

    if (is ('r')) { transport.record(); return true; }
    if (is ('c')) { engine.metronomeOn = ! engine.metronomeOn.load(); settings.setValue ("metronome", engine.metronomeOn.load()); return true; }
    if (is ('l')) { edit.loopEnabled = ! edit.loopEnabled; edit.changed(); return true; }
    if (ch == '+' || ch == '=') { arrangement.zoom (1.25, 0.0f); return true; }
    if (ch == '-') { arrangement.zoom (0.8, 0.0f); return true; }

    return false;
}
} // namespace lathe

namespace lathe
{
void MainComponent::openPatcher()
{
    // Lathe Patcher ships with Lathe: inside the app bundle on the Mac, beside the executable elsewhere.
    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
   #if JUCE_MAC
    const auto patcher = exe.getParentDirectory().getSiblingFile ("Helpers").getChildFile ("LathePatcher.app");
   #elif JUCE_WINDOWS
    const auto patcher = exe.getSiblingFile ("LathePatcher.exe");
   #else
    const auto patcher = exe.getSiblingFile ("LathePatcher");
   #endif

    if (patcher.exists() && patcher.startAsProcess ("--from-lathe"))
        return;

    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::NoIcon)
                                .withTitle ("Check for Updates")
                                .withMessage ("Lathe Patcher isn't installed with this copy of Lathe. "
                                              "Download the latest release from github.com/taddeimania/lathe/releases.")
                                .withButton ("Open Releases Page")
                                .withButton ("Cancel"),
                            [] (int result)
                            {
                                if (result == 1)
                                    URL ("https://github.com/taddeimania/lathe/releases").launchInDefaultBrowser();
                            });
}
} // namespace lathe

namespace lathe
{
void MainComponent::confirmQuit (std::function<void()> proceed)
{
    if (cloud == nullptr || ! cloud->isBusy())
    {
        confirmDiscard (std::move (proceed));
        return;
    }

    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::NoIcon)
                                .withTitle ("Still working on the cloud")
                                .withMessage ("Lathe is still busy: " + cloud->getActivity() + ".\n\n"
                                              "If you quit now it stops. Nothing is lost: files already uploaded stay "
                                              "uploaded, and Cloud > Push finishes the job later.")
                                .withButton ("Stop and Quit")
                                .withButton ("Keep Working"),
                            [this, proceed] (int button)
    {
        // JUCE numbers buttons 1, 2, ... and the last one 0 (so Escape picks it).
        if (button != 1)
            return;
        if (cloud != nullptr)
            cloud->cancel();
        confirmDiscard (proceed);
    });
}

//==============================================================================
// Screenshots for the website: Lathe --screenshots <folder>
//==============================================================================
namespace
{
bool saveShot (Component& c, const File& f, Rectangle<int> area = {})
{
    // 2x, so they're sharp on high-density screens.
    const auto image = c.createComponentSnapshot (area.isEmpty() ? c.getLocalBounds() : area, true, 2.0f);
    f.deleteFile();
    FileOutputStream out (f);
    if (! out.openedOk())
        return false;
    JPEGImageFormat jpeg;
    jpeg.setQuality (0.9f);
    return jpeg.writeImageToStream (image, out);
}

/** The AI assistant demo on getlathe.app/ai.html: what an agent does, step by step, through
    the same tool calls it makes over MCP. After each step Lathe draws ai-<n>.jpg; ai-session.json
    lists every call with the status line Lathe showed for it. The chat on the page goes with it. */
struct AiStep
{
    const char* frame;
    const char* view;      // "arrange" or "mixer"
    const char* select;    // the track whose devices show at the bottom ("" for none)
    const char* human;     // what the person does first, no tool: "guitar" (records a take), "learn" (Mastering's Learn), or ""
    const char* panel;     // a device on the selected track whose own panel is drawn too, as ai-<n>-panel.jpg ("" for none)
    const char* calls;     // JSON: [ { "tool": ..., "args": { ... } } ]
};

const AiStep aiSteps[] = {
    // "New song at 100 BPM: drums, bass, electric piano, something sparkly on top, and a track for my guitar."
    { "ai-1.jpg", "arrange", "Sparkle", "", "", R"([
        { "tool": "new_project", "args": {} },
        { "tool": "edit_session", "args": { "changes": [
            { "action": "set_tempo", "bpm": 100 },
            { "action": "rename_track", "track": "Audio 1", "name": "Guitar" },
            { "action": "set_track", "track": "Guitar", "arm": true },
            { "action": "rename_track", "track": "MIDI 1", "name": "Drums" },
            { "action": "add_plugin", "track": "Drums", "plugin": "Drums" },
            { "action": "add_track", "kind": "midi", "name": "Bass", "plugins": [ "Bass Synth" ] },
            { "action": "add_track", "kind": "midi", "name": "Keys", "plugins": [ "Multi Synth" ] },
            { "action": "add_track", "kind": "midi", "name": "Sparkle", "plugins": [ "Random Notes", "Resonator", "Grains" ] },
            { "action": "add_track", "kind": "return", "name": "Verb", "plugins": [ "Reverb" ] },
            { "action": "set_send", "track": "Keys", "return": "Verb", "level_db": -12 },
            { "action": "set_send", "track": "Sparkle", "return": "Verb", "level_db": -8 } ] } },
        { "tool": "set_plugin_params", "args": { "track": "Drums", "plugin": "Drums", "params": [ { "param": "Kit", "value": "Simple 808" } ] } },
        { "tool": "set_plugin_params", "args": { "track": "Keys", "plugin": "Multi Synth", "params": [ { "param": "Model", "value": "Six-op FM: keys" } ] } }
    ])" },

    // "Write eight bars in A minor: Am, F, C, G, two bars each. Keep the drums laid back."
    { "ai-2.jpg", "arrange", "Keys", "", "Multi Synth", R"([
        { "tool": "write_notes", "args": { "track": "Drums", "from_bar": 1, "to_bar": 2, "repeat": 8, "notes": [
            { "pitch": 36, "start_bar": 1, "length_beats": 0.5, "velocity": 112 },
            { "pitch": 36, "start_bar": 1.625, "length_beats": 0.5, "velocity": 92 },
            { "pitch": 38, "start_bar": 1.25, "length_beats": 0.5, "velocity": 100 },
            { "pitch": 38, "start_bar": 1.75, "length_beats": 0.5, "velocity": 104 },
            { "pitch": 42, "start_bar": 1, "velocity": 80 }, { "pitch": 42, "start_bar": 1.125, "velocity": 56 },
            { "pitch": 42, "start_bar": 1.25, "velocity": 76 }, { "pitch": 42, "start_bar": 1.375, "velocity": 54 },
            { "pitch": 42, "start_bar": 1.5, "velocity": 80 }, { "pitch": 42, "start_bar": 1.625, "velocity": 58 },
            { "pitch": 46, "start_bar": 1.875, "length_beats": 0.5, "velocity": 70 } ] } },
        { "tool": "write_notes", "args": { "track": "Bass", "from_bar": 1, "to_bar": 9, "notes": [
            { "pitch": "A1", "start_bar": 1, "length_beats": 1.5 }, { "pitch": "A1", "start_bar": 1.375, "length_beats": 0.5 },
            { "pitch": "E2", "start_bar": 1.625, "length_beats": 0.5 }, { "pitch": "A1", "start_bar": 2, "length_beats": 1.5 },
            { "pitch": "G1", "start_bar": 2.75, "length_beats": 1 },
            { "pitch": "F1", "start_bar": 3, "length_beats": 1.5 }, { "pitch": "F1", "start_bar": 3.375, "length_beats": 0.5 },
            { "pitch": "C2", "start_bar": 3.625, "length_beats": 0.5 }, { "pitch": "F1", "start_bar": 4, "length_beats": 1.5 },
            { "pitch": "E1", "start_bar": 4.75, "length_beats": 1 },
            { "pitch": "C2", "start_bar": 5, "length_beats": 1.5 }, { "pitch": "C2", "start_bar": 5.375, "length_beats": 0.5 },
            { "pitch": "G2", "start_bar": 5.625, "length_beats": 0.5 }, { "pitch": "C2", "start_bar": 6, "length_beats": 1.5 },
            { "pitch": "B1", "start_bar": 6.75, "length_beats": 1 },
            { "pitch": "G1", "start_bar": 7, "length_beats": 1.5 }, { "pitch": "G1", "start_bar": 7.375, "length_beats": 0.5 },
            { "pitch": "D2", "start_bar": 7.625, "length_beats": 0.5 }, { "pitch": "G1", "start_bar": 8, "length_beats": 2 },
            { "pitch": "G#1", "start_bar": 8.75, "length_beats": 1 } ] } },
        { "tool": "write_notes", "args": { "track": "Keys", "from_bar": 1, "to_bar": 9, "notes": [
            { "pitch": "A2", "start_bar": 1, "length_beats": 3.5, "velocity": 84 }, { "pitch": "C3", "start_bar": 1, "length_beats": 3.5, "velocity": 80 },
            { "pitch": "E3", "start_bar": 1, "length_beats": 3.5, "velocity": 82 }, { "pitch": "G3", "start_bar": 1, "length_beats": 3.5, "velocity": 74 },
            { "pitch": "C3", "start_bar": 2.5, "length_beats": 2, "velocity": 70 }, { "pitch": "E3", "start_bar": 2.5, "length_beats": 2, "velocity": 72 },
            { "pitch": "A3", "start_bar": 2.5, "length_beats": 2, "velocity": 70 },
            { "pitch": "F2", "start_bar": 3, "length_beats": 3.5, "velocity": 84 }, { "pitch": "A2", "start_bar": 3, "length_beats": 3.5, "velocity": 80 },
            { "pitch": "C3", "start_bar": 3, "length_beats": 3.5, "velocity": 82 }, { "pitch": "E3", "start_bar": 3, "length_beats": 3.5, "velocity": 74 },
            { "pitch": "A2", "start_bar": 4.5, "length_beats": 2, "velocity": 70 }, { "pitch": "C3", "start_bar": 4.5, "length_beats": 2, "velocity": 72 },
            { "pitch": "F3", "start_bar": 4.5, "length_beats": 2, "velocity": 70 },
            { "pitch": "G2", "start_bar": 5, "length_beats": 3.5, "velocity": 84 }, { "pitch": "C3", "start_bar": 5, "length_beats": 3.5, "velocity": 80 },
            { "pitch": "E3", "start_bar": 5, "length_beats": 3.5, "velocity": 82 }, { "pitch": "B3", "start_bar": 5, "length_beats": 3.5, "velocity": 70 },
            { "pitch": "C3", "start_bar": 6.5, "length_beats": 2, "velocity": 70 }, { "pitch": "E3", "start_bar": 6.5, "length_beats": 2, "velocity": 72 },
            { "pitch": "G3", "start_bar": 6.5, "length_beats": 2, "velocity": 70 },
            { "pitch": "G2", "start_bar": 7, "length_beats": 3.5, "velocity": 84 }, { "pitch": "B2", "start_bar": 7, "length_beats": 3.5, "velocity": 80 },
            { "pitch": "D3", "start_bar": 7, "length_beats": 3.5, "velocity": 82 }, { "pitch": "F3", "start_bar": 7, "length_beats": 3.5, "velocity": 72 },
            { "pitch": "B2", "start_bar": 8.5, "length_beats": 2, "velocity": 70 }, { "pitch": "D3", "start_bar": 8.5, "length_beats": 2, "velocity": 72 },
            { "pitch": "G3", "start_bar": 8.5, "length_beats": 2, "velocity": 74 } ] } },
        { "tool": "set_loop", "args": { "from_bar": 1, "to_bar": 9 } }
    ])" },

    // "Make the sparkle stay in key, and have it repeat itself more, so it sounds like a hook."
    { "ai-3.jpg", "arrange", "Sparkle", "", "Random Notes", R"([
        { "tool": "set_plugin_params", "args": { "track": "Sparkle", "plugin": "Random Notes", "params": [
            { "param": "Scale", "value": "Major" }, { "param": "Lowest note", "value": 69 },
            { "param": "Steps", "value": "1/8" }, { "param": "Deja vu", "value": "75%" }, { "param": "Loop", "value": "8" } ] } },
        { "tool": "set_plugin_params", "args": { "track": "Sparkle", "plugin": "Resonator", "params": [
            { "param": "Model", "value": "Bells & bars" }, { "param": "Brightness", "value": "70%" } ] } },
        { "tool": "set_plugin_params", "args": { "track": "Sparkle", "plugin": "Grains", "params": [ { "param": "Mix", "value": "35%" } ] } }
    ])" },

    // (They record a guitar part over bars 5 to 8.) "I put guitar on the second half. Mix it so the guitar sits on top."
    { "ai-4.jpg", "mixer", "", "guitar", "", R"([
        { "tool": "analyze_mix", "args": { "from_bar": 1, "to_bar": 9 } },
        { "tool": "edit_session", "args": { "changes": [
            { "action": "add_plugin", "track": "Guitar", "plugin": "Compressor" },
            { "action": "set_mixer", "track": "Guitar", "volume_db": -6, "pan": 0.3 },
            { "action": "set_mixer", "track": "Bass", "volume_db": -9 },
            { "action": "set_mixer", "track": "Drums", "volume_db": -5 },
            { "action": "set_mixer", "track": "Keys", "volume_db": -6, "pan": -0.3 },
            { "action": "set_mixer", "track": "Sparkle", "volume_db": -2, "pan": -0.35 },
            { "action": "set_send", "track": "Guitar", "return": "Verb", "level_db": -16 },
            { "action": "set_track", "track": "Guitar", "arm": false } ] } },
        { "tool": "analyze_mix", "args": { "from_bar": 1, "to_bar": 9 } }
    ])" },

    // "Master it for streaming."
    { "ai-5.jpg", "arrange", "Master", "", "Mastering", R"([
        { "tool": "edit_session", "args": { "changes": [ { "action": "add_plugin", "track": "Master", "plugin": "Mastering" } ] } },
        { "tool": "set_plugin_params", "args": { "track": "Master", "plugin": "Mastering", "params": [
            { "param": "Target", "value": "Streaming" }, { "param": "Style", "value": "Warm" } ] } }
    ])" },

    // (They press Learn and play the song through.) "Export it."
    { "ai-6.jpg", "arrange", "Master", "learn", "Mastering", R"([
        { "tool": "export_audio", "args": { "from_bar": 1, "to_bar": 9, "tail_seconds": 3 } }
    ])" },
};
} // namespace

void MainComponent::runScreenshots (const File& outDir)
{
    struct Run
    {
        File out, work;
        demo::Songs songs;
        StringArray written;
        bool ok = true;
    };
    auto run = std::make_shared<Run>();
    run->out = outDir;
    run->work = File::getSpecialLocation (File::tempDirectory)
                    .getChildFile ("lathe-screenshots-" + String (Time::currentTimeMillis()));

    auto finish = [run]
    {
        run->work.deleteRecursively();
        std::cout << (run->ok ? "Wrote " : "Failed. Wrote ") << run->written.joinIntoString (", ")
                  << " in " << run->out.getFullPathName() << std::endl;
        JUCEApplicationBase::getInstance()->setApplicationReturnValue (run->ok ? 0 : 1);
        JUCEApplicationBase::quit();
    };
    auto shot = [run] (Component& c, const String& name, Rectangle<int> area = {})
    {
        if (saveShot (c, run->out.getChildFile (name), area))
            run->written.add (name);
        else
            run->ok = false;
    };

    run->out.createDirectory();
    if (auto r = demo::write (run->work, run->songs); r.failed())
    {
        std::cerr << "Couldn't make the demo song: " << r.getErrorMessage() << std::endl;
        run->ok = false;
        finish();
        return;
    }

    // A fixed window size, so the pictures are the same size every time.
    if (auto* window = findParentComponentOfClass<DocumentWindow>())
        window->setContentComponentSize (1440, 860);
    else
        setSize (1440, 860);
    loadProjectFile (run->songs.plain, run->songs.plain);
    detailShown = false;   // no plug-in panel: more room for the song
    resized();

    Component::SafePointer<MainComponent> self (this);
    // Give the waveform overviews a moment to build, then take the pictures in turn.
    Timer::callAfterDelay (2000, [self, run, shot, finish]
    {
        if (self == nullptr) return;
        self->setStatus ("Opened Night Drive");
        shot (*self, "arrange.jpg");

        // The note on the vocal, open, the way clicking its marker shows it.
        if (auto view = self->notes->makeThreadView (run->songs.noteId))
        {
            const auto area = self->getLocalArea (nullptr, self->arrangement.screenAreaFor (run->songs.noteTrackId, run->songs.noteBeat));
            {
                CallOutBox box (*view, area, self.getComponent());
                shot (*self, "notes.jpg");
                // A close-up: the open note and the lanes around it.
                const auto around = box.getBounds().getUnion (area).expanded (260, 150).getIntersection (self->getLocalBounds());
                shot (*self, "notes-detail.jpg", around);
            }
        }
        else
        {
            run->ok = false;
        }

        self->setMixerShown (true);
        Timer::callAfterDelay (500, [self, run, shot, finish]
        {
            if (self == nullptr) return;
            shot (*self, "mixer.jpg");
            self->setMixerShown (false);

            // The shared copy, for its History window.
            self->loadProjectFile (run->songs.shared, run->songs.shared);
            Timer::callAfterDelay (1000, [self, run, shot, finish]
            {
                if (self == nullptr) return;
                auto view = self->cloud->makeHistoryView();
                view->setSize (900, 540);
                shot (*view, "history.jpg");
                self->runDawShots (run->out, run->work, run->songs.plain, [self, run, finish] (bool dawOk, StringArray dawWritten)
                {
                    run->ok = run->ok && dawOk;
                    run->written.addArray (dawWritten);
                    if (self == nullptr) return;
                    self->runAssistantShots (run->out, run->work, [run, finish] (bool ok, StringArray written)
                    {
                        run->ok = run->ok && ok;
                        run->written.addArray (written);
                        finish();
                    });
                });
            });
        });
    });
}

void MainComponent::runDawShots (const File& outDir, const File& work, const File& song, std::function<void (bool, StringArray)> done)
{
    struct Run
    {
        StringArray written;
        bool ok = true;
        File outDir;
    };
    auto run = std::make_shared<Run>();
    run->outDir = outDir;
    auto shot = [outDir, run] (Component& c, const String& name)
    {
        if (saveShot (c, outDir.getChildFile (name)))
            run->written.add (name);
        else
        {
            run->ok = false;
            std::cerr << "Couldn't draw " << name << std::endl;
        }
    };
    auto fail = [run] (const String& why) { run->ok = false; std::cerr << why << std::endl; };

    loadProjectFile (song, song);
    auto named = [this] (const String& name) -> std::shared_ptr<Track>
    {
        for (auto* list : { &edit.tracks, &edit.buses })
            for (auto& t : *list)
                if (t->name == name)
                    return t;
        return {};
    };
    // Some of Lathe's own devices, as a band would put them on Night Drive.
    auto add = [this] (Track& t, const String& id)
    {
        for (auto& info : devices::all())
            if (info.id == id)
            {
                String error;
                auto slot = engine.makeSlot (devices::describe (info), {}, error);
                if (slot->instance != nullptr && t.isBus())
                    devices::startOnReturn (*slot->instance);
                t.plugins.push_back (slot);
                return slot->instance != nullptr;
            }
        return false;
    };
    auto keys = named ("Keys"), lead = named ("Lead"), plate = named ("Plate"), echo = named ("Echo"), bass = named ("Bass"), vocal = named ("Vocal");
    if (keys == nullptr || lead == nullptr || plate == nullptr || echo == nullptr || bass == nullptr || vocal == nullptr
        || ! add (*keys, "poly") || ! add (*keys, "compressor") || ! add (*keys, "saturator") || ! add (*lead, "multi") || ! add (*plate, "reverb") || ! add (*echo, "delay"))
    {
        fail ("The DAW pictures need Night Drive's tracks and Lathe's devices");
        done (false, {});
        return;
    }
    // The synths come in hot: bring the master down to a level a mix would sit at.
    edit.master->volumeDb = -9.0f;
    edit.changed();
    engine.rebuild();
    edit.clearUndo();
    edit.dirty = false;
    // A loop over the chorus.
    edit.loopEnabled = true;
    edit.loopStart = 32.0;
    edit.loopEnd = 48.0;

    Component::SafePointer<MainComponent> self (this);

    // 1. The piano roll, open on the lead line, with the loop on.
    ctx.selection = {};
    ctx.selection.trackId = lead->id;
    if (! lead->clips.empty())
        ctx.openClipEditor (lead->id, lead->clips.front().id);
    arrangement.refresh();
    Timer::callAfterDelay (800, [self, run, shot, fail, keys, bass, vocal, work, done]
    {
        if (self == nullptr) return;
        shot (*self, "daw-pianoroll.jpg");

        // 2. Lathe's own synth on the keys, in the panel under the song.
        self->ctx.selection = {};
        self->ctx.selection.trackId = keys->id;
        if (self->ctx.selectionChanged)
            self->ctx.selectionChanged();
        self->showDetail (false);
        self->detailHeight = 290;
        self->resized();
        Timer::callAfterDelay (800, [self, run, shot, fail, bass, vocal, work, done]
        {
            if (self == nullptr) return;
            shot (*self, "daw-devices.jpg");

            // 3. Compare: Before, then a few mix moves (they make the Undo History too), Now, and
            // a reference: the same song mixed brighter and louder, rendered to a file.
            auto& cmp = *self->compare;
            auto r = cmp.render (Compare::before, [self, run, shot, fail, bass, vocal, work, done] (Result rb)
            {
                if (self == nullptr) return;
                if (rb.failed()) fail ("Compare Before: " + rb.getErrorMessage());
                auto& e = self->edit;

                // Your moves, one step each, then the assistant's (labelled the way its tools do).
                e.commitUndoStep();
                vocal->volumeDb = vocal->volumeDb.load() + 1.5f;
                e.adjusted();
                e.commitUndoStep();
                bass->eq.bands[2].freq = 250.0f;
                bass->eq.bands[2].gainDb = -3.0f;
                bass->eq.bands[2].q = 1.2f;
                e.adjusted();
                e.commitUndoStep();
                vocal->getSend (vocal->sends.empty() ? -1 : vocal->sends.front()->busId, false);
                for (auto& s : vocal->sends) s->levelDb = s->levelDb.load() - 3.0f;
                e.adjusted();
                e.labelNextStep ("\"Vocal\": send to \"Plate\" -9.0 dB; \"Vocal\": send to \"Echo\" -15.0 dB", true);
                e.commitUndoStep();
                // One more of yours, taken back, so the list shows a step that can be redone.
                if (auto g = self->edit.tracks.size() > 2 ? self->edit.tracks[2] : nullptr)
                {
                    g->pan = -0.3f;
                    e.adjusted();
                    e.commitUndoStep();
                    e.undo();
                }
                self->engine.rebuild();

                // The reference: master up and brighter for one render, then back as it was.
                const auto refFile = work.getChildFile ("Finished single.wav");
                {
                    auto& m = *e.master;
                    const float oldVol = m.volumeDb.load();
                    const auto oldOn = m.eq.bands[4].on.load();
                    const float oldGain = m.eq.bands[4].gainDb.load(), oldFreq = m.eq.bands[4].freq.load();
                    m.volumeDb = oldVol + 4.0f;
                    m.eq.bands[4].on = true;
                    m.eq.bands[4].freq = 6000.0f;
                    m.eq.bands[4].gainDb = 3.5f;
                    AudioBuffer<float> mix;
                    Engine::ExportSettings s;
                    s.mix = true;
                    s.stems = false;
                    s.startBeat = e.loopStart;
                    s.endBeat = e.loopEnd;
                    s.tailSeconds = 0.0;
                    auto rr = self->engine.renderOffline (s, [] (double) { return true; },
                        [&mix] (const std::vector<Engine::RenderTap>&, int64 length) { mix.setSize (2, (int) length); mix.clear(); return Result::ok(); },
                        [&mix] (int tap, const AudioBuffer<float>& b, int start, int count, int64 at)
                        {
                            if (tap != 0 || at >= mix.getNumSamples()) return;
                            const int n = (int) jmin ((int64) count, (int64) mix.getNumSamples() - at);
                            for (int ch = 0; ch < 2; ++ch)
                                mix.copyFrom (ch, (int) at, b, jmin (ch, b.getNumChannels() - 1), start, n);
                        });
                    m.volumeDb = oldVol;
                    m.eq.bands[4].on = oldOn;
                    m.eq.bands[4].gainDb = oldGain;
                    m.eq.bands[4].freq = oldFreq;
                    if (rr.failed())
                        fail ("The reference render: " + rr.getErrorMessage());
                    refFile.deleteFile();
                    std::unique_ptr<OutputStream> stream (refFile.createOutputStream());
                    WavAudioFormat wav;
                    if (auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (self->engine.getSampleRate())
                                                                  .withNumChannels (2).withBitsPerSample (24)))
                        w->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples());
                }

                auto& cmp2 = *self->compare;
                auto rn = cmp2.render (Compare::now, [self, run, shot, fail, refFile, done] (Result rnow)
                {
                    if (self == nullptr) return;
                    if (rnow.failed()) fail ("Compare Now: " + rnow.getErrorMessage());
                    auto rl = self->compare->loadReference (refFile, [self, run, shot, fail, done] (Result rref)
                    {
                        if (self == nullptr) return;
                        if (rref.failed()) fail ("Compare reference: " + rref.getErrorMessage());
                        self->compare->setHeard (Compare::now);
                        self->compare->setPositionSeconds (self->compare->getLengthSeconds() * 0.38);
                        CompareWindow::show (*self->compare, nullptr);
                        UndoHistoryWindow::show (self->edit, nullptr, nullptr);
                        Timer::callAfterDelay (600, [self, run, shot, done]
                        {
                            if (self == nullptr) return;
                            for (int i = 0; i < TopLevelWindow::getNumTopLevelWindows(); ++i)
                            {
                                auto* w = TopLevelWindow::getTopLevelWindow (i);
                                if (dynamic_cast<CompareWindow*> (w) != nullptr)
                                    shot (*dynamic_cast<DocumentWindow*> (w)->getContentComponent(), "daw-compare.jpg");
                                else if (dynamic_cast<UndoHistoryWindow*> (w) != nullptr)
                                    shot (*dynamic_cast<DocumentWindow*> (w)->getContentComponent(), "daw-undo-history.jpg");
                            }
                            {
                                // The one-time question about anonymous milestones (privacy page).
                                const auto f = run->outDir.getChildFile ("welcome-milestones.jpg");
                                f.deleteFile();
                                FileOutputStream out (f);
                                JPEGImageFormat jpeg;
                                jpeg.setQuality (0.9f);
                                if (out.openedOk() && jpeg.writeImageToStream (milestonesDialogImage(), out))
                                    run->written.add (f.getFileName());
                            }
                            CompareWindow::closeIfOpen();
                            UndoHistoryWindow::closeIfOpen();
                            self->compare->projectChanged();
                            self->showDetail (false);
                            self->detailHeight = 190;
                            // Leave nothing behind for the assistant pictures, which start a new song.
                            self->edit.loopEnabled = false;
                            self->edit.clearUndo();
                            self->edit.dirty = false;
                            done (run->ok, run->written);
                        });
                    });
                    if (rl.failed()) { fail ("Compare reference: " + rl.getErrorMessage()); done (false, run->written); }
                });
                if (rn.failed()) { fail ("Compare Now: " + rn.getErrorMessage()); done (false, run->written); }
            });
            if (r.failed()) { fail ("Compare Before: " + r.getErrorMessage()); done (false, run->written); }
        });
    });
}

void MainComponent::runAssistantShots (const File& outDir, const File& work, std::function<void (bool, StringArray)> done)
{
    struct State
    {
        assistant::Tools tools;
        Array<var> log;
        StringArray written;
        bool ok = true;
        size_t step = 0;
        std::function<void()> next;
        State (Edit& e, Engine& en) : tools (e, en) {}
    };
    auto st = std::make_shared<State> (edit, engine);
    // New, as File > New does it; never Open or Save (that would touch the real projects folder).
    st->tools.project.newProject = [this, work] (const File& templateFile) -> String
    {
        newProject (templateFile);
        // Its audio and exports go in the run's scratch folder, not the real Unsaved folder.
        edit.workDir = work.getChildFile ("AI assistant demo");
        updateTitle();
        return {};
    };
    st->tools.showStatus = [this] (const String& s) { setStatus (s); };

    // A smaller window than the other pictures: on the page it's shown beside the chat, and
    // the controls need to be big enough to read there.
    if (auto* window = findParentComponentOfClass<DocumentWindow>())
        window->setContentComponentSize (1180, 720);
    else
        setSize (1180, 720);
    detailHeight = 190;

    Component::SafePointer<MainComponent> self (this);
    using Calls = std::shared_ptr<Array<var>>;

    // One call, the way the connection makes it: long tools (analyze_mix, export_audio) run on
    // their own thread while this waits for them, without blocking the message thread.
    auto callTool = [self, weak = std::weak_ptr<State> (st)] (const var& c, std::function<void (var result, String status)> then)
    {
        auto st = weak.lock();
        if (st == nullptr)
            return;
        const auto name = c["tool"].toString();
        if (! assistant::Tools::isLongTool (name))
        {
            String status;
            const auto result = st->tools.call (name, c["args"], status);
            then (result, status);
            return;
        }
        var immediate;
        auto job = st->tools.startLongTool (name, c["args"], immediate);
        if (job == nullptr)
        {
            then (immediate, {});
            return;
        }
        auto poll = std::make_shared<std::function<void()>>();
        *poll = [self, job, then, poll, waited = 0]() mutable
        {
            if (self == nullptr)
                return;
            if (job->done.wait (0) || (waited += 50) > assistant::kLongToolSeconds * 1000)
            {
                const auto result = job->done.wait (0) ? job->result : assistant::toolText ("timed out", true);
                auto callback = then;
                MessageManager::callAsync ([poll] { *poll = nullptr; });   // it holds itself: let it go, once it's finished
                callback (result, {});
                return;
            }
            Timer::callAfterDelay (50, [poll] { if (*poll) (*poll)(); });
        };
        (*poll)();
    };

    // The state owns the step function, which only holds the state weakly; each pending step holds
    // it strongly. Once the last step is done nothing holds it, and it all goes.
    st->next = [self, weak = std::weak_ptr<State> (st), outDir, work, done, callTool]
    {
        auto st = weak.lock();
        if (self == nullptr || st == nullptr)
            return;
        if (st->step >= std::size (aiSteps))
        {
            outDir.getChildFile ("ai-session.json").replaceWithText (JSON::toString (var (st->log)));
            st->written.add ("ai-session.json");
            self->setMixerShown (false);
            done (st->ok, st->written);
            return;
        }
        const auto& step = aiSteps[st->step++];
        const String human (step.human);

        DynamicObject::Ptr entry = new DynamicObject();
        entry->setProperty ("frame", step.frame);

        if (human == "guitar")
        {
            // The person records their part: the take lands on Guitar at bar 5, as a recording would.
            const auto wav = work.getChildFile ("Guitar take.wav");
            if (auto r = demo::writeGuitarTake (wav, self->edit.tempo); r.failed())
                st->ok = false;
            int guitarIndex = -1;
            for (size_t i = 0; i < self->edit.tracks.size(); ++i)
                if (self->edit.tracks[i]->name == "Guitar")
                    guitarIndex = (int) i;
            self->arrangement.importFiles ({ wav }, guitarIndex, 4.0 * self->edit.beatsPerBar());
            entry->setProperty ("person", "Recorded a guitar take over bars 5 to 8");
        }
        else if (human == "learn")
        {
            // The person presses Learn on Mastering, plays the song through, and presses Learn again.
            devices::Device* mastering = nullptr;
            for (auto& slot : self->edit.master->plugins)
                if (auto* d = dynamic_cast<devices::Device*> (slot->instance.get()); d != nullptr && slot->getName() == "Mastering")
                    mastering = d;
            if (mastering == nullptr)
            {
                st->ok = false;
                std::cerr << step.frame << ": no Mastering to learn with" << std::endl;
            }
            else
            {
                mastering->action();
                Engine::ExportSettings s;
                s.mix = true;
                s.stems = false;
                s.startBeat = 0.0;
                s.endBeat = self->edit.getEndBeat();
                s.tailSeconds = 2.0;
                auto playThrough = [&]
                {
                    return self->engine.renderOffline (s, [] (double) { return true; },
                                                       [] (const std::vector<Engine::RenderTap>&, int64) { return Result::ok(); },
                                                       [] (int, const AudioBuffer<float>&, int, int, int64) {});
                };
                auto r = playThrough();
                // Learn again: Mastering takes it at its next block, so play a moment more.
                mastering->action();
                s.endBeat = 1.0;
                s.tailSeconds = 0.0;
                if (r.wasOk())
                    r = playThrough();
                if (r.failed())
                {
                    st->ok = false;
                    std::cerr << step.frame << ": couldn't play it through for Learn: " << r.getErrorMessage() << std::endl;
                }
            }
            entry->setProperty ("person", "Pressed Learn on Mastering and played the song through");
        }

        const auto script = JSON::parse (String (step.calls));
        Calls calls = std::make_shared<Array<var>>();
        if (script.getArray() == nullptr || script.getArray()->isEmpty())
        {
            st->ok = false;
            std::cerr << step.frame << ": its calls aren't readable JSON" << std::endl;
        }
        Calls todo = std::make_shared<Array<var>> (script.getArray() != nullptr ? *script.getArray() : Array<var>());

        // After the calls: show it the way someone watching would see it, then draw it.
        auto show = [self, st, outDir, step, entry, calls]
        {
            entry->setProperty ("calls", *calls);
            st->log.add (var (entry.get()));

            self->setMixerShown (String (step.view) == "mixer");
            const String select (step.select);
            if (select.isNotEmpty())
                if (auto track = st->tools.findTrack (select))
                {
                    self->ctx.selectTrack (track->id);
                    self->showDetail (false);
                }
            if (select.isEmpty())
            {
                self->detailShown = false;
                self->resized();
            }
            self->refreshAll();

            Timer::callAfterDelay (900, [self, st, outDir, step]
            {
                if (self == nullptr)
                    return;
                if (saveShot (*self, outDir.getChildFile (step.frame)))
                    st->written.add (step.frame);
                else
                    st->ok = false;

                // The device's own panel, as Open shows it in its window.
                if (String (step.panel).isNotEmpty())
                {
                    bool drawn = false;
                    if (auto track = st->tools.findTrack (String (step.select)))
                        for (auto& slot : track->plugins)
                            if (slot->instance != nullptr && slot->getName() == step.panel)
                                if (std::unique_ptr<AudioProcessorEditor> editor (slot->instance->createEditor()); editor != nullptr)
                                {
                                    const auto name = String (step.frame).upToLastOccurrenceOf (".", false, false) + "-panel.jpg";
                                    if (saveShot (*editor, outDir.getChildFile (name)))
                                    {
                                        st->written.add (name);
                                        drawn = true;
                                    }
                                    break;
                                }
                    if (! drawn)
                    {
                        st->ok = false;
                        std::cerr << step.frame << ": couldn't draw " << step.panel << "'s panel" << std::endl;
                    }
                }
                st->next();
            });
        };

        // The calls, one after another.
        auto runNext = std::make_shared<std::function<void()>>();
        *runNext = [self, st, step, todo, calls, callTool, show, runNext]
        {
            if (self == nullptr)
                return;
            if (todo->isEmpty())
            {
                auto finish = show;
                MessageManager::callAsync ([runNext] { *runNext = nullptr; });   // it holds itself: let it go, once it's finished
                finish();
                return;
            }
            const auto c = todo->removeAndReturn (0);
            callTool (c, [self, st, step, c, calls, runNext] (var result, String status)
            {
                if (self == nullptr)
                    return;
                if (status.isNotEmpty())
                    self->setStatus (status);   // as the connection shows it
                const bool failed = (bool) result.getProperty ("isError", false);
                String text;
                if (auto* content = result["content"].getArray(); content != nullptr && ! content->isEmpty())
                    text = (*content)[0]["text"].toString();
                if (failed)
                {
                    st->ok = false;
                    std::cerr << step.frame << ": " << c["tool"].toString() << " failed: " << text << std::endl;
                }
                DynamicObject::Ptr call = new DynamicObject();
                call->setProperty ("tool", c["tool"]);
                call->setProperty ("args", c["args"]);
                call->setProperty ("status", status);
                call->setProperty ("result", text.substring (0, 6000));
                if (failed)
                    call->setProperty ("error", true);
                calls->add (var (call.get()));
                MessageManager::callAsync ([runNext] { if (*runNext) (*runNext)(); });
            });
        };
        // After Learn, give Mastering a moment to set what it learned (its timer does it).
        Timer::callAfterDelay (human == "learn" ? 600 : 1, [runNext] { if (*runNext) (*runNext)(); });
    };
    st->next();
}
} // namespace lathe
