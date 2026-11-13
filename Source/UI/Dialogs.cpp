#include "Dialogs.h"
#include "../Cloud/Milestones.h"
#include "../Cloud/CrashReports.h"
#include "BinaryData.h"

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

        // The sample rate new projects record at. 48 kHz is plenty for most music and half
        // the size of 96 kHz (which matters for cloud storage and uploads).
        rateLabel = makeNote (*this, labels, "New projects record at");
        rate.addItem ("44.1 kHz", 44100);
        rate.addItem ("48 kHz (recommended)", 48000);
        rate.addItem ("88.2 kHz", 88200);
        rate.addItem ("96 kHz (twice the size)", 96000);
        rate.addItem ("Whatever the interface is set to", 1);
        const double saved = ctx.settings.getDoubleValue ("newProjectRate", 48000.0);
        rate.setSelectedId (saved <= 0 ? 1 : roundToInt (saved), dontSendNotification);
        if (rate.getSelectedId() == 0)
            rate.setSelectedId (48000, dontSendNotification);
        rate.setTooltip ("Projects you already have keep their own rate. You can still change the interface's rate above for the open project.");
        rate.onChange = [this] { ctx.settings.setValue ("newProjectRate", rate.getSelectedId() == 1 ? 0.0 : (double) rate.getSelectedId()); };
        addAndMakeVisible (rate);

        // How many cores render tracks at once (docs/multicore.md). Auto: one per performance core.
        processingHeading = makeHeading (*this, labels, "Processing");
        coresLabel = makeNote (*this, labels, "Processing cores");
        cores.addItem ("Auto (" + String (Engine::autoProcessingThreads()) + ")", 1000);
        cores.addItem ("1 (one track at a time)", 1);
        for (int n = 2; n <= jmax (2, SystemStats::getNumCpus()); ++n)
            cores.addItem (String (n), n);
        const int savedCores = ctx.settings.getIntValue ("processingThreads", 0);
        cores.setSelectedId (savedCores <= 0 ? 1000 : savedCores, dontSendNotification);
        if (cores.getSelectedId() == 0)
            cores.setSelectedId (1000, dontSendNotification);
        cores.onChange = [this]
        {
            const int n = cores.getSelectedId() == 1000 ? 0 : cores.getSelectedId();
            ctx.settings.setValue ("processingThreads", n);
            ctx.engine.setProcessingThreads (n);
            updateNotes();
        };
        addAndMakeVisible (cores);
        coresNote = makeNote (*this, labels, "Lathe plays tracks and their plug-ins on several cores at once, so big sessions "
                                             "fit where one core runs out. If a plug-in misbehaves, try 1.");

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

        privacyHeading = makeHeading (*this, labels, "Privacy");
        shareMilestones.setButtonText ("Share anonymous milestones");
        shareMilestones.setToggleState (milestones::choice (ctx.settings) == milestones::Choice::yes, dontSendNotification);
        shareMilestones.onClick = [this] { milestones::setChoice (ctx.settings, shareMilestones.getToggleState(), false); };
        addAndMakeVisible (shareMilestones);
        privacyNote = makeNote (*this, labels, "Off unless you turn it on. When it's on, Lathe sends a count the first time you "
                                               "do a few things (open it, record, export...), once each, with no ID, songs or "
                                               "names. Turn it off and nothing more is sent.");
        whatsSent.setButtonText ("Exactly what's sent");
        whatsSent.setURL (URL (milestones::explainUrl));
        whatsSent.setFont (theme::font (12.0f), false, Justification::centredLeft);
        whatsSent.setColour (HyperlinkButton::textColourId, theme::accent);
        addAndMakeVisible (whatsSent);

        crashLabel = makeNote (*this, labels, "Crash reports");
        crashBox.addItem ("Ask each time", 1);
        crashBox.addItem ("Always send", 2);
        crashBox.addItem ("Never send", 3);
        crashBox.setSelectedId (1 + (int) crashes::choice (ctx.settings), dontSendNotification);
        crashBox.onChange = [this] { crashes::setChoice (ctx.settings, (crashes::Choice) (crashBox.getSelectedId() - 1)); };
        addAndMakeVisible (crashBox);
        crashNote = makeNote (*this, labels, "If Lathe crashes, the next time it opens it asks before sending the report: "
                                             "where in Lathe it stopped, its version and your system. Nothing from your songs.");

        ctx.engine.deviceManager.addChangeListener (this);
        updateNotes();
        startTimer (500);
        setSize (640, 1076);
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
        r.removeFromTop (6);
        auto rateRow = r.removeFromTop (24);
        rateLabel->setBounds (rateRow.removeFromLeft (160));
        rate.setBounds (rateRow.removeFromLeft (260));
        r.removeFromTop (14);

        processingHeading->setBounds (r.removeFromTop (22));
        auto coresRow = r.removeFromTop (24);
        coresLabel->setBounds (coresRow.removeFromLeft (160));
        cores.setBounds (coresRow.removeFromLeft (260));
        r.removeFromTop (4);
        coresNote->setBounds (r.removeFromTop (34));
        r.removeFromTop (10);

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
        r.removeFromTop (14);

        privacyHeading->setBounds (r.removeFromTop (22));
        auto p = r.removeFromTop (24);
        shareMilestones.setBounds (p.removeFromLeft (240));
        whatsSent.setBounds (p.removeFromLeft (160));
        r.removeFromTop (2);
        privacyNote->setBounds (r.removeFromTop (34));
        r.removeFromTop (8);
        auto c = r.removeFromTop (24);
        crashLabel->setBounds (c.removeFromLeft (120));
        crashBox.setBounds (c.removeFromLeft (160));
        r.removeFromTop (2);
        crashNote->setBounds (r.removeFromTop (34));
    }

private:
    void changeListenerCallback (ChangeBroadcaster*) override   { updateNotes(); }
    void timerCallback() override                               { updateNotes(); }

    AppContext& ctx;
    AudioDeviceSelectorComponent devices;
    OwnedArray<Label> labels;
    Label *audioHeading {}, *offsetNote {}, *pluginHeading {}, *pluginNote {}, *startupHeading {}, *rateLabel {};
    Label *processingHeading {}, *coresLabel {}, *coresNote {}, *privacyHeading {}, *privacyNote {}, *crashLabel {}, *crashNote {};
    ComboBox crashBox;
    ToggleButton shareMilestones;
    HyperlinkButton whatsSent;
    Slider offset;
    TextEditor folders;
    TextButton scan { "Scan for new plug-ins" }, rescan { "Clear list and rescan all" }, retry { "Retry skipped plug-ins" };
    ComboBox startup, templateBox, rate, cores;
};

//==============================================================================
/** "Thanks for installing Lathe!": the one time Lathe asks whether it may send anonymous
    milestones (new installs only). It says what's never sent; the full list is on the privacy
    page, so it can be kept up to date there. The two answers look the same, nothing is sent
    until "Share" is pressed, and closing the window is a no. */
class MilestonesPanel : public Component
{
public:
    explicit MilestonesPanel (PropertiesFile& s) : settings (s)
    {
        logo = ImageCache::getFromMemory (BinaryData::iconsmall_png, BinaryData::iconsmall_pngSize);

        heading.setText ("Thanks for installing Lathe!", dontSendNotification);
        heading.setFont (theme::font (19.0f, true));
        addAndMakeVisible (heading);

        intro.setText ("While we're in the early stages, Lathe can report a few anonymous milestones, like the first time "
                       "you record something. It shows us where new people get stuck, so we know what to make better.",
                       dontSendNotification);
        never.setText ("It never sends your name, email, songs, recordings, track names, or anything that tells your "
                       "computer apart from anyone else's. Nothing is sent unless you choose Share.", dontSendNotification);
        for (auto* l : { &intro, &never, &later })
        {
            l->setFont (theme::font (13.0f));
            l->setJustificationType (Justification::topLeft);
            l->setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (l);
        }
        later.setText ("You can change your mind any time in Settings, under Privacy.", dontSendNotification);
        later.setColour (Label::textColourId, theme::textDim);
        later.setFont (theme::font (12.0f));

        whatsSent.setButtonText ("Exactly what's sent, and what we do with it");
        whatsSent.setURL (URL (milestones::explainUrl));
        whatsSent.setFont (theme::font (13.0f), false, Justification::centredLeft);
        whatsSent.setColour (HyperlinkButton::textColourId, theme::accent);
        addAndMakeVisible (whatsSent);

        dontShare.onClick = [this] { answer (false); };
        share.onClick = [this] { answer (true); };
        addAndMakeVisible (dontShare);
        addAndMakeVisible (share);
        setSize (520, 308);
    }

    ~MilestonesPanel() override
    {
        // Closed without an answer: that's a no, and it won't ask again.
        if (! answered)
            milestones::setChoice (settings, false, true);
    }

    void paint (Graphics& g) override
    {
        if (logo.isValid())
            g.drawImage (logo, logoArea.toFloat(), RectanglePlacement::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24, 22);
        auto top = r.removeFromTop (40);
        logoArea = top.removeFromLeft (40);
        top.removeFromLeft (6);
        heading.setBounds (top);
        r.removeFromTop (14);
        intro.setBounds (r.removeFromTop (54));
        r.removeFromTop (8);
        never.setBounds (r.removeFromTop (40));
        r.removeFromTop (6);
        whatsSent.setBounds (r.removeFromTop (22).withWidth (300).withTrimmedLeft (4));

        auto buttons = r.removeFromBottom (30);
        r.removeFromBottom (6);
        later.setBounds (r.removeFromBottom (20));
        share.setBounds (buttons.removeFromRight (170));
        buttons.removeFromRight (10);
        dontShare.setBounds (buttons.removeFromRight (170));
    }

private:
    void answer (bool yes)
    {
        answered = true;
        milestones::setChoice (settings, yes, true);
        if (auto* w = findParentComponentOfClass<DialogWindow>())
            w->exitModalState (0);
    }

    PropertiesFile& settings;
    bool answered = false;
    Image logo;
    Rectangle<int> logoArea;
    Label heading, intro, never, later;
    HyperlinkButton whatsSent;
    TextButton dontShare { "Don't share" }, share { "Share milestones" };
};

//==============================================================================
/** "Lathe quit unexpectedly last time": asks before sending crash reports, and shows them. */
class CrashPanel : public Component
{
public:
    CrashPanel (PropertiesFile& s, Array<File> r) : settings (s), reports (std::move (r))
    {
        heading.setText (reports.size() == 1 ? "Lathe quit unexpectedly last time."
                                             : "Lathe quit unexpectedly " + String (reports.size()) + " times.",
                         dontSendNotification);
        heading.setFont (theme::font (17.0f, true));
        addAndMakeVisible (heading);
        intro.setText ("Sending the report helps me find what went wrong and fix it. It's the text below: where in "
                       "Lathe's code it stopped, Lathe's version and your system. Nothing from your songs, no names, "
                       "no account.", dontSendNotification);
        intro.setFont (theme::font (13.0f));
        intro.setJustificationType (Justification::topLeft);
        addAndMakeVisible (intro);

        String all;
        for (auto& f : reports)
            all << crashes::reportText (f) << "\n";
        report.setMultiLine (true);
        report.setReadOnly (true);
        report.setScrollbarsShown (true);
        report.setFont (Font (FontOptions (Font::getDefaultMonospacedFontName(), 11.5f, Font::plain)));
        report.setText (all.trimEnd(), false);
        addAndMakeVisible (report);

        remember.setButtonText ("Remember my answer");
        addAndMakeVisible (remember);
        whatsSent.setButtonText ("What's sent, and what I do with it");
        whatsSent.setURL (URL (crashes::explainUrl));
        whatsSent.setFont (theme::font (12.5f), false, Justification::centredLeft);
        whatsSent.setColour (HyperlinkButton::textColourId, theme::accent);
        addAndMakeVisible (whatsSent);
        dontSend.onClick = [this] { answer (false); };
        sendIt.onClick = [this] { answer (true); };
        addAndMakeVisible (dontSend);
        addAndMakeVisible (sendIt);
        setSize (560, 470);
    }

    // Closed without an answer: the reports stay, and Lathe asks again next time.

    void resized() override
    {
        auto r = getLocalBounds().reduced (22, 18);
        heading.setBounds (r.removeFromTop (26));
        r.removeFromTop (6);
        intro.setBounds (r.removeFromTop (54));
        auto buttons = r.removeFromBottom (30);
        r.removeFromBottom (8);
        auto under = r.removeFromBottom (24);
        remember.setBounds (under.removeFromLeft (200));
        whatsSent.setBounds (under.removeFromRight (240));
        r.removeFromBottom (6);
        report.setBounds (r);
        sendIt.setBounds (buttons.removeFromRight (160));
        buttons.removeFromRight (10);
        dontSend.setBounds (buttons.removeFromRight (160));
    }

private:
    void answer (bool yes)
    {
        for (auto& f : reports)
            yes ? crashes::send (f) : crashes::dismiss (f);
        if (remember.getToggleState())
            crashes::setChoice (settings, yes ? crashes::Choice::always : crashes::Choice::never);
        if (auto* w = findParentComponentOfClass<DialogWindow>())
            w->exitModalState (0);
    }

    PropertiesFile& settings;
    Array<File> reports;
    Label heading, intro;
    TextEditor report;
    ToggleButton remember;
    HyperlinkButton whatsSent;
    TextButton dontSend { "Don't send" }, sendIt { "Send report" };
};

//==============================================================================
class ExportThread : public ThreadWithProgressWindow
{
public:
    ExportThread (Engine& e, Engine::ExportSettings s, PropertiesFile& p)
        : ThreadWithProgressWindow ("Exporting...", true, true, 10000, "Cancel"), engine (e), settings (std::move (s)), appSettings (p) {}

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
            milestones::reach (appSettings, "exported");
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
    PropertiesFile& appSettings;
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

        using F = Engine::ExportSettings::Format;
        format.addItem ("WAV", 1 + (int) F::wav);
        format.addItem ("AIFF", 1 + (int) F::aiff);
        format.addItem ("FLAC", 1 + (int) F::flac);
        format.addItem ("MP3", 1 + (int) F::mp3);
        format.addItem ("Ogg Vorbis", 1 + (int) F::ogg);
        auto saved = F::wav;
        Engine::parseFormat (ctx.settings.getValue ("exportFormat", "wav"), saved);
        format.setSelectedId (1 + (int) saved, dontSendNotification);
        format.onChange = [this] { updateQuality(); };
        addAndMakeVisible (format);
        // Bit depth or bit rate, whichever the format has. The ids are the numbers themselves.
        bits.onChange = [this]
        {
            if (lossy())
                ctx.settings.setValue ("exportKbps", bits.getSelectedId());
            else
                ctx.settings.setValue ("exportBits", bits.getSelectedId());
        };
        addAndMakeVisible (bits);
        formatHint.setFont (theme::font (11.5f));
        formatHint.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (formatHint);
        updateQuality();

        tail.setSliderStyle (Slider::LinearHorizontal);
        tail.setTextBoxStyle (Slider::TextBoxRight, false, 60, 20);
        tail.setRange (0.0, 10.0, 0.5);
        tail.setTextValueSuffix (" s");
        tail.setValue (ctx.settings.getDoubleValue ("exportTail", 2.0), dontSendNotification);
        addAndMakeVisible (tail);

        // Where the files go: by default a new dated folder in the song's Exports folder; or a
        // folder you choose (Desktop, a shared drive...), which Lathe remembers for next time.
        edit.ensureWorkDir();
        folderLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (folderLabel);
        folderHint.setFont (theme::font (11.5f));
        folderHint.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (folderHint);
        const File remembered (ctx.settings.getValue ("exportFolder"));
        setFolder (File::isAbsolutePath (remembered.getFullPathName()) && remembered.isDirectory() ? remembered : File());

        chooseFolder.onClick = [this]
        {
            const auto start = folder.isDirectory() ? folder : File::getSpecialLocation (File::userDesktopDirectory);
            chooser = std::make_unique<FileChooser> ("Export to folder", start);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                  [this] (const FileChooser& fc)
            {
                if (fc.getResult() != File())
                    setFolder (fc.getResult());
            });
        };
        addAndMakeVisible (chooseFolder);
        useExports.setFont (theme::font (11.5f), false, Justification::centredRight);
        useExports.setColour (HyperlinkButton::textColourId, theme::accent);
        useExports.onClick = [this] { setFolder (File()); };
        addChildComponent (useExports);

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

        setSize (520, 556);
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
        {
            auto line = r.removeFromTop (26);
            bitsLabel.setBounds (line.removeFromLeft (90));
            format.setBounds (line.removeFromLeft (130));
            line.removeFromLeft (8);
            bits.setBounds (line.removeFromLeft (130));
            auto hint = r.removeFromTop (20);
            hint.removeFromLeft (90);
            formatHint.setBounds (hint);
            r.removeFromTop (8);
        }
        row (tailLabel, tail);

        auto f = r.removeFromTop (26);
        folderTitle.setBounds (f.removeFromLeft (90));
        chooseFolder.setBounds (f.removeFromRight (90));
        f.removeFromRight (6);
        folderLabel.setBounds (f);
        auto fh = r.removeFromTop (20);
        fh.removeFromLeft (90);
        if (useExports.isVisible())
            useExports.setBounds (fh.removeFromRight (190));
        folderHint.setBounds (fh);

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
        s.format = chosenFormat();
        if (lossy())
            s.kbps = bits.getSelectedId();
        else
            s.bitDepth = bits.getSelectedId();
        s.tailSeconds = tail.getValue();
        s.folder = chosenFolder.isDirectory() ? chosenFolder
                                              : edit.getExportsDir().getChildFile (edit.getName() + " " + Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M"));
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
        ctx.settings.setValue ("exportFormat", Engine::formatName (s.format));
        ctx.settings.setValue ("exportTail", s.tailSeconds);
        ctx.settings.setValue ("exportFolder", chosenFolder.isDirectory() ? chosenFolder.getFullPathName() : String());

        auto* thread = new ExportThread (ctx.engine, s, ctx.settings);
        close();
        thread->launchThread();
    }

    Engine::ExportSettings::Format chosenFormat() const
    {
        return (Engine::ExportSettings::Format) jlimit (0, 4, format.getSelectedId() - 1);
    }

    bool lossy() const
    {
        using F = Engine::ExportSettings::Format;
        return chosenFormat() == F::mp3 || chosenFormat() == F::ogg;
    }

    /** Fills the second box with the bit depths or bit rates the chosen format has. */
    void updateQuality()
    {
        using F = Engine::ExportSettings::Format;
        const auto f = chosenFormat();
        bits.clear (dontSendNotification);
        if (f == F::mp3 || f == F::ogg)
        {
            for (auto kbps : { 320, 256, 192, 128 })
                bits.addItem (String (kbps) + " kbps", kbps);
            const auto want = ctx.settings.getIntValue ("exportKbps", 320);
            bits.setSelectedId (bits.indexOfItemId (want) >= 0 ? want : 320, dontSendNotification);
        }
        else
        {
            bits.addItem ("16-bit", 16);
            bits.addItem ("24-bit", 24);
            if (f == F::wav)
                bits.addItem ("32-bit float", 32);
            const auto want = ctx.settings.getIntValue ("exportBits", 24);
            bits.setSelectedId (bits.indexOfItemId (want) >= 0 ? want : 24, dontSendNotification);
        }

        switch (f)
        {
            case F::wav:  formatHint.setText ("Uncompressed. Opens anywhere; the one to keep or send for mastering.", dontSendNotification); break;
            case F::aiff: formatHint.setText ("Uncompressed, like WAV, for apps or people that ask for AIFF.", dontSendNotification); break;
            case F::flac: formatHint.setText ("Lossless: sounds the same as WAV at about half the size.", dontSendNotification); break;
            case F::mp3:  formatHint.setText ("Small and plays everywhere, but some detail is lost. For sharing.", dontSendNotification); break;
            case F::ogg:  formatHint.setText ("Small, and better than MP3 at the same size. Fewer players open it.", dontSendNotification); break;
        }
        updateTrackList();
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
                                       : "Mix of the ticked tracks: \"" + Engine::exportMixName (ctx.edit.getName(), ctx.edit, ids) + "."
                                         + Engine::formatName (chosenFormat()) + "\"");
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
    ComboBox range, format, bits;
    Slider tail;
    Label rangeLabel { {}, "Range" }, bitsLabel { {}, "Format" }, tailLabel { {}, "Tail" }, folderTitle { {}, "Folder" };
    Label folderLabel, formatHint, folderHint;
    TextButton chooseFolder { "Choose..." }, exportButton { "Export" }, cancelButton { "Cancel" };
    HyperlinkButton useExports { "Use the song's Exports folder", {} };
    File folder;          // what the label shows
    File chosenFolder;    // a folder you picked; File() = a new one in the song's Exports folder

    void setFolder (const File& chosen)
    {
        chosenFolder = chosen;
        if (chosen.isDirectory())
        {
            folder = chosen;
            folderLabel.setText (chosen.getFullPathName(), dontSendNotification);
            folderHint.setText ("Straight into this folder. Files already there aren't replaced.", dontSendNotification);
        }
        else
        {
            folder = ctx.edit.getExportsDir();
            folderLabel.setText (ctx.edit.getExportsDir().getFullPathName(), dontSendNotification);
            folderHint.setText ("In the song's Exports folder, in a new folder for each export.", dontSendNotification);
        }
        useExports.setVisible (chosen.isDirectory());
        resized();
    }
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

void handleCrashReports (PropertiesFile& settings)
{
    const auto reports = crashes::pending (crashes::defaultFolder());
    if (reports.isEmpty())
        return;
    switch (crashes::choice (settings))
    {
        case crashes::Choice::always: for (auto& f : reports) crashes::send (f);    return;
        case crashes::Choice::never:  for (auto& f : reports) crashes::dismiss (f); return;
        case crashes::Choice::ask:    break;
    }
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new CrashPanel (settings, reports));
    o.dialogTitle = "Crash report";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void showMilestonesDialog (PropertiesFile& settings)
{
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new MilestonesPanel (settings));
    o.dialogTitle = "Welcome to Lathe";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

Image milestonesDialogImage()
{
    // A throwaway settings file: the panel saves "no" when it closes unanswered.
    const auto file = File::createTempFile (".settings");
    {
        PropertiesFile scratch (file, PropertiesFile::Options());
        MilestonesPanel panel (scratch);
        const auto image = [&]
        {
            Image img (Image::RGB, panel.getWidth() * 2, panel.getHeight() * 2, true);
            Graphics g (img);
            g.addTransform (AffineTransform::scale (2.0f));
            g.fillAll (theme::panel);
            panel.paintEntireComponent (g, true);
            return img;
        }();
        file.deleteFile();
        return image;
    }
}

void showExportDialog (AppContext& ctx, const std::set<int>& onlyTracks)
{
    launch (new ExportPanel (ctx, onlyTracks), "Export", false);
}
} // namespace lathe
