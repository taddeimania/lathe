#include "Revisions.h"
#include "ProjectDiff.h"

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING ""
#endif

namespace lathe::cloud
{
namespace
{
String fileNameOf (const String& path)
{
    return path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
}

bool report (const Progress& progress, const String& what, double p)
{
    return progress == nullptr || progress (what, jlimit (0.0, 1.0, p));
}

/** Calls fn (clipVar) for every audio clip in a project JSON. */
void forEachAudioClip (const var& project, const std::function<void (const var&)>& fn)
{
    for (auto* list : { "tracks", "returns" })
        if (auto* tracks = project[list].getArray())
            for (auto& t : *tracks)
                if (auto* clips = t["clips"].getArray())
                    for (auto& c : *clips)
                        if (c["kind"].toString() == "audio")
                            fn (c);
}
} // namespace

//==============================================================================
var Revision::toVar() const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("id", id);
    o->setProperty ("seq", seq);
    o->setProperty ("parent", parent.isNotEmpty() ? var (parent) : var());
    o->setProperty ("author", author);
    o->setProperty ("message", message);
    o->setProperty ("summary", summary);
    o->setProperty ("snapshot", snapshot);
    Array<var> b;
    for (auto& h : blobs)
        b.add (h);
    o->setProperty ("blobs", b);
    o->setProperty ("createdAt", createdAt);
    o->setProperty ("local", local);
    if (pinned)
        o->setProperty ("pinned", true);
    if (! inCloud)
        o->setProperty ("gone", true);
    return var (o.get());
}

Revision Revision::fromVar (const var& v)
{
    Revision r;
    r.id = v["id"].toString();
    r.seq = (int) v["seq"];
    r.parent = v["parent"].isString() ? v["parent"].toString() : String();
    r.author = v["author"].toString();
    r.message = v["message"].toString();
    r.summary = v["summary"].toString();
    r.snapshot = v["snapshot"].toString();
    if (auto* b = v["blobs"].getArray())
        for (auto& h : *b)
            r.blobs.add (h.toString());
    r.createdAt = (int64) (double) v["createdAt"];
    r.local = (bool) v["local"];
    r.pinned = (bool) v["pinned"];
    r.inCloud = ! (bool) v["gone"];
    return r;
}

//==============================================================================
LocalStore::LocalStore (const File& f) : projectFile (f) {}

var LocalStore::readLink() const
{
    return JSON::parse (getDir().getChildFile ("cloud.json"));
}

void LocalStore::writeLink (const var& v)
{
    getDir().createDirectory();
    getDir().getChildFile ("cloud.json").replaceWithText (JSON::toString (v));
}

bool LocalStore::isLinked() const       { return getProjectId().isNotEmpty(); }
String LocalStore::getProjectId() const { return readLink()["projectId"].toString(); }
String LocalStore::getCloudName() const { return readLink()["name"].toString(); }
String LocalStore::getBase() const      { return readLink()["base"].toString(); }

void LocalStore::link (const String& projectId, const String& name)
{
    const ScopedLock sl (lock);
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("projectId", projectId);
    o->setProperty ("name", name);
    o->setProperty ("base", "");
    writeLink (var (o.get()));
}

void LocalStore::setBase (const String& revisionId)
{
    const ScopedLock sl (lock);
    auto v = readLink();
    if (auto* o = v.getDynamicObject())
    {
        o->setProperty ("base", revisionId);
        writeLink (v);
    }
}

String LocalStore::getSetAside() const   { return readLink()["setAside"].toString(); }

void LocalStore::setSetAside (const String& revisionId)
{
    const ScopedLock sl (lock);
    auto v = readLink();
    if (auto* o = v.getDynamicObject())
    {
        if (revisionId.isEmpty())
            o->removeProperty ("setAside");
        else
            o->setProperty ("setAside", revisionId);
        writeLink (v);
    }
}

void LocalStore::unlink()
{
    getDir().getChildFile ("cloud.json").deleteFile();
}

File LocalStore::objectFile (const String& hash) const
{
    return getDir().getChildFile ("objects").getChildFile (hash + ".json");
}

bool LocalStore::hasObject (const String& hash) const
{
    return objectFile (hash).existsAsFile();
}

String LocalStore::putObject (const MemoryBlock& data)
{
    const auto hash = sha256OfData (data);
    const auto f = objectFile (hash);
    if (! f.existsAsFile())
    {
        f.getParentDirectory().createDirectory();
        f.replaceWithData (data.getData(), data.getSize());
    }
    return hash;
}

var LocalStore::readSnapshot (const String& hash) const
{
    if (hash.isEmpty() || ! hasObject (hash))
        return {};
    return JSON::parse (objectFile (hash));
}

void LocalStore::putRevision (const Revision& r)
{
    const ScopedLock sl (lock);
    const auto dir = getDir().getChildFile ("revisions");
    dir.createDirectory();
    dir.getChildFile (r.id + ".json").replaceWithText (JSON::toString (r.toVar()));
}

std::vector<Revision> LocalStore::getRevisions() const
{
    std::vector<Revision> out;
    for (auto& f : getDir().getChildFile ("revisions").findChildFiles (File::findFiles, false, "*.json"))
    {
        auto r = Revision::fromVar (JSON::parse (f));
        if (r.id.isNotEmpty())
            out.push_back (r);
    }
    std::sort (out.begin(), out.end(), [] (const Revision& a, const Revision& b)
    {
        if (a.createdAt != b.createdAt)
            return a.createdAt > b.createdAt;
        return a.seq > b.seq;
    });
    return out;
}

std::optional<Revision> LocalStore::getRevision (const String& id) const
{
    if (id.isEmpty())
        return {};
    const auto f = getDir().getChildFile ("revisions").getChildFile (id + ".json");
    if (! f.existsAsFile())
        return {};
    return Revision::fromVar (JSON::parse (f));
}

bool LocalStore::hasRevision (const String& id) const
{
    return id.isNotEmpty() && getDir().getChildFile ("revisions").getChildFile (id + ".json").existsAsFile();
}

void LocalStore::loadHashCache()
{
    if (hashCacheLoaded)
        return;
    hashCacheLoaded = true;
    hashCache = JSON::parse (getDir().getChildFile ("hashes.json"));
    if (! hashCache.isObject())
        hashCache = var (new DynamicObject());
}

String LocalStore::hashOf (const File& f, const std::function<bool (double)>& progress)
{
    {
        const ScopedLock sl (lock);
        loadHashCache();
        const auto entry = hashCache[Identifier (f.getFullPathName())];
        if (entry.isObject() && (int64) entry["size"] == f.getSize()
            && (int64) entry["mtime"] == f.getLastModificationTime().toMilliseconds())
            return entry["hash"].toString();
    }

    // Hash without holding the lock: a big file takes a while.
    const auto hash = sha256OfFile (f, progress);
    if (hash.isNotEmpty())
        rememberHash (f, hash);
    return hash;
}

void LocalStore::rememberHash (const File& f, const String& hash)
{
    const ScopedLock sl (lock);
    loadHashCache();
    DynamicObject::Ptr e = new DynamicObject();
    e->setProperty ("size", f.getSize());
    e->setProperty ("mtime", f.getLastModificationTime().toMilliseconds());
    e->setProperty ("hash", hash);
    if (auto* o = hashCache.getDynamicObject())
        o->setProperty (Identifier (f.getFullPathName()), var (e.get()));
    hashCacheDirty = true;
}

File LocalStore::findLocalFile (const String& hash)
{
    const ScopedLock sl (lock);
    loadHashCache();
    if (auto* o = hashCache.getDynamicObject())
    {
        for (auto& p : o->getProperties())
        {
            if (p.value["hash"].toString() != hash)
                continue;
            const File f (p.name.toString());
            if (f.existsAsFile() && f.getSize() == (int64) p.value["size"]
                && f.getLastModificationTime().toMilliseconds() == (int64) p.value["mtime"])
                return f;
        }
    }
    return {};
}

void LocalStore::saveHashCache()
{
    const ScopedLock sl (lock);
    if (! hashCacheDirty)
        return;
    getDir().createDirectory();
    getDir().getChildFile ("hashes.json").replaceWithText (JSON::toString (hashCache, true));
    hashCacheDirty = false;
}

//==============================================================================
Snapshot Snapshot::capture (const Edit& edit)
{
    Snapshot s;
    s.project = edit.toVar (false);
    if (auto* o = s.project.getDynamicObject())
    {
        o->removeProperty ("insertMarker");   // where Play starts isn't part of the song
        o->removeProperty ("notes");          // notes live on the server for cloud songs
    }

    auto describe = [&s] (const var& trackVar, const Track& t, bool isTrack)
    {
        // Readable plug-in settings, so a change can say "Decay 2.1 s -> 3.4 s".
        if (auto* plugins = trackVar["plugins"].getArray())
        {
            for (size_t i = 0; i < t.plugins.size() && (int) i < plugins->size(); ++i)
            {
                auto& slot = t.plugins[i];
                auto* po = plugins->getReference ((int) i).getDynamicObject();
                if (slot->instance == nullptr || po == nullptr)
                    continue;
                DynamicObject::Ptr params = new DynamicObject();
                int n = 0;
                for (auto* p : slot->instance->getParameters())
                {
                    if (++n > 512)
                        break;
                    const auto name = p->getName (64).trim();
                    if (name.isEmpty())
                        continue;
                    auto text = p->getCurrentValueAsText().trim();
                    if (const auto unit = p->getLabel().trim(); unit.isNotEmpty() && ! text.endsWith (unit))
                        text << " " << unit;
                    params->setProperty (Identifier (name), text);
                }
                po->setProperty ("params", var (params.get()));
            }
        }

        // Audio clips, to be hashed later.
        if (auto* clips = trackVar["clips"].getArray())
            for (size_t i = 0; i < t.clips.size() && (int) i < clips->size(); ++i)
                if (t.clips[i].kind == ClipKind::audio)
                    s.audio.push_back ({ clips->getReference ((int) i), t.clips[i].file, isTrack ? t.id : -1 });
    };

    if (auto* tracks = s.project["tracks"].getArray())
        for (size_t i = 0; i < edit.tracks.size() && (int) i < tracks->size(); ++i)
            describe (tracks->getReference ((int) i), *edit.tracks[i], true);
    if (auto* returns = s.project["returns"].getArray())
        for (size_t i = 0; i < edit.buses.size() && (int) i < returns->size(); ++i)
            describe (returns->getReference ((int) i), *edit.buses[i], false);
    if (edit.master != nullptr)
        describe (s.project["master"], *edit.master, false);
    return s;
}

Result Snapshot::finish (LocalStore& store, const Progress& progress)
{
    // Clips often share a file (a take split into pieces, a loop pasted along the
    // song): hash and count each file once, then stamp every clip that uses it.
    std::vector<File> files;
    std::vector<std::vector<var>> clipsOf;
    for (auto& a : audio)
    {
        auto it = std::find (files.begin(), files.end(), a.file);
        if (it == files.end())
        {
            files.push_back (a.file);
            clipsOf.emplace_back();
            it = files.end() - 1;
        }
        clipsOf[(size_t) (it - files.begin())].push_back (a.clip);
    }

    StringArray missing;
    const int total = (int) files.size();
    for (int index = 1; index <= total; ++index)
    {
        const auto& file = files[(size_t) index - 1];
        const auto label = "Checking audio files (" + String (index) + " of " + String (total) + ")";
        if (! report (progress, label, (double) (index - 1) / (double) total))
        {
            store.saveHashCache();   // keep what's done: next time is quicker
            return Result::fail ("Cancelled.");
        }
        if (! file.existsAsFile())
        {
            missing.addIfNotAlreadyThere (file.getFileName());
            continue;
        }
        const auto fileHash = store.hashOf (file, [&] (double p)
        {
            return report (progress, label, ((double) (index - 1) + p) / (double) total);
        });
        if (fileHash.isEmpty())
        {
            store.saveHashCache();
            return Result::fail ("Cancelled.");
        }
        for (auto& clip : clipsOf[(size_t) index - 1])
            if (auto* o = clip.getDynamicObject())
            {
                o->setProperty ("hash", fileHash);
                // Everyone keeps shared audio in Audio/, whatever it's called on this machine.
                o->setProperty ("file", "Audio/" + file.getFileName());
            }
    }
    store.saveHashCache();

    const auto text = JSON::toString (project);
    bytes.reset();
    bytes.append (text.toRawUTF8(), text.getNumBytesAsUTF8());
    hash = sha256OfData (bytes);

    if (! missing.isEmpty())
        return Result::fail ("These audio files are missing, so the project can't be shared: "
                             + missing.joinIntoString (", "));
    return Result::ok();
}

//==============================================================================
Result createCloudProject (Client& client, LocalStore& store, const String& name)
{
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("name", name);
    ApiError err;
    const auto resp = client.call ("POST", "/projects", var (body.get()), err);
    if (err.status != 0)
        return Result::fail (err.message);
    store.link (resp["id"].toString(), resp["name"].toString());
    return Result::ok();
}

Result createCloudSong (Client& client, LocalStore& store, const String& name, const String& projectId)
{
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("name", name);
    body->setProperty ("project", projectId);
    ApiError err;
    const auto resp = client.call ("POST", "/v2/songs", var (body.get()), err);
    if (err.status != 0)
        return Result::fail (err.message);
    store.link (resp["id"].toString(), resp["name"].toString());
    return Result::ok();
}

PushOutcome push (Client& client, LocalStore& store, Snapshot& snap, const String& parent,
                  const String& message, const Progress& progress, const StringArray& keepBlobs)
{
    PushOutcome out;
    const auto projectId = store.getProjectId();
    if (projectId.isEmpty())
    {
        out.result = Result::fail ("This project isn't shared yet (Cloud > Share This Project).");
        return out;
    }

    if (auto r = snap.finish (store, [&] (const String& what, double p) { return report (progress, what, 0.3 * p); });
        r.failed())
    {
        out.result = r;
        return out;
    }
    store.putObject (snap.bytes);

    var before;
    if (auto rev = store.getRevision (parent))
        before = store.readSnapshot (rev->snapshot);

    std::vector<Client::UploadItem> items;
    StringArray blobs;
    items.push_back ({ snap.hash, File(), snap.bytes });
    blobs.add (snap.hash);
    for (auto& a : snap.audio)
    {
        const auto h = a.clip["hash"].toString();
        if (h.isNotEmpty() && ! blobs.contains (h))
        {
            blobs.add (h);
            items.push_back ({ h, a.file, {} });
        }
    }

    int uploaded = 0;
    auto r = client.upload (projectId, items, [&] (double p)
    {
        return report (progress, p < 0.25 ? "Compressing audio" : "Uploading", 0.3 + 0.65 * p);
    }, uploaded);
    if (r.failed())
    {
        out.result = r;
        return out;
    }

    report (progress, "Saving the revision...", 0.95);
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("parent", parent.isNotEmpty() ? var (parent) : var());
    body->setProperty ("snapshot", snap.hash);
    Array<var> blobList;
    for (auto& h : blobs)
        blobList.add (h);
    for (auto& h : keepBlobs)
        if (! blobs.contains (h))
            blobList.add (h), blobs.add (h);
    body->setProperty ("blobs", blobList);
    body->setProperty ("message", message);
    body->setProperty ("summary", summariseChanges (before, snap.project));
    body->setProperty ("appVersion", "Lathe " + String (JUCE_APPLICATION_VERSION_STRING));

    ApiError err;
    const auto resp = client.call ("POST", "/projects/" + projectId + "/revisions", var (body.get()), err);
    if (err.status == 409)
    {
        out.conflict = true;
        out.theirHead = err.body["head"].toString();
        out.result = Result::fail (err.message);
        return out;
    }
    if (err.status != 0)
    {
        out.result = Result::fail (err.message);
        return out;
    }

    out.revision = Revision::fromVar (resp);
    if (out.revision.id.isEmpty())
    {
        out.result = Result::fail ("The server didn't confirm the revision. Nothing was lost: push again.");
        return out;
    }
    out.revision.blobs = blobs;
    store.putRevision (out.revision);
    store.setBase (out.revision.id);
    report (progress, "Pushed", 1.0);
    return out;
}

Result mergeTracks (const var& base, const var& local, const std::set<int>& trackIds, var& merged)
{
    // A deep copy of the base, so the newest revision's content goes up exactly as it is.
    merged = JSON::parse (JSON::toString (base));
    auto* into = merged["tracks"].getArray();
    auto* from = local["tracks"].getArray();
    if (into == nullptr || from == nullptr)
        return Result::fail ("The project has no tracks.");
    for (const int id : trackIds)
    {
        int at = -1;
        for (int i = 0; i < into->size(); ++i)
            if ((int) (*into)[i]["id"] == id)
                at = i;
        var mine;
        for (auto& t : *from)
            if ((int) t["id"] == id)
                mine = t;
        if (at < 0)
            return Result::fail ("The owner has removed this track from the song, so there's nothing to push it into. "
                                 "Ask them to share another track.");
        if (! mine.isObject())
            return Result::fail ("\"" + (*into)[at]["name"].toString() + "\" isn't in your copy anymore. Open the newest "
                                 "revision (Cloud > History) to get it back, then push.");
        // The same object as in `local`, so hashing its audio fills in the merged project too.
        into->set (at, mine);
    }
    return Result::ok();
}

PushOutcome pushTracks (Client& client, LocalStore& store, Snapshot& local, const std::set<int>& trackIds,
                        const String& message, const Progress& progress)
{
    PushOutcome out;
    // The newest revision: yours go on top of it, whoever made it.
    auto pulled = pull (client, store, [&] (const String& what, double p) { return report (progress, what, 0.2 * p); });
    if (pulled.result.failed() || pulled.head.isEmpty())
    {
        out.result = pulled.result.failed() ? pulled.result
                                            : Result::fail ("The song has nothing in the cloud yet. Ask the owner to push it first.");
        return out;
    }
    const auto head = store.getRevision (pulled.head);
    if (! head || ! store.hasObject (head->snapshot))
    {
        out.result = Result::fail ("Couldn't get the newest revision. Try again.");
        return out;
    }

    var merged;
    if (auto r = mergeTracks (store.readSnapshot (head->snapshot), local.project, trackIds, merged); r.failed())
    {
        out.result = r;
        return out;
    }
    // Only your tracks' audio goes up; the rest is what the newest revision already has.
    local.audio.erase (std::remove_if (local.audio.begin(), local.audio.end(),
                                       [&] (const Snapshot::Audio& a) { return trackIds.count (a.trackId) == 0; }),
                       local.audio.end());
    local.project = merged;
    StringArray keep;
    for (auto& b : head->blobs)
        if (b != head->snapshot)
            keep.add (b);
    return push (client, store, local, head->id, message,
                 [&] (const String& what, double p) { return report (progress, what, 0.2 + 0.8 * p); }, keep);
}

PullOutcome pull (Client& client, LocalStore& store, const Progress& progress)
{
    PullOutcome out;
    const auto projectId = store.getProjectId();
    if (projectId.isEmpty())
    {
        out.result = Result::fail ("This project isn't shared.");
        return out;
    }

    report (progress, "Checking for new revisions...", 0.0);
    ApiError err;
    const auto meta = client.call ("GET", "/projects/" + projectId, {}, err);
    if (err.status != 0)
    {
        out.result = Result::fail (err.message);
        return out;
    }
    out.head = meta["head"].isString() ? meta["head"].toString() : String();
    if (out.head.isEmpty())
        return out;

    // Walk back through history until we reach revisions we already have. The first page
    // also says which revisions the cloud still keeps (it keeps the newest few, and pinned ones).
    String before;
    std::map<String, bool> inCloud;   // id -> pinned
    bool listIsComplete = false;
    for (int page = 0; page < 20; ++page)
    {
        const auto list = client.call ("GET", "/projects/" + projectId + "/revisions?limit=50"
                                                  + (before.isNotEmpty() ? "&before=" + before : String()), {}, err);
        if (err.status != 0)
        {
            out.result = Result::fail (err.message);
            return out;
        }
        const auto* revs = list["revisions"].getArray();
        if (revs == nullptr || revs->isEmpty())
            break;
        if (page == 0)
        {
            for (auto& v : *revs)
                inCloud[v["id"].toString()] = (bool) v["pinned"];
            listIsComplete = revs->size() < 50;
        }

        bool reachedKnown = false;
        for (auto& v : *revs)
        {
            const auto id = v["id"].toString();
            if (store.hasRevision (id))
            {
                reachedKnown = true;
                break;
            }
            const auto full = client.call ("GET", "/projects/" + projectId + "/revisions/" + id, {}, err);
            if (err.status != 0)
            {
                out.result = Result::fail (err.message);
                return out;
            }
            out.newRevisions.push_back (Revision::fromVar (full));
        }
        if (reachedKnown || revs->size() < 50)
            break;
        before = revs->getLast()["id"].toString();
    }

    // Their project snapshots (small), so every revision can be compared and opened.
    std::vector<std::pair<String, File>> snapshots;
    for (auto& r : out.newRevisions)
        if (! store.hasObject (r.snapshot))
            snapshots.push_back ({ r.snapshot, store.objectFile (r.snapshot) });
    report (progress, "Downloading revisions...", 0.1);
    if (auto r = client.download (projectId, snapshots, [&] (double p) { return report (progress, "Downloading revisions...", 0.1 + 0.1 * p); });
        r.failed())
    {
        out.result = r;
        return out;
    }
    for (auto& r : out.newRevisions)
        store.putRevision (r);

    // Mark what the cloud still keeps (and which are pinned).
    if (listIsComplete)
        for (auto r : store.getRevisions())
        {
            if (r.local)
                continue;
            const auto it = inCloud.find (r.id);
            const bool kept = it != inCloud.end(), pinned = kept && it->second;
            if (kept != r.inCloud || pinned != r.pinned)
            {
                r.inCloud = kept;
                r.pinned = pinned;
                store.putRevision (r);
            }
        }

    // And the newest revision's audio, so it opens straight away (even offline later).
    if (auto head = store.getRevision (out.head))
    {
        var unused;
        auto r = prepareRevision (&client, store, *head, unused, [&] (const String& what, double p)
        {
            return report (progress, what, 0.2 + 0.8 * p);
        });
        if (r.failed())
            out.result = r;
    }
    report (progress, "Up to date", 1.0);
    return out;
}

Result prepareRevision (Client* client, LocalStore& store, const Revision& rev, var& projectOut, const Progress& progress)
{
    if (! store.hasObject (rev.snapshot))
    {
        if (client == nullptr)
            return Result::fail ("This revision hasn't been downloaded. Sign in and pull first.");
        if (auto r = client->download (store.getProjectId(), { { rev.snapshot, store.objectFile (rev.snapshot) } }, {}); r.failed())
            return r;
    }

    auto project = store.readSnapshot (rev.snapshot);
    if (! project.isObject())
        return Result::fail ("Revision " + rev.label() + " is damaged.");

    const auto dir = store.getProjectDir();
    const auto audioDir = dir.getChildFile ("Audio");
    std::map<String, File> planned;
    std::vector<std::pair<String, File>> downloads;

    forEachAudioClip (project, [&] (const var& clip)
    {
        const auto hash = clip["hash"].toString();
        auto* o = clip.getDynamicObject();
        if (hash.isEmpty() || o == nullptr)
            return;

        const auto path = clip["file"].toString();
        const auto named = File::isAbsolutePath (path) ? File (path) : dir.getChildFile (path);
        File chosen;
        if (auto it = planned.find (hash); it != planned.end())
            chosen = it->second;
        else if (named.existsAsFile() && store.hashOf (named) == hash)
            chosen = named;
        else if (auto known = store.findLocalFile (hash); known.existsAsFile())
            chosen = known;
        else
        {
            // Download it into Audio/, next to a different file of the same name if need be.
            auto dest = audioDir.getChildFile (fileNameOf (path));
            if (dest.exists() || std::any_of (planned.begin(), planned.end(), [&] (auto& p) { return p.second == dest; }))
                dest = audioDir.getChildFile (dest.getFileNameWithoutExtension() + " " + hash.substring (0, 8) + dest.getFileExtension());
            chosen = dest;
            downloads.push_back ({ hash, dest });
        }
        planned[hash] = chosen;
        o->setProperty ("file", chosen.isAChildOf (dir) ? chosen.getRelativePathFrom (dir).replaceCharacter ('\\', '/')
                                                        : chosen.getFullPathName());
    });

    if (! downloads.empty())
    {
        if (client == nullptr)
            return Result::fail (String ((int) downloads.size()) + " audio file(s) for " + rev.label()
                                 + " haven't been downloaded. Sign in and pull first.");
        auto r = client->download (store.getProjectId(), downloads, [&] (double p)
        {
            return report (progress, "Downloading " + String ((int) downloads.size()) + " audio file(s)...", p);
        });
        if (r.failed())
            return r;
        for (auto& [hash, f] : downloads)
            store.rememberHash (f, hash);
    }
    store.saveHashCache();

    // Read back as a plain project (the extra fields are harmless to Lathe).
    projectOut = project;
    return Result::ok();
}

Result setPinned (Client& client, LocalStore& store, const String& revisionId, bool pinned)
{
    DynamicObject::Ptr body = new DynamicObject();
    body->setProperty ("pinned", pinned);
    ApiError err;
    client.call ("PATCH", "/v2/songs/" + URL::addEscapeChars (store.getProjectId(), true) + "/revisions/"
                              + URL::addEscapeChars (revisionId, true), var (body.get()), err);
    if (err.status != 0)
        return Result::fail (err.status == 404 && err.message == "Not found." ? String ("Keeping revisions needs the updated Lathe Cloud server.")
                                                                               : err.message);
    if (auto r = store.getRevision (revisionId))
    {
        auto copy = *r;
        copy.pinned = pinned;
        store.putRevision (copy);
    }
    return Result::ok();
}

Revision keepWorkingCopy (LocalStore& store, Snapshot& snap, const String& author, const String& message)
{
    if (snap.hash.isEmpty())
        snap.finish (store);

    for (auto& r : store.getRevisions())
        if (r.snapshot == snap.hash)
            return r;

    store.putObject (snap.bytes);
    Revision r;
    r.id = "local-" + String (Time::currentTimeMillis());
    r.local = true;
    r.parent = store.getBase();
    r.author = author;
    r.message = message;
    var before;
    if (auto base = store.getRevision (r.parent))
        before = store.readSnapshot (base->snapshot);
    r.summary = summariseChanges (before, snap.project);
    r.snapshot = snap.hash;
    r.blobs.add (snap.hash);
    for (auto& a : snap.audio)
        r.blobs.addIfNotAlreadyThere (a.clip["hash"].toString());
    r.createdAt = Time::currentTimeMillis();
    store.putRevision (r);
    return r;
}

//==============================================================================
SetAsideOutcome setAsideAndPull (Client& client, LocalStore& store, Snapshot& yours, const String& author, const Progress& progress)
{
    SetAsideOutcome out;
    auto pulled = pull (client, store, [&] (const String& what, double p) { return report (progress, what, 0.75 * p); });
    if (pulled.result.failed())
    {
        out.result = pulled.result;
        return out;
    }
    out.head = store.getRevision (pulled.head);
    if (! out.head || ! store.hasObject (out.head->snapshot))
    {
        out.result = Result::fail ("Couldn't get the newest revision. Try again.");
        return out;
    }
    if (out.head->id == store.getBase())
    {
        out.upToDate = true;
        return out;
    }
    if (auto r = yours.finish (store, [&] (const String& what, double p) { return report (progress, what, 0.75 + 0.25 * p); });
        r.failed())
    {
        out.result = r;
        return out;
    }
    // Only something you changed is set aside (an exact copy of a revision has nothing to put back).
    if (const auto kept = keepWorkingCopy (store, yours, author, "Your changes, set aside to get " + out.head->label()); kept.local)
        store.setSetAside (kept.id);
    report (progress, "Your changes are set aside", 1.0);
    return out;
}

Result putBack (LocalStore& store, Snapshot& current, MergeResult& out, const Progress& progress)
{
    const auto aside = store.getRevision (store.getSetAside());
    if (! aside || ! store.hasObject (aside->snapshot))
        return Result::fail ("There are no changes set aside.");
    if (auto r = current.finish (store, progress); r.failed())
        return r;

    // Your changes are what the set-aside copy changed since the revision it started from.
    var base;
    if (auto from = store.getRevision (aside->parent); from && store.hasObject (from->snapshot))
        base = store.readSnapshot (from->snapshot);
    out = mergeProjects (base, store.readSnapshot (aside->snapshot), current.project);
    return Result::ok();
}

Revision saveCombined (LocalStore& store, const var& project, const Revision& parent, const String& author)
{
    const auto text = JSON::toString (project);
    MemoryBlock bytes (text.toRawUTF8(), text.getNumBytesAsUTF8());
    Revision r;
    r.id = "local-" + String (Time::currentTimeMillis());
    r.local = true;
    r.parent = parent.id;
    r.author = author;
    r.message = "Your changes put back on top of " + parent.label();
    r.summary = summariseChanges (store.readSnapshot (parent.snapshot), project);
    r.snapshot = store.putObject (bytes);
    r.blobs.add (r.snapshot);
    forEachAudioClip (project, [&r] (const var& clip)
    {
        if (const auto h = clip["hash"].toString(); h.isNotEmpty())
            r.blobs.addIfNotAlreadyThere (h);
    });
    r.createdAt = Time::currentTimeMillis();
    store.putRevision (r);
    return r;
}
} // namespace lathe::cloud
