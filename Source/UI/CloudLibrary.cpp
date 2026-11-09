// Cloud > Projects and Cloud > Friends: browsing your cloud projects and songs, sharing them
// with friends (as editors or viewers), unsharing, renaming, moving, deleting, and managing
// friend requests. All network calls go through CloudController::request (its own thread).

#include "CloudUI.h"
#include "CloudUIHelpers.h"

namespace lathe
{
using namespace cloudui;

namespace
{
String roleText (const String& role)
{
    if (role == "owner")  return "owner";
    if (role == "viewer") return "can view";
    return "can edit";
}

String esc (const String& s)
{
    return URL::addEscapeChars (s, true);
}

void confirm (const String& title, const String& message, const String& yes, std::function<void()> then)
{
    AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::NoIcon)
                                .withTitle (title).withMessage (message).withButton (yes).withButton ("Cancel"),
                            [then] (int button) { if (button == 1 && then) then(); });
}

TextButton* styled (TextButton* b, bool primary = false)
{
    if (primary)
    {
        b->setColour (TextButton::buttonColourId, theme::accent);
        b->setColour (TextButton::textColourOffId, theme::textDark);
    }
    return b;
}
} // namespace

//==============================================================================
/** Who has access: one row each, with a role menu and Remove for the owner to use. */
class CloudController::PeopleList : public Component
{
public:
    std::function<void (const String& id, const String& role)> onRole;
    std::function<void (const String& id, const String& email)> onRemove;

    void setPeople (const Array<var>& people, bool canManage, const String& footnote)
    {
        rows.clear();
        for (auto& p : people)
        {
            auto* row = rows.add (new Row());
            row->id = p["id"].toString();
            row->email.setText (p["email"].toString(), dontSendNotification);
            row->email.setFont (theme::font (13.0f));
            row->addAndMakeVisible (row->email);
            const auto role = p["role"].toString();
            if (canManage && role != "owner")
            {
                row->role.addItem ("Can edit", 1);
                row->role.addItem ("Can view", 2);
                row->role.setSelectedId (role == "viewer" ? 2 : 1, dontSendNotification);
                row->role.onChange = [this, row] { if (onRole) onRole (row->id, row->role.getSelectedId() == 2 ? "viewer" : "editor"); };
                row->addAndMakeVisible (row->role);
                row->remove.setButtonText ("Remove");
                row->remove.onClick = [this, row] { if (onRemove) onRemove (row->id, row->email.getText()); };
                row->addAndMakeVisible (row->remove);
            }
            else
            {
                row->roleText.setText (roleText (role), dontSendNotification);
                row->roleText.setColour (Label::textColourId, theme::textDim);
                row->roleText.setFont (theme::font (12.5f));
                row->addAndMakeVisible (row->roleText);
            }
            addAndMakeVisible (row);
        }
        note.setText (footnote, dontSendNotification);
        note.setColour (Label::textColourId, theme::textDim);
        note.setFont (theme::font (12.0f));
        note.setJustificationType (Justification::topLeft);
        addAndMakeVisible (note);
        resized();
    }

    int getIdealHeight() const   { return rows.size() * 30 + (note.getText().isNotEmpty() ? 40 : 0); }

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto* row : rows)
        {
            row->setBounds (r.removeFromTop (30));
            auto b = row->getLocalBounds().reduced (0, 3);
            if (row->remove.isVisible())
            {
                row->remove.setBounds (b.removeFromRight (70));
                b.removeFromRight (6);
                row->role.setBounds (b.removeFromRight (100));
            }
            else
            {
                row->roleText.setBounds (b.removeFromRight (100));
            }
            row->email.setBounds (b);
        }
        note.setBounds (r.removeFromTop (40));
    }

private:
    struct Row : public Component
    {
        String id;
        Label email, roleText;
        ComboBox role;
        TextButton remove;
    };
    OwnedArray<Row> rows;
    Label note;
};

//==============================================================================
class CloudController::LibraryWindow : public DocumentWindow
{
public:
    LibraryWindow (CloudController& c, const String& selectSong)
        : DocumentWindow ("Lathe Cloud: Projects", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content = new Content (c, selectSong), true);
        setResizable (true, true);
        setResizeLimits (720, 420, 1600, 1200);
        centreWithSize (860, 560);
    }

    void closeButtonPressed() override   { delete this; }
    void reload()                         { content->reload(); }
    void select (const String& songId)    { content->selectSong (songId); }

private:
    //==========================================================================
    class Content : public Component, private ListBoxModel
    {
    public:
        /** cc.request, but nothing happens if this window has closed by the time it answers. */
        void api (const String& method, const String& path, const var& body,
                  std::function<void (const var&, const cloud::ApiError&)> fn)
        {
            Component::SafePointer<Content> safe (this);
            cc.request (method, path, body, [safe, fn] (const var& r, const cloud::ApiError& e)
            {
                if (safe != nullptr && fn)
                    fn (r, e);
            });
        }
        Content (CloudController& c, const String& selectSong) : cc (c), wanted ("s:" + selectSong)
        {
            for (auto* b : { &newProject, &friendsButton, &refresh })
                addAndMakeVisible (b);
            newProject.onClick = [this] { createProject(); };
            friendsButton.onClick = [this] { cc.showFriends(); };
            refresh.onClick = [this] { reload(); };

            list.setModel (this);
            list.setRowHeight (30);
            list.setColour (ListBox::backgroundColourId, theme::lane);
            addAndMakeVisible (list);

            title.setFont (theme::font (18.0f, true));
            subtitle.setFont (theme::font (12.5f));
            subtitle.setColour (Label::textColourId, theme::textDim);
            subtitle.setJustificationType (Justification::topLeft);
            peopleHeading.setFont (theme::font (12.5f, true));
            peopleHeading.setColour (Label::textColourId, theme::textDim);
            message.setJustificationType (Justification::centred);
            message.setColour (Label::textColourId, theme::textDim);
            for (auto* l : { &title, &subtitle, &peopleHeading, &message })
                addAndMakeVisible (l);
            addAndMakeVisible (people);
            people.onRole = [this] (const String& id, const String& role) { setRole (id, role); };
            people.onRemove = [this] (const String& id, const String& email) { removePerson (id, email); };

            for (auto* b : actionButtons())
                addChildComponent (b);
            styled (&openButton, true);
            openButton.onClick = [this] { if (selected.isObject()) cc.openCloudSong (selected); };
            shareButton.onClick = [this] { share(); };
            renameButton.onClick = [this] { rename(); };
            moveButton.onClick = [this] { move(); };
            stopButton.onClick = [this] { stopSharing(); };
            deleteButton.onClick = [this] { remove(); };
            leaveButton.onClick = [this] { leave(); };

            setSize (860, 560);
            api ("GET", "/me", {}, [this] (const var& me, const cloud::ApiError&) { mySub = me["sub"].toString(); });
            reload();
        }

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::border);
            g.drawVerticalLine (list.getRight(), 0.0f, (float) getHeight());
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto left = r.removeFromLeft (300);
            auto bar = left.removeFromBottom (44).reduced (10, 8);
            newProject.setBounds (bar.removeFromLeft (110));
            bar.removeFromLeft (6);
            friendsButton.setBounds (bar.removeFromLeft (80));
            refresh.setBounds (bar.removeFromRight (70));
            list.setBounds (left);

            auto d = r.reduced (20, 16);
            message.setBounds (d);
            title.setBounds (d.removeFromTop (28));
            subtitle.setBounds (d.removeFromTop (38));
            d.removeFromTop (8);

            // The actions, wrapping onto a second row when they don't fit.
            auto buttons = d.removeFromBottom (66);
            int x = buttons.getX(), y = buttons.getY();
            for (auto* b : actionButtons())
                if (b->isVisible())
                {
                    const int w = jmax (70, b->getBestWidthForHeight (30) + 16);
                    if (x + w > buttons.getRight() && x > buttons.getX())
                    {
                        x = buttons.getX();
                        y += 36;
                    }
                    b->setBounds (x, y, w, 30);
                    x += w + 6;
                }
            d.removeFromBottom (10);
            peopleHeading.setBounds (d.removeFromTop (22));
            people.setBounds (d.removeFromTop (jmin (d.getHeight(), people.getIdealHeight())));
        }

        void reload()
        {
            if (! cc.isSignedIn())
            {
                showMessage ("Sign in to see your projects (Cloud > Sign In).");
                return;
            }
            api ("GET", "/v2/library", {}, [this] (const var& lib, const cloud::ApiError& err)
            {
                if (err.status != 0)
                {
                    showMessage (cc.isOldServer (err)
                                     ? "Projects and friends aren't available on Lathe Cloud yet. Try again later."
                                     : "Couldn't load your projects:\n" + err.message);
                    return;
                }
                library = lib;
                rebuildRows();
                if (auto* n = lib["notices"].getArray(); n != nullptr && ! n->isEmpty())
                    cc.showNotices (*n);
            });
        }

        void selectSong (const String& songId)
        {
            wanted = "s:" + songId;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].key == wanted)
                    list.selectRow (i);
        }

    private:
        struct Row
        {
            enum Kind { heading, folder, song } kind = heading;
            String text, key;
            var item, project;     // for a song: the project it is in (if we can see it)
        };

        Array<TextButton*> actionButtons()
        {
            return { &openButton, &shareButton, &renameButton, &moveButton, &stopButton, &leaveButton, &deleteButton };
        }

        void showMessage (const String& text)
        {
            message.setText (text, dontSendNotification);
            message.setVisible (true);
            for (auto* c : std::initializer_list<Component*> { &title, &subtitle, &peopleHeading, &people })
                c->setVisible (false);
            for (auto* b : actionButtons())
                b->setVisible (false);
        }

        void rebuildRows()
        {
            const auto keep = list.getSelectedRow() >= 0 && list.getSelectedRow() < rows.size()
                                  ? rows[list.getSelectedRow()].key : wanted;
            rows.clear();
            rows.add ({ Row::heading, "PROJECTS", {}, {}, {} });
            if (auto* projects = library["projects"].getArray())
            {
                for (auto& p : *projects)
                {
                    rows.add ({ Row::folder, p["name"].toString(), "p:" + p["id"].toString(), p, {} });
                    if (auto* songs = p["songs"].getArray())
                        for (auto& s : *songs)
                            rows.add ({ Row::song, s["name"].toString(), "s:" + s["id"].toString(), s, p });
                }
                if (projects->isEmpty())
                    rows.add ({ Row::heading, "  None yet: New Project, or Cloud > Put This Song in the Cloud", {}, {}, {} });
            }
            if (auto* shared = library["sharedSongs"].getArray(); shared != nullptr && ! shared->isEmpty())
            {
                rows.add ({ Row::heading, "SONGS SHARED WITH YOU", {}, {}, {} });
                for (auto& s : *shared)
                    rows.add ({ Row::song, s["name"].toString(), "s:" + s["id"].toString(), s, {} });
            }
            list.updateContent();
            list.repaint();

            int pick = -1;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].key == keep)
                    pick = i;
            if (pick < 0)
                for (int i = 0; i < rows.size() && pick < 0; ++i)
                    if (rows[i].kind != Row::heading)
                        pick = i;
            if (pick >= 0)
            {
                list.selectRow (pick);
                selectedRowsChanged (pick);
            }
            else
            {
                showMessage ("Nothing in the cloud yet.\n\nMake a project with New Project, or open a song and choose "
                             "Cloud > Put This Song in the Cloud.");
            }
        }

        // ListBoxModel
        int getNumRows() override   { return rows.size(); }

        void paintListBoxItem (int index, Graphics& g, int w, int h, bool isSelected) override
        {
            if (! isPositiveAndBelow (index, rows.size()))
                return;
            const auto& row = rows.getReference (index);
            if (row.kind == Row::heading)
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.0f, true));
                g.drawText (row.text, 10, 0, w - 20, h - 4, Justification::bottomLeft, true);
                return;
            }
            if (isSelected)
                g.fillAll (theme::headerSel);
            const bool isSong = row.kind == Row::song;
            const int indent = isSong && row.project.isObject() ? 26 : 10;
            g.setColour (theme::text);
            g.setFont (theme::font (isSong ? 13.5f : 14.0f, ! isSong));
            const auto role = row.item["role"].toString();
            auto tag = role == "owner" ? String() : roleText (role);
            if (isSong && ! row.project.isObject() && row.item["projectName"].toString().isNotEmpty())
                tag = "from " + row.item["projectName"].toString() + (tag.isNotEmpty() ? ", " + tag : String());
            g.drawText (row.text, indent, 0, w - indent - 110, h, Justification::centredLeft, true);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText (tag, w - 120, 0, 112, h, Justification::centredRight, true);
        }

        void selectedRowsChanged (int index) override
        {
            if (! isPositiveAndBelow (index, rows.size()) || rows[index].kind == Row::heading)
                return;
            const auto row = rows[index];
            wanted = row.key;
            selected = row.item;
            selectedIsSong = row.kind == Row::song;
            showDetails (row);
        }

        void listBoxItemDoubleClicked (int index, const MouseEvent&) override
        {
            if (isPositiveAndBelow (index, rows.size()) && rows[index].kind == Row::song)
                cc.openCloudSong (rows[index].item);
        }

        // details
        bool isOwner() const   { return selected["role"].toString() == "owner"; }
        String selectedPath() const
        {
            return (selectedIsSong ? "/v2/songs/" : "/v2/projects/") + esc (selected["id"].toString());
        }

        void showDetails (const Row& row)
        {
            message.setVisible (false);
            for (auto* c : std::initializer_list<Component*> { &title, &subtitle, &peopleHeading, &people })
                c->setVisible (true);
            title.setText (row.text, dontSendNotification);

            const auto role = selected["role"].toString();
            String sub;
            if (selectedIsSong)
            {
                const int seq = (int) selected["seq"];
                sub << (seq > 0 ? "r" + String (seq) : String ("no revisions yet"));
                if (! selected["updatedAt"].isVoid())
                    sub << ", changed " << timeAgo ((int64) selected["updatedAt"]);
                const auto projectName = row.project.isObject() ? row.project["name"].toString() : selected["projectName"].toString();
                if (projectName.isNotEmpty())
                    sub << "\nIn the project \"" << projectName << "\"";
            }
            sub << (sub.isEmpty() ? "" : ". ")
                << (role == "owner" ? String ("You own it.")
                    : "Owner: " + selected["owner"].toString() + ". You " + (role == "viewer" ? "can view (pull, not push)." : "can edit (pull and push)."));
            subtitle.setText (sub, dontSendNotification);

            const bool owner = role == "owner";
            openButton.setVisible (selectedIsSong);
            shareButton.setVisible (owner);
            renameButton.setVisible (owner);
            moveButton.setVisible (owner && selectedIsSong);
            stopButton.setVisible (owner);
            deleteButton.setVisible (owner);
            leaveButton.setVisible (! owner);
            openButton.setButtonText (selectedIsSong ? "Open" : "");
            shareButton.setButtonText ("Share with a Friend...");
            renameButton.setButtonText ("Rename...");
            moveButton.setButtonText ("Move to Project...");
            stopButton.setButtonText ("Stop Sharing...");
            deleteButton.setButtonText ("Delete from Cloud...");
            leaveButton.setButtonText (selectedIsSong ? "Leave Song..." : "Leave Project...");

            peopleHeading.setText ("WHO HAS IT", dontSendNotification);
            people.setPeople ({}, false, "Loading...");
            resized();

            const auto key = wanted;
            api ("GET", selectedPath(), {}, [this, key] (const var& detail, const cloud::ApiError& err)
            {
                if (key != wanted)
                    return;   // the selection moved on
                if (err.status != 0)
                {
                    people.setPeople ({}, false, err.status == 404 ? "You don't have this anymore." : err.message);
                    return;
                }
                details = detail;
                Array<var> memberList;
                if (auto* m = detail["members"].getArray())
                    memberList = *m;
                String note;
                if (selectedIsSong)
                {
                    StringArray via;
                    if (auto* pm = detail["projectMembers"].getArray())
                        for (auto& m : *pm)
                            if (m["role"].toString() != "owner")
                                via.add (m["email"].toString() + " (" + roleText (m["role"].toString()) + ")");
                    if (via.isEmpty())
                        note = memberList.size() <= 1 ? "Only you. Share it with a friend, or share its whole project."
                                                : "Shared with the people above on its own.";
                    else
                        note = "Also everyone in its project \"" + detail["projectName"].toString() + "\": " + via.joinIntoString (", ") + ".";
                }
                else if (memberList.size() <= 1)
                {
                    note = "Only you. People you share a project with get every song in it, including ones you add later.";
                }
                else
                {
                    note = "They get every song in this project, including ones added later.";
                }
                people.setPeople (memberList, isOwner(), note);
                resized();
            });
        }

        void afterChange (const String& statusText)
        {
            if (statusText.isNotEmpty())
                cc.hooks.setStatus (statusText);
            reload();
            if (cc.isLinked() && cc.isSignedIn())
                cc.poll();
        }

        void failed (const String& what, const cloud::ApiError& err)
        {
            alert ("Couldn't " + what, err.message);
        }

        void createProject()
        {
            askForText ("New project", "A project holds songs. Share it with friends and they get every song in it.",
                        {}, [this] (const String& entered)
            {
                const auto name = entered.trim();
                if (name.isEmpty())
                    return;
                DynamicObject::Ptr body = new DynamicObject();
                body->setProperty ("name", name);
                api ("POST", "/v2/projects", var (body.get()), [this, name] (const var& p, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("make the project", err);
                    wanted = "p:" + p["id"].toString();
                    afterChange ("Made the project \"" + name + "\".");
                });
            });
        }

        void share()
        {
            const auto what = selected["name"].toString();
            const auto path = selectedPath() + "/members";
            const bool song = selectedIsSong;
            StringArray already;
            if (auto* m = details["members"].getArray())
                for (auto& x : *m)
                    already.add (x["id"].toString());

            api ("GET", "/v2/friends", {}, [this, what, path, song, already] (const var& resp, const cloud::ApiError& err)
            {
                if (err.status != 0)
                    return failed ("load your friends", err);
                Array<var> friendsList;
                if (auto* f = resp["friends"].getArray())
                    for (auto& x : *f)
                        if (! already.contains (x["id"].toString()))
                            friendsList.add (x);
                if (friendsList.isEmpty())
                {
                    confirm ("Share \"" + what + "\"",
                             already.size() > 1 ? "It's already shared with all your friends. Add more friends in Cloud > Friends."
                                                : "You can share with friends. Add one first: they get a request, and once "
                                                  "they accept you can share with them.",
                             "Open Friends", [this] { cc.showFriends(); });
                    return;
                }
                PopupMenu m;
                m.addSectionHeader (song ? "Share this song with..." : "Share every song in this project with...");
                for (int i = 0; i < friendsList.size(); ++i)
                {
                    PopupMenu roles;
                    roles.addItem (1 + i * 2, "Can edit (pull and push)");
                    roles.addItem (2 + i * 2, "Can view (pull only)");
                    m.addSubMenu (friendsList[i]["email"].toString(), roles);
                }
                m.showMenuAsync (PopupMenu::Options().withTargetComponent (&shareButton),
                                 [this, friendsList, path, what] (int r)
                {
                    if (r <= 0)
                        return;
                    const auto f = friendsList[(r - 1) / 2];
                    const auto role = (r % 2 == 1) ? "editor" : "viewer";
                    DynamicObject::Ptr body = new DynamicObject();
                    body->setProperty ("friend", f["id"]);
                    body->setProperty ("role", role);
                    const auto email = f["email"].toString();
                    api ("POST", path, var (body.get()), [this, email, what] (const var&, const cloud::ApiError& e)
                    {
                        if (e.status != 0)
                            return failed ("share", e);
                        afterChange ("Shared \"" + what + "\" with " + email + ".");
                    });
                });
            });
        }

        void setRole (const String& id, const String& role)
        {
            DynamicObject::Ptr body = new DynamicObject();
            body->setProperty ("role", role);
            api ("PATCH", selectedPath() + "/members/" + esc (id), var (body.get()), [this] (const var&, const cloud::ApiError& err)
            {
                if (err.status != 0)
                    failed ("change their access", err);
                afterChange ({});
            });
        }

        void removePerson (const String& id, const String& email)
        {
            const auto what = selected["name"].toString();
            const auto path = selectedPath() + "/members/" + esc (id);
            confirm ("Stop sharing with " + email + "?",
                     email + (selectedIsSong ? " loses \"" + what + "\"" : " loses every song in \"" + what + "\"")
                         + " (unless they have it another way). They can't pull or push anymore, but their copy stays on "
                           "their computer. They'll get a notice.",
                     "Stop Sharing", [this, path, email]
            {
                api ("DELETE", path, {}, [this, email] (const var&, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("unshare", err);
                    afterChange ("Stopped sharing with " + email + ".");
                });
            });
        }

        void stopSharing()
        {
            const auto what = selected["name"].toString();
            const auto path = selectedPath() + "/members";
            confirm ("Stop sharing \"" + what + "\" with everyone?",
                     selectedIsSong ? String ("Everyone it's shared with on its own loses it. People in its project keep it "
                                              "(unshare the project for that). Their copies stay on their computers, and they'll get a notice.")
                                    : String ("Everyone in the project loses its songs (except songs shared with them on their own). "
                                              "Their copies stay on their computers, and they'll get a notice."),
                     "Stop Sharing", [this, path, what]
            {
                api ("DELETE", path, {}, [this, what] (const var&, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("stop sharing", err);
                    afterChange ("\"" + what + "\" isn't shared with anyone now.");
                });
            });
        }

        void rename()
        {
            const auto path = selectedPath();
            askForText (selectedIsSong ? "Rename song" : "Rename project",
                        "Everyone it's shared with keeps it.", selected["name"].toString(), [this, path] (const String& entered)
            {
                const auto name = entered.trim();
                if (name.isEmpty())
                    return;
                DynamicObject::Ptr body = new DynamicObject();
                body->setProperty ("name", name);
                api ("PATCH", path, var (body.get()), [this, name] (const var&, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("rename", err);
                    afterChange ("Renamed to \"" + name + "\".");
                });
            });
        }

        void move()
        {
            const auto songName = selected["name"].toString();
            const auto current = selected["project"].toString();
            const auto path = selectedPath();
            Array<var> targets;
            if (auto* projects = library["projects"].getArray())
                for (auto& p : *projects)
                    if (p["role"].toString() == "owner" && p["id"].toString() != current)
                        targets.add (p);
            if (targets.isEmpty())
            {
                alert ("Move \"" + songName + "\"", "Make another project first (New Project): a song can move between your projects.");
                return;
            }
            PopupMenu m;
            m.addSectionHeader ("Move to...");
            for (int i = 0; i < targets.size(); ++i)
                m.addItem (i + 1, targets[i]["name"].toString());
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&moveButton), [this, targets, songName, path] (int r)
            {
                if (r <= 0)
                    return;
                const auto target = targets[r - 1];
                const auto targetName = target["name"].toString();
                confirm ("Move \"" + songName + "\" to \"" + targetName + "\"?",
                         "People it was shared with on its own, and people in its current project who aren't in \"" + targetName
                             + "\", lose it: they can't pull or push anymore (their copies stay on their computers) and they'll "
                               "get a notice. Everyone in \"" + targetName + "\" gets it.",
                         "Move", [this, target, targetName, path, songName]
                {
                    DynamicObject::Ptr body = new DynamicObject();
                    body->setProperty ("project", target["id"]);
                    api ("PATCH", path, var (body.get()), [this, targetName, songName] (const var&, const cloud::ApiError& err)
                    {
                        if (err.status != 0)
                            return failed ("move it", err);
                        afterChange ("Moved \"" + songName + "\" to \"" + targetName + "\".");
                    });
                });
            });
        }

        void remove()
        {
            const auto what = selected["name"].toString();
            const auto path = selectedPath();
            int songs = 0;
            if (! selectedIsSong)
                if (auto* s = selected["songs"].getArray())
                    songs = s->size();
            confirm ("Delete \"" + what + "\" from Lathe Cloud?",
                     (selectedIsSong ? String ("The song, all its revisions and its audio are deleted from the cloud. ")
                                     : "The project and its " + String (songs) + (songs == 1 ? " song" : " songs")
                                           + " (all revisions and audio) are deleted from the cloud. ")
                         + "This can't be undone. Copies on computers (yours and your collaborators') stay; "
                           "anyone it was shared with gets a notice.",
                     "Delete", [this, path, what]
            {
                api ("DELETE", path, {}, [this, what] (const var&, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("delete it", err);
                    afterChange ("Deleted \"" + what + "\" from the cloud.");
                });
            });
        }

        void leave()
        {
            if (mySub.isEmpty())
                return;
            const auto what = selected["name"].toString();
            const auto path = selectedPath() + "/members/" + esc (mySub);
            confirm ("Leave \"" + what + "\"?",
                     selectedIsSong ? String ("You won't get its new revisions or be able to push. Your copy stays.")
                                    : String ("You lose its songs (unless they're shared with you on their own). Your copies stay."),
                     "Leave", [this, path, what]
            {
                api ("DELETE", path, {}, [this, what] (const var&, const cloud::ApiError& err)
                {
                    if (err.status != 0)
                        return failed ("leave", err);
                    afterChange ("You left \"" + what + "\".");
                });
            });
        }

        CloudController& cc;
        String wanted, mySub;
        var library, selected, details;
        bool selectedIsSong = false;
        Array<Row> rows;
        ListBox list;
        TextButton newProject { "New Project..." }, friendsButton { "Friends..." }, refresh { "Refresh" };
        Label title, subtitle, peopleHeading, message;
        PeopleList people;
        TextButton openButton, shareButton, renameButton, moveButton, stopButton, deleteButton, leaveButton;
    };

    Content* content = nullptr;
};

//==============================================================================
class CloudController::FriendsWindow : public DocumentWindow
{
public:
    explicit FriendsWindow (CloudController& c)
        : DocumentWindow ("Lathe Cloud: Friends", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content = new Content (c), true);
        setResizable (true, true);
        setResizeLimits (440, 360, 1000, 1200);
        centreWithSize (520, 520);
    }

    void closeButtonPressed() override   { delete this; }
    void reload()                         { content->reload(); }

private:
    class Content : public Component
    {
    public:
        /** cc.request, but nothing happens if this window has closed by the time it answers. */
        void api (const String& method, const String& path, const var& body,
                  std::function<void (const var&, const cloud::ApiError&)> fn)
        {
            Component::SafePointer<Content> safe (this);
            cc.request (method, path, body, [safe, fn] (const var& r, const cloud::ApiError& e)
            {
                if (safe != nullptr && fn)
                    fn (r, e);
            });
        }
        explicit Content (CloudController& c) : cc (c)
        {
            intro.setText ("You share projects and songs with friends. Send a request to their email; once they accept "
                           "(in Lathe, Cloud > Friends), you can share with each other.", dontSendNotification);
            intro.setColour (Label::textColourId, theme::textDim);
            intro.setFont (theme::font (12.5f));
            intro.setJustificationType (Justification::topLeft);
            addAndMakeVisible (intro);
            styleEditor (email);
            email.setTextToShowWhenEmpty ("their@email.com", theme::textDim);
            email.onReturnKey = [this] { send(); };
            addAndMakeVisible (email);
            styled (&sendButton, true);
            sendButton.onClick = [this] { send(); };
            addAndMakeVisible (sendButton);
            view.setViewedComponent (&rowsHolder, false);
            view.setScrollBarsShown (true, false);
            addAndMakeVisible (view);
            setSize (520, 520);
            reload();
        }

        void paint (Graphics& g) override   { g.fillAll (theme::panel); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            intro.setBounds (r.removeFromTop (40));
            r.removeFromTop (6);
            auto line = r.removeFromTop (30);
            sendButton.setBounds (line.removeFromRight (120));
            line.removeFromRight (8);
            email.setBounds (line);
            r.removeFromTop (12);
            view.setBounds (r);
            layoutRows();
        }

        void reload()
        {
            api ("GET", "/v2/friends", {}, [this] (const var& resp, const cloud::ApiError& err)
            {
                rows.clear();
                if (err.status != 0)
                {
                    addHeading (cc.isOldServer (err) ? "Friends aren't available on Lathe Cloud yet. Try again later."
                                                     : "Couldn't load your friends: " + err.message);
                    layoutRows();
                    return;
                }
                auto section = [&] (const char* key, const String& heading, const String& empty,
                                    std::function<void (const var&, Row&)> buttons)
                {
                    const auto* items = resp[key].getArray();
                    if ((items == nullptr || items->isEmpty()) && empty.isEmpty())
                        return;
                    addHeading (heading);
                    if (items == nullptr || items->isEmpty())
                    {
                        auto* row = add (empty, true);
                        juce::ignoreUnused (row);
                        return;
                    }
                    for (auto& item : *items)
                        buttons (item, *add (item["email"].toString(), false));
                };

                section ("incoming", "WANT TO BE YOUR FRIEND", {}, [this] (const var& item, Row& row)
                {
                    const auto id = item["id"].toString();
                    const auto who = item["email"].toString();
                    row.addButton ("Accept", true, [this, id, who]
                    {
                        api ("POST", "/v2/friends/" + esc (id) + "/accept", {}, [this, who] (const var&, const cloud::ApiError& e)
                        {
                            if (e.status != 0) alert ("Couldn't accept", e.message);
                            else cc.hooks.setStatus ("You and " + who + " are friends now.");
                            changed();
                        });
                    });
                    row.addButton ("Decline", false, [this, id] { drop (id, {}); });
                });
                section ("outgoing", "WAITING FOR THEM TO ACCEPT", {}, [this] (const var& item, Row& row)
                {
                    const auto id = item["id"].toString();
                    row.addButton ("Cancel", false, [this, id] { drop (id, {}); });
                });
                section ("friends", "FRIENDS", "No friends yet. Send someone a request above.", [this] (const var& item, Row& row)
                {
                    const auto id = item["id"].toString();
                    const auto who = item["email"].toString();
                    row.addButton ("Remove", false, [this, id, who]
                    {
                        confirm ("Remove " + who + " as a friend?",
                                 "You won't be able to share new things with each other. Anything already shared stays shared "
                                 "(unshare it in Cloud > Projects).",
                                 "Remove", [this, id, who] { drop (id, "Removed " + who + " from your friends."); });
                    });
                });
                layoutRows();
            });
        }

    private:
        struct Row : public Component
        {
            Label label;
            OwnedArray<TextButton> buttons;
            bool heading = false;

            void addButton (const String& text, bool primary, std::function<void()> onClick)
            {
                auto* b = buttons.add (new TextButton (text));
                styled (b, primary);
                b->onClick = std::move (onClick);
                addAndMakeVisible (b);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (0, heading ? 0 : 3);
                for (int i = buttons.size(); --i >= 0;)
                {
                    buttons[i]->setBounds (r.removeFromRight (80));
                    r.removeFromRight (6);
                }
                label.setBounds (r);
            }
        };

        void addHeading (const String& text)
        {
            auto* row = rows.add (new Row());
            row->heading = true;
            row->label.setText (text, dontSendNotification);
            row->label.setFont (theme::font (11.0f, true));
            row->label.setColour (Label::textColourId, theme::textDim);
            row->label.setJustificationType (Justification::bottomLeft);
            row->addAndMakeVisible (row->label);
            rowsHolder.addAndMakeVisible (row);
        }

        Row* add (const String& text, bool dim)
        {
            auto* row = rows.add (new Row());
            row->label.setText (text, dontSendNotification);
            row->label.setFont (theme::font (13.5f));
            if (dim)
                row->label.setColour (Label::textColourId, theme::textDim);
            row->addAndMakeVisible (row->label);
            rowsHolder.addAndMakeVisible (row);
            return row;
        }

        void layoutRows()
        {
            const int w = jmax (100, view.getWidth() - view.getScrollBarThickness());
            int y = 0;
            for (auto* row : rows)
            {
                const int h = row->heading ? 30 : 32;
                row->setBounds (0, y, w, h);
                y += h;
            }
            rowsHolder.setSize (w, jmax (1, y));
        }

        void send()
        {
            const auto address = email.getText().trim();
            if (address.isEmpty())
                return;
            DynamicObject::Ptr body = new DynamicObject();
            body->setProperty ("email", address);
            api ("POST", "/v2/friends", var (body.get()), [this, address] (const var& resp, const cloud::ApiError& err)
            {
                if (err.status != 0)
                    return alert ("Couldn't send the request", err.message);
                email.clear();
                cc.hooks.setStatus (resp["status"].toString() == "friends"
                                        ? "You and " + address + " are friends now."
                                        : "Sent a friend request to " + address + ". Once they accept, you can share with each other.");
                changed();
            });
        }

        void drop (const String& id, const String& statusText)
        {
            api ("DELETE", "/v2/friends/" + esc (id), {}, [this, statusText] (const var&, const cloud::ApiError& err)
            {
                if (err.status != 0)
                    alert ("Couldn't do that", err.message);
                else if (statusText.isNotEmpty())
                    cc.hooks.setStatus (statusText);
                changed();
            });
        }

        void changed()
        {
            reload();
            cc.pollAccount();
        }

        CloudController& cc;
        Label intro;
        TextEditor email;
        TextButton sendButton { "Send Request" };
        Viewport view;
        Component rowsHolder;
        OwnedArray<Row> rows;
    };

    Content* content = nullptr;
};

//==============================================================================
void CloudController::showLibrary (const String& selectSongId)
{
    if (! isSignedIn())
    {
        showSignIn ([this, selectSongId] { showLibrary (selectSongId); });
        return;
    }
    if (auto* w = dynamic_cast<LibraryWindow*> (library.getComponent()))
    {
        w->toFront (true);
        w->reload();
        if (selectSongId.isNotEmpty())
            w->select (selectSongId);
        return;
    }
    auto* w = new LibraryWindow (*this, selectSongId);
    library = w;
    w->setVisible (true);
}

void CloudController::showFriends()
{
    if (! isSignedIn())
    {
        showSignIn ([this] { showFriends(); });
        return;
    }
    if (auto* w = dynamic_cast<FriendsWindow*> (friends.getComponent()))
    {
        w->toFront (true);
        w->reload();
        return;
    }
    auto* w = new FriendsWindow (*this);
    friends = w;
    w->setVisible (true);
}

void CloudController::refreshWindows()
{
    if (auto* w = dynamic_cast<LibraryWindow*> (library.getComponent()))
        w->reload();
    if (auto* w = dynamic_cast<FriendsWindow*> (friends.getComponent()))
        w->reload();
}
} // namespace lathe
