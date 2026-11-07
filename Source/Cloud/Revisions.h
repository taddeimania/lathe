#pragma once

#include "CloudClient.h"
#include "../Model/Edit.h"

namespace lathe::cloud
{
//==============================================================================
// A shared project's history, kept in a hidden ".lathe" folder next to the
// project file:
//
//   Song/Song.lathe            the working copy (what Lathe opens and saves)
//   Song/Audio/                audio, as always
//   Song/.lathe/cloud.json     which cloud project this is, and which revision
//                              the working copy started from ("base")
//   Song/.lathe/revisions/     one JSON file per revision (pushed, pulled or local)
//   Song/.lathe/objects/       project snapshots, named by SHA-256
//   Song/.lathe/hashes.json    cache of audio files' SHA-256 (by path, size, date)
//
// Audio isn't copied into .lathe: a revision's snapshot names each clip's file and
// its hash, and opening a revision finds (or downloads) a file with that hash.
//==============================================================================

struct Revision
{
    String id;          // server id, or "local-<time>" for one that was never pushed
    int seq = 0;        // r1, r2, ... (0 for local ones)
    String parent;
    String author, message, summary;
    String snapshot;    // SHA-256 of the project snapshot
    StringArray blobs;  // every file the revision needs (snapshot + audio)
    int64 createdAt = 0;
    bool local = false;

    var toVar() const;
    static Revision fromVar (const var&);
    String label() const   { return local ? String ("Local") : "r" + String (seq); }
};

class LocalStore
{
public:
    explicit LocalStore (const File& projectFile);

    File getProjectFile() const   { return projectFile; }
    File getProjectDir() const    { return projectFile.getParentDirectory(); }
    File getDir() const           { return getProjectDir().getChildFile (".lathe"); }

    // --- the cloud link
    bool isLinked() const;
    String getProjectId() const;
    String getCloudName() const;
    String getBase() const;         // revision the working copy started from ("" = none)
    void link (const String& projectId, const String& name);
    void setBase (const String& revisionId);
    void unlink();

    // --- snapshots (by hash)
    File objectFile (const String& hash) const;
    bool hasObject (const String& hash) const;
    String putObject (const MemoryBlock&);     // returns its hash
    var readSnapshot (const String& hash) const;

    // --- revisions
    void putRevision (const Revision&);
    std::vector<Revision> getRevisions() const;   // newest first
    std::optional<Revision> getRevision (const String& id) const;
    bool hasRevision (const String& id) const;

    // --- audio hashes
    String hashOf (const File&);                  // cached by path, size and date
    void rememberHash (const File&, const String& hash);
    File findLocalFile (const String& hash);      // a file we know has that hash
    void saveHashCache();

private:
    var readLink() const;
    void writeLink (const var&);
    void loadHashCache();

    File projectFile;
    CriticalSection lock;
    var hashCache;           // {path: {size, mtime, hash}}
    bool hashCacheLoaded = false, hashCacheDirty = false;
};

//==============================================================================
/** The project as it'll be stored: Lathe's JSON plus each plug-in's readable
    parameter values and each audio clip's hash. Capture on the message thread,
    then finish (hashing, which may be slow the first time) on a background one. */
struct Snapshot
{
    var project;
    struct Audio { var clip; File file; };
    std::vector<Audio> audio;
    String hash;            // set by finish()
    MemoryBlock bytes;      // the JSON that hash is of

    static Snapshot capture (const Edit&);
    Result finish (LocalStore&);
};

/** Progress callback: what's happening, 0..1. Return false to cancel. */
using Progress = std::function<bool (const String&, double)>;

struct PushOutcome
{
    Result result = Result::ok();
    Revision revision;
    bool conflict = false;      // someone pushed first
    String theirHead;           // the revision that's newest on the server now
};

/** Creates the cloud project and links this folder to it (no revision yet). */
Result createCloudProject (Client&, LocalStore&, const String& name);

/** Pushes the snapshot as a new revision on top of parent ("" for the first). */
PushOutcome push (Client&, LocalStore&, Snapshot&, const String& parent, const String& message, const Progress&);

struct PullOutcome
{
    Result result = Result::ok();
    std::vector<Revision> newRevisions;   // newest first
    String head;
};

/** Fetches revisions the folder doesn't know yet, and the audio the newest one needs. */
PullOutcome pull (Client&, LocalStore&, const Progress&);

/** The revision's project, ready to save as the working copy: audio paths point at
    local files, downloading any that are missing (client may be null if offline). */
Result prepareRevision (Client*, LocalStore&, const Revision&, var& projectOut, const Progress&);

/** Records the working copy as a local revision, unless an existing revision already
    has exactly this content. Returns the revision it matches or made. */
Revision keepWorkingCopy (LocalStore&, Snapshot&, const String& author, const String& message);
} // namespace lathe::cloud
