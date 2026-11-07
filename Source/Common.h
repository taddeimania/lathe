#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace lathe
{
using namespace juce;

/** Faders and sends at or below this level are treated as fully off. */
constexpr float kSilenceDb = -70.0f;

inline float dbToGain (float db) noexcept   { return db <= kSilenceDb ? 0.0f : Decibels::decibelsToGain (db); }

/** Where Lathe keeps its own files: templates, unsaved sessions, plug-in list. */
inline File getUserDir()
{
    return File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Lathe");
}
inline File getTemplatesDir()   { return getUserDir().getChildFile ("Templates"); }
inline File getProjectsDir()    { return getUserDir().getChildFile ("Projects"); }
inline File getUnsavedDir()     { return getUserDir().getChildFile ("Unsaved"); }

inline const char* kProjectExtension = ".lathe";
} // namespace lathe
