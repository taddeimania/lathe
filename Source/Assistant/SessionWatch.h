#pragma once

#include "../Common.h"

namespace lathe::assistant
{
/** What changed in the session since the assistant last looked, made by someone else (the person
    at Lathe, usually). "Structural" is anything but live mixing: tracks, plug-ins and their
    settings, clips and notes, routing, tempo. Faders, pans, mutes, solos, sends, record arm,
    monitoring and the loop are live: people move them while listening. */
struct SessionChange
{
    bool any = false;
    bool structural = false;
    String otherProject;   // set when a different project is open now
    StringArray words;     // as in Lathe's History: "\"Bass\": fader -8.0 dB -> -6.0 dB"

    /** The changes as a short list for the assistant (at most `maxLines`). */
    String list (int maxLines = 12) const;
};

/** Remembers the session as the assistant last saw it (after each of its tool calls), so the next
    call can tell whether the person changed things in between. */
class SessionWatch
{
public:
    /** The assistant has now seen `project` (Snapshot::capture's project), open from `file`. */
    void seen (const var& project, const File& file);
    void forget()   { has = false; last = var(); lastFile = File(); }
    bool hasBaseline() const   { return has; }

    SessionChange compare (const var& project, const File& file) const;

    /** Tools that change the song, so must not run on an out-of-date picture of it. */
    static bool changesSong (const String& tool);

private:
    var last;
    File lastFile;
    bool has = false;
};
} // namespace lathe::assistant
