#pragma once

#include "../Model/Edit.h"

namespace lathe::demo
{
//==============================================================================
// "Night Drive", a made-up song for screenshots (Lathe --screenshots). Everything
// is generated: the audio (simple synthesis), the tracks and returns, a note thread,
// and a shared copy with a few pushes in its history. Nothing real, nobody real.
//==============================================================================

struct Songs
{
    File plain;      // Night Drive/Night Drive.lathe: the song, with a note thread
    File shared;     // Night Drive (shared)/Night Drive.lathe: linked to the cloud, five revisions
    String noteId;   // the note to show open
    int noteTrackId = -1;
    double noteBeat = 0.0;
};

/** Writes both songs under folder (which should be empty). */
Result write (const File& folder, Songs& out);

/** "Guitar take": the part the person records in the AI assistant demo (Lathe --screenshots).
    Four bars of picked chords at bpm, C for two bars then G, made like the rest. */
Result writeGuitarTake (const File& wav, double bpm);

/** A pretend audio interface ("Audio Interface", 8 in, 8 out, 48 kHz) that never
    plays anything, so screenshots show inputs and a device without touching yours. */
std::unique_ptr<AudioIODeviceType> makeDeviceType();
} // namespace lathe::demo
