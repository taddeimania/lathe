#pragma once

#include "Widgets.h"
#include "../Cloud/CloudClient.h"

namespace lathe
{
class CloudController;

//==============================================================================
/** Notes on the open song: on the whole song, or on a track at a time, each with a
    thread of replies. A cloud song's notes live on the server (everyone sees them
    within a minute, no Push needed); other songs keep them in the project file and
    upload them when the song goes to the cloud.

    Views listen for changes (ChangeBroadcaster). Every action runs in the background;
    `done` (if given) gets true when it worked. */
class NotesController : public ChangeBroadcaster, private Timer
{
public:
    NotesController (AppContext&, CloudController&);
    ~NotesController() override;

    /** What the arrangement shows: every note, unresolved ones, or none. */
    enum class Show { all = 1, unresolved = 2, none = 3 };
    Show getShow() const;
    void setShow (Show);
    bool isShownOnTracks (const SongNote&) const;

    const std::vector<SongNote>& getNotes() const noexcept   { return isCloud() ? notes : ctx.edit.localNotes; }
    const SongNote* find (const String& id) const;
    bool isCloud() const noexcept     { return songId.isNotEmpty(); }
    String getError() const           { return error; }
    /** Why notes aren't syncing right now ("" when they are). */
    String getSyncProblem() const     { return syncProblem; }
    /** Changes waiting to be sent (made while the cloud couldn't be reached). */
    int numUnsent() const noexcept    { return (int) outbox.size(); }
    String getMe() const;

    bool canEditText (const SongNote&) const;
    bool canDelete (const String& author) const;

    /** Notes and replies from others you haven't opened yet. */
    int unreadCount() const;
    bool isUnread (const SongNote&) const;
    void markSeen (const SongNote&);
    void markAllSeen();

    void add (int trackId, const String& trackName, double beat, const String& text, std::function<void (bool)> done = {});
    void reply (const String& noteId, const String& text, std::function<void (bool)> done = {});
    void setResolved (const String& noteId, bool resolved);
    void editText (const String& noteId, const String& text, std::function<void (bool)> done = {});
    void remove (const String& noteId);
    void removeReply (const String& noteId, const String& replyId);
    void refresh();

    // --- UI
    /** The conversation, in a bubble pointing at screenArea (screen coordinates). */
    void showThread (const String& noteId, Rectangle<int> screenArea);
    /** The thread's view on its own (for screenshots); null if there's no such note. */
    std::unique_ptr<Component> makeThreadView (const String& noteId);
    /** A new note: on a track at a beat, or (trackId < 0) on the song. */
    void showCompose (int trackId, double beat, Rectangle<int> screenArea);
    void showWindow();

    /** "bar 33 (1:04)" */
    String describePosition (double beat) const;
    /** "Bass, bar 33 (1:04)" or "Song note". The track's current name if it's in this copy;
        if not, the name it had and why it isn't here: "not pulled yet" (a newer revision is
        waiting), "not pushed yet" (it's only on their computer so far) or "deleted". */
    String describeWhere (const SongNote&) const;
    /** Why a note's track isn't in this copy ("" if it is, or for a song note). */
    String whyTrackMissing (const SongNote&) const;

    /** Called when a note on a track is picked in the Notes window: show that spot. */
    std::function<void (int trackId, double beat)> reveal;

private:
    class ThreadView;
    class NotesWindow;

    void timerCallback() override;
    void changed();
    void uploadLocalNotes();
    void forgetUploadedInFile (const String& noteId);
    void apply (const var& serverNote);   // replace (or add) one note from the server's answer

    // Every change to a cloud song's notes goes through the outbox: shown at once,
    // sent in order, kept (in the song's .lathe/notes.json) until the server has it.
    void enqueue (const var& op);
    void flush();
    void optimistic (const var& op);      // what the change looks like before the server has it
    void reapplyUnsent();
    bool isConnectionProblem (const cloud::ApiError&) const;
    void setSyncProblem (const String&);
    String offlineMessage (const cloud::ApiError&) const;
    File stateFile() const;
    void loadState();
    void saveState();
    SongNote* findMutable (const String& id);
    void saveLocal();
    void loadSeen();
    void saveSeen();

    AppContext& ctx;
    CloudController& cloud;
    std::vector<SongNote> notes;
    String songId, sourceKey, error, syncProblem;
    std::vector<var> outbox;
    bool flushing = false;
    int flushCountdown = 0;
    var seen;                     // {noteId: last activity seen}
    std::set<String> seenTracks;  // tags of every track this song has had here (so a gone one is "deleted")
    void rememberTracks();
    int pollCountdown = 0;
    bool uploading = false;
    Component::SafePointer<DocumentWindow> window;

    JUCE_DECLARE_WEAK_REFERENCEABLE (NotesController)
};
} // namespace lathe
