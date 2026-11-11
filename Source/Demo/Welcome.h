#pragma once

#include "../Common.h"

namespace lathe::welcome
{
//==============================================================================
// "Welcome to Lathe", the song a new install opens with: four MIDI tracks played by
// Lathe's own instruments (Drums, Bass Synth, Poly Synth) through its Reverb and Delay,
// so the first thing anyone hears is a finished-sounding song, not an empty session.
// It's Resources/Welcome.lathe, built into the app.
//==============================================================================

/** "Welcome to Lathe". */
String songName();

/** Writes the song into projectsDir/Welcome to Lathe/ (or a new "Welcome to Lathe 2"
    beside it if that folder holds something else) and returns the project file. If the
    song is there from before, that copy is the one returned: it's theirs now. */
Result write (const File& projectsDir, File& projectFile);
} // namespace lathe::welcome
