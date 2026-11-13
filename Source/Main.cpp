#include "Bench/Benchmark.h"
#include "UI/MainComponent.h"
#include "Cloud/Milestones.h"
#include "Devices/Device.h"

#include <iostream>

namespace lathe
{
class MainWindow : public DocumentWindow
{
public:
    MainWindow (PropertiesFile& s, bool forScreenshots)
        : DocumentWindow ("Lathe", theme::window, DocumentWindow::allButtons), settings (s)
    {
        setUsingNativeTitleBar (true);
        main = new MainComponent (settings, forScreenshots);
        setContentOwned (main, true);

       #if JUCE_MAC
        MenuBarModel::setMacMainMenu (main);
       #else
        setMenuBar (main, 24);
       #endif

        setResizable (true, false);
        setResizeLimits (900, 560, 10000, 10000);

        const auto state = settings.getValue ("window");
        if (state.isEmpty() || ! restoreWindowStateFromString (state))
            centreWithSize (1360, 860);

        setVisible (true);
    }

    ~MainWindow() override
    {
        settings.setValue ("window", getWindowStateAsString());
       #if ! JUCE_MAC
        setMenuBar (nullptr);
       #endif
    }

    void closeButtonPressed() override
    {
        JUCEApplication::getInstance()->systemRequestedQuit();
    }

    MainComponent* main = nullptr;

private:
    PropertiesFile& settings;
};

class LatheApplication : public JUCEApplication
{
public:
    const String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    // A screenshot run (--screenshots) can go alongside a Lathe you have open.
    bool moreThanOneInstanceAllowed() override
    {
        const auto params = getCommandLineParameters();
        return params.contains ("--screenshots") || params.contains ("--benchmark") || params.contains ("--device-shots");
    }

    void initialise (const String& commandLine) override
    {
        // Lathe --screenshots <folder>: draws a made-up demo song's windows to image files
        // (for the website) and quits. It uses its own throwaway settings, so it never
        // touches yours, your audio device or your plug-in list.
        const auto args = StringArray::fromTokens (commandLine, true);

        // Lathe --benchmark [out.csv]: times the audio engine on one core and on all of them,
        // with made-up sessions (no audio device, no plug-ins of yours), prints the comparison
        // and quits. See docs/multicore.md.
        if (const int benchArg = args.indexOf ("--benchmark"); benchArg >= 0)
        {
            const auto name = args[benchArg + 1].unquoted();
            const auto csv = File::getCurrentWorkingDirectory().getChildFile (name.isNotEmpty() && ! name.startsWith ("-")
                                                                                  ? name : String ("lathe-benchmark.csv"));
            setApplicationReturnValue (bench::run (csv, { "1", "full" }, std::cout));
            quit();
            return;
        }

        // Lathe --device-shots <folder>: draws each built-in device's window to a PNG and quits.
        if (const int devArg = args.indexOf ("--device-shots"); devArg >= 0)
        {
            const auto dir = File::getCurrentWorkingDirectory().getChildFile (args[devArg + 1].unquoted());
            dir.createDirectory();
            LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            for (auto& info : devices::all())
            {
                auto device = info.create();
                device->prepareToPlay (48000.0, 256);
                std::unique_ptr<AudioProcessorEditor> editor (device->createEditor());
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                const auto file = dir.getChildFile (info.id + ".png");
                file.deleteFile();
                FileOutputStream out (file);
                PNGImageFormat().writeImageToStream (image, out);
                std::cout << file.getFullPathName() << "  " << editor->getWidth() << " x " << editor->getHeight() << std::endl;
                editor.reset();
            }
            LookAndFeel::setDefaultLookAndFeel (nullptr);
            quit();
            return;
        }

        const int shotsArg = args.indexOf ("--screenshots");
        screenshotsDir = shotsArg >= 0 ? File::getCurrentWorkingDirectory().getChildFile (args[shotsArg + 1].unquoted())
                                       : File();
        if (shotsArg >= 0 && args[shotsArg + 1].isEmpty())
        {
            std::cerr << "Usage: Lathe --screenshots <folder>" << std::endl;
            setApplicationReturnValue (2);
            quit();
            return;
        }

        PropertiesFile::Options options;
        options.applicationName = "Lathe";
       #if JUCE_LINUX
        options.folderName = ".config/Lathe";
       #else
        options.folderName = "Lathe";
       #endif
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        if (screenshotsDir != File())
        {
            screenshotSettings = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("lathe-screenshot-settings", "");
            screenshotSettings.createDirectory();
            options.folderName = screenshotSettings.getFullPathName();   // absolute: wins over the usual place
        }
        // No settings file yet: Lathe has never run on this computer (as this user). That's the
        // only time it asks about anonymous milestones; an update or a reinstall over an old
        // Lathe has a settings file already, so it never asks there (Cloud/Milestones.h).
        const bool neverRunBefore = screenshotsDir == File() && ! options.getDefaultFile().existsAsFile();
        properties.setStorageParameters (options);
        if (neverRunBefore)
            milestones::markNeverRunBefore (*properties.getUserSettings());
        if (screenshotsDir != File())
        {
            properties.getUserSettings()->setValue ("autoUpdateCheck", false);
            properties.getUserSettings()->setValue ("metronome", false);
        }

       #if ! JUCE_MAC
        // Lathe Patcher renames files that were in use during an update to *.old.
        for (auto& f : File::getSpecialLocation (File::currentExecutableFile).getParentDirectory()
                           .findChildFiles (File::findFiles, false, "*.old"))
            f.deleteFile();
       #endif

        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        window = std::make_unique<MainWindow> (*properties.getUserSettings(), screenshotsDir != File());
        if (screenshotsDir != File())
            window->main->runScreenshots (screenshotsDir);
        else
            window->main->startup (commandLine);
    }

    void shutdown() override
    {
        window.reset();
        properties.saveIfNeeded();
        if (screenshotSettings != File())
            screenshotSettings.deleteRecursively();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (window == nullptr || window->main == nullptr)
        {
            quit();
            return;
        }

        window->main->confirmQuit ([] { JUCEApplication::getInstance()->quit(); });
    }

    void anotherInstanceStarted (const String& commandLine) override
    {
        const auto arg = commandLine.unquoted().trim();
        if (window != nullptr && File::isAbsolutePath (arg) && File (arg).existsAsFile())
        {
            const File f (arg);
            window->main->confirmDiscard ([this, f] { window->main->openProject (f); });
        }
    }

private:
    LatheLookAndFeel lookAndFeel;
    ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
    File screenshotsDir, screenshotSettings;
};
} // namespace lathe

START_JUCE_APPLICATION (lathe::LatheApplication)
