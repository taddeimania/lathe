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
    Returns the new clip's id in newClipId. Needs at least two clips of the same kind. */
Result joinClips (Edit&, AudioPool&, Track&, const std::vector<int>& clipIds, int& newClipId);
} // namespace lathe
