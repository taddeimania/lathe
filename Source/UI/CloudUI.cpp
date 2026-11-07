#include "CloudUI.h"
#include "../Cloud/ProjectDiff.h"

namespace lathe
{
namespace
{
enum MenuIds
{
    signInId = CloudController::menuBase, signOutId, openSharedId, shareId, inviteId,
    pushId, pullId, historyId, settingsId
};

String timeAgo (int64 ms)
{
    const auto seconds = (Time::currentTimeMillis() - ms) / 1000;
    if (seconds < 60)       return "just now";
    if (seconds < 3600)     return String (seconds / 60) + " min ago";
    if (seconds < 86400)    return String (seconds / 3600) + " h ago";
    if (seconds < 7 * 86400) return String (seconds / 86400) + " days ago";
    return Time (ms).formatted ("%d %b %Y");
}

String shortName (const String& email)
{
    return email.upToFirstOccurrenceOf ("@", false, false);
}

void styleEditor (TextEditor& e, bool password = false)
{
    e.setFont (theme::font (14.0f));
    e.setIndents (8, 6);
    if (password)
        e.setPasswordCharacter ((juce_wchar) 0x2022);
}

void alert (const String& title, const String& message)
{
    AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::NoIcon)
                                .withTitle (title).withMessage (message).withButton ("OK"), nullptr);
}
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
        auto* client = owner.client.get();

        owner.run ("Signing in", [client, mail, pass, chosen, result, needsNew]
        {
            *result = client->signIn (mail, pass, [chosen, needsNew]
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
                auto then = safe->then;
                safe->close();
                if (then)
                    then();
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
class CloudController::ProjectPicker : public Component, private ListBoxModel
{
public:
    ProjectPicker (Array<var> p, std::function<void (const var&)> onPick)
        : projects (std::move (p)), pick (std::move (onPick))
    {
        heading.setText ("Shared projects you're in", dontSendNotification);
        heading.setFont (theme::font (15.0f, true));
        addAndMakeVisible (heading);

        list.setModel (this);
        list.setRowHeight (40);
        list.setColour (ListBox::backgroundColourId, theme::lane);
        addAndMakeVisible (list);
        if (! projects.isEmpty())
            list.selectRow (0);

        open.setButtonText ("Open...");
        open.onClick = [this] { choose (list.getSelectedRow()); };
        cancel.setButtonText ("Cancel");
        cancel.onClick = [this] { close(); };
        addAndMakeVisible (open);
        addAndMakeVisible (cancel);
        setSize (520, 380);
    }

    void paint (Graphics& g) override   { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        heading.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        auto buttons = r.removeFromBottom (30);
        cancel.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        open.setBounds (buttons.removeFromRight (110));
        r.removeFromBottom (10);
        list.setBounds (r);
    }

private:
    int getNumRows() override   { return projects.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
    {
        if (! isPositiveAndBelow (row, projects.size()))
            return;
        const auto& p = projects.getReference (row);
        if (selected)
            g.fillAll (theme::headerSel);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (p["name"].toString(), 10, 3, w - 20, 18, Justification::centredLeft, true);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        const int seq = (int) p["seq"];
        g.drawText ((seq > 0 ? "r" + String (seq) : String ("no revisions yet")) + "   ·   owner " + p["owner"].toString(),
                    10, 21, w - 20, 16, Justification::centredLeft, true);
        g.setColour (theme::border);
        g.drawHorizontalLine (h - 1, 0.0f, (float) w);
    }

    void listBoxItemDoubleClicked (int row, const MouseEvent&) override   { choose (row); }

    void choose (int row)
    {
        if (! isPositiveAndBelow (row, projects.size()))
            return;
        const auto p = projects[row];
        auto callback = pick;
        close();
        if (callback)
            callback (p);
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<DialogWindow>())
            w->exitModalState (0);
    }

    Array<var> projects;
    std::function<void (const var&)> pick;
    Label heading;
    ListBox list;
    TextButton open, cancel;
};

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

private:
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
            for (auto* b : { &openButton, &pullButton, &pushButton })
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
            pullButton.setEnabled (owner.isSignedIn());
            pushButton.setEnabled (owner.isSignedIn());
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
            if (! isPositiveAndBelow (row, (int) revisions.size()))
            {
                details.setText (revisions.empty() ? "No revisions yet. Push to make the first one." : String(), false);
                return;
            }

            const auto& r = revisions[(size_t) row];
            String text;
            text << r.label() << (r.local ? "  (kept on this computer, not pushed)" : String()) << "\n"
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
        TextButton openButton, pullButton, pushButton;
        Label note;
    };

    Content* content;
};

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
    cancelled = true;
    worker.removeAllJobs (true, 20000);
    if (history != nullptr)
        delete history.getComponent();
}

void CloudController::makeClient()
{
    client = std::make_unique<cloud::Client> (cloud::Config::load (settings));
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

void CloudController::projectChanged (bool force)
{
    const auto file = ctx.edit.projectFile;
    const bool moved = (store == nullptr) != (file == File()) || (store != nullptr && store->getProjectFile() != file);
    if (! force && ! moved)
        return;
    if (moved)
        store = file != File() ? std::make_unique<cloud::LocalStore> (file) : nullptr;
    waitingHead = waitingText = {};
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
    else if (! isLinked())
        text = isSignedIn() ? "Cloud: not shared" : "Cloud: sign in";
    else
    {
        text = "Cloud: " + store->getCloudName();
        if (auto base = store->getRevision (store->getBase()))
            text << "  " << base->label();
        if (! isSignedIn())
            text << "  (sign in)";
        else if (waitingHead.isNotEmpty())
            text << "  ·  new revision waiting";
    }
    status->setText (text, dontSendNotification);
    status->setColour (Label::textColourId, waitingHead.isNotEmpty() ? theme::accent : theme::textDim);
    status->setTooltip (waitingText.isNotEmpty() ? waitingText : String ("Click for the project's history"));
}

void CloudController::addMenuItems (PopupMenu& m)
{
    const bool configured = client->getConfig().isValid();
    const bool signedIn = isSignedIn();
    const bool linked = isLinked();
    const bool idle = ! busy;

    if (signedIn)
    {
        m.addItem (PopupMenu::Item ("Signed in as " + client->getSession().email).setEnabled (false));
        m.addItem (signOutId, "Sign Out", idle);
    }
    else
    {
        m.addItem (signInId, "Sign In...", configured && idle);
    }
    m.addSeparator();
    m.addItem (openSharedId, "Open Shared Project...", configured && idle);
    if (linked)
        m.addItem (inviteId, "Invite Someone to \"" + store->getCloudName() + "\"...", signedIn && idle);
    else
        m.addItem (shareId, "Share This Project...", configured && idle);
    m.addSeparator();
    m.addItem (pushId, "Push...", linked && signedIn && idle);
    m.addItem (pullId, waitingHead.isNotEmpty() ? "Pull (new revision waiting)" : "Pull", linked && signedIn && idle);
    m.addItem (historyId, "History...", linked);
    m.addSeparator();
    m.addItem (settingsId, configured ? "Cloud Settings..." : "Cloud Settings (set up first)...", idle);
}

bool CloudController::handleMenuItem (int id)
{
    switch (id)
    {
        case signInId:      showSignIn(); return true;
        case signOutId:     signOut(); return true;
        case openSharedId:  openSharedProject(); return true;
        case shareId:       shareProject(); return true;
        case inviteId:      invite(); return true;
        case pushId:        push(); return true;
        case pullId:        pull(); return true;
        case historyId:     showHistory(); return true;
        case settingsId:    showSettings(); return true;
        default:            return false;
    }
}

//==============================================================================
void CloudController::run (const String& what, std::function<void()> work, std::function<void()> done)
{
    busy = true;
    cancelled = false;
    if (what.isNotEmpty())
        hooks.setStatus (what + "...");
    WeakReference<CloudController> weak (this);
    worker.addJob ([weak, work = std::move (work), done = std::move (done)]() mutable
    {
        work();
        MessageManager::callAsync ([weak, done = std::move (done)]
        {
            if (auto* self = weak.get())
            {
                self->busy = false;
                self->updateStatus();
                if (done)
                    done();
            }
        });
    });
}

cloud::Progress CloudController::progressReporter()
{
    WeakReference<CloudController> weak (this);
    auto last = std::make_shared<std::atomic<uint32>> (0);
    return [weak, last] (const String& what, double p)
    {
        const auto now = Time::getMillisecondCounter();
        if (now - last->load() > 150 || p >= 1.0)
        {
            last->store (now);
            MessageManager::callAsync ([weak, what, p]
            {
                if (auto* self = weak.get())
                    self->hooks.setStatus (what + (p > 0.0 && p < 1.0 ? "  " + String (roundToInt (p * 100.0)) + "%" : String()));
            });
        }
        auto* self = weak.get();
        return self != nullptr && ! self->cancelled.load();
    };
}

void CloudController::ensureReady (std::function<void()> then)
{
    if (! client->getConfig().isValid())
    {
        showSettings();
        return;
    }
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
    if (! client->getConfig().isValid())
    {
        showSettings();
        return;
    }
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

void CloudController::showSettings()
{
    const auto current = client->getConfig();
    askForText ("Cloud settings",
                "Paste your Lathe cloud settings: the output of\n"
                "terraform -chdir=infra output -raw lathe_cloud_json\n"
                "(it looks like {\"apiUrl\": ..., \"region\": ..., \"clientId\": ...})",
                current.isValid() ? current.toJson() : String(),
                [this] (const String& text)
    {
        const auto c = cloud::Config::fromJson (text);
        if (! c.isValid())
        {
            alert ("Cloud settings", "That isn't it: it needs apiUrl, region and clientId.");
            return;
        }
        settings.setValue ("cloudConfig", c.toJson());
        settings.removeValue ("cloudRefreshToken");
        makeClient();
        hooks.setStatus ("Cloud settings saved. Sign in from the Cloud menu.");
    });
}

void CloudController::shareProject()
{
    if (isLinked())
    {
        invite();
        return;
    }
    ensureReady ([this]
    {
        askForText ("Share this project",
                    "Name it in the cloud. You can then invite people, and push and pull revisions.",
                    ctx.edit.getName(), [this] (const String& entered)
        {
            const auto name = entered.trim();
            if (name.isEmpty())
                return;
            auto snapshot = std::make_shared<cloud::Snapshot> (cloud::Snapshot::capture (ctx.edit));
            auto result = std::make_shared<Result> (Result::ok());
            auto outcome = std::make_shared<cloud::PushOutcome>();
            auto* c = client.get();
            auto* s = store.get();
            auto progress = progressReporter();

            run ("Sharing", [c, s, name, snapshot, result, outcome, progress]
            {
                *result = cloud::createCloudProject (*c, *s, name);
                if (result->wasOk())
                    *outcome = cloud::push (*c, *s, *snapshot, {}, "First version", progress);
            },
            [this, result, outcome, name]
            {
                if (result->failed() || outcome->result.failed())
                {
                    alert ("Couldn't share the project", result->failed() ? result->getErrorMessage() : outcome->result.getErrorMessage());
                    return;
                }
                hooks.setStatus ("Shared \"" + name + "\" (r1). Invite people from the Cloud menu.");
                updateStatus();
                invite();
            });
        });
    });
}

void CloudController::invite()
{
    if (! isLinked())
        return;
    askForText ("Invite someone to \"" + store->getCloudName() + "\"",
                "Their email. If they don't have a Lathe cloud account yet, the project shows up for them "
                "after they first sign in.", {}, [this] (const String& email)
    {
        if (email.trim().isEmpty())
            return;
        auto err = std::make_shared<cloud::ApiError>();
        auto resp = std::make_shared<var>();
        auto* c = client.get();
        const auto path = "/projects/" + store->getProjectId() + "/members";
        const auto address = email.trim();
        run ("Inviting " + address, [c, path, address, err, resp]
        {
            DynamicObject::Ptr body = new DynamicObject();
            body->setProperty ("email", address);
            *resp = c->call ("POST", path, var (body.get()), *err);
        },
        [this, err, resp, address]
        {
            if (err->status != 0)
                alert ("Couldn't invite " + address, err->message);
            else
                hooks.setStatus ((*resp)["status"].toString() == "added"
                                     ? address + " can now open this project (Cloud > Open Shared Project)."
                                     : address + " is invited: it'll appear once they sign in for the first time.");
        });
    });
}

void CloudController::push()
{
    if (! isLinked())
    {
        shareProject();
        return;
    }
    ensureReady ([this]
    {
        askForText ("Push", "Describe what you changed (your collaborators see this in History):", {},
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
    auto* c = client.get();
    auto* s = store.get();
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
    auto* c = client.get();
    auto* s = store.get();
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
        auto* c = client.get();
        auto* s = store.get();
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
    auto* c = isSignedIn() ? client.get() : nullptr;
    auto* s = store.get();
    const auto author = isSignedIn() ? client->getSession().email : settings.getValue ("cloudEmail");
    auto progress = progressReporter();

    run ("Opening " + rev.label(), [c, s, rev, snapshot, project, result, kept, author, progress]
    {
        // Keep what's open first (unless a revision already has exactly this content).
        *kept = cloud::keepWorkingCopy (*s, *snapshot, author, "Your work before opening " + rev.label());
        *result = cloud::prepareRevision (c, *s, rev, *project, progress);
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
    if (! client->getConfig().isValid())
    {
        showSettings();
        return;
    }
    if (! isSignedIn())
    {
        showSignIn ([this] { openSharedProject(); });
        return;
    }

    auto err = std::make_shared<cloud::ApiError>();
    auto resp = std::make_shared<var>();
    auto* c = client.get();
    run ("Looking up your shared projects", [c, err, resp] { *resp = c->call ("GET", "/projects", {}, *err); },
         [this, err, resp]
    {
        if (err->status != 0)
        {
            alert ("Couldn't list shared projects", err->message);
            return;
        }
        Array<var> projects;
        if (auto* a = (*resp)["projects"].getArray())
            projects = *a;
        if (projects.isEmpty())
        {
            alert ("No shared projects yet", "Nobody has shared a project with you yet. To share one of yours, open it and "
                                             "choose Cloud > Share This Project.");
            return;
        }

        DialogWindow::LaunchOptions o;
        WeakReference<CloudController> weak (this);
        o.content.setOwned (new ProjectPicker (projects, [weak] (const var& picked)
        {
          if (auto* first = weak.get())
            first->hooks.confirmDiscard ([weak, p = picked]
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
            auto store = std::make_shared<cloud::LocalStore> (projectFile);
            auto outcome = std::make_shared<cloud::PullOutcome>();
            auto project = std::make_shared<var>();
            auto result = std::make_shared<Result> (Result::ok());
            auto* c = self->client.get();
            auto progress = self->progressReporter();

            self->run ("Downloading \"" + name + "\"", [c, store, dest, id, name, outcome, project, result, progress]
            {
                dest.getChildFile ("Audio").createDirectory();
                store->link (id, name);
                *outcome = cloud::pull (*c, *store, progress);
                if (outcome->result.failed())
                {
                    *result = outcome->result;
                    return;
                }
                if (auto head = store->getRevision (outcome->head))
                    *result = cloud::prepareRevision (c, *store, *head, *project, progress);
                else
                    *result = Result::fail ("\"" + name + "\" has no revisions yet. Ask its owner to push one.");
            },
            [weak, store, outcome, project, result, projectFile, dest, name]
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
                store->setBase (outcome->head);
                me->hooks.openProject (projectFile);
                me->projectChanged();
                me->hooks.setStatus ("Opened \"" + name + "\" from the cloud, in " + dest.getFullPathName());
            });
          });
        }));
        o.dialogTitle = "Open Shared Project";
        o.dialogBackgroundColour = theme::panel;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.escapeKeyTriggersCloseButton = true;
        o.launchAsync();
    });
}

//==============================================================================
void CloudController::timerCallback()
{
    // Every two minutes, quietly ask whether a collaborator pushed something.
    if (--pollCountdown > 0)
        return;
    pollCountdown = 120;
    if (isLinked() && isSignedIn() && ! busy)
        poll();
}

void CloudController::poll()
{
    auto* c = client.get();
    const auto id = store->getProjectId();
    auto latest = std::make_shared<var>();
    auto err = std::make_shared<cloud::ApiError>();

    busy = true;
    WeakReference<CloudController> weak (this);
    worker.addJob ([c, id, latest, err, weak]
    {
        *latest = c->call ("GET", "/projects/" + id + "/revisions?limit=1", {}, *err);
        MessageManager::callAsync ([weak, latest, err]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;
            self->busy = false;
            if (err->status != 0 || self->store == nullptr)
            {
                self->updateStatus();
                return;
            }
            const auto newest = (*latest)["revisions"][0];
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
