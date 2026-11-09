#include "Dialogs.h"

namespace lathe
{
namespace
{
Label* makeHeading (Component& parent, OwnedArray<Label>& labels, const String& text)
{
    auto* l = labels.add (new Label ({}, text));
    l->setFont (theme::font (13.0f, true));
    parent.addAndMakeVisible (l);
    return l;
}

Label* makeNote (Component& parent, OwnedArray<Label>& labels, const String& text)
{
    auto* l = labels.add (new Label ({}, text));
    l->setFont (theme::font (12.0f));
    l->setColour (Label::textColourId, theme::textDim);
    l->setJustificationType (Justification::topLeft);
    l->setMinimumHorizontalScale (1.0f);
    parent.addAndMakeVisible (l);
    return l;
}

//==============================================================================
class SettingsPanel : public Component, private ChangeListener, private Timer
{
public:
    explicit SettingsPanel (AppContext& c)
        : ctx (c),
          devices (c.engine.deviceManager, 0, 256, 0, 256, true, false, true, false)
    {
        addAndMakeVisible (devices);

        audioHeading = makeHeading (*this, labels, "Recording");
        offsetNote = makeNote (*this, labels, {});
        offset.setSliderStyle (Slider::LinearHorizontal);
        offset.setTextBoxStyle (Slider::TextBoxRight, false, 64, 20);
        offset.setRange (-50.0, 50.0, 0.1);
        offset.setTextValueSuffix (" ms");
        offset.setValue (ctx.settings.getDoubleValue ("recordOffsetMs", 0.0), dontSendNotification);
        offset.setDoubleClickReturnValue (true, 0.0);
        offset.onValueChange = [this] { ctx.settings.setValue ("recordOffsetMs", offset.getValue()); updateNotes(); };
        addAndMakeVisible (offset);

        pluginHeading = makeHeading (*this, labels, "VST3 plug-ins");
        pluginNote = makeNote (*this, labels, {});
        folders.setText (ctx.host.getExtraFolders().joinIntoString ("; "), false);
        folders.setTextToShowWhenEmpty ("Extra folders to scan, separated by ;  (the standard VST3 folders are always scanned)", theme::textDim);
        folders.onTextChange = [this] { ctx.host.setExtraFolders (StringArray::fromTokens (folders.getText(), ";", "")); };
        addAndMakeVisible (folders);

        scan.onClick = [this] { ctx.host.startScan (false); updateNotes(); };
        rescan.onClick = [this] { ctx.host.startScan (true); updateNotes(); };
        retry.setTooltip ("Scan the skipped plug-ins again (e.g. after updating them)");
        retry.onClick = [this]
        {
            for (auto& f : ctx.host.known.getBlacklistedFiles())
                ctx.host.unblacklist (f);
            ctx.host.startScan (false);
            updateNotes();
        };
        addAndMakeVisible (scan);
        addAndMakeVisible (rescan);
        addAndMakeVisible (retry);

        startupHeading = makeHeading (*this, labels, "When Lathe starts");
        startup.addItem ("Open the last project", 1);
        startup.addItem ("Start a new project from the default template", 2);
        startup.setSelectedId (ctx.settings.getValue ("startup", "last") == "template" ? 2 : 1, dontSendNotification);
        startup.onChange = [this] { ctx.settings.setValue ("startup", startup.getSelectedId() == 2 ? "template" : "last"); };
        addAndMakeVisible (startup);

        templateBox.onChange = [this] { ctx.settings.setValue ("defaultTemplate", templateBox.getText()); };
        int id = 1;
        for (auto& f : getTemplatesDir().findChildFiles (File::findFiles, false, "*.lathe"))
            templateBox.addItem (f.getFileNameWithoutExtension(), id++);
        const auto current = ctx.settings.getValue ("defaultTemplate", "Basic");
        for (int i = 0; i < templateBox.getNumItems(); ++i)
            if (templateBox.getItemText (i) == current)
                templateBox.setSelectedItemIndex (i, dontSendNotification);
        templateBox.setTooltip ("Default template");
        addAndMakeVisible (templateBox);

        ctx.engine.deviceManager.addChangeListener (this);
        updateNotes();
        startTimer (500);
        setSize (640, 790);
    }

    ~SettingsPanel() override
    {
        ctx.engine.deviceManager.removeChangeListener (this);
        ctx.engine.saveDeviceSettings();
        ctx.settings.saveIfNeeded();
    }

    void updateNotes()
    {
        const double sr = jmax (1.0, ctx.engine.getSampleRate());
        const double reportedMs = 1000.0 * (ctx.engine.getRecordOffsetSamples() - ctx.settings.getDoubleValue ("recordOffsetMs", 0.0) * sr / 1000.0) / sr;
        offsetNote->setText ("New recordings are shifted earlier by the round-trip latency the driver reports ("
                             + String (reportedMs, 1) + " ms, including plug-in latency). If takes still land early or late, "
                             "nudge them here: positive moves recordings earlier.", dontSendNotification);

        const auto skipped = ctx.host.getSkippedPlugins();
        pluginNote->setText (String (ctx.host.known.getNumTypes()) + " plug-ins found."
                             + (ctx.host.isScanning() ? "  " + ctx.host.getScanStatus() : String())
                             + "  Each plug-in is checked in a separate helper process, so one that crashes or hangs is skipped instead of taking Lathe down."
                             + (skipped.isEmpty() ? String() : "\nSkipped: " + skipped.joinIntoString (", ")),
                             dontSendNotification);
        retry.setEnabled (! skipped.isEmpty() && ! ctx.host.isScanning());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        devices.setBounds (r.removeFromTop (380));
        r.removeFromTop (8);

        audioHeading->setBounds (r.removeFromTop (22));
        offsetNote->setBounds (r.removeFromTop (48));
        offset.setBounds (r.removeFromTop (24).withTrimmedLeft (4));
        r.removeFromTop (14);

        pluginHeading->setBounds (r.removeFromTop (22));
        pluginNote->setBounds (r.removeFromTop (62));
        folders.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        auto row = r.removeFromTop (26);
        scan.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (8);
        rescan.setBounds (row.removeFromLeft (190));
        row.removeFromLeft (8);
        retry.setBounds (row.removeFromLeft (170));
        r.removeFromTop (14);

        startupHeading->setBounds (r.removeFromTop (22));
        auto s = r.removeFromTop (24);
        startup.setBounds (s.removeFromLeft (330));
        s.removeFromLeft (8);
        templateBox.setBounds (s.removeFromLeft (180));
    }

private:
    void changeListenerCallback (ChangeBroadcaster*) override   { updateNotes(); }
    void timerCallback() override                               { updateNotes(); }

    AppContext& ctx;
    AudioDeviceSelectorComponent devices;
    OwnedArray<Label> labels;
    Label *audioHeading {}, *offsetNote {}, *pluginHeading {}, *pluginNote {}, *startupHeading {};
    Slider offset;
    TextEditor folders;
    TextButton scan { "Scan for new plug-ins" }, rescan { "Clear list and rescan all" }, retry { "Retry skipped plug-ins" };
    ComboBox startup, templateBox;
};

//==============================================================================
class ExportThread : public ThreadWithProgressWindow
{
public:
    ExportThread (Engine& e, Engine::ExportSettings s)
        : ThreadWithProgressWindow ("Exporting...", true, true, 10000, "Cancel"), engine (e), settings (std::move (s)) {}

    void run() override
    {
        result = engine.exportAudio (settings, [this] (double p)
        {
            setProgress (p);
            return ! threadShouldExit();
        }, files);
    }

    void threadComplete (bool cancelled) override
    {
        const auto r = result;
        const auto list = files;
        const auto folder = settings.folder;

        if (! cancelled && r.wasOk())
        {
            auto options = MessageBoxOptions()
                               .withIconType (MessageBoxIconType::InfoIcon)
                               .withTitle ("Export finished")
                               .withMessage (String (list.size()) + (list.size() == 1 ? " file" : " files") + " written to\n" + folder.getFullPathName())
                               .withButton ("Show folder")
                               .withButton ("OK");
            AlertWindow::showAsync (options, [folder] (int button)
            {
                if (button == 1)
                    folder.revealToUser();
            });
        }
        else if (! cancelled)
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Export failed", r.getErrorMessage());
        }

        MessageManager::callAsync ([this] { delete this; });
    }

private:
    Engine& engine;
    Engine::ExportSettings settings;
    Result result { Result::ok() };
    StringArray files;
};

class ExportPanel : public Component
{
public:
    ExportPanel (AppContext& c, const std::set<int>& onlyTracks) : ctx (c)
    {
        auto& edit = ctx.edit;

        // Which tracks: everything, or the ticked ones.
        which.addItem ("All tracks, as you hear them", 1);
        which.addItem ("Only the tracks ticked below", 2);
        which.setSelectedId (onlyTracks.empty() ? 1 : 2, dontSendNotification);
        which.onChange = [this] { updateTrackList(); };
        addAndMakeVisible (which);

        auto addRow = [&] (const Track& t, const String& suffix)
        {
            auto* b = trackBoxes.add (new ToggleButton (t.name + suffix));
            b->getProperties().set ("trackId", t.id);
            b->setToggleState (onlyTracks.empty() || onlyTracks.count (t.id) > 0, dontSendNotification);
            b->onClick = [this] { updateTrackList(); };
            trackList.addAndMakeVisible (b);
        };
        for (auto& t : edit.tracks)
            if (t->holdsClips())
                addRow (*t, t->mute.load() ? "  (muted)" : String());
        for (auto& t : edit.buses)
            addRow (*t, "  (return)");
        trackView.setViewedComponent (&trackList, false);
        trackView.setScrollBarsShown (true, false);
        addAndMakeVisible (trackView);

        for (auto* b : { &tickAll, &tickNone })
        {
            b->setColour (TextButton::buttonColourId, theme::control);
            addAndMakeVisible (b);
        }
        tickAll.onClick  = [this] { for (auto* b : trackBoxes) b->setToggleState (true, dontSendNotification);  updateTrackList(); };
        tickNone.onClick = [this] { for (auto* b : trackBoxes) b->setToggleState (false, dontSendNotification); updateTrackList(); };

        trackHint.setFont (theme::font (11.5f));
        trackHint.setColour (Label::textColourId, theme::textDim);
        trackHint.setJustificationType (Justification::topLeft);
        addAndMakeVisible (trackHint);

        mix.setButtonText ("Master mix (one stereo file)");
        stems.setButtonText ("Stems (one file per track and return, post-fader)");
        mix.setToggleState (ctx.settings.getBoolValue ("exportMix", true), dontSendNotification);
        stems.setToggleState (ctx.settings.getBoolValue ("exportStems", false), dontSendNotification);
        addAndMakeVisible (mix);
        addAndMakeVisible (stems);

        const double end = edit.getEndBeat();
        const double barLen = edit.beatsPerBar();
        range.addItem ("Whole song (bar 1 to bar " + String ((int) std::ceil (end / barLen) + 1) + ")", 1);
        range.addItem ("Loop region (bar " + String ((int) (edit.loopStart / barLen) + 1) + " to bar "
                       + String ((int) std::ceil (edit.loopEnd / barLen) + 1) + ")", 2);
        range.setSelectedId (edit.loopEnabled ? 2 : 1, dontSendNotification);
        addAndMakeVisible (range);

        bits.addItem ("16-bit", 16);
        bits.addItem ("24-bit", 24);
        bits.addItem ("32-bit float", 32);
        bits.setSelectedId (ctx.settings.getIntValue ("exportBits", 24), dontSendNotification);
        addAndMakeVisible (bits);

        tail.setSliderStyle (Slider::LinearHorizontal);
        tail.setTextBoxStyle (Slider::TextBoxRight, false, 60, 20);
        tail.setRange (0.0, 10.0, 0.5);
        tail.setTextValueSuffix (" s");
        tail.setValue (ctx.settings.getDoubleValue ("exportTail", 2.0), dontSendNotification);
        addAndMakeVisible (tail);

        edit.ensureWorkDir();
        folder = edit.getExportsDir().getChildFile (edit.getName() + " " + Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M"));
        folderLabel.setText (folder.getFullPathName(), dontSendNotification);
        folderLabel.setColour (Label::textColourId, theme::textDim);
        folderLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (folderLabel);

        chooseFolder.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Export to folder", folder.getParentDirectory());
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                  [this] (const FileChooser& fc)
            {
                if (fc.getResult() != File())
                {
                    folder = fc.getResult();
                    folderLabel.setText (folder.getFullPathName(), dontSendNotification);
                }
            });
        };
        addAndMakeVisible (chooseFolder);

        for (auto* l : { &tracksLabel, &rangeLabel, &bitsLabel, &tailLabel, &folderTitle })
        {
            l->setFont (theme::font (12.5f));
            l->setColour (Label::textColourId, theme::textDim);
            addAndMakeVisible (l);
        }

        exportButton.onClick = [this] { startExport(); };
        exportButton.setColour (TextButton::buttonColourId, theme::accent);
        exportButton.setColour (TextButton::textColourOffId, theme::textDark);
        cancelButton.onClick = [this] { close(); };
        addAndMakeVisible (exportButton);
        addAndMakeVisible (cancelButton);

        setSize (520, 520);
        updateTrackList();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto row = [&] (Label& l, Component& c)
        {
            auto line = r.removeFromTop (26);
            l.setBounds (line.removeFromLeft (90));
            c.setBounds (line);
            r.removeFromTop (8);
        };

        row (tracksLabel, which);
        {
            auto area = r.removeFromTop (150);
            area.removeFromLeft (90);
            auto side = area.removeFromRight (64);
            tickAll.setBounds (side.removeFromTop (24));
            side.removeFromTop (4);
            tickNone.setBounds (side.removeFromTop (24));
            area.removeFromRight (8);
            trackView.setBounds (area);
            trackList.setSize (area.getWidth() - trackView.getScrollBarThickness(), jmax (1, trackBoxes.size() * 24));
            for (int i = 0; i < trackBoxes.size(); ++i)
                trackBoxes[i]->setBounds (4, i * 24, trackList.getWidth() - 8, 24);
        }
        {
            auto hint = r.removeFromTop (34);
            hint.removeFromLeft (90);
            trackHint.setBounds (hint);
        }
        r.removeFromTop (6);

        mix.setBounds (r.removeFromTop (26));
        stems.setBounds (r.removeFromTop (26));
        r.removeFromTop (12);
        row (rangeLabel, range);
        row (bitsLabel, bits);
        row (tailLabel, tail);

        auto f = r.removeFromTop (26);
        folderTitle.setBounds (f.removeFromLeft (90));
        chooseFolder.setBounds (f.removeFromRight (90));
        f.removeFromRight (6);
        folderLabel.setBounds (f);

        auto buttons = getLocalBounds().reduced (16).removeFromBottom (30);
        exportButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        cancelButton.setBounds (buttons.removeFromRight (90));
    }

private:
    void close()
    {
        if (auto* dw = findParentComponentOfClass<DialogWindow>())
            dw->exitModalState (0);
    }

    void startExport()
    {
        auto& edit = ctx.edit;
        if (! mix.getToggleState() && ! stems.getToggleState())
        {
            ctx.showStatus ("Choose a mix, stems, or both.");
            return;
        }
        const auto chosen = chosenTracks();
        if (which.getSelectedId() == 2 && chosen.empty())
        {
            ctx.showStatus ("Tick at least one track to export.");
            return;
        }
        if (ctx.engine.isPlaying())
            ctx.engine.stop();

        Engine::ExportSettings s;
        s.mix = mix.getToggleState();
        s.stems = stems.getToggleState();
        s.bitDepth = bits.getSelectedId();
        s.tailSeconds = tail.getValue();
        s.folder = folder;
        s.baseName = edit.getName();
        s.trackIds = chosen;
        if (range.getSelectedId() == 2)
        {
            s.startBeat = edit.loopStart;
            s.endBeat = edit.loopEnd;
        }
        else
        {
            s.startBeat = 0.0;
            s.endBeat = jmax (edit.beatsPerBar(), edit.getEndBeat());
        }

        ctx.settings.setValue ("exportMix", s.mix);
        ctx.settings.setValue ("exportStems", s.stems);
        ctx.settings.setValue ("exportBits", s.bitDepth);
        ctx.settings.setValue ("exportTail", s.tailSeconds);

        auto* thread = new ExportThread (ctx.engine, s);
        close();
        thread->launchThread();
    }

    /** Empty when exporting everything. */
    std::set<int> chosenTracks() const
    {
        std::set<int> ids;
        if (which.getSelectedId() != 2)
            return ids;
        for (auto* b : trackBoxes)
            if (b->getToggleState())
                ids.insert ((int) b->getProperties()["trackId"]);
        return ids;
    }

    void updateTrackList()
    {
        const bool choosing = which.getSelectedId() == 2;
        for (auto* c : std::initializer_list<Component*> { &trackView, &tickAll, &tickNone })
            c->setEnabled (choosing);
        trackView.setAlpha (choosing ? 1.0f : 0.45f);

        if (! choosing)
        {
            trackHint.setText ("Everything plays as it does in the song: mute and solo count.", dontSendNotification);
            mix.setButtonText ("Master mix (one stereo file)");
            stems.setButtonText ("Stems (one file per track and return, post-fader)");
            return;
        }

        const auto ids = chosenTracks();
        int tracks = 0;
        for (auto& t : ctx.edit.tracks)
            tracks += (t->holdsClips() && ids.count (t->id) > 0) ? 1 : 0;
        if (tracks == 0 && ! ids.empty())
            trackHint.setText ("Only returns ticked: every track plays into them, and you get a file for each ticked return.", dontSendNotification);
        else
            trackHint.setText ("Only the ticked tracks play, even if muted. Returns pass on what those tracks send them; "
                               "tick a return to get its own file.", dontSendNotification);
        mix.setButtonText (tracks == 0 ? String ("Master mix (one stereo file)")
                                       : "Mix of the ticked tracks: \"" + Engine::exportMixName (ctx.edit.getName(), ctx.edit, ids) + ".wav\"");
        stems.setButtonText ("A file for each ticked track and return (post-fader)");
    }

    AppContext& ctx;
    ComboBox which;
    Component trackList;
    Viewport trackView;
    OwnedArray<ToggleButton> trackBoxes;
    TextButton tickAll { "All" }, tickNone { "None" };
    Label tracksLabel { {}, "Tracks" }, trackHint;
    ToggleButton mix, stems;
    ComboBox range, bits;
    Slider tail;
    Label rangeLabel { {}, "Range" }, bitsLabel { {}, "Format" }, tailLabel { {}, "Tail" }, folderTitle { {}, "Folder" };
    Label folderLabel;
    TextButton chooseFolder { "Choose..." }, exportButton { "Export" }, cancelButton { "Cancel" };
    File folder;
    std::unique_ptr<FileChooser> chooser;
};

void launch (Component* content, const String& title, bool resizable)
{
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = title;
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = resizable;
    o.launchAsync();
}
} // namespace

void showSettingsDialog (AppContext& ctx)
{
    launch (new SettingsPanel (ctx), "Audio / MIDI Settings", true);
}

void showExportDialog (AppContext& ctx, const std::set<int>& onlyTracks)
{
    launch (new ExportPanel (ctx, onlyTracks), "Export", false);
}
} // namespace lathe
