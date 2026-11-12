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
} // namespace lathe::globalkeys
