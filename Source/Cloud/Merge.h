#pragma once

#include "../Common.h"

namespace lathe::cloud
{
/** Something you and a collaborator both changed, differently. The merged project holds
    their side; takeYours() puts yours in instead. */
struct MergeConflict
{
    String what;            // "\"Guitar\": fader", "Tempo", "\"Bass\" (track)"
    String yours, theirs;   // what each side did, in words
    std::function<void (var& project)> takeYours;
};

struct MergeResult
{
    var project;                           // theirs, with your changes on top
    std::vector<MergeConflict> conflicts;  // their side is in `project` until you choose yours
    bool yoursChangedAnything = false;     // false: your copy had nothing theirs didn't already have
};

/** Three-way merge, like git: `base` is the revision your copy started from, `theirs` the newest
    revision. Everything you changed since `base` is put on top of `theirs`. Changes are compared
    per song setting, per channel setting (fader, pan, plug-ins, EQ, sends...), and per clip, so
    you and a collaborator can work on different tracks, or different things on one track, and
    both changes stay. Where you both changed the same thing differently it's a conflict.
    New tracks, clips, groups and monitor mixes you made get new ids if theirs used the same ones.
    All three are project snapshots (Lathe's JSON with clip hashes). */
MergeResult mergeProjects (const var& base, const var& yours, const var& theirs);
} // namespace lathe::cloud
