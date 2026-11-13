#pragma once

#include "../Common.h"

namespace lathe::crashes
{
//==============================================================================
// Crash reports, sent only when the person says so.
//
// If Lathe crashes, it writes a short text report into its Crashes folder before it goes:
// Lathe's version, the system (macOS 14.7, Windows 11...), how long it had been open, and
// where in Lathe's code it stopped (each step as "Lathe + 0x1a2b3c", which can be matched to
// Lathe's source with the same release's files). No song, no audio, no file or track names, no
// account: paths are cut down to the file's own name, and the user's name is taken out.
//
// Next time Lathe opens it asks: "Lathe quit unexpectedly last time. Send the report?",
// with the report itself one click away. Settings > Privacy can make that "always send" or
// "never send". What's sent: POST <Lathe Cloud>/crash {"version", "system", "report"}.
// What people are told: getlathe.app/privacy.html#crashes; keep the two the same.
//==============================================================================

/** Call once, early in main: crashes from now on leave a report in `folder`. */
void install (const File& folder);

/** Where reports go: <app data>/Lathe/Crashes. */
File defaultFolder();

/** Reports waiting to be sent or dismissed, oldest first. */
Array<File> pending (const File& folder);

/** The text of a report as it would be sent (cleaned up again, and cut to 16 KB). */
String reportText (const File&);

/** Builds a report: header, then the steps. For the crash handler, and tests. */
String compose (const String& what, const StringArray& frames, double secondsOpen);

/** Takes the user's name and home folder out of `text`, and long paths down to file names. */
String scrub (const String& text);

enum class Choice { ask, always, never };
Choice choice (PropertiesFile&);
void setChoice (PropertiesFile&, Choice);

/** Sends one report (in the background; one try) and deletes it either way. */
void send (const File& report);
/** Deletes a report without sending it. */
void dismiss (const File& report);

/** Where people can read what a report holds. */
inline const char* explainUrl = "https://getlathe.app/privacy.html#crashes";

/** The report a crash right here would write (tests: the stack really is read). */
String captureForTests();

/** Tests: called with the body instead of the network. */
void setSenderForTests (std::function<void (const var&)>);
var payload (const String& report);
}
