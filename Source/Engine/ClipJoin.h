#pragma once

#include "AudioPool.h"
#include "../Model/Edit.h"

namespace lathe
{
/** Joins clips on one track into a single new clip that runs from the earliest start
    to the latest end. Gaps between them become silence (or empty MIDI).

    MIDI clips: the notes and controller data are moved into the new clip.
    Audio clips: what you hear is rendered into a new WAV in the project's Audio folder
    (clip gain and the engine's short edge fades included), so the new clip plays back
    exactly as the pieces did.

    The originals are removed. Call Edit::beginClipEdit() first so it can be undone.
    Returns the new clip's id in newClipId. Needs at least two clips of the same kind.

    It's planJoin + renderJoin + applyJoin, which the app runs apart so a long audio join
    can render off the message thread with a progress bar. */
Result joinClips (Edit&, AudioPool&, Track&, const std::vector<int>& clipIds, int& newClipId);

/** One track's join, copied out of the project so the audio can render on any thread. */
struct JoinPlan
{
    int trackId = -1;
    std::vector<int> clipIds;
    std::vector<Clip> parts;   // timeline order, then track order (later ones sit on top)
    ClipKind kind = ClipKind::audio;
    double start = 0.0, end = 0.0;   // beats
    double sampleRate = 48000.0, samplesPerBeat = 24000.0;
    File audioDir;
    String problem;   // set when they can't be joined

    bool isAudio() const noexcept   { return kind == ClipKind::audio; }
    double seconds() const noexcept { return (end - start) * samplesPerBeat / sampleRate; }
};

/** Message thread. Makes the project's Audio folder for an audio join. */
JoinPlan planJoin (Edit&, const Track&, const std::vector<int>& clipIds);

/** Renders an audio join's WAV into the plan's Audio folder (MIDI joins need nothing). Any thread.
    `progress` gets 0..1 and returns false to stop: then nothing is left behind and it fails
    with "Cancelled.". */
Result renderJoin (const JoinPlan&, AudioFormatManager&, File& wav, const std::function<bool (double)>& progress = {});

/** Message thread. Puts the joined clip on the track in place of the parts. Fails (and deletes
    the WAV) if the parts aren't on the track any more. */
Result applyJoin (Edit&, Track&, const JoinPlan&, const File& wav, int& newClipId);
} // namespace lathe
