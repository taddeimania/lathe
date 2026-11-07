// Lathe Patcher: checks GitHub for a newer Lathe and installs it in place, so an
// update doesn't mean downloading and unpacking a release by hand.
//
// It ships with Lathe: inside Lathe.app/Contents/Helpers on the Mac, next to
// Lathe(.exe) on Windows and Linux. Lathe's Options > Check for Updates opens it.
//   LathePatcher [--from-lathe] [--install <path>]

#include <juce_gui_extra/juce_gui_extra.h>
#include "Updater.h"

using namespace juce;
namespace up = lathe::updater;

namespace
{
namespace colours
{
    const Colour window { 0xff2b2b2b }, panel { 0xff333333 }, control { 0xff4a4a4a }, border { 0xff1d1d1d };
    const Colour text { 0xffdadada }, textDim { 0xff8e8e8e }, textDark { 0xff1e1e1e };
    const Colour accent { 0xffe9a23b }, error { 0xffdb4b40 };
}

class Look : public LookAndFeel_V4
{
public:
    Look()
    {
        setColour (ResizableWindow::backgroundColourId, colours::window);
        setColour (TextButton::buttonColourId, colours::control);
        setColour (TextButton::buttonOnColourId, colours::accent);
        setColour (TextButton::textColourOffId, colours::text);
        setColour (TextButton::textColourOnId, colours::textDark);
        setColour (ComboBox::outlineColourId, colours::border);
        setColour (TextEditor::backgroundColourId, colours::panel);
        setColour (TextEditor::textColourId, colours::text);
        setColour (TextEditor::outlineColourId, colours::border);
        setColour (TextEditor::focusedOutlineColourId, colours::border);
        setColour (ProgressBar::backgroundColourId, colours::panel);
        setColour (ProgressBar::foregroundColourId, colours::accent);
        setColour (Label::textColourId, colours::text);
        setColour (ScrollBar::thumbColourId, colours::control);
    }

    void drawButtonBackground (Graphics& g, Button& b, const Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? findColour (TextButton::buttonOnColourId) : bg;
        if (! b.isEnabled()) c = c.withAlpha (0.4f);
        else if (down)       c = c.darker (0.15f);
        else if (over)       c = c.brighter (0.08f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }
};

//==============================================================================
class PatcherComponent : public Component
{
public:
    PatcherComponent (File installLocation, bool launchedFromLathe)
        : install (std::move (installLocation)), fromLathe (launchedFromLathe)
    {
        title.setFont (FontOptions (17.0f, Font::bold));
        addAndMakeVisible (title);

        message.setFont (FontOptions (13.5f));
        message.setJustificationType (Justification::topLeft);
        message.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (message);

        notes.setMultiLine (true);
        notes.setReadOnly (true);
        notes.setScrollbarsShown (true);
        notes.setCaretVisible (false);
        notes.setFont (FontOptions (12.5f));
        addChildComponent (notes);

        addChildComponent (progressBar);

        for (auto* b : { &primary, &secondary })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        primary.setToggleState (true, dontSendNotification);   // accent colour
        primary.setClickingTogglesState (false);

        if (install.exists())
            up::cleanUpOldFiles (install, up::defaultLayout());

        setSize (520, 360);
        check();
    }

    ~PatcherComponent() override
    {
        *cancelled = true;
    }

    void paint (Graphics& g) override
    {
        g.fillAll (colours::window);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        title.setBounds (r.removeFromTop (26));
        r.removeFromTop (6);

        auto buttons = r.removeFromBottom (30);
        secondary.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        primary.setBounds (buttons.removeFromRight (150));
        r.removeFromBottom (14);

        message.setBounds (r.removeFromTop (54));
        if (progressBar.isVisible())
            progressBar.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        notes.setBounds (r);
    }

private:
    //==============================================================================
    void show (const String& heading, const String& text, Colour textColour = colours::text)
    {
        title.setText (heading, dontSendNotification);
        message.setText (text, dontSendNotification);
        message.setColour (Label::textColourId, textColour);
        resized();
    }

    void setButtons (const String& primaryText, std::function<void()> primaryAction,
                     const String& secondaryText, std::function<void()> secondaryAction)
    {
        primary.setButtonText (primaryText);
        primary.onClick = std::move (primaryAction);
        primary.setVisible (primaryText.isNotEmpty());
        secondary.setButtonText (secondaryText);
        secondary.onClick = std::move (secondaryAction);
        secondary.setVisible (secondaryText.isNotEmpty());
    }

    static void quit()   { JUCEApplication::getInstance()->systemRequestedQuit(); }

    /** Runs work on a background thread, then done() back on the message thread. */
    template <typename Work, typename Done>
    void inBackground (Work work, Done done)
    {
        SafePointer<PatcherComponent> safe (this);
        Thread::launch ([work = std::move (work), done = std::move (done), safe]() mutable
        {
            auto result = work();
            MessageManager::callAsync ([done = std::move (done), result, safe]() mutable
            {
                if (safe != nullptr)
                    done (result);
            });
        });
    }

    //==============================================================================
    void check()
    {
        progressBar.setVisible (false);
        notes.setVisible (false);
        show ("Checking for updates", "Asking GitHub for the latest Lathe...");
        setButtons ({}, {}, "Close", quit);

        inBackground ([] { up::Release r; auto res = up::fetchLatest (r); return std::make_pair (res, r); },
                      [this] (std::pair<Result, up::Release> p)
        {
            latest = p.second;
            if (p.first.failed())
            {
                show ("Couldn't check for updates", p.first.getErrorMessage(), colours::error);
                setButtons ("Try again", [this] { check(); }, "Close", quit);
                return;
            }

            notes.setText (latest.notes.isNotEmpty() ? latest.notes : "(No release notes.)", false);
            notes.setVisible (true);

            if (! up::isInstall (install, up::defaultLayout()))
            {
                show ("Lathe " + latest.version + " is available",
                      "The patcher couldn't find a Lathe installation to update. It needs to stay next to "
                      "Lathe (inside Lathe.app on the Mac). You can download the release from its page instead.",
                      colours::error);
                setButtons ("Open release page", [this] { URL (latest.pageUrl).launchInDefaultBrowser(); }, "Close", quit);
                return;
            }

            if (up::compareVersions (latest.version, currentVersion) <= 0)
            {
                show ("Lathe is up to date", "You have Lathe " + currentVersion + ", the latest version.");
                setButtons ({}, {}, "Close", quit);
                return;
            }

            show ("Lathe " + latest.version + " is available",
                  "You have " + currentVersion + ". Updating downloads " + latest.assetName
                  + (latest.assetSize > 0 ? " (" + File::descriptionOfSizeInBytes (latest.assetSize) + ")" : String())
                  + " and replaces " + install.getFullPathName() + ". Your projects, settings and plug-ins aren't touched.");
            setButtons ("Update to " + latest.version, [this] { update(); }, "Not now", quit);
        });
    }

    void update()
    {
        progress = 0.0;
        progressBar.setVisible (true);
        show ("Updating to Lathe " + latest.version, "Downloading...");
        setButtons ({}, {}, "Cancel", [this] { *cancelled = true; });
        *cancelled = false;

        const auto layout = up::defaultLayout();
        const auto staging = up::stagingDirFor (install, layout);
        const auto archive = staging.getChildFile (latest.assetName);
        const auto unpacked = staging.getChildFile ("unpacked");
        auto release = latest;
        auto flag = cancelled;
        const auto installLocation = install;
        SafePointer<PatcherComponent> safe (this);

        inBackground ([=]
        {
            auto stage = [safe] (const String& text, double p)
            {
                MessageManager::callAsync ([safe, text, p]
                {
                    if (safe != nullptr)
                    {
                        safe->progress = p;
                        if (text.isNotEmpty())
                            safe->message.setText (text, dontSendNotification);
                    }
                });
            };

            staging.deleteRecursively();
            if (! staging.createDirectory())
                return Result::fail ("Couldn't write next to " + installLocation.getFullPathName()
                                     + ". Check that you can write to that folder.");

            uint32 lastReport = 0;
            auto r = up::download (release, archive, [&] (double p)
            {
                if (Time::getMillisecondCounter() - lastReport > 80)
                {
                    lastReport = Time::getMillisecondCounter();
                    stage ({}, 0.85 * p);
                }
                return ! flag->load();
            });

            if (r.wasOk()) { stage ("Checking the download...", 0.87); r = up::verify (release, archive); }
            if (r.wasOk()) { stage ("Unpacking...", 0.9);              r = up::unpack (archive, unpacked); }
            if (r.wasOk()) { stage ("Installing...", 0.97);            r = up::install (unpacked, installLocation, layout); }

            staging.deleteRecursively();
            return r;
        },
        [this] (Result r)
        {
            progressBar.setVisible (false);
            if (r.failed())
            {
                const bool cancelledByUser = cancelled->load();
                show (cancelledByUser ? "Update cancelled" : "The update didn't work",
                      cancelledByUser ? "Nothing was changed." : r.getErrorMessage() + "\nYour current Lathe is unchanged.",
                      cancelledByUser ? colours::text : colours::error);
                setButtons ("Try again", [this] { check(); }, "Close", quit);
                return;
            }

            currentVersion = latest.version;
            show ("Lathe " + latest.version + " is installed",
                  fromLathe ? "Quit Lathe and open it again to start using the new version."
                            : "If Lathe is open, quit it and open it again to start using the new version.");
            setButtons (fromLathe ? String() : String ("Open Lathe"), [this] { openLathe(); quit(); }, "Close", quit);
        });
    }

    void openLathe()
    {
       #if JUCE_MAC
        install.startAsProcess();
       #elif JUCE_WINDOWS
        install.getChildFile ("Lathe.exe").startAsProcess();
       #else
        install.getChildFile ("Lathe").startAsProcess();
       #endif
    }

    File install;
    bool fromLathe = false;
    String currentVersion { LATHE_PATCHER_VERSION };
    up::Release latest;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>> (false);
    double progress = 0.0;

    Label title, message;
    TextEditor notes;
    ProgressBar progressBar { progress };
    TextButton primary, secondary;
};

//==============================================================================
class PatcherApplication : public JUCEApplication
{
public:
    const String getApplicationName() override       { return "Lathe Patcher"; }
    const String getApplicationVersion() override    { return LATHE_PATCHER_VERSION; }
    bool moreThanOneInstanceAllowed() override       { return false; }

    void initialise (const String&) override
    {
        LookAndFeel::setDefaultLookAndFeel (&look);

        const auto args = getCommandLineParameterArray();
        File install = up::findInstall();
        if (const int i = args.indexOf ("--install"); i >= 0 && i + 1 < args.size())
            install = File (args[i + 1]);

        window = std::make_unique<Window> (new PatcherComponent (install, args.contains ("--from-lathe")));
    }

    void shutdown() override
    {
        window.reset();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override   { quit(); }

private:
    class Window : public DocumentWindow
    {
    public:
        explicit Window (Component* content)
            : DocumentWindow ("Lathe Patcher", colours::window, DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override   { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    Look look;
    std::unique_ptr<Window> window;
};
} // namespace

START_JUCE_APPLICATION (PatcherApplication)
