#pragma once

#include "../Common.h"

namespace lathe::milestones
{
//==============================================================================
// Anonymous one-time milestones, and only if the person said yes.
//
// The first time someone opens Lathe, it asks (MilestonesWindow). If they agree, each
// milestone below is sent once in the life of the install, the first time it happens:
//
//     POST <Lathe Cloud>/hit
//     {"kind": "app", "milestone": "recorded", "cohort": "new", "system": "macos", "version": "0.12.8"}
//
// That's the whole message. No ID, name, email, song or track, nothing that tells one
// computer from another. The server adds 1 to that day's count and keeps nothing else, so
// comparing the counts ("200 opened Lathe, 60 recorded") shows where new people get stuck.
// Offline: it isn't sent, and isn't tried again. Turning it off stops it at once.
// What's sent is described for people at getlathe.app/privacy#app; keep the two the same.
//==============================================================================

/** In the order a new person reaches them. */
const StringArray& names();

/** What each one means, for the window and Settings ("The first time you record something"). */
String describe (const String& name);

enum class Choice { notAsked, yes, no };
Choice choice (PropertiesFile&);

/** Main.cpp calls this when there was no settings file at all: Lathe has never run here.
    Only then does Lathe ask. An update (through Lathe Patcher or a new download over an old
    Lathe) keeps the settings file, so it's never asked after one. */
void markNeverRunBefore (PropertiesFile&);

/** Whether to show the question: a never-run-before install that hasn't answered yet (it
    asks again next time if Lathe quit before the window appeared). */
bool shouldAsk (PropertiesFile&);

/** Call once at start-up, before anything else here. Notes the first day this install ran,
    and whether it's new (nothing in the settings yet) or was here before milestones existed.
    Sends "came_back" if this is a later day, a week or more after the first. */
void startup (PropertiesFile&, bool newInstall);

/** The answer from the welcome window or Settings. Yes from the welcome window on a new
    install counts as "new"; any other yes as "existing" (its early milestones have passed
    unseen, so it would skew the new-install numbers). Yes sends "launched". */
void setChoice (PropertiesFile&, bool share, bool fromWelcome);

/** Sends `name` if the person said yes and it hasn't been sent from this install before.
    Safe to call as often as you like; it sends at most once. ("came_back" is startup()'s.) */
void reach (PropertiesFile&, const String& name);

/** Where people can read exactly what's sent. */
inline const char* explainUrl = "https://getlathe.app/privacy#app";

/** Sends `name` once if they said yes (no check on which one: reach() is for everyone else). */
void sendOnce (PropertiesFile&, const String& name);

/** The body reach() would send, for tests and for the explanation. */
var payload (PropertiesFile&, const String& name);

/** Tests: called instead of the network. */
void setSenderForTests (std::function<void (const var&)>);
} // namespace lathe::milestones
