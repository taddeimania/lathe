#pragma once

#include "Widgets.h"

namespace lathe
{
/** Audio/MIDI device, recording offset, plug-in folders, startup behaviour. */
void showSettingsDialog (AppContext&);

/** Mix and/or stems to WAV. With track ids, it opens with just those tracks ticked. */
void showExportDialog (AppContext&, const std::set<int>& onlyTracks = {});
} // namespace lathe
