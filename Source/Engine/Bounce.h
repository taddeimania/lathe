#pragma once

#include "Engine.h"

/** Bounce Group to Audio Track: what a track group plays, rendered once onto a new audio track.

    With a group bus, that's the bus after its plug-ins, EQ and fader. Without one, it's the
    group's tracks added up after their faders (the ones that play to the master). Sends to
    returns (a shared reverb) aren't in it. The render is the export's, so it lines up sample for
    sample with the song. */
namespace lathe::bounce
{
struct Plan
{
    int groupId = -1;
    String groupName;
    std::set<int> playing;      // the members (and the bus): what the render plays
    std::set<int> taps;         // the signals added up: the bus, or the members going to the master
    double startBeat = 0.0;     // the first member clip's start
    double endBeat = 0.0;       // the last one's end
    double tempo = 120.0;
    bool anySends = false;      // a member sends to a return (that's not in the bounce)
    String problem;             // why it can't be bounced (no clips, say); empty if it can
};

/** Message thread. */
Plan plan (const Edit&, int groupId);

struct Bounced
{
    File file;
    double startBeat = 0.0, lengthBeats = 0.0;
    String error;
    bool cancelled = false;
};

/** Renders `plan` into a new 32-bit float WAV in `folder` (the project's Audio folder). The file
    goes on to `tailSeconds` past the last clip; the length stops where the sound does. Any
    thread but the audio thread (it calls Engine::renderOffline). */
Bounced render (Engine&, const Plan&, const File& folder, double tailSeconds,
               const std::function<bool (double)>& progress = {});

/** Adds "<group> (bounced)" right under the group (not in it), in the group's colour, with the
    file as one clip, and mutes the group as part of the step (undo unmutes it). Doesn't call
    Edit::changed() or commit the step. Returns the new track. */
std::shared_ptr<Track> place (Edit&, const Plan&, const Bounced&);
} // namespace lathe::bounce
