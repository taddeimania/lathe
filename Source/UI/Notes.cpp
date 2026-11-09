#include "Notes.h"
#include "CloudUI.h"
#include "CloudUIHelpers.h"

namespace lathe
{
using namespace cloudui;

namespace
{
String newLocalId()
{
    return String (Time::currentTimeMillis()).paddedLeft ('0', 13) + "-" + String::toHexString (Random::getSystemRandom().nextInt()).paddedLeft ('0', 8);
}

String who (const String& email, const String& me)
{
    if (email.isEmpty() || email == me)
        return "You";
    return shortName (email);
}

/** The error to show, in words that say what to do about it. */
String explain (CloudController& cloud, const cloud::ApiError& err)
{
    if (cloud.isOldServer (err))
        return "Notes need the updated Lathe Cloud server. Whoever runs it: update lathe-backend and run terraform apply.";
    if (err.status == 404)
        return "That note or song isn't there anymore (it may have been deleted or unshared).";
    return err.message;
}

void styleSmall (TextButton& b, bool primary = false)
{
    if (primary)
    {
        b.setColour (TextButton::buttonColourId, theme::accent);
        b.setColour (TextButton::textColourOffId, theme::textDark);
    }
}
} // namespace

//==============================================================================
NotesController::NotesController (AppContext& c, CloudController& cc) : ctx (c), cloud (cc)
{
    loadSeen();
    startTimer (1000);
}

NotesController::~NotesController()
{
    stopTimer();
    if (window != nullptr)
        delete window.getComponent();
}

NotesController::Show NotesController::getShow() const
{
    return (Show) jlimit (1, 3, ctx.settings.getIntValue ("notesShow", 1));
}

void NotesController::setShow (Show s)
{
    ctx.settings.setValue ("notesShow", (int) s);
    changed();
}

bool NotesController::isShownOnTracks (const SongNote& n) const
{
    const auto show = getShow();
    return ! n.isSongNote() && show != Show::none && (show == Show::all || ! n.resolved);
}

const SongNote* NotesController::find (const String& id) const
{
    for (auto& n : getNotes())
        if (n.id == id)
            return &n;
    return nullptr;
}

String NotesController::getMe() const
{
    return cloud.getEmail();
}

bool NotesController::canEditText (const SongNote& n) const
{
    return ! isCloud() || n.author.isEmpty() || n.author == getMe();
}

bool NotesController::canDelete (const String& author) const
{
    return ! isCloud() || author.isEmpty() || author == getMe() || cloud.getSongRole() == "owner";
}

//==============================================================================
void NotesController::loadSeen()
{
    seen = JSON::parse (ctx.settings.getValue ("notesSeen"));
    if (! seen.isObject())
        seen = var (new DynamicObject());
}

void NotesController::saveSeen()
{
    ctx.settings.setValue ("notesSeen", JSON::toString (seen, true));
}

bool NotesController::isUnread (const SongNote& n) const
{
    if (! isCloud())
        return false;
    const auto me = getMe();
    int64 newest = 0;
    if (n.author != me)
        newest = n.createdAt;
    for (auto& r : n.replies)
        if (r.author != me)
            newest = jmax (newest, r.createdAt);
    const auto key = songId + "/" + n.id;
    return newest > (int64) seen[Identifier (key)];
}

int NotesController::unreadCount() const
{
    int count = 0;
    for (auto& n : getNotes())
        count += isUnread (n) ? 1 : 0;
    return count;
}

void NotesController::markSeen (const SongNote& n)
{
    if (! isCloud() || ! isUnread (n))
        return;
    if (auto* o = seen.getDynamicObject())
        o->setProperty (Identifier (songId + "/" + n.id), n.lastActivity());
    saveSeen();
    changed();
}

void NotesController::markAllSeen()
{
    if (! isCloud())
        return;
    bool any = false;
    if (auto* o = seen.getDynamicObject())
        for (auto& n : notes)
            if (isUnread (n))
            {
                o->setProperty (Identifier (songId + "/" + n.id), n.lastActivity());
                any = true;
            }
    if (any)
    {
        saveSeen();
        changed();
    }
}

//==============================================================================
void NotesController::changed()
{
    sendChangeMessage();
}

void NotesController::saveLocal()
{
    ctx.edit.touched();   // saved with the project
    changed();
}

//==============================================================================
// Where a cloud song's notes are kept between runs: the last copy from the server
// (so they show offline) and the changes not sent yet.
File NotesController::stateFile() const
{
    const auto f = ctx.edit.projectFile;
    return f == File() ? File() : f.getParentDirectory().getChildFile (".lathe").getChildFile ("notes.json");
}

void NotesController::loadState()
{
    notes.clear();
    outbox.clear();
    const auto f = stateFile();
    if (! f.existsAsFile())
        return;
    const auto data = JSON::parse (f);
    if (data["song"].toString() != songId)
        return;   // a copy of a different song
    if (auto* list = data["notes"].getArray())
        for (auto& v : *list)
            notes.push_back (SongNote::fromVar (v));
    if (auto* list = data["outbox"].getArray())
        for (auto& v : *list)
            outbox.push_back (v);
}

void NotesController::saveState()
{
    const auto f = stateFile();
    if (f == File() || ! isCloud())
        return;
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("song", songId);
    Array<var> list, ops;
    for (auto& n : notes)
        list.add (n.toVar());
    for (auto& op : outbox)
        ops.add (op);
    o->setProperty ("notes", list);
    o->setProperty ("outbox", ops);
    f.getParentDirectory().createDirectory();
    f.replaceWithText (JSON::toString (var (o.get())));
}

SongNote* NotesController::findMutable (const String& id)
{
    for (auto& n : notes)
        if (n.id == id)
            return &n;
    return nullptr;
}

bool NotesController::isConnectionProblem (const cloud::ApiError& err) const
{
    // No connection or a timeout (599), the server having a bad moment, a sign-in to renew,
    // or a server too old for notes: keep the change and try again later.
    return err.status == 599 || err.status >= 500 || err.status == 401 || err.status == 429 || cloud.isOldServer (err);
}

String NotesController::offlineMessage (const cloud::ApiError& err) const
{
    const int n = (int) outbox.size();
    const auto kept = n == 0 ? String ("Your notes are kept here")
                             : String (n) + (n == 1 ? " change is kept here" : " changes are kept here");
    if (cloud.isOldServer (err))
        return "Notes aren't syncing: the Lathe Cloud server needs updating (lathe-backend: terraform apply). " + kept + " until then.";
    if (err.status == 401)
        return "Notes aren't syncing: sign in again (Cloud > Sign In). " + kept + " until then.";
    return "Notes aren't syncing: can't reach Lathe Cloud. " + kept + " and sent when it's back.";
}

void NotesController::setSyncProblem (const String& problem)
{
    if (problem == syncProblem)
        return;
    const bool wasOk = syncProblem.isEmpty();
    syncProblem = problem;
    if (wasOk && problem.isNotEmpty())
        ctx.showStatus (problem);
    else if (! wasOk && problem.isEmpty())
        ctx.showStatus ("Notes are syncing again.");
    changed();
}

//==============================================================================
void NotesController::timerCallback()
{
    // Which song is open, and where do its notes live?
    const auto id = cloud.getSongId();
    if (id != songId)
    {
        songId = id;
        error = syncProblem = {};
        pollCountdown = 0;
        flushCountdown = 0;
        loadState();
        changed();
    }

    if (! isCloud())
        return;
    if (! ctx.edit.localNotes.empty() && ! uploading)
        uploadLocalNotes();
    if (! outbox.empty() && ! flushing && --flushCountdown <= 0)
        flush();
    if (--pollCountdown <= 0)
    {
        pollCountdown = 60;   // once a minute
        refresh();
    }
}

void NotesController::refresh()
{
    if (! isCloud())
    {
        changed();
        return;
    }
    if (! cloud.isSignedIn())
    {
        setSyncProblem (offlineMessage ({ 401, {}, {} }));
        return;
    }
    const auto forSong = songId;
    WeakReference<NotesController> weak (this);
    cloud.request ("GET", "/v2/songs/" + URL::addEscapeChars (songId, true) + "/notes", {},
                   [weak, forSong] (const var& resp, const cloud::ApiError& err)
    {
        auto* self = weak.get();
        if (self == nullptr || forSong != self->songId)
            return;
        if (err.status != 0)
        {
            if (self->isConnectionProblem (err))
                self->setSyncProblem (self->offlineMessage (err));
            else
            {
                self->error = err.status == 404 ? String ("This song isn't shared with you anymore, so its notes can't be loaded.")
                                                : "Couldn't load notes: " + err.message;
                self->changed();
            }
            return;
        }
        const int before = self->unreadCount();
        self->notes.clear();
        if (auto* list = resp["notes"].getArray())
            for (auto& v : *list)
                self->notes.push_back (SongNote::fromVar (v));
        self->reapplyUnsent();
        self->error = {};
        if (self->outbox.empty())
            self->setSyncProblem ({});
        self->saveState();
        const int after = self->unreadCount();
        if (after > before)
        {
            // Tell about the newest one.
            const SongNote* newest = nullptr;
            for (auto& n : self->notes)
                if (self->isUnread (n) && (newest == nullptr || n.lastActivity() > newest->lastActivity()))
                    newest = &n;
            if (newest != nullptr)
            {
                const bool isReply = ! newest->replies.empty() && newest->replies.back().createdAt >= newest->createdAt
                                     && newest->replies.back().author != self->getMe();
                const auto from = isReply ? newest->replies.back().author : newest->author;
                self->ctx.showStatus ((isReply ? "New reply from " : "New note from ") + shortName (from) + ": "
                                      + self->describeWhere (*newest) + ". Click Notes in the top bar to read it.");
            }
        }
        self->changed();
    });
}

void NotesController::apply (const var& serverNote)
{
    auto n = SongNote::fromVar (serverNote);
    if (auto* existing = findMutable (n.id))
        *existing = n;
    else
        notes.push_back (n);
}

//==============================================================================
// The outbox. An op is {kind, note, reply?, body?}; "note" may be a temporary id
// ("pending-...") for a note that hasn't reached the server yet.
void NotesController::optimistic (const var& op)
{
    const auto kind = op["kind"].toString();
    const auto noteId = op["note"].toString();
    const auto body = op["body"];
    const auto now = (int64) op["at"];

    if (kind == "add")
    {
        if (findMutable (noteId) != nullptr)
            return;
        SongNote n;
        n.id = noteId;
        n.text = body["text"].toString();
        n.track = body["track"].isVoid() ? -1 : (int) body["track"];
        n.trackName = body["trackName"].toString();
        n.beat = body["beat"].isVoid() ? 0.0 : (double) body["beat"];
        n.author = getMe();
        n.createdAt = n.updatedAt = now;
        n.pending = true;
        notes.push_back (n);
        return;
    }
    if (kind == "delete")
    {
        notes.erase (std::remove_if (notes.begin(), notes.end(), [&] (const SongNote& n) { return n.id == noteId; }), notes.end());
        return;
    }
    auto* n = findMutable (noteId);
    if (n == nullptr)
        return;
    if (kind == "reply")
    {
        const auto replyId = op["reply"].toString();
        for (auto& r : n->replies)
            if (r.id == replyId)
                return;
        n->replies.push_back ({ replyId, getMe(), {}, body["text"].toString(), now });
    }
    else if (kind == "update")
    {
        if (body.hasProperty ("resolved"))
        {
            n->resolved = (bool) body["resolved"];
            n->resolvedBy = n->resolved ? getMe() : String();
        }
        if (body.hasProperty ("text"))
            n->text = body["text"].toString();
    }
    else if (kind == "deleteReply")
    {
        const auto replyId = op["reply"].toString();
        n->replies.erase (std::remove_if (n->replies.begin(), n->replies.end(), [&] (const SongNoteReply& r) { return r.id == replyId; }),
                          n->replies.end());
    }
}

void NotesController::reapplyUnsent()
{
    for (auto& op : outbox)
        optimistic (op);
}

void NotesController::enqueue (const var& op)
{
    const auto kind = op["kind"].toString();
    const auto noteId = op["note"].toString();

    // Undoing something that never left this computer: just drop it.
    if (noteId.startsWith ("pending-") && kind == "delete")
    {
        outbox.erase (std::remove_if (outbox.begin(), outbox.end(), [&] (const var& o) { return o["note"].toString() == noteId; }), outbox.end());
        optimistic (op);
    }
    else if (kind == "deleteReply" && op["reply"].toString().startsWith ("pending-"))
    {
        const auto replyId = op["reply"].toString();
        outbox.erase (std::remove_if (outbox.begin(), outbox.end(), [&] (const var& o) { return o["reply"].toString() == replyId && o["kind"].toString() == "reply"; }),
                      outbox.end());
        optimistic (op);
    }
    else
    {
        outbox.push_back (op);
        optimistic (op);
    }
    saveState();
    changed();
    if (! flushing)
        flush();
}

void NotesController::flush()
{
    if (outbox.empty() || flushing || ! isCloud())
        return;
    if (! cloud.isSignedIn())
    {
        setSyncProblem (offlineMessage ({ 401, {}, {} }));
        flushCountdown = 15;
        return;
    }

    flushing = true;
    const auto op = outbox.front();
    const auto kind = op["kind"].toString();
    const auto noteId = op["note"].toString();
    const auto base = "/v2/songs/" + URL::addEscapeChars (songId, true) + "/notes";
    String method = "POST", path = base;
    var body = op["body"];
    if (kind == "reply")            path = base + "/" + URL::addEscapeChars (noteId, true) + "/replies";
    else if (kind == "update")      { method = "PATCH"; path = base + "/" + URL::addEscapeChars (noteId, true); }
    else if (kind == "delete")      { method = "DELETE"; path = base + "/" + URL::addEscapeChars (noteId, true); body = var(); }
    else if (kind == "deleteReply") { method = "DELETE"; path = base + "/" + URL::addEscapeChars (noteId, true) + "/replies/"
                                                                 + URL::addEscapeChars (op["reply"].toString(), true); body = var(); }

    const auto forSong = songId;
    WeakReference<NotesController> weak (this);
    cloud.request (method, path, body, [weak, op, kind, noteId, forSong] (const var& resp, const cloud::ApiError& err)
    {
        auto* self = weak.get();
        if (self == nullptr)
            return;
        self->flushing = false;
        if (forSong != self->songId)
            return;

        if (err.status != 0 && self->isConnectionProblem (err))
        {
            self->setSyncProblem (self->offlineMessage (err));
            self->flushCountdown = 15;   // try again in 15 s
            return;
        }

        // Sent (or refused for good): it leaves the outbox either way.
        if (! self->outbox.empty() && self->outbox.front()["id"].toString() == op["id"].toString())
            self->outbox.erase (self->outbox.begin());

        if (err.status != 0)
        {
            if (err.status != 404 || kind == "add" || kind == "reply")
                alert ("A note change couldn't be saved", explain (self->cloud, err));
            self->pollCountdown = 0;   // show what's really there
        }
        else if (kind == "delete")
        {
            auto& list = self->notes;
            list.erase (std::remove_if (list.begin(), list.end(), [&] (const SongNote& n) { return n.id == noteId; }), list.end());
        }
        else
        {
            // The real note replaces our stand-in. A new note gets its real id: later changes follow it.
            const auto realId = resp["id"].toString();
            if (kind == "add")
            {
                auto& list = self->notes;
                list.erase (std::remove_if (list.begin(), list.end(), [&] (const SongNote& n) { return n.id == noteId; }), list.end());
                for (auto& later : self->outbox)
                    if (later["note"].toString() == noteId)
                        if (auto* o = later.getDynamicObject())
                            o->setProperty ("note", realId);
            }
            self->apply (resp);
            self->reapplyUnsent();
        }

        if (self->outbox.empty())
            self->setSyncProblem ({});
        self->saveState();
        self->changed();
        if (! self->outbox.empty())
            self->flush();
    });
}

namespace
{
var makeOp (const String& kind, const String& noteId, const var& body, const String& replyId = {})
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("id", newLocalId());
    o->setProperty ("kind", kind);
    o->setProperty ("note", noteId);
    if (replyId.isNotEmpty())
        o->setProperty ("reply", replyId);
    if (! body.isVoid())
        o->setProperty ("body", body);
    o->setProperty ("at", Time::currentTimeMillis());
    return var (o.get());
}
} // namespace

void NotesController::forgetUploadedInFile (const String& noteId)
{
    // The note is on the server now: take it out of the saved project too, so closing
    // without saving can't upload it twice. (Only the "notes" list changes.)
    const auto file = ctx.edit.projectFile;
    if (! file.existsAsFile())
        return;
    auto data = JSON::parse (file);
    auto* o = data.getDynamicObject();
    auto* list = data["notes"].getArray();
    if (o == nullptr || list == nullptr)
        return;
    for (int i = list->size(); --i >= 0;)
        if ((*list)[i]["id"].toString() == noteId)
            list->remove (i);
    if (list->isEmpty())
        o->removeProperty ("notes");
    file.replaceWithText (JSON::toString (data));
}


void NotesController::uploadLocalNotes()
{
    // A song just went to the cloud: its notes go with it, through the outbox (so they
    // aren't lost if the connection drops), and out of the project file.
    uploading = true;
    for (auto& local : ctx.edit.localNotes)
    {
        DynamicObject::Ptr body = new DynamicObject();
        body->setProperty ("text", local.text);
        if (! local.isSongNote())
        {
            body->setProperty ("track", local.track);
            body->setProperty ("trackName", local.trackName);
            body->setProperty ("beat", local.beat);
        }
        const auto tempId = "pending-" + local.id;
        outbox.push_back (makeOp ("add", tempId, var (body.get())));
        for (auto& r : local.replies)
        {
            DynamicObject::Ptr rb = new DynamicObject();
            rb->setProperty ("text", r.text);
            outbox.push_back (makeOp ("reply", tempId, var (rb.get()), "pending-" + r.id));
        }
        forgetUploadedInFile (local.id);
    }
    ctx.edit.localNotes.clear();
    reapplyUnsent();
    saveState();
    uploading = false;
    changed();
    flush();
}

//==============================================================================
void NotesController::add (int trackId, const String& trackName, double beat, const String& text, std::function<void (bool)> done)
{
    if (! isCloud())
    {
        SongNote n;
        n.id = newLocalId();
        n.track = trackId;
        n.trackName = trackName;
        n.beat = trackId >= 0 ? jmax (0.0, beat) : 0.0;
        n.text = text;
        n.author = getMe();
        n.createdAt = n.updatedAt = Time::currentTimeMillis();
        ctx.edit.localNotes.push_back (n);
        saveLocal();
        if (done) done (true);
        return;
    }
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("text", text);
    if (trackId >= 0)
    {
        body->setProperty ("track", trackId);
        body->setProperty ("trackName", trackName);
        body->setProperty ("beat", jmax (0.0, beat));
    }
    enqueue (makeOp ("add", "pending-" + newLocalId(), var (body.get())));
    if (done) done (true);
}

void NotesController::reply (const String& noteId, const String& text, std::function<void (bool)> done)
{
    if (! isCloud())
    {
        for (auto& n : ctx.edit.localNotes)
            if (n.id == noteId)
                n.replies.push_back ({ newLocalId(), getMe(), {}, text, Time::currentTimeMillis() });
        saveLocal();
        if (done) done (true);
        return;
    }
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("text", text);
    enqueue (makeOp ("reply", noteId, var (body.get()), "pending-" + newLocalId()));
    if (done) done (true);
}

void NotesController::setResolved (const String& noteId, bool resolved)
{
    if (! isCloud())
    {
        for (auto& n : ctx.edit.localNotes)
            if (n.id == noteId)
            {
                n.resolved = resolved;
                n.resolvedBy = resolved ? getMe() : String();
            }
        saveLocal();
        return;
    }
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("resolved", resolved);
    enqueue (makeOp ("update", noteId, var (body.get())));
}

void NotesController::editText (const String& noteId, const String& text, std::function<void (bool)> done)
{
    if (! isCloud())
    {
        for (auto& n : ctx.edit.localNotes)
            if (n.id == noteId)
                n.text = text;
        saveLocal();
        if (done) done (true);
        return;
    }
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("text", text);
    enqueue (makeOp ("update", noteId, var (body.get())));
    if (done) done (true);
}

void NotesController::remove (const String& noteId)
{
    if (! isCloud())
    {
        auto& list = ctx.edit.localNotes;
        list.erase (std::remove_if (list.begin(), list.end(), [&] (const SongNote& n) { return n.id == noteId; }), list.end());
        saveLocal();
        return;
    }
    enqueue (makeOp ("delete", noteId, {}));
}

void NotesController::removeReply (const String& noteId, const String& replyId)
{
    if (! isCloud())
    {
        for (auto& n : ctx.edit.localNotes)
            if (n.id == noteId)
                n.replies.erase (std::remove_if (n.replies.begin(), n.replies.end(), [&] (const SongNoteReply& r) { return r.id == replyId; }),
                                 n.replies.end());
        saveLocal();
        return;
    }
    enqueue (makeOp ("deleteReply", noteId, {}, replyId));
}

//==============================================================================
String NotesController::describePosition (double beat) const
{
    const auto& e = ctx.edit;
    const int bar = (int) std::floor (beat / e.beatsPerBar()) + 1;
    const double seconds = beat * 60.0 / e.tempo;
    const int mins = (int) (seconds / 60.0);
    const int secs = (int) seconds - mins * 60;
    return "bar " + String (bar) + " (" + String (mins) + ":" + String (secs).paddedLeft ('0', 2) + ")";
}

String NotesController::describeWhere (const SongNote& n) const
{
    if (n.isSongNote())
        return "Song note";
    String name = n.trackName;
    bool gone = true;
    if (auto t = ctx.edit.findTrack (n.track))
    {
        name = t->name;
        gone = false;
    }
    return (name.isNotEmpty() ? name : String ("A track")) + (gone ? " (track not here)" : String()) + ", " + describePosition (n.beat);
}

//==============================================================================
/** One note's conversation (or a new note being written), in a call-out bubble. */
class NotesController::ThreadView : public Component, private ChangeListener
{
public:
    /** An existing note. */
    ThreadView (NotesController& c, const String& id) : nc (c), noteId (id)
    {
        init();
        nc.addChangeListener (this);
        rebuild();
        if (auto* n = nc.find (noteId))
            nc.markSeen (*n);
    }

    /** A new note on a track (or on the song when trackId < 0). */
    ThreadView (NotesController& c, int trackId, double atBeat) : nc (c), newTrack (trackId), newBeat (atBeat), composing (true)
    {
        init();
        rebuild();
    }

    ~ThreadView() override   { nc.removeChangeListener (this); }

    void paint (Graphics& g) override   { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        where.setBounds (r.removeFromTop (20));
        if (state.getText().isNotEmpty())
            state.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        auto buttons = r.removeFromBottom (28);
        send.setBounds (buttons.removeFromRight (send.getBestWidthForHeight (28) + 18));
        for (auto* b : { &resolve, &edit, &del })
            if (b->isVisible())
            {
                b->setBounds (buttons.removeFromLeft (b->getBestWidthForHeight (28) + 14));
                buttons.removeFromLeft (6);
            }
        r.removeFromBottom (8);
        editor.setBounds (r.removeFromBottom (composing ? 96 : 58));
        r.removeFromBottom (8);
        view.setBounds (r);
        messages.setSize (jmax (100, view.getWidth() - view.getScrollBarThickness()), messages.getHeight());
        layoutMessages();
    }

private:
    struct Message : public Component
    {
        String author, when, text;
        bool main = false;
        TextButton remove { "Delete" };
        std::function<void()> onDelete;

        int heightFor (int width) const
        {
            return 22 + (int) std::ceil (layoutFor (width).getHeight()) + 10;
        }

        TextLayout layoutFor (int width) const
        {
            AttributedString s;
            s.setWordWrap (AttributedString::byWord);
            s.append (text, theme::font (13.5f), theme::text);
            TextLayout l;
            l.createLayout (s, (float) jmax (40, width - 8));
            return l;
        }

        void paint (Graphics& g) override
        {
            if (main)
            {
                g.setColour (theme::lane);
                g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
            }
            g.setColour (theme::text);
            g.setFont (theme::font (12.5f, true));
            g.drawText (author, 4, 2, getWidth() - 120, 18, Justification::centredLeft, true);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText (when, 4 + GlyphArrangement::getStringWidthInt (theme::font (12.5f, true), author) + 8, 2, 120, 18,
                        Justification::centredLeft, true);
            layoutFor (getWidth()).draw (g, Rectangle<float> (4.0f, 22.0f, (float) getWidth() - 8.0f, (float) getHeight() - 26.0f));
        }

        void resized() override   { remove.setBounds (getWidth() - 62, 1, 58, 20); }
    };

    void init()
    {
        where.setFont (theme::font (13.0f, true));
        state.setFont (theme::font (11.5f));
        state.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (where);
        addAndMakeVisible (state);
        view.setViewedComponent (&messages, false);
        view.setScrollBarsShown (true, false);
        addAndMakeVisible (view);
        styleEditor (editor);
        editor.setMultiLine (true, true);
        editor.setReturnKeyStartsNewLine (true);
        addAndMakeVisible (editor);
        styleSmall (send, true);
        send.onClick = [this] { submit(); };
        addAndMakeVisible (send);
        resolve.onClick = [this] { if (auto* n = nc.find (noteId)) nc.setResolved (noteId, ! n->resolved); };
        edit.onClick = [this] { startEditing(); };
        del.onClick = [this] { deleteNote(); };
        addChildComponent (resolve);
        addChildComponent (edit);
        addChildComponent (del);
    }

    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (! composing && nc.find (noteId) == nullptr)
        {
            close();   // deleted (here or by someone else)
            return;
        }
        rebuild();
    }

    void close()
    {
        if (auto* box = findParentComponentOfClass<CallOutBox>())
            box->dismiss();
    }

    void rebuild()
    {
        const auto me = nc.getMe();
        messages.removeAllChildren();
        items.clear();

        if (composing)
        {
            where.setText (newTrack < 0 ? String ("New song note")
                                        : "New note: " + (nc.ctx.edit.findTrack (newTrack) != nullptr ? nc.ctx.edit.findTrack (newTrack)->name : String ("track"))
                                              + ", " + nc.describePosition (newBeat),
                           dontSendNotification);
            state.setText (nc.isCloud() ? String ("Everyone this song is shared with will see it.")
                                        : String ("Saved with the song. If you put it in the cloud, the note goes too."),
                           dontSendNotification);
            editor.setTextToShowWhenEmpty (newTrack < 0 ? "Something about the whole song: the plan so far, what's left..."
                                                        : "What about this part? (\"The bass doesn't sit right here, can you try...\")",
                                           theme::textDim);
            send.setButtonText ("Add Note");
            setSize (380, 220);
            resized();
            return;
        }

        const auto* n = nc.find (noteId);
        if (n == nullptr)
            return;

        where.setText (nc.describeWhere (*n), dontSendNotification);
        state.setText (n->resolved ? "Resolved" + (n->resolvedBy.isNotEmpty() ? " by " + who (n->resolvedBy, me) : String()) + ". Reopen it to keep talking."
                                   : String(), dontSendNotification);

        auto add = [&] (const String& author, int64 when, const String& text, bool main, std::function<void()> onDelete, bool unsent = false)
        {
            auto* m = items.add (new Message());
            m->author = who (author, me);
            m->when = unsent ? String ("not sent yet") : when > 0 ? timeAgo (when) : String();
            m->text = text;
            m->main = main;
            if (onDelete)
            {
                m->onDelete = onDelete;
                m->remove.onClick = [m]
                {
                    if (m->remove.getButtonText() == "Delete")
                        m->remove.setButtonText ("Sure?");
                    else if (m->onDelete)
                        m->onDelete();
                };
                m->addAndMakeVisible (m->remove);
            }
            messages.addAndMakeVisible (m);
        };
        add (n->author, n->createdAt, n->text, true, {}, n->pending);
        const auto id = noteId;
        for (auto& r : n->replies)
        {
            const auto replyId = r.id;
            add (r.author, r.createdAt, r.text, false,
                 nc.canDelete (r.author) ? std::function<void()> ([this, id, replyId] { nc.removeReply (id, replyId); }) : nullptr,
                 n->pending || replyId.startsWith ("pending-"));
        }

        resolve.setButtonText (n->resolved ? "Reopen" : "Resolve");
        resolve.setVisible (true);
        edit.setVisible (nc.canEditText (*n) && ! editing);
        del.setVisible (nc.canDelete (n->author));
        del.setButtonText ("Delete Note");
        send.setButtonText (editing ? "Save" : "Reply");
        if (! editing)
            editor.setTextToShowWhenEmpty ("Reply...", theme::textDim);

        const int w = 380;
        int contentH = 0;
        for (auto* m : items)
            contentH += m->heightFor (w - 24 - view.getScrollBarThickness()) + 6;
        const int extra = 20 + (state.getText().isNotEmpty() ? 18 : 0) + 4 + 8 + 58 + 8 + 28 + 20;
        setSize (w, jlimit (200, 520, extra + contentH));
        resized();
        view.setViewPosition (0, messages.getHeight());   // newest at the bottom, in view
    }

    void layoutMessages()
    {
        int y = 0;
        const int w = messages.getWidth();
        for (auto* m : items)
        {
            const int h = m->heightFor (w);
            m->setBounds (0, y, w, h);
            y += h + 6;
        }
        messages.setSize (w, jmax (1, y));
    }

    void startEditing()
    {
        if (auto* n = nc.find (noteId))
        {
            editing = true;
            editor.setText (n->text, false);
            editor.grabKeyboardFocus();
            rebuild();
        }
    }

    void deleteNote()
    {
        if (del.getButtonText() == "Delete Note")
        {
            const auto* n = nc.find (noteId);
            del.setButtonText (n != nullptr && ! n->replies.empty() ? "Delete All " + String ((int) n->replies.size() + 1) + "?" : String ("Sure?"));
            resized();
            return;
        }
        nc.remove (noteId);
    }

    void submit()
    {
        const auto text = editor.getText().trim();
        if (text.isEmpty())
            return;
        Component::SafePointer<ThreadView> safe (this);
        send.setEnabled (false);
        auto finished = [safe] (bool ok)
        {
            if (safe == nullptr)
                return;
            safe->send.setEnabled (true);
            if (! ok)
                return;
            safe->editor.clear();
            if (safe->composing)
                safe->close();
            else
            {
                safe->editing = false;
                safe->rebuild();
            }
        };
        if (composing)
        {
            auto t = nc.ctx.edit.findTrack (newTrack);
            nc.add (newTrack, t != nullptr ? t->name : String(), newBeat, text, finished);
        }
        else if (editing)
            nc.editText (noteId, text, finished);
        else
            nc.reply (noteId, text, finished);
    }

    NotesController& nc;
    String noteId;
    int newTrack = -1;
    double newBeat = 0.0;
    bool composing = false, editing = false;
    Label where, state;
    Viewport view;
    Component messages;
    OwnedArray<Message> items;
    TextEditor editor;
    TextButton send, resolve, edit { "Edit" }, del;
};

void NotesController::showThread (const String& noteId, Rectangle<int> screenArea)
{
    if (find (noteId) == nullptr)
        return;
    CallOutBox::launchAsynchronously (std::make_unique<ThreadView> (*this, noteId), screenArea, nullptr);
}

void NotesController::showCompose (int trackId, double beat, Rectangle<int> screenArea)
{
    auto view = std::make_unique<ThreadView> (*this, trackId, beat);
    auto* raw = view.get();
    CallOutBox::launchAsynchronously (std::move (view), screenArea, nullptr);
    Component::SafePointer<Component> safe (raw);
    MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            for (auto* c : safe->getChildren())
                if (auto* e = dynamic_cast<TextEditor*> (c))
                    e->grabKeyboardFocus();
    });
}

//==============================================================================
/** Every note on the song: song notes first, then notes on tracks in time order. */
class NotesController::NotesWindow : public DocumentWindow
{
public:
    explicit NotesWindow (NotesController& c)
        : DocumentWindow ("Notes", theme::panel, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new Content (c), true);
        setResizable (true, true);
        setResizeLimits (420, 300, 1200, 1400);
        centreWithSize (520, 560);
    }

    void closeButtonPressed() override   { delete this; }

private:
    class Content : public Component, private ListBoxModel, private ChangeListener
    {
    public:
        explicit Content (NotesController& c) : nc (c)
        {
            showLabel.setText ("On tracks", dontSendNotification);
            showLabel.setColour (Label::textColourId, theme::textDim);
            showLabel.setFont (theme::font (12.5f));
            addAndMakeVisible (showLabel);
            show.addItem ("Show every note", (int) Show::all);
            show.addItem ("Hide resolved notes", (int) Show::unresolved);
            show.addItem ("Hide all notes", (int) Show::none);
            show.setSelectedId ((int) nc.getShow(), dontSendNotification);
            show.onChange = [this] { nc.setShow ((Show) show.getSelectedId()); };
            addAndMakeVisible (show);
            styleSmall (addSong, true);
            addSong.onClick = [this]
            {
                nc.showCompose (-1, 0.0, addSong.getScreenBounds());
            };
            addAndMakeVisible (addSong);
            hint.setColour (Label::textColourId, theme::textDim);
            hint.setFont (theme::font (12.0f));
            hint.setJustificationType (Justification::centred);
            addAndMakeVisible (hint);
            list.setModel (this);
            list.setRowHeight (46);
            list.setColour (ListBox::backgroundColourId, theme::lane);
            addAndMakeVisible (list);
            nc.addChangeListener (this);
            setSize (520, 560);
            update();
            nc.markAllSeen();
            nc.refresh();
        }

        ~Content() override   { nc.removeChangeListener (this); }

        void paint (Graphics& g) override   { g.fillAll (theme::panel); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12, 10);
            auto top = r.removeFromTop (28);
            addSong.setBounds (top.removeFromRight (130));
            showLabel.setBounds (top.removeFromLeft (66));
            show.setBounds (top.removeFromLeft (170));
            r.removeFromTop (10);
            hint.setBounds (r.removeFromBottom (36));
            list.setBounds (r);
        }

    private:
        void changeListenerCallback (ChangeBroadcaster*) override   { update(); }

        void update()
        {
            order.clear();
            const auto& all = nc.getNotes();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].isSongNote())
                    order.push_back ((int) i);
            std::vector<int> onTracks;
            for (size_t i = 0; i < all.size(); ++i)
                if (! all[i].isSongNote())
                    onTracks.push_back ((int) i);
            std::stable_sort (onTracks.begin(), onTracks.end(), [&] (int a, int b) { return all[(size_t) a].beat < all[(size_t) b].beat; });
            order.insert (order.end(), onTracks.begin(), onTracks.end());
            show.setSelectedId ((int) nc.getShow(), dontSendNotification);
            list.updateContent();
            list.repaint();

            String text = nc.getError();
            if (text.isEmpty())
                text = nc.getSyncProblem();
            if (text.isEmpty())
                text = all.empty() ? String ("No notes yet. Right-click a track at a moment in the song: Add Note Here. Or add a song note above.")
                                   : nc.isCloud() ? String ("Everyone this song is shared with sees these. Click one to open the conversation.")
                                                  : String ("These are saved with the song. Put it in the cloud and they go with it.");
            hint.setText (text, dontSendNotification);
        }

        int getNumRows() override   { return (int) order.size(); }

        void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
        {
            if (! isPositiveAndBelow (row, (int) order.size()))
                return;
            const auto& all = nc.getNotes();
            if (! isPositiveAndBelow (order[(size_t) row], (int) all.size()))
                return;
            const auto& n = all[(size_t) order[(size_t) row]];
            if (selected)
                g.fillAll (theme::headerSel);
            const float alpha = n.resolved ? 0.55f : 1.0f;

            if (nc.isUnread (n))
            {
                g.setColour (theme::accent);
                g.fillEllipse (6.0f, 10.0f, 7.0f, 7.0f);
            }
            g.setColour (theme::text.withAlpha (alpha));
            g.setFont (theme::font (13.0f, true));
            g.drawText (nc.describeWhere (n), 18, 4, w - 140, 18, Justification::centredLeft, true);

            String right;
            if (! n.replies.empty())
                right << (int) n.replies.size() << (n.replies.size() == 1 ? " reply" : " replies");
            if (n.resolved)
                right << (right.isEmpty() ? "" : ", ") << "resolved";
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText (right, w - 130, 4, 122, 18, Justification::centredRight, true);

            g.setColour (theme::textDim.withAlpha (alpha));
            g.setFont (theme::font (12.5f));
            g.drawText (who (n.author, nc.getMe()) + ": " + n.text.replaceCharacters ("\r\n", "  "), 18, 23, w - 26, 18,
                        Justification::centredLeft, true);
            g.setColour (theme::border);
            g.drawHorizontalLine (h - 1, 0.0f, (float) w);
        }

        void listBoxItemClicked (int row, const MouseEvent&) override
        {
            if (! isPositiveAndBelow (row, (int) order.size()))
                return;
            const auto& all = nc.getNotes();
            const auto n = all[(size_t) order[(size_t) row]];
            if (! n.isSongNote() && nc.reveal)
                nc.reveal (n.track, n.beat);
            auto area = list.getRowPosition (row, true);
            nc.showThread (n.id, list.localAreaToGlobal (area));
        }

        NotesController& nc;
        Label showLabel, hint;
        ComboBox show;
        TextButton addSong { "Add Song Note..." };
        ListBox list;
        std::vector<int> order;
    };
};

void NotesController::showWindow()
{
    if (auto* w = window.getComponent())
    {
        w->toFront (true);
        markAllSeen();
        return;
    }
    auto* w = new NotesWindow (*this);
    window = w;
    w->setVisible (true);
}
} // namespace lathe
