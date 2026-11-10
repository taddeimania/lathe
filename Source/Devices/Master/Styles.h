// Styles.h — what each STYLE does at MASTER 100%, and what each TARGET aims
// for. MASTER scales every stage's amount between nothing (0%) and these
// values (100%). The loudness and the peak ceiling come from TARGET alone.
#pragma once

namespace lathe::mst
{
enum Style { Gentle, Warm, Bright, Punch, Dense, NumStyles };
enum Target { Streaming, Download, Cd, Vinyl, NumTargets };
enum Stage { ToneStage, GlueStage, ColorStage, WidthStage, LimitStage, NumStages };

struct StyleRecipe
{
    // TONE: dB at MASTER 100%.
    float lowDb, mudDb, presenceDb, airDb;
    // GLUE
    float ratio;          // at MASTER 100%; 1:1 at 0%
    float depth;          // how far the threshold drops with MASTER (1 = 12 dB)
    float attackMs, releaseMs;
    // COLOR: saturation drive, 0..1
    float drive;
    // WIDTH: extra side level at MASTER 100% (0.1 = +10%)
    float width;
    // LIMIT: release time
    float limitReleaseMs;
};

inline const StyleRecipe& recipe (int style)
{
    static const StyleRecipe r[NumStyles] {
        //  low   mud   pres  air    ratio depth  att   rel    drive width  limRel
        {  0.6f, -0.8f, 0.3f, 1.0f,  1.5f, 0.7f, 30.0f, 250.0f, 0.06f, 0.05f, 220.0f },   // GENTLE
        {  1.8f, -0.3f,-1.0f,-0.6f,  2.0f, 0.9f, 40.0f, 300.0f, 0.45f, 0.00f, 200.0f },   // WARM
        {  0.0f, -1.5f, 1.0f, 2.5f,  2.0f, 0.9f, 25.0f, 200.0f, 0.15f, 0.12f, 160.0f },   // BRIGHT
        {  2.0f, -2.0f, 1.5f, 0.8f,  3.0f, 1.0f, 35.0f, 120.0f, 0.25f, 0.06f, 120.0f },   // PUNCH
        {  1.2f, -1.0f, 0.8f, 1.2f,  3.0f, 1.1f,  8.0f, 150.0f, 0.60f, 0.10f,  60.0f } }; // DENSE
    return r[style < 0 ? 0 : (style >= NumStyles ? NumStyles - 1 : style)];
}

struct TargetSpec
{
    float lufs;          // integrated loudness to aim for
    float ceilingDb;     // true-peak ceiling, dBTP
    float monoBassHz;    // fixed mono-bass crossover; 0 = follow MASTER
};

inline const TargetSpec& targetSpec (int target)
{
    static const TargetSpec t[NumTargets] {
        { -14.0f, -1.0f,   0.0f },   // STREAMING
        { -11.0f, -1.0f,   0.0f },   // DOWNLOAD
        { -10.0f, -0.3f,   0.0f },   // CD
        { -13.0f, -1.0f, 150.0f } }; // VINYL
    return t[target < 0 ? 0 : (target >= NumTargets ? NumTargets - 1 : target)];
}

// Calibration: LEARN sets the input gain so the song arrives at this
// integrated loudness, which is where GLUE and COLOR's settings are tuned.
constexpr float kReferenceLufs = -16.0f;

// Band centres.
constexpr float kLowHz = 90.0f, kMudHz = 250.0f, kPresenceHz = 3200.0f, kAirHz = 10000.0f, kRumbleHz = 20.0f;
} // namespace lathe::mst
