#pragma once

#include "Widgets.h"

namespace lathe
{
namespace assistant { class Service; }

/** Audio/MIDI device, recording offset, plug-in folders, startup behaviour. */
void showSettingsDialog (AppContext&);

/** Asks, once, on a new install, whether Lathe may send anonymous one-time milestones (Cloud/Milestones.h). */
void showMilestonesDialog (PropertiesFile&);
/** That window, drawn at 2x, for Lathe --screenshots (the privacy page shows it). */
Image milestonesDialogImage();

/** Mix and/or stems to WAV. With track ids, it opens with just those tracks ticked. */
void showExportDialog (AppContext&, const std::set<int>& onlyTracks = {});

/** Turn the AI assistant (MCP) connection on or off, and how to set up AI apps. */
void showAssistantDialog (AppContext&, std::weak_ptr<assistant::Service>);
} // namespace lathe
