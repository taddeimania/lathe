// Types.h — the delay's catalogue of delay types. Each type has its own name,
// category, description and search words, its own range for TIME, its own
// meaning for FILTER and GRIT (and sometimes SPEED and DEPTH), and its own
// meaning for the three CHARACTER knobs and the CHARACTER choice. The panel,
// the type browser and the engine all read this one table. No JUCE.
#pragma once

#include <string>
#include <vector>

namespace lathe::dly
{
enum TypeIndex
{
    TDigital, TDual, TPattern,          // CLEAN
    TTape, TBucket, TLofi,              // VINTAGE
    TTrem, TFilter, TSwell,             // MOVING
    TReverse, TIce, TCloud,             // STRANGE
    NumTypes
};

// Where a type starts when picked.
struct Starting
{
    float timeMs = 400.0f, repeats = 0.4f, filter = 0.0f, grit = 0.0f;
    float speed = 0.3f, depth = 0.1f;
    float a = 0.0f, b = 0.0f, c = 0.0f;
    int mode = 0;
};

struct TypeInfo
{
    const char* name;
    const char* category;
    const char* blurb;      // one sentence for the panel and the browser
    const char* keywords;   // extra words the search matches
    float minMs, maxMs;     // TIME is held inside this range
    Starting start;
    const char* charLabel[3];               // empty: knob unused for this type
    const char* modeLabel;                  // empty: no CHARACTER choice
    std::vector<const char*> modeNames;
    // Knobs that mean something else for this type (empty: unchanged).
    const char* filterCaption = "";
    const char* gritCaption = "";
    const char* speedCaption = "";
    const char* depthCaption = "";
    const char* charTips[3] = { "", "", "" };   // tooltips for the CHARACTER knobs
    const char* modeTip = "";
    bool bipolar[3] = { false, false, false };  // CHARACTER knobs with a centre detent
};

const std::vector<TypeInfo>& types();
const TypeInfo& typeInfo (int index);
const std::vector<const char*>& categories();   // in display order
inline constexpr int kMaxModes = 16;

// TIME held inside the type's range.
float timeFor (int type, int mode, float ms);

// Text for a CHARACTER knob value (0..1) as the given type reads it.
std::string charText (int type, int slot, float value);

// Shared mappings.
struct Ratio { const char* name; float value; };
const std::vector<Ratio>& dualRatios();      // TIME 2 as a multiple of TIME
const std::vector<Ratio>& lfoRatios();       // trem and filter LFO cycles per echo
int   ratioIndex (float knob, int count);    // a 0..1 knob snapped to a list
int   iceSemis (int mode);                   // the Ice interval in semitones

// One tap of a rhythm pattern: when (as a fraction of TIME), how loud, where.
struct Tap { float at, level, pan; };
struct Pattern { const char* name; std::vector<Tap> taps; };
const std::vector<Pattern>& patterns();

// Searching: a score above zero means the type matches every word of the query.
int matchScore (const std::string& query, const std::string& name, const std::string& category,
                const std::string& blurb, const std::string& keywords);
} // namespace lathe::dly
