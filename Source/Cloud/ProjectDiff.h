#pragma once

#include "../Common.h"

namespace lathe::cloud
{
/** What changed between two project snapshots, in plain words, one line per
    track (or song setting). Empty when nothing a person would notice changed.
    Snapshots are Lathe project JSON; plug-ins may carry a "params" object of
    readable values ({"Decay": "2.1 s"}) so knob changes can be named. */
StringArray describeChanges (const var& before, const var& after);

/** describeChanges joined into one block of text ("No changes" when empty). */
String summariseChanges (const var& before, const var& after);
} // namespace lathe::cloud
