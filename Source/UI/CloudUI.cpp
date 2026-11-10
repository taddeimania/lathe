#include "CloudUI.h"
#include "../Cloud/ProjectDiff.h"
#include "CloudUIHelpers.h"

namespace lathe
{
using namespace cloudui;

namespace
{
enum MenuIds
{
    signInId = CloudController::menuBase, signOutId, openSharedId, shareId, inviteId,
    pushId, pullId, historyId, libraryId, friendsId, unlinkId, uploadId,
    parallelBase = CloudController::menuBase + 20   // + 1, 2, 4, 6, 8
};

constexpr int parallelChoices[] = { 1, 2, 4, 6, 8 };

} // namespace

//==============================================================================
class CloudController::StatusLabel : public Label
{
public:
    explicit StatusLabel (CloudController& c) : owner (c)
    {
        setFont (theme::font (12.0f));
        setColour (Label::textColourId, theme::textDim);
        setJustificationType (Justification::centredRight);
        setMouseCursor (MouseCursor::PointingHandCursor);
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (! e.mouseWasClicked())
            return;
        if (! owner.isSignedIn())
            owner.showSignIn();
        else if (owner.isLinked())
            owner.showHistory();
        else
            owner.shareProject();
    }

private:
    CloudController& owner;
};

//==============================================================================
class CloudController::SignInDialog : public Component
{
public:
    SignInDialog (CloudController& c, std::function<void()> afterSignIn)
        : owner (c), then (std::move (afterSignIn))
    {
        title.setText ("Sign in to share projects", dontSendNotification);
        title.setFont (theme::font (16.0f, true));
        addAndMakeVisible (title);

        for (auto* l : { &emailLabel, &passwordLabel, &newLabel, &againLabel })
        {
            l->setFont (theme::font (12.5f));
            l->setColour (Label::textColourId, theme::textDim);
            addAndMakeVisible (l);
        }
        emailLabel.setText ("Email", dontSendNotification);
        passwordLabel.setText ("Password", dontSendNotification);
        newLabel.setText ("New password", dontSendNotification);
        againLabel.setText ("Again", dontSendNotification);

        styleEditor (email);
        styleEditor (password, true);
        styleEditor (newPassword, true);
        styleEditor (again, true);
        email.setText (owner.settings.getValue ("cloudEmail"), false);
        for (auto* e : { &email, &password, &newPassword, &again })
        {
            e->onReturnKey = [this] { submit(); };
            addAndMakeVisible (e);
        }

        message.setFont (theme::font (12.5f));
        message.setJustificationType (Justification::topLeft);
        message.setColour (Label::textColourId, theme::textDim);
        message.setText ("New here? Ask the person who runs your Lathe cloud to add you. You'll get an email "
                         "with a temporary password.", dontSendNotification);
        addAndMakeVisible (message);

        signIn.setButtonText ("Sign In");
        signIn.onClick = [this] { submit(); };
        cancel.setButtonText ("Cancel");
        cancel.onClick = [this] { close(); };
        addAndMakeVisible (signIn);
        addAndMakeVisible (cancel);

        showNewPassword (false);
        setSize (420, 300);
    }

    void visibilityChanged() override
    {
        if (isShowing())
            (email.isEmpty() ? email : password).grabKeyboardFocus();
    }

    void paint (Graphics& g) override   { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        title.setBounds (r.removeFromTop (26));
        r.removeFromTop (10);
        auto row = [&] (Label& l, TextEditor& e)
        {
            if (! e.isVisible())
                return;
            auto line = r.removeFromTop (30);
            l.setBounds (line.removeFromLeft (110));
            e.setBounds (line);
            r.removeFromTop (8);
        };
        row (emailLabel, email);
        row (passwordLabel, password);
        row (newLabel, newPassword);
        row (againLabel, again);

        auto buttons = r.removeFromBottom (30);
        cancel.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        signIn.setBounds (buttons.removeFromRight (110));
        r.removeFromBottom (8);
        message.setBounds (r);
    }

private:
    void showNewPassword (bool show)
    {
        for (Component* c : { (Component*) &newLabel, (Component*) &newPassword, (Component*) &againLabel, (Component*) &again })
            c->setVisible (show);
        resized();
    }

    void say (const String& text, bool error)
    {
        message.setText (text, dontSendNotification);
        message.setColour (Label::textColourId, error ? theme::record : theme::textDim);
    }

    void submit()
    {
        const auto mail = email.getText().trim();
        const auto pass = password.getText();
        if (mail.isEmpty() || pass.isEmpty())
        {
            say ("Enter your email and password.", true);
            return;
        }

        String chosen;
        if (newPassword.isVisible())
        {
            if (newPassword.getText().length() < 10)
            {
                say ("Choose a password of at least 10 characters, with a number in it.", true);
                return;
            }
            if (newPassword.getText() != again.getText())
            {
                say ("The two new passwords don't match.", true);
                return;
            }
            chosen = newPassword.getText();
        }

        signIn.setEnabled (false);
        say ("Signing in...", false);
        auto result = std::make_shared<Result> (Result::ok());
        auto needsNew = std::make_shared<bool> (false);
        Component::SafePointer<SignInDialog> safe (this);
        auto api = owner.client;

        owner.run ("Signing in", [api, mail, pass, chosen, result, needsNew]
        {
            *result = api->signIn (mail, pass, [chosen, needsNew]
            {
                *needsNew = true;
                return chosen;
            });
        },
        [safe, result, needsNew, mail]
        {
            if (safe == nullptr)
                return;
            safe->signIn.setEnabled (true);
            if (result->wasOk())
            {
                safe->owner.settings.setValue ("cloudEmail", mail);
                safe->owner.ctx.showStatus ("Signed in as " + mail);
                auto next = safe->then;
                safe->close();
                if (next)
                    next();
                return;
            }
            if (*needsNew && ! safe->newPassword.isVisible())
            {
                safe->showNewPassword (true);
                safe->say ("First sign-in: choose your own password (at least 10 characters, with a number).", false);
                safe->newPassword.grabKeyboardFocus();
                return;
            }
            safe->say (result->getErrorMessage(), true);
        });
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<DialogWindow>())
            w->exitModalState (0);
    }

    CloudController& owner;
    std::function<void()> then;
    Label title, emailLabel, passwordLabel, newLabel, againLabel, message;
    TextEditor email, password, newPassword, again;
    TextButton signIn, cancel;
};

//==============================================================================
/** Lists the cloud projects you're in; double-click or Open picks one. */
//==============================================================================
/** The project's revisions: pushed, pulled and local. Select one to see what it
    changed and what opening it would change; open any of them. */
class CloudController::HistoryWindow : public DocumentWindow
{
public:
    explicit HistoryWindow (CloudController& c)
        : DocumentWindow ("History", theme::panel, DocumentWindow::closeButton), content (new Content (c))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (560, 360, 1600, 1400);
        centreWithSize (760, 520);
    }

    void refresh()                      { content->refresh(); }
    void closeButtonPressed() override  { delete this; }

    class Content : public Component, private ListBoxModel
    {
    public:
        explicit Content (CloudController& c) : owner (c)
        {
            list.setModel (this);
            list.setRowHeight (42);
            list.setColour (ListBox::backgroundColourId, theme::lane);
            addAndMakeVisible (list);

            details.setMultiLine (true);
            details.setReadOnly (true);
            details.setCaretVisible (false);
            details.setScrollbarsShown (true);
            details.setFont (theme::font (13.0f));
            details.setColour (TextEditor::backgroundColourId, theme::panel);
            details.setColour (TextEditor::outlineColourId, theme::border);
            addAndMakeVisible (details);

            openButton.setButtonText ("Open This Revision");
            openButton.onClick = [this] { openSelected(); };
            pullButton.setButtonText ("Pull");
            pullButton.setTooltip ("Get revisions your collaborators pushed");
            pullButton.onClick = [this] { owner.pull(); };
            pushButton.setButtonText ("Push...");
            pushButton.setTooltip ("Share what you have open as a new revision");
            pushButton.onClick = [this] { owner.push(); };
            keepButton.onClick = [this]
            {
                const int row = list.getSelectedRow();
                if (isPositiveAndBelow (row, (int) revisions.size()))
                    owner.setPinned (revisions[(size_t) row].id, ! revisions[(size_t) row].pinned);
            };
            for (auto* b : { &openButton, &pullButton, &pushButton, &keepButton })
                addAndMakeVisible (b);

            note.setFont (theme::font (12.0f));
            note.setColour (Label::textColourId, theme::textDim);
            note.setText ("Opening a revision never loses work: what you have open is kept here as \"Local\" first.",
                          dontSendNotification);
            addAndMakeVisible (note);

            refresh();
            setSize (760, 520);
        }

        void refresh()
        {
            const auto selectedId = isPositiveAndBelow (list.getSelectedRow(), (int) revisions.size())
                                        ? revisions[(size_t) list.getSelectedRow()].id : String();
            revisions.clear();
            base = {};
            if (auto* s = owner.getStore())
            {
                revisions = s->getRevisions();
                base = s->getBase();
            }
            current = cloud::Snapshot::capture (owner.getContext().edit).project;
            list.updateContent();

            int row = 0;
            for (size_t i = 0; i < revisions.size(); ++i)
                if (revisions[i].id == selectedId)
                    row = (int) i;
            list.selectRow (revisions.empty() ? -1 : row);
            showDetails();
            pullButton.setEnabled (owner.isSignedIn() || owner.screenshotMode);
            pushButton.setEnabled (owner.isSignedIn() || owner.screenshotMode);
            repaint();
        }

        void paint (Graphics& g) override   { g.fillAll (theme::panel); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            auto bottom = r.removeFromBottom (30);
            pushButton.setBounds (bottom.removeFromRight (90));
            bottom.removeFromRight (6);
            pullButton.setBounds (bottom.removeFromRight (80));
            bottom.removeFromRight (6);
            openButton.setBounds (bottom.removeFromRight (160));
            bottom.removeFromRight (6);
            keepButton.setBounds (bottom.removeFromRight (120));
            bottom.removeFromRight (10);
            note.setBounds (bottom);
            r.removeFromBottom (10);
            list.setBounds (r.removeFromLeft (jmax (260, r.getWidth() * 2 / 5)));
            r.removeFromLeft (10);
            details.setBounds (r);
        }

    private:
        int getNumRows() override   { return (int) revisions.size(); }

        void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
        {
            if (! isPositiveAndBelow (row, (int) revisions.size()))
                return;
            const auto& r = revisions[(size_t) row];
            if (selected)
                g.fillAll (theme::headerSel);

            const bool isBase = r.id == base;
            g.setColour (r.local ? theme::textDim : theme::accent);
            g.setFont (theme::font (13.0f, true));
            g.drawText (r.label(), 10, 4, 54, 18, Justification::centredLeft);

            g.setColour (theme::text);
            g.setFont (theme::font (13.0f));
            g.drawText (r.message.isNotEmpty() ? r.message : String ("(no description)"), 64, 4, w - 74, 18,
                        Justification::centredLeft, true);

            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            String sub = shortName (r.author) + "  ·  " + timeAgo (r.createdAt);
            if (r.local)  sub << "  ·  not pushed";
            if (r.pinned) sub << "  ·  kept";
            if (! r.local && ! r.inCloud) sub << "  ·  only on this computer";
            if (isBase)   sub << "  ·  you started from this";
            g.drawText (sub, 64, 22, w - 74, 16, Justification::centredLeft, true);

            g.setColour (theme::border);
            g.drawHorizontalLine (h - 1, 0.0f, (float) w);
        }

        void selectedRowsChanged (int) override        { showDetails(); }
        void listBoxItemDoubleClicked (int, const MouseEvent&) override   { openSelected(); }

        void showDetails()
        {
            const int row = list.getSelectedRow();
            openButton.setEnabled (isPositiveAndBelow (row, (int) revisions.size()) && ! owner.isBusy());
            const bool pushed = isPositiveAndBelow (row, (int) revisions.size()) && ! revisions[(size_t) row].local
                                && revisions[(size_t) row].inCloud;
            keepButton.setEnabled (pushed && (owner.isSignedIn() || owner.screenshotMode) && ! owner.isBusy());
            keepButton.setButtonText (pushed && revisions[(size_t) row].pinned ? "Don't Keep" : "Keep");
            const int n = owner.getRevisionsKept();
            keepButton.setTooltip ("The cloud keeps each song's newest " + (n > 0 ? String (n) : String ("few"))
                                   + " revisions. Keep one (a mix you sent, say) and it stays however many come after.");
            if (! isPositiveAndBelow (row, (int) revisions.size()))
            {
                details.setText (revisions.empty() ? "No revisions yet. Push to make the first one." : String(), false);
                return;
            }

            const auto& r = revisions[(size_t) row];
            String text;
            text << r.label() << (r.local ? "  (kept on this computer, not pushed)" : String())
                 << (! r.local && ! r.inCloud ? "  (no longer in the cloud: only on this computer)" : String())
                 << (r.pinned ? "  (kept in the cloud)" : String()) << "\n"
                 << r.author << ",  " << Time (r.createdAt).formatted ("%d %b %Y %H:%M") << "\n";
            if (r.message.isNotEmpty())
                text << "\n" << r.message << "\n";
            text << "\nWhat it changed:\n" << (r.summary.isNotEmpty() ? r.summary : String ("(not recorded)")) << "\n";

            if (auto* s = owner.getStore())
            {
                const auto snapshot = s->readSnapshot (r.snapshot);
                if (snapshot.isObject())
                {
                    const auto diff = cloud::describeChanges (current, snapshot);
                    text << "\nOpening it would change what you have open:\n"
                         << (diff.isEmpty() ? String ("Nothing: it's the same as what you have open.") : diff.joinIntoString ("\n"));
                }
                else
                {
                    text << "\n(Not downloaded yet: pull to get it.)";
                }
            }
            details.setText (text, false);
        }

        void openSelected()
        {
            const int row = list.getSelectedRow();
            if (isPositiveAndBelow (row, (int) revisions.size()) && ! owner.isBusy())
                owner.openRevision (revisions[(size_t) row]);
        }

        CloudController& owner;
        std::vector<cloud::Revision> revisions;
        String base;
        var current;
        ListBox list;
        TextEditor details;
        TextButton openButton, pullButton, pushButton, keepButton { "Keep" };
        Label note;
    };

private:
    Content* content;
};

std::unique_ptr<Component> CloudController::makeHistoryView()
{
    return std::make_unique<HistoryWindow::Content> (*this);
}

//==============================================================================
CloudController::CloudController (AppContext& c, PropertiesFile& s, Hooks h)
    : ctx (c), settings (s), hooks (std::move (h))
{
    status = std::make_unique<StatusLabel> (*this);
    makeClient();
    startTimer (1000);
}

CloudController::~CloudController()
{
    stopTimer();
    *cancelFlag = true;
    worker.removeAllJobs (true, 10000);
    if (history != nullptr)
        delete history.getComponent();
}

void CloudController::makeClient()
{
    client = std::make_shared<cloud::Client> (cloud::Config::load());
    WeakReference<CloudController> weak (this);
    client->onSessionChanged = [weak] (const cloud::Session& s)
    {
        MessageManager::callAsync ([weak, s]
        {
            if (auto* self = weak.get())
            {
                self->settings.setValue ("cloudRefreshToken", s.refreshToken);
                if (s.email.isNotEmpty())
                    self->settings.setValue ("cloudEmail", s.email);
                self->updateStatus();
            }
        });
    };
    client->setParallelTransfers (settings.getIntValue ("cloudParallel", 4));
    restoreSession();
    updateStatus();
}

void CloudController::restoreSession()
{
    const auto refresh = settings.getValue ("cloudRefreshToken");
    if (refresh.isNotEmpty())
    {
        cloud::Session s;
        s.email = settings.getValue ("cloudEmail");
        s.refreshToken = refresh;
        client->setSession (s);
    }
}

Component& CloudController::getStatusComponent()
{
    return *status;
}

bool CloudController::isLinked() const
{
    return store != nullptr && store->isLinked();
}

String CloudController::getSongId() const
{
    return isLinked() ? store->getProjectId() : String();
}

String CloudController::getEmail() const
{
    return isSignedIn() ? client->getSession().email : String();
}

bool CloudController::isUnfinishedShare() const
{
    // Linked to a cloud project but never pushed: sharing was interrupted.
    return store != nullptr && store->getBase().isEmpty() && store->getRevisions().empty();
}

void CloudController::projectChanged (bool force)
{
    const auto file = ctx.edit.projectFile;
    const bool moved = (store == nullptr) != (file == File()) || (store != nullptr && store->getProjectFile() != file);
    if (! force && ! moved)
        return;
    if (moved)
        store = file != File() ? std::make_shared<cloud::LocalStore> (file) : nullptr;
    waitingHead = waitingText = {};
    if (moved || force)
    {
        songRole = songProjectName = {};
        lostAccess = false;
    }
    pollCountdown = 2;   // check for news shortly after opening
    updateStatus();
    if (history != nullptr)
        if (auto* w = dynamic_cast<HistoryWindow*> (history.getComponent()))
            w->refresh();
}

void CloudController::updateStatus()
{
    String text;
    if (! client->getConfig().isValid())
        text = {};
    else if (busy && activity.isNotEmpty())
        text = "Cloud: " + activity;
    else if (! isLinked())
        text = isSignedIn() ? "Cloud: not shared" : "Cloud: sign in";
    else
    {
        text = "Cloud: " + store->getCloudName();
        if (auto base = store->getRevision (store->getBase()))
            text << "  " << base->label();
        else if (isUnfinishedShare())
            text << "  ·  not uploaded yet";
        if (! isSignedIn())
            text << "  (sign in)";
        else if (lostAccess)
            text << "  ·  not shared with you anymore";
        else if (waitingHead.isNotEmpty())
            text << "  ·  new revision waiting";
        else if (songRole == "viewer")
            text << "  ·  view only";
    }
    const bool attention = busy || waitingHead.isNotEmpty() || (isLinked() && (isUnfinishedShare() || lostAccess));
    status->setText (text, dontSendNotification);
    status->setColour (Label::textColourId, attention ? theme::accent : theme::textDim);
    status->setTooltip (busy ? String ("Working... Keep Lathe open until this finishes.")
                             : isLinked() && lostAccess ? String ("The owner stopped sharing this song with you, moved it or deleted it. Your copy here is "
                                                                  "yours: keep working on it, or choose Cloud > Disconnect This Song.")
                             : isLinked() && isUnfinishedShare() ? String ("Sharing didn't finish. Cloud > Push uploads the rest; files already sent aren't sent again.")
                             : songRole == "viewer" ? String ("You can pull new versions of this song but not push: the owner shared it view-only.")
                             : waitingText.isNotEmpty() ? waitingText : String ("Click for the project's history"));
}

void CloudController::addMenuItems (PopupMenu& m)
{
    const bool configured = client->getConfig().isValid();
    const bool signedIn = isSignedIn();
    const bool linked = isLinked();
    const bool idle = ! busy;

    if (signedIn)
    {
        m.addItem (PopupMenu::Item ("Signed in as " + client->getSession().email
                                    + (planName.isNotEmpty() ? "  (" + planName + ")" : String())).setEnabled (false));
        m.addItem (signOutId, "Sign Out", idle);
    }
    else
    {
        m.addItem (signInId, "Sign In...", configured && idle);
    }
    m.addSeparator();
    m.addItem (libraryId, "Projects...", configured);
    m.addItem (friendsId, friendRequests > 0 ? "Friends (" + String (friendRequests) + (friendRequests == 1 ? " request)..." : " requests)...")
                                             : String ("Friends..."), configured);
    m.addSeparator();
    if (! linked)
        m.addItem (uploadId, "Put This Song in the Cloud...", configured && idle);
    else if (lostAccess)
        m.addItem (PopupMenu::Item ("\"" + store->getCloudName() + "\" isn't shared with you anymore").setEnabled (false));
    else if (songRole == "owner")
        m.addItem (shareId, "Share \"" + store->getCloudName() + "\"...", signedIn);
    const bool viewer = songRole == "viewer";
    m.addItem (pushId, viewer ? String ("Push (you can only view this song)")
                              : linked && isUnfinishedShare() ? "Push (finish sharing)..." : "Push...",
               linked && signedIn && idle && ! viewer && ! lostAccess);
    m.addItem (pullId, waitingHead.isNotEmpty() ? "Pull (new revision waiting)" : "Pull", linked && signedIn && idle && ! lostAccess);
    m.addItem (historyId, "History...", linked);
    if (linked)
        m.addItem (unlinkId, "Disconnect This Song from the Cloud...", idle);
    m.addSeparator();
    PopupMenu parallel;
    for (int n : parallelChoices)
        parallel.addItem (parallelBase + n, n == 1 ? String ("1 (one file at a time)") : String (n),
                          true, client->getParallelTransfers() == n);
    m.addSubMenu ("Transfers at Once", parallel);
}

bool CloudController::handleMenuItem (int id)
{
    switch (id)
    {
        case signInId:      showSignIn(); return true;
        case signOutId:     signOut(); return true;
        case openSharedId:  showLibrary(); return true;
        case libraryId:     showLibrary(); return true;
        case friendsId:     showFriends(); return true;
        case unlinkId:      unlinkSong(); return true;
        case uploadId:      uploadSong(); return true;
        case shareId:       shareProject(); return true;
        case inviteId:      invite(); return true;
        case pushId:        push(); return true;
        case pullId:        pull(); return true;
        case historyId:     showHistory(); return true;
        default:
            if (id > parallelBase && id <= parallelBase + 8)
            {
                const int n = id - parallelBase;
                settings.setValue ("cloudParallel", n);
                client->setParallelTransfers (n);
                hooks.setStatus (n == 1 ? String ("Cloud transfers: one at a time.")
                                        : "Cloud transfers: up to " + String (n) + " at once.");
                return true;
            }
            return false;
    }
}

//==============================================================================
void CloudController::run (const String& what, std::function<void()> work, std::function<void()> done)
{
    busy = true;
    *cancelFlag = false;
    activity = what;
    if (what.isNotEmpty())
        hooks.setStatus (what + "...");
    updateStatus();
    WeakReference<CloudController> weak (this);
    worker.addJob ([weak, job = std::move (work), onDone = std::move (done)]() mutable
    {
        job();
        MessageManager::callAsync ([weak, callback = std::move (onDone)]
        {
            if (auto* self = weak.get())
            {
                self->busy = false;
                self->activity = {};
                self->updateStatus();
                if (callback)
                    callback();
            }
        });
    });
}

cloud::Progress CloudController::progressReporter()
{
    WeakReference<CloudController> weak (this);
    auto flag = cancelFlag;
    auto last = std::make_shared<std::atomic<uint32>> (0);
    return [weak, flag, last] (const String& what, double p)
    {
        const auto now = Time::getMillisecondCounter();
        if (now - last->load() > 150 || p >= 1.0)
        {
            last->store (now);
            const auto text = what + (p > 0.0 && p < 1.0 ? "  " + String (roundToInt (p * 100.0)) + "%" : String());
            MessageManager::callAsync ([weak, text]
            {
                if (auto* self = weak.get())
                {
                    self->activity = text;
                    self->hooks.setStatus (text);
                    self->updateStatus();
                }
            });
        }
        return ! flag->load();
    };
}

void CloudController::ensureReady (std::function<void()> then)
{
    if (! isSignedIn())
    {
        showSignIn ([this, then] { ensureReady (then); });
        return;
    }
    if (ctx.edit.projectFile == File() || ctx.edit.dirty)
    {
        WeakReference<CloudController> weak (this);
        hooks.saveProject ([weak, then] (bool ok)
        {
            if (auto* self = weak.get(); self != nullptr && ok)
            {
                self->projectChanged();
                then();
            }
        });
        return;
    }
    if (store == nullptr)
        projectChanged();
    then();
}

//==============================================================================
void CloudController::showSignIn (std::function<void()> then)
{
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new SignInDialog (*this, std::move (then)));
    o.dialogTitle = "Sign In";
    o.dialogBackgroundColour = theme::panel;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.escapeKeyTriggersCloseButton = true;
    o.launchAsync();
}

void CloudController::signOut()
{
    client->signOut();
    settings.removeValue ("cloudRefreshToken");
    hooks.setStatus ("Signed out of the cloud.");
    updateStatus();
}

void CloudController::shareProject()
{
    if (! isLinked())
        uploadSong();
    else
        showLibrary (store->getProjectId());
}

void CloudController::invite()
{
    shareProject();
}

void CloudController::uploadSong()
{
    if (isLinked())
    {
        showLibrary (store->getProjectId());
        return;
    }
    ensureReady ([this]
    {
        // Which of your projects does it go in? (Ask the server for them first.)
        request ("GET", "/v2/library", {}, [this] (const var& lib, const cloud::ApiError& err)
        {
            if (err.status != 0)
            {
                alert ("Couldn't reach Lathe Cloud", isOldServer (err) ? String ("Projects and friends aren't available on Lathe Cloud yet. Try again later.") : err.message);
                return;
            }
            Array<var> mine;
            if (auto* a = lib["projects"].getArray())
                for (auto& p : *a)
                    if (p["role"].toString() == "owner")
                        mine.add (p);

            auto* w = new AlertWindow ("Put this song in the cloud",
                                       "It goes in one of your projects. Share the project (or just this song) with friends "
                                       "from Cloud > Projects.", MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", ctx.edit.getName(), "Song name");
            StringArray names;
            for (auto& p : mine)
                names.add (p["name"].toString());
            names.add ("New project...");
            w->addComboBox ("project", names, "Project");
            if (auto* box = w->getComboBoxComponent ("project"))
                box->setSelectedItemIndex (0, dontSendNotification);
            w->addTextEditor ("newName", ctx.edit.getName(), "New project's name (if new)");
            w->addButton ("Upload", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

            WeakReference<CloudController> weak (this);
            w->enterModalState (true, ModalCallbackFunction::create ([weak, w, mine] (int r)
            {
                const auto name = w->getTextEditorContents ("name").trim();
                const int index = w->getComboBoxComponent ("project")->getSelectedItemIndex();
                const auto newName = w->getTextEditorContents ("newName").trim();
                w->setVisible (false);
                MessageManager::callAsync ([w] { delete w; });
                auto* self = weak.get();
                if (self == nullptr || r != 1)
                    return;
                const bool makeNew = index >= mine.size();
                if (name.isEmpty() || (makeNew && newName.isEmpty()))
                    return;
                const auto projectId = makeNew ? String() : mine[index]["id"].toString();

                auto snapshot = std::make_shared<cloud::Snapshot> (cloud::Snapshot::capture (self->ctx.edit));
                auto result = std::make_shared<Result> (Result::ok());
                auto outcome = std::make_shared<cloud::PushOutcome>();
                auto c = self->client;
                auto s = self->store;
                auto progress = self->progressReporter();

                self->hooks.setStatus ("Uploading \"" + name + "\". Keep Lathe open until it finishes; large songs take a while.");
                self->run ("Uploading", [c, s, name, projectId, newName, snapshot, result, outcome, progress]
                {
                    auto folder = projectId;
                    if (folder.isEmpty())
                    {
                        DynamicObject::Ptr body = new DynamicObject();
                        body->setProperty ("name", newName);
                        cloud::ApiError e;
                        folder = c->call ("POST", "/v2/projects", var (body.get()), e)["id"].toString();
                        if (e.status != 0)
                        {
                            *result = Result::fail (e.message);
                            return;
                        }
                    }
                    *result = cloud::createCloudSong (*c, *s, name, folder);
                    if (result->wasOk())
                        *outcome = cloud::push (*c, *s, *snapshot, {}, "First version", progress);
                },
                [weak, result, outcome, name]
                {
                    auto* me = weak.get();
                    if (me == nullptr)
                        return;
                    if (result->failed() || outcome->result.failed())
                    {
                        alert ("Couldn't upload the song", result->failed() ? result->getErrorMessage() : outcome->result.getErrorMessage());
                        return;
                    }
                    me->songRole = "owner";
                    me->hooks.setStatus ("\"" + name + "\" is in the cloud (r1). Share it from Cloud > Projects.");
                    me->updateStatus();
                    me->refreshWindows();
                    me->showLibrary (me->store->getProjectId());
                });
            }), false);
        });
    });
}

void CloudController::unlinkSong()
{
    if (! isLinked())
        return;
    const auto name = store->getCloudName();
    WeakReference<CloudController> weak (this);
    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::NoIcon)
                                .withTitle ("Disconnect \"" + name + "\" from the cloud?")
                                .withMessage ("This copy stops pulling and pushing. Nothing is deleted: not this project, not its "
                                              "History, and not the song in the cloud.")
                                .withButton ("Disconnect")
                                .withButton ("Cancel"),
                            [weak, name] (int button)
    {
        auto* self = weak.get();
        if (self == nullptr || button != 1 || self->store == nullptr)
            return;
        self->store->unlink();
        self->projectChanged();
        self->hooks.setStatus ("\"" + name + "\" is no longer linked to the cloud. Your project is unchanged.");
    });
}

void CloudController::push()
{
    if (! isLinked())
    {
        uploadSong();
        return;
    }
    if (lostAccess)
    {
        alert ("Can't push", "\"" + store->getCloudName() + "\" isn't shared with you anymore. Your copy here is yours to keep.");
        return;
    }
    if (songRole == "viewer")
    {
        alert ("Can't push", "The owner shared \"" + store->getCloudName() + "\" with you view-only: you can pull new versions, "
                             "but not push. Ask them to make you an editor.");
        return;
    }
    ensureReady ([this]
    {
        const bool finishing = isUnfinishedShare();
        askForText (finishing ? "Finish sharing" : "Push",
                    finishing ? "Sharing \"" + store->getCloudName() + "\" didn't finish. This uploads the rest (files already "
                                "sent aren't sent again). Keep Lathe open until it's done.\n\nDescription:"
                              : "Describe what you changed (your collaborators see this in History):",
                    finishing ? String ("First version") : String(),
                    [this] (const String& message)
        {
            doPush (store->getBase(), message.trim());
        });
    });
}

void CloudController::doPush (const String& parent, const String& message)
{
    auto snapshot = std::make_shared<cloud::Snapshot> (cloud::Snapshot::capture (ctx.edit));
    auto outcome = std::make_shared<cloud::PushOutcome>();
    auto c = client;
    auto s = store;
    auto progress = progressReporter();

    run ("Pushing", [c, s, snapshot, parent, message, outcome, progress]
    {
        *outcome = cloud::push (*c, *s, *snapshot, parent, message, progress);
    },
    [this, outcome, message]
    {
        if (outcome->conflict)
        {
            const auto theirs = outcome->theirHead;
            AlertWindow::showAsync (MessageBoxOptions()
                                        .withIconType (MessageBoxIconType::NoIcon)
                                        .withTitle ("Someone pushed first")
                                        .withMessage ("A collaborator pushed a newer revision since you started from yours.\n\n"
                                                      "Pull theirs first to look at it (your work stays in History), or push yours "
                                                      "on top of it: theirs stays in History, and yours becomes the latest.")
                                        .withButton ("Pull Theirs First")
                                        .withButton ("Push Mine on Top")
                                        .withButton ("Cancel"),
                                    [this, theirs, message] (int button)
            {
                if (button == 1)       pull();
                else if (button == 2)  pushOnTop (theirs, message);
            });
            return;
        }
        if (outcome->result.failed())
        {
            alert ("Couldn't push", outcome->result.getErrorMessage());
            return;
        }
        const auto& r = outcome->revision;
        hooks.setStatus ("Pushed " + r.label() + ": " + r.summary.upToFirstOccurrenceOf ("\n", false, false));
        waitingHead = waitingText = {};
        updateStatus();
        if (history != nullptr)
            if (auto* w = dynamic_cast<HistoryWindow*> (history.getComponent()))
                w->refresh();
    });
}

void CloudController::pushOnTop (const String& theirHead, const String& message)
{
    // Get their revision first (so the summary compares against it), then push on top.
    auto c = client;
    auto s = store;
    auto pulled = std::make_shared<cloud::PullOutcome>();
    auto progress = progressReporter();
    run ("Getting their revision", [c, s, pulled, progress] { *pulled = cloud::pull (*c, *s, progress); },
         [this, theirHead, message, pulled]
    {
        if (pulled->result.failed())
        {
            alert ("Couldn't get their revision", pulled->result.getErrorMessage());
            return;
        }
        doPush (pulled->head.isNotEmpty() ? pulled->head : theirHead, message);
    });
}

void CloudController::pull()
{
    if (! isLinked())
        return;
    ensureReady ([this]
    {
        auto c = client;
        auto s = store;
        auto outcome = std::make_shared<cloud::PullOutcome>();
        auto progress = progressReporter();
        run ("Pulling", [c, s, outcome, progress] { *outcome = cloud::pull (*c, *s, progress); },
             [this, outcome]
        {
            if (outcome->result.failed())
            {
                alert ("Couldn't pull", outcome->result.getErrorMessage());
                return;
            }
            waitingHead = waitingText = {};
            updateStatus();
            if (history != nullptr)
                if (auto* w = dynamic_cast<HistoryWindow*> (history.getComponent()))
                    w->refresh();

            if (outcome->newRevisions.empty())
            {
                hooks.setStatus ("Up to date: nothing new in the cloud.");
                return;
            }

            const auto newest = outcome->newRevisions.front();
            const int count = (int) outcome->newRevisions.size();
            hooks.setStatus ("Pulled " + String (count) + " new revision" + (count == 1 ? "" : "s") + ". Nothing you have open has changed.");
            if (newest.id == store->getBase())
                return;

            AlertWindow::showAsync (MessageBoxOptions()
                                        .withIconType (MessageBoxIconType::NoIcon)
                                        .withTitle (newest.label() + " from " + shortName (newest.author))
                                        .withMessage ((newest.message.isNotEmpty() ? newest.message + "\n\n" : String())
                                                      + newest.summary
                                                      + "\n\nOpen it now? What you have open is kept in History.")
                                        .withButton ("Open " + newest.label())
                                        .withButton ("Show History")
                                        .withButton ("Later"),
                                    [this, newest] (int button)
            {
                if (button == 1)       openRevision (newest);
                else if (button == 2)  showHistory();
            });
        });
    });
}

void CloudController::openRevision (const cloud::Revision& rev)
{
    if (store == nullptr || busy)
        return;

    auto snapshot = std::make_shared<cloud::Snapshot> (cloud::Snapshot::capture (ctx.edit));
    auto project = std::make_shared<var>();
    auto result = std::make_shared<Result> (Result::ok());
    auto kept = std::make_shared<cloud::Revision>();
    auto c = isSignedIn() ? client : std::shared_ptr<cloud::Client>();
    auto s = store;
    const auto author = isSignedIn() ? client->getSession().email : settings.getValue ("cloudEmail");
    auto progress = progressReporter();

    run ("Opening " + rev.label(), [c, s, rev, snapshot, project, result, kept, author, progress]
    {
        // Keep what's open first (unless a revision already has exactly this content).
        *kept = cloud::keepWorkingCopy (*s, *snapshot, author, "Your work before opening " + rev.label());
        *result = cloud::prepareRevision (c.get(), *s, rev, *project, progress);
    },
    [this, rev, project, result, kept]
    {
        if (result->failed())
        {
            alert ("Couldn't open " + rev.label(), result->getErrorMessage());
            return;
        }
        const auto file = ctx.edit.projectFile;
        if (! file.replaceWithText (JSON::toString (*project)))
        {
            alert ("Couldn't open " + rev.label(), "Couldn't write " + file.getFullPathName());
            return;
        }
        store->setBase (rev.local ? rev.parent : rev.id);
        hooks.openProject (file);
        projectChanged();
        hooks.setStatus ("Opened " + rev.label() + (kept->local && kept->id != rev.id
                                                        ? ". What you had open is kept in History as Local."
                                                        : String (".")));
    });
}

void CloudController::showHistory()
{
    if (history != nullptr)
    {
        if (auto* w = dynamic_cast<HistoryWindow*> (history.getComponent()))
            w->refresh();
        history->toFront (true);
        return;
    }
    if (! isLinked())
        return;
    auto* w = new HistoryWindow (*this);
    history = w;
    w->setVisible (true);
}

void CloudController::openSharedProject()
{
    showLibrary();
}

void CloudController::openCloudSong (const var& song)
{
    WeakReference<CloudController> weak (this);
    hooks.confirmDiscard ([weak, p = song]
    {
        auto* self = weak.get();
        if (self == nullptr)
            return;
        const auto id = p["id"].toString();
        const auto name = p["name"].toString();
        const auto folder = self->hooks.projectsDir().getChildFile (File::createLegalFileName (name));

        // Already here? Just open it.
        const auto file = folder.getChildFile (File::createLegalFileName (name) + ".lathe");
        if (file.existsAsFile() && cloud::LocalStore (file).getProjectId() == id)
        {
            self->hooks.openProject (file);
            self->projectChanged();
            self->pull();
            return;
        }

        const auto dest = folder.exists() ? folder.getNonexistentSibling (true) : folder;
        const auto projectFile = dest.getChildFile (dest.getFileName() + ".lathe");
        auto songStore = std::make_shared<cloud::LocalStore> (projectFile);
        auto outcome = std::make_shared<cloud::PullOutcome>();
        auto project = std::make_shared<var>();
        auto result = std::make_shared<Result> (Result::ok());
        auto c = self->client;
        auto progress = self->progressReporter();

        self->run ("Downloading \"" + name + "\"", [c, songStore, dest, id, name, outcome, project, result, progress]
        {
            dest.getChildFile ("Audio").createDirectory();
            songStore->link (id, name);
            *outcome = cloud::pull (*c, *songStore, progress);
            if (outcome->result.failed())
            {
                *result = outcome->result;
                return;
            }
            if (auto head = songStore->getRevision (outcome->head))
                *result = cloud::prepareRevision (c.get(), *songStore, *head, *project, progress);
            else
                *result = Result::fail ("\"" + name + "\" has no revisions yet. Ask its owner to push one.");
        },
        [weak, songStore, outcome, project, result, projectFile, dest, name]
        {
            auto* me = weak.get();
            if (me == nullptr)
                return;
            if (result->failed())
            {
                if (! project->isObject())
                    dest.deleteRecursively();
                alert ("Couldn't open \"" + name + "\"", result->getErrorMessage());
                return;
            }
            projectFile.replaceWithText (JSON::toString (*project));
            songStore->setBase (outcome->head);
            me->hooks.openProject (projectFile);
            me->projectChanged();
            me->hooks.setStatus ("Opened \"" + name + "\" from the cloud, in " + dest.getFullPathName());
        });
    });
}

//==============================================================================
void CloudController::timerCallback()
{
    // Every two minutes, quietly ask whether a collaborator pushed something.
    if (--pollCountdown > 0)
        return;
    pollCountdown = 120;
    if (isSignedIn())
        pollAccount();
    if (isLinked() && isSignedIn() && ! busy)
        poll();
}

void CloudController::request (const String& method, const String& path, const var& body,
                               std::function<void (const var&, const cloud::ApiError&)> done)
{
    auto c = client;
    WeakReference<CloudController> weak (this);
    apiWorker.addJob ([c, method, path, body, weak, onDone = std::move (done)]() mutable
    {
        cloud::ApiError err;
        // An empty POST still sends a body, so every platform's HTTP stack treats it as a POST.
        const auto sendBody = body.isVoid() && method != "GET" && method != "DELETE" ? var (new DynamicObject()) : body;
        auto resp = c->call (method, path, sendBody, err);
        MessageManager::callAsync ([weak, resp, err, callback = std::move (onDone)]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;
            if (self->isOldServer (err))
                self->oldServer = true;
            else if (err.status == 0)
                self->oldServer = false;
            if (callback)
                callback (resp, err);
        });
    });
}

bool CloudController::isOldServer (const cloud::ApiError& err) const
{
    // The old server answers "Not found." for routes it doesn't have (missing things say "No such ...").
    return err.status == 404 && err.message == "Not found.";
}

void CloudController::setPinned (const String& revisionId, bool pinned)
{
    if (! isLinked())
        return;
    ensureReady ([this, revisionId, pinned]
    {
        auto c = client;
        auto s = store;
        auto result = std::make_shared<Result> (Result::ok());
        run (pinned ? "Keeping the revision" : "Letting the revision go", [c, s, result, revisionId, pinned]
        {
            *result = cloud::setPinned (*c, *s, revisionId, pinned);
        },
        [this, result, pinned]
        {
            if (result->failed())
                alert (pinned ? "Couldn't keep it" : "Couldn't change it", result->getErrorMessage());
            else
                hooks.setStatus (pinned ? "Kept: the cloud keeps this revision however many come after it."
                                        : "The cloud will let this revision go once newer ones replace it.");
            if (history != nullptr)
                if (auto* w = dynamic_cast<HistoryWindow*> (history.getComponent()))
                    w->refresh();
        });
    });
}

void CloudController::pollAccount()
{
    request ("GET", "/me", {}, [this] (const var& resp, const cloud::ApiError& err)
    {
        if (err.status != 0)
            return;
        planName = resp["planName"].toString();
        revisionsKept = (int) resp["limits"]["revisions"];
    });
    request ("GET", "/v2/friends", {}, [this] (const var& resp, const cloud::ApiError& err)
    {
        if (err.status != 0)
            return;
        const int before = friendRequests;
        const auto* incoming = resp["incoming"].getArray();
        friendRequests = incoming != nullptr ? incoming->size() : 0;
        if (friendRequests > before && incoming != nullptr)
            hooks.setStatus ("Friend request from " + incoming->getLast()["email"].toString() + ". See Cloud > Friends.");
    });
    request ("GET", "/v2/notices", {}, [this] (const var& resp, const cloud::ApiError& err)
    {
        if (err.status == 0)
            if (auto* a = resp["notices"].getArray(); a != nullptr && ! a->isEmpty())
            {
                showNotices (*a);
                refreshWindows();   // what we can see may have changed
            }
    });
}

void CloudController::showNotices (const Array<var>& notices)
{
    if (showingNotices)
        return;
    showingNotices = true;

    String text;
    for (auto& n : notices)
    {
        text << (text.isEmpty() ? "" : "\n\n") << n["text"].toString();
        // Seen: the server can forget it.
        request ("DELETE", "/v2/notices/" + URL::addEscapeChars (n["id"].toString(), true), {}, {});
    }
    hooks.setStatus (notices.getFirst()["text"].toString());

    WeakReference<CloudController> weak (this);
    AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::NoIcon)
                                .withTitle ("Lathe Cloud").withMessage (text).withButton ("OK"),
                            [weak] (int) { if (auto* self = weak.get()) self->showingNotices = false; });

    if (isLinked() && isSignedIn() && ! busy)
        poll();   // our access to the open song may have changed
}


void CloudController::poll()
{
    auto c = client;
    const auto id = store->getProjectId();
    auto latest = std::make_shared<var>();
    auto err = std::make_shared<cloud::ApiError>();

    auto song = std::make_shared<var>();
    auto songErr = std::make_shared<cloud::ApiError>();

    busy = true;
    WeakReference<CloudController> weak (this);
    worker.addJob ([c, id, latest, err, song, songErr, weak]
    {
        // Our role (and whether we still have the song at all), then the newest revision.
        *song = c->call ("GET", "/v2/songs/" + id, {}, *songErr);
        *latest = c->call ("GET", "/projects/" + id + "/revisions?limit=1", {}, *err);
        MessageManager::callAsync ([weak, latest, err, song, songErr]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;
            self->busy = false;
            if (self->store == nullptr)
                return;

            if (songErr->status == 0)
            {
                self->songRole = (*song)["role"].toString();
                self->songProjectName = (*song)["projectName"].toString();
                self->lostAccess = false;
            }
            else if (self->isOldServer (*songErr))
            {
                self->oldServer = true;
                self->songRole = {};
            }
            else if (songErr->status == 404 && err->status == 404)
            {
                const bool news = ! self->lostAccess;
                self->lostAccess = true;
                self->waitingHead = self->waitingText = {};
                if (news)
                    self->hooks.setStatus ("\"" + self->store->getCloudName() + "\" isn't shared with you anymore. "
                                           "Your copy here is yours to keep.");
            }

            if (err->status != 0)
            {
                self->updateStatus();
                return;
            }
            // An empty list is normal (a project nobody has pushed to yet).
            const auto* revisions = (*latest)["revisions"].getArray();
            if (revisions == nullptr || revisions->isEmpty())
            {
                self->updateStatus();
                return;
            }
            const auto newest = revisions->getFirst();
            const auto headId = newest["id"].toString();
            if (headId.isNotEmpty() && ! self->store->hasRevision (headId))
            {
                const bool isNew = self->waitingHead != headId;
                self->waitingHead = headId;
                self->waitingText = "r" + String ((int) newest["seq"]) + " from " + shortName (newest["author"].toString())
                                    + (newest["message"].toString().isNotEmpty() ? ": " + newest["message"].toString() : String());
                if (isNew)
                    self->hooks.setStatus ("New in the cloud: " + self->waitingText + ". Cloud > Pull to get it.");
            }
            self->updateStatus();
        });
    });
}
} // namespace lathe
