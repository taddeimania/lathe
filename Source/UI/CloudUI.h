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
        /** The Cloud menu's contents changed (the Mac's menu bar only rebuilds when told). */
        std::function<void()> menuChanged;
        /** Changes were set aside or put back (the main window shows a bar while they're aside). */
        std::function<void()> setAsideChanged;
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
    /** Keeps (pins) a pushed revision in the cloud, or lets it go. */
    void setPinned (const String& revisionId, bool pinned);
    /** "Starter", "Pro", "Free" ("" until known). */
    String getPlanName() const   { return planName; }
    int getRevisionsKept() const { return revisionsKept; }
    void openSharedProject();
    /** Opens a revision (what is open is kept in History first); then runs once it's open. */
    void openRevision (const cloud::Revision&, std::function<void()> then = {});
    /** Your changes set aside while you listen to a newer revision: put them back on top of what's
        open (asking about anything you both changed), or forget them (they stay in History). */
    bool hasSetAside() const;
    void putBackChanges();
    void forgetSetAside();
    /** Cloud > Projects: your projects and songs, sharing, moving, deleting. */
    void showLibrary (const String& selectSongId = {});
    /** Cloud > Friends: requests and the people you can share with. */
    void showFriends();
    /** Downloads (or just opens, if it's here already) a cloud song: {id, name}. */
    void openCloudSong (const var& song);
    /** Forgets this song's cloud link (after losing access, say). The local copy stays. */
    void unlinkSong();

    // --- links for the website (CloudLinks.cpp)
    /** Cloud > Invite Someone by Link: a getlathe.app/join.html link that makes an account if
        they need one, makes you friends and, for the open song (if it's yours), shares it. */
    void showInviteLink();
    /** Cloud > Join with an Invite Link: paste one someone sent you. */
    void joinWithLink();
    /** Cloud > Share a Listening Link: renders the mix, uploads it, and gives you a
        getlathe.app/listen.html link anyone can play (and leave notes on) in a browser. */
    void shareListeningLink();

    /** A quick API call on its own thread (doesn't wait behind a push), then done() on
        the message thread. Notices the server being too old for /v2. */
    void request (const String& method, const String& path, const var& body,
                  std::function<void (const var&, const cloud::ApiError&)> done);
    /** "The server is too old" if an API error says /v2 doesn't exist. */
    bool isOldServer (const cloud::ApiError&) const;

    /** The open song's cloud id ("" if it isn't in the cloud). */
    String getSongId() const;
    /** Signed in, linked, and still allowed to open it: notes can go to the server. */
    bool canUseServer() const      { return isLinked() && isSignedIn() && ! lostAccess && ! oldServer; }
    String getEmail() const;
    /** owner, editor, viewer, or "" while unknown. */
    String getSongRole() const     { return songRole; }
    bool isLinked() const;
    bool isUnfinishedShare() const;
    bool isSignedIn() const        { return client != nullptr && client->isSignedIn(); }
    bool isBusy() const noexcept   { return busy; }
    /** What's running ("Uploading  45%"), for the quit warning. */
    String getActivity() const     { return activity; }
    /** Stops what's running at the next safe point (between files or upload parts). */
    void cancel()                  { *cancelFlag = true; }
    cloud::LocalStore* getStore()  { return store.get(); }
    AppContext& getContext()       { return ctx; }

    /** Screenshots (Lathe --screenshots): the History window's contents, drawn as if signed in. */
    std::unique_ptr<Component> makeHistoryView();
    bool screenshotMode = false;

private:
    class StatusLabel;
    class SignInDialog;
    class HistoryWindow;
    class LibraryWindow;
    class FriendsWindow;
    class PeopleList;

    void timerCallback() override;
    void makeClient();
    void restoreSession();
    void updateStatus();
    void ensureReady (std::function<void()> then);   // signed in, project saved
    void doPush (const String& parent, const String& message);
    /** Someone pushed while you were working: keeps your changes aside (safe in History), pulls,
        and opens the newest revision so you can listen. `message` is remembered for the push. */
    void setAsideAndGetNewest (const String& message);
    void poll();
    void pollAccount();                       // notices and friend requests
    void showNotices (const Array<var>& notices);
    void uploadSong();

    void refreshWindows();
    void reloadFriendsWindow();
    /** Signed in, out, or as someone else: forget what we knew about the last account. */
    void forgetAccount();
    /** Just signed in: fetch the plan, friend requests and this song's role right away. */
    void accountChanged();
    /** Projects and Friends belong to one account: close them when it changes. */
    void closeAccountWindows();

    /** Runs work on the cloud thread, then done() on the message thread. */
    void run (const String& what, std::function<void()> work, std::function<void()> done);
    cloud::Progress progressReporter();

    AppContext& ctx;
    PropertiesFile& settings;
    Hooks hooks;
    // Shared with background jobs, so a job finishing after the window closes is safe.
    std::shared_ptr<cloud::Client> client;
    String pendingPushMessage;   // what you typed for a push that someone else beat
    std::shared_ptr<cloud::LocalStore> store;
    ThreadPool worker { 1 };
    ThreadPool apiWorker { 2 };
    std::atomic<bool> busy { false };
    std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>> (false);
    String activity;                     // what the cloud is doing right now, for the status label
    String waitingHead, waitingText;     // a newer revision on the server, if any
    std::unique_ptr<StatusLabel> status;
    Component::SafePointer<DocumentWindow> history, library, friends;
    String songRole;                     // this song: owner, editor or viewer ("" until known)
    /** The tracks the owner lets you push (from the server), or none: no limit. Updates
        ctx.pushTracks and tells the app. */
    void setPushTracks (const var& tracks);
    void pushYourTracks (const String& message, int attempt);
    String songProjectName;
    bool lostAccess = false;             // the server says we can't open this song anymore
    bool oldServer = false;              // a Lathe Cloud server without /v2 (projects, friends)
    int friendRequests = 0;
    String planName;
    int revisionsKept = 0;
    bool showingNotices = false;
    int pollCountdown = 0;
    String menuState;                    // what the Cloud menu shows, to rebuild it only when it changes
    bool accountActive = false;          // we're showing someone's account (cleared by any sign-out)

    JUCE_DECLARE_WEAK_REFERENCEABLE (CloudController)
    JUCE_DECLARE_NON_COPYABLE (CloudController)
};
} // namespace lathe
