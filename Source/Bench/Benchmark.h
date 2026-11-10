#pragma once
#include "../Common.h"

namespace lathe::bench
{
/** The multi-core benchmark (docs/multicore.md): runs the engine on made-up sessions full of
    CPU-heavy stand-in plug-ins, with no audio device, and times every block.

        Lathe --benchmark [out.csv]          (also: LatheTests --bench [out.csv] [1,full])

    `modes`: "1" = one core (the old single-threaded engine), "full" = every core Lathe would use.
    Prints a table to `out`, writes the CSV, returns 0. Takes a couple of minutes. */
int run (const File& csv, const StringArray& modes, std::ostream& out);

/** For the engine's own checks: renders four sessions (every routing path, a band, chained
    tracks, synths) at several buffer sizes and writes each output, sample for sample, to
    <folder>/<session>-<buffer>.f32. Two engines are bit-identical if the files are. */
int dump (const File& folder, int threads, std::ostream& out);

/** Renders `seeds` random sessions on one thread and on `threads` threads (sharing out every
    block) and compares them sample for sample. Returns how many differ (0 = all identical). */
int checkRandom (int seeds, int threads, std::ostream& out);
}
