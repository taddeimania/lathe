#include "UI/MainComponent.h"

namespace lathe
{
class MainWindow : public DocumentWindow
{
public:
    explicit MainWindow (PropertiesFile& s)
        : DocumentWindow ("Lathe", theme::window, DocumentWindow::allButtons), settings (s)
    {
        setUsingNativeTitleBar (true);
        main = new MainComponent (settings);
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
    bool moreThanOneInstanceAllowed() override       { return false; }

    void initialise (const String& commandLine) override
    {
        PropertiesFile::Options options;
        options.applicationName = "Lathe";
       #if JUCE_LINUX
        options.folderName = ".config/Lathe";
       #else
        options.folderName = "Lathe";
       #endif
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);

       #if ! JUCE_MAC
        // Lathe Patcher renames files that were in use during an update to *.old.
        for (auto& f : File::getSpecialLocation (File::currentExecutableFile).getParentDirectory()
                           .findChildFiles (File::findFiles, false, "*.old"))
            f.deleteFile();
       #endif

        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        window = std::make_unique<MainWindow> (*properties.getUserSettings());
        window->main->startup (commandLine);
    }

    void shutdown() override
    {
        window.reset();
        properties.saveIfNeeded();
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
};
} // namespace lathe

START_JUCE_APPLICATION (lathe::LatheApplication)
