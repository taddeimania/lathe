#pragma once

#include "../Common.h"

namespace lathe
{
/** A reply in a note's thread. */
struct SongNoteReply
{
    String id, author, authorId, text;
    int64 createdAt = 0;
};

/** A note left on the song (track < 0) or on a track at a time, with its replies.
    Cloud songs keep notes on the server; songs that aren't in the cloud keep them in
    the project file. The same shape (JSON) serves both. */
struct SongNote
{
    String id;
    int track = -1;              // the track's id; -1 for a note about the whole song
    String trackName;            // what the track was called when the note was left
    double beat = 0.0;
    String text, author, authorId;
    int64 createdAt = 0, updatedAt = 0;
    bool resolved = false;
    String resolvedBy;
    std::vector<SongNoteReply> replies;

    bool isSongNote() const noexcept   { return track < 0; }

    /** The newest thing in the thread (the note or a reply). */
    int64 lastActivity() const
    {
        auto t = createdAt;
        for (auto& r : replies)
            t = jmax (t, r.createdAt);
        return t;
    }

    var toVar() const
    {
        DynamicObject::Ptr o = new DynamicObject();
        o->setProperty ("id", id);
        o->setProperty ("track", track >= 0 ? var (track) : var());
        o->setProperty ("trackName", trackName);
        o->setProperty ("beat", track >= 0 ? var (beat) : var());
        o->setProperty ("text", text);
        o->setProperty ("author", author);
        o->setProperty ("createdAt", createdAt);
        o->setProperty ("updatedAt", updatedAt);
        o->setProperty ("resolved", resolved);
        o->setProperty ("resolvedBy", resolvedBy);
        Array<var> list;
        for (auto& r : replies)
        {
            DynamicObject::Ptr ro = new DynamicObject();
            ro->setProperty ("id", r.id);
            ro->setProperty ("author", r.author);
            ro->setProperty ("text", r.text);
            ro->setProperty ("createdAt", r.createdAt);
            list.add (var (ro.get()));
        }
        o->setProperty ("replies", list);
        return var (o.get());
    }

    /** Reads the server's JSON or Lathe's own. */
    static SongNote fromVar (const var& v)
    {
        SongNote n;
        n.id = v["id"].toString();
        n.track = v["track"].isVoid() ? -1 : (int) v["track"];
        n.trackName = v["trackName"].toString();
        n.beat = v["beat"].isVoid() ? 0.0 : (double) v["beat"];
        n.text = v["text"].toString();
        n.author = v["author"].toString();
        n.authorId = v["authorId"].toString();
        n.createdAt = (int64) v["createdAt"];
        n.updatedAt = (int64) v["updatedAt"];
        n.resolved = (bool) v["resolved"];
        n.resolvedBy = v["resolvedBy"].toString();
        if (auto* list = v["replies"].getArray())
            for (auto& r : *list)
                n.replies.push_back ({ r["id"].toString(), r["author"].toString(), r["authorId"].toString(),
                                       r["text"].toString(), (int64) r["createdAt"] });
        return n;
    }
};
} // namespace lathe
