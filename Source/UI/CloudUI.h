#pragma once

#include "Widgets.h"
#include "../Cloud/Revisions.h"

namespace lathe
{
//==============================================================================
/** Everything cloud in the app: the Cloud menu, sign-in, sharing, push / pull,
    the History window, and a status-bar label. Network work runs on one
    background thread; nothing here blocks the UI. */
class CloudController : private Timer
{
public:
    /** What the controller needs from the main window. */
    struct Hooks
    {
        std::function<void (std::function<void (bool)>)> saveProject;   // save (asks where if new)
        std::function<void (const File&)> openProject;                  // load from disk
        std::function<void (std::function<void()>)> confirmDiscard;     // unsaved changes? ask first
        std::function<void (const String&)> setStatus;
        std::function<File()> projectsDir;
    };

    CloudController (AppContext&, PropertiesFile&, Hooks);
    ~CloudController() override;

    /** Call after a project is opened or created (force), or saved (only acts if it moved). */
    void projectChanged (bool force = true);

    void addMenuItems (PopupMenu&);
    bool handleMenuItem (int id);
    static constexpr int menuBase = 600, menuEnd = 640;

    /** The status-bar label ("Cloud: Jam · r3"); click it for History. */
    Component& getStatusComponent();

    // --- actions (also the menu's)
    void showSignIn (std::function<void()> then = {});
    void signOut();
    void shareProject();
    void invite();
    void push();
    void pull();
    void showHistory();
    void openSharedProject();
    void showSettings();
    void openRevision (const cloud::Revision&);

    bool isLinked() const;
    bool isSignedIn() const        { return client != nullptr && client->isSignedIn(); }
    bool isBusy() const noexcept   { return busy; }
    cloud::LocalStore* getStore()  { return store.get(); }
    AppContext& getContext()       { return ctx; }

private:
    class StatusLabel;
    class SignInDialog;
    class HistoryWindow;
    class ProjectPicker;

    void timerCallback() override;
    void makeClient();
    void restoreSession();
    void updateStatus();
    void ensureReady (std::function<void()> then);   // signed in, project saved
    void doPush (const String& parent, const String& message);
    void pushOnTop (const String& theirHead, const String& message);
    void poll();

    /** Runs work on the cloud thread, then done() on the message thread. */
    void run (const String& what, std::function<void()> work, std::function<void()> done);
    cloud::Progress progressReporter();

    AppContext& ctx;
    PropertiesFile& settings;
    Hooks hooks;
    std::unique_ptr<cloud::Client> client;
    std::unique_ptr<cloud::LocalStore> store;
    ThreadPool worker { 1 };
    std::atomic<bool> busy { false }, cancelled { false };
    String waitingHead, waitingText;     // a newer revision on the server, if any
    std::unique_ptr<StatusLabel> status;
    Component::SafePointer<DocumentWindow> history;
    int pollCountdown = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (CloudController)
    JUCE_DECLARE_NON_COPYABLE (CloudController)
};
} // namespace lathe
