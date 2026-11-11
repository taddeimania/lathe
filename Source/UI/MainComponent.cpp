#include "MainComponent.h"
#include "../Assistant/AssistantService.h"
#include "../Demo/DemoSong.h"
#include "../Demo/Welcome.h"

#include <iostream>

namespace lathe
{
namespace menu
{
    enum
    {
        newProject = 1, open, save, saveAs, saveTemplate, importAudio, exportAudio, exportTracks, showFolder, quit, openWelcome,
        undo = 100, redo, deleteClip, duplicate, split, cutClips, copyClips, pasteClips, joinClips, selectAllClips,
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
    engine.onDeviceChanged = [this] { refreshAll(); };
    engine.onRecordingFinished = [this] { refreshAll(); };

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

    // AI assistant (MCP) connection: off unless you turned it on (Options > AI Assistant).
    if (! forScreenshots)
    {
        aiAssistant = std::make_shared<assistant::Service> (ctx);
        connectAssistantToProjects();
    }

    updateNotice = std::make_unique<UpdateNotice> (settings, [this] { openPatcher(); });
    updateNotice->onShowOrHide = [this] { resized(); };
    addChildComponent (*updateNotice);

    setSize (1360, 860);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    aiAssistant.reset();   // before the project and engine it reads
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
    return [this, &missing] (const PluginDescription& desc, const MemoryBlock& state)
    {
        String error;
        auto slot = engine.makeSlot (desc, state, error);
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
    for (auto* list : { &edit.tracks, &edit.buses })
    {
        for (auto& t : *list)
        {
            for (auto& slot : t->plugins)
            {
                if (slot->instance != nullptr)
                    continue;
                String error;
                auto fresh = engine.makeSlot (slot->description, slot->savedState, error);
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

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transport.setBounds (r.removeFromTop (theme::transportHeight));

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
            break;
        case menu::redo:
            if (engine.isRecording()) setStatus ("Stop recording first, then redo.");
            else                      edit.redo();
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

    if (code == KeyPress::spaceKey && ! cmd)   { engine.togglePlay(); return true; }
    if (code == KeyPress::homeKey)             { edit.insertMarker = 0.0; engine.seekTo (0.0); edit.sendChangeMessage(); return true; }
    if (code == KeyPress::tabKey && ! cmd)     { setMixerShown (! mixerShown); return true; }

    if (cmd)
    {
        if (is ('z')) { menuItemSelected (shift ? menu::redo : menu::undo, 0); return true; }
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
                finish();
            });
        });
    });
}
} // namespace lathe
