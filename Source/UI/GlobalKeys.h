#pragma once

#include "../Common.h"

namespace lathe::globalkeys
{
//==============================================================================
// Space starts and stops playback wherever the keyboard is: the arrangement, the mixer,
// a focused button, a plug-in's window (Lathe's devices and other makers' plug-ins alike).
//
// On the Mac (an app-wide key monitor) and on Windows (a hook on the app's message loop)
// Lathe sees the key before any window or plug-in view does. Elsewhere, Lathe's windows
// pass it on (MainComponent and plug-in windows call handleSpace()).
//
// Space is left alone when someone is typing: a text field in Lathe or in a plug-in.
// Also with a modifier held, and while a dialog is open. Holding space down doesn't
// start and stop over and over.
//==============================================================================

/** Sets what space does (start/stop) and hooks the keyboard where the platform allows.
    Call once, from the message thread. */
void install (std::function<void()> toggle);

/** Removes the hook and forgets the action. */
void uninstall();

/** For windows that see a space press themselves: runs the action if it's time to.
    True if it did (the key's used). */
bool handleSpace();

/** Lathe's own part of the decision: false while a dialog is open or a text field has focus. */
bool spaceIsForPlayback();

//==============================================================================
// The file shortcuts (Cmd/Ctrl+S, Shift+S, O, N, Shift+E) from Lathe's other windows. A plug-in's
// window keeps the keys it's given, so with EZdrummer or an amp sim in front, Cmd+S used to go
// to the plug-in and nothing saved. The same hook as Space (Mac, Windows) hands these to Lathe
// first, when the key is pressed in one of Lathe's own windows (not the Save panel or another
// app's); plug-in windows pass them on elsewhere. Everything else (Cmd+Z, Cmd+C...) stays with
// the plug-in, which may have its own undo and copy.
//==============================================================================

/** What to do with a shortcut from one of Lathe's windows: true if it was used. nullptr removes it. */
void installCommands (std::function<bool (const KeyPress&)> handler);

/** Runs the handler for a key press with Cmd/Ctrl held, unless a dialog is open. True if it was used. */
bool handleCommand (const KeyPress&);
} // namespace lathe::globalkeys
