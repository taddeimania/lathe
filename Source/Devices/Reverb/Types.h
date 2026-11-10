// Types.h — the reverb's catalogue of reverb types. Each type is a voicing of one of
// six engines (machines), with its own name, category, description and
// search words, and its own meaning for the three CHARACTER knobs and the
// CHARACTER choice. The panel, the type browser and the engine all read this
// one table. No JUCE.
#pragma once

#include <string>
#include <vector>

namespace lathe::rvb
{
enum class Machine
{
    Space,         // feedback delay network: rooms, halls, chambers and the ambient types
    Plate,         // a figure-eight plate tank
    Spring,        // dispersive springs
    Reflections,   // a calculated small room with a movable source
    Nonlinear,     // shaped multi-tap envelopes with a late tank
    Tape           // multi-head tape echo smeared into a room
};

// Extras the Space machine adds for some types.
enum Feature : unsigned
{
    FeatCloud    = 1u << 0,   // long modulated diffusers in front of the tank
    FeatBloom    = 1u << 1,   // a feedback bloom stage in front of the tank
    FeatShimmer  = 1u << 2,   // pitch-shifted voices fed into and/or out of the tank
    FeatChoir    = 1u << 3,   // vowel formant filters on the tail
    FeatSwell    = 1u << 4,   // onset-triggered fade-in of wet or dry
    FeatLofi     = 1u << 5,   // crush, wobble and grit
    FeatResonant = 1u << 6,   // tuned comb bank
    FeatSmash    = 1u << 7,   // a hard compressor on the tail (char C)
    FeatEcho     = 1u << 8    // a discrete echo off the far wall (char C)
};

// How a Space type sounds: line lengths, early reflections and diffusion.
struct Voicing
{
    Machine machine = Machine::Space;
    float lineMinMs = 20.0f, lineMaxMs = 80.0f;   // tank lines at SIZE 50%
    int   erPattern = 2;                          // 0 none, 1 booth, 2 room, 3 hall, 4 huge
    float erMs = 50.0f;                           // early reflection window at SIZE 50%
    float erGain = 0.7f;                          // early level at EARLY 100%
    int   diffusers = 4;                          // input diffuser stages, 0..8
    float diffuserMs = 10.0f;                     // longest diffuser at SIZE 50%
    float darkness = 1.0f;                        // >1 darker tail for the same TONE
    float lowBias = 0.0f;                         // added to LOW END (-10..10 scale)
    float modScale = 1.0f;                        // MOD depth multiplier
    float decayMin = 0.2f, decayMax = 20.0f;      // DECAY is held inside this range
    unsigned features = 0;
};

// Where a new type starts when picked in the browser.
struct Starting
{
    float decay = 2.0f, size = 0.5f, diffusion = 0.7f, early = 0.5f;
    float a = 0.0f, b = 0.0f, c = 0.0f;
    int mode = 0;
};

struct TypeInfo
{
    const char* name;
    const char* category;
    const char* blurb;      // one sentence for the panel and the browser
    const char* keywords;   // extra words the search matches
    Voicing voicing;
    Starting start;
    const char* charLabel[3];               // empty: knob unused for this type
    const char* modeLabel;                  // empty: no CHARACTER choice
    std::vector<const char*> modeNames;
    // Global knobs that mean something else for this type (empty: unchanged).
    const char* decayCaption = "";
    const char* sizeCaption = "";
    const char* earlyCaption = "";
    const char* diffusionCaption = "";
    const char* charTips[3] = { "", "", "" };   // tooltips for the CHARACTER knobs
    const char* modeTip = "";
};

enum TypeIndex
{
    TAmbience, TStudio, TLiveRoom, TDrumRoom, TClub, TChamber, TReflections,
    TSmallHall, TConcertHall, TArena, TCathedral,
    TPlate, TVocalPlate, TVintagePlate,
    TSpring, TSurfSpring,
    TCloud, TBloom, TShimmer, THalo, TAbyss, TChoir, TSwell,
    TNonlinear, TGated, TReverse, TTape, TLofi, TResonant,
    NumTypes
};

const std::vector<TypeInfo>& types();
const TypeInfo& typeInfo (int index);
const std::vector<const char*>& categories();   // in display order
inline constexpr int kMaxModes = 12;

// Text for a CHARACTER knob value (0..1) as the given type reads it.
std::string charText (int type, int slot, float value);

// Mappings shared by the engine and the panel.
float decayFor (int type, float decaySeconds);   // DECAY held in the type's range
int   shiftSemis100 (float knob, bool allowOff); // shimmer interval in cents; -100000 = off
const char* shiftName (float knob, bool allowOff);
int   chordIndex (float knob);
const char* chordName (int index);
const std::vector<int>& chordSemis (int index);
const char* noteName (int pc);

// Searching: a score above zero means the type matches every word of the query.
int matchScore (const std::string& query, const std::string& name, const std::string& category,
                const std::string& blurb, const std::string& keywords);
} // namespace lathe::rvb
