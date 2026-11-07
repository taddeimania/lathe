#pragma once

#include "Widgets.h"

namespace lathe
{
/** Audio/MIDI device, recording offset, plug-in folders, startup behaviour. */
void showSettingsDialog (AppContext&);

/** Mix and/or stems to WAV. */
void showExportDialog (AppContext&);
} // namespace lathe
