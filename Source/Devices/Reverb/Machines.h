// Machines.h — the reverb's six reverb engines. Every type in Types.h is a voicing
// of one of these. The engine resolves the panel's knobs into Controls once
// per control block (32 samples) and hands them to the active machine.
#pragma once

#include "Blocks.h"
#include "Types.h"

namespace lathe::rvb
{
struct Controls
{
    int type = TConcertHall;
    const TypeInfo* info = nullptr;
    float sr = 48000.0f;

    float decay = 2.0f;          // seconds, held in the type's range
    float size = 0.5f;           // 0..1
    float sizeScale = 1.0f;      // 0.5..2, from SIZE
    float diffusion = 0.7f;      // 0..1
    float early = 0.5f;          // 0..1
    float a = 0.0f, b = 0.0f, c = 0.0f;
    int   mode = 0;

    float tone = 0.5f;           // 0..1
    float hfRatio = 0.35f;       // high-band RT / mid RT, from TONE and the type's darkness
    float toneHz = 6000.0f;      // a low-pass corner that follows TONE
    float dampHz = 3000.0f;      // where a tank's highs start dying faster
    float lowEnd = 0.0f;         // -10..10, with the type's bias
    float lowMult = 1.0f;        // low-band RT / mid RT

    float modDepth = 0.3f;       // 0..1
    float modRate = 0.6f;        // Hz

    bool infinite = false;       // HOLD: the tail never decays
};

class MachineBase
{
public:
    virtual ~MachineBase() = default;
    virtual void prepare (float sampleRate) = 0;
    virtual void reset() = 0;
    virtual void set (const Controls&, int blockSamples) = 0;
    // Wet only. n <= 32 per call.
    virtual void process (const float* inL, const float* inR, float* outL, float* outR, int n) = 0;
};

// ---------------------------------------------------------------------------
// SPACE: rooms, halls, chambers and the ambient types.
// ---------------------------------------------------------------------------
class SpaceMachine final : public MachineBase
{
public:
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

private:
    float sr = 48000.0f;
    unsigned features = 0;
    Tank tank;
    std::array<Diffuser, 2> diff, bloom;
    EarlyTaps early;
    DelayLine buildLineL, buildLineR, echoLine;
    std::array<PitchShifter, 4> shifters;   // two voices per side
    FormantBank choir;
    CombBank combs;
    std::array<Biquad, 2> shimmerHp, shimmerLp;
    std::array<OnePole, 2> echoLp;
    EnvFollower shimmerEnv;
    std::array<float, 2> tankOut {}, bloomFb {};
    Rng noise { 5 };

    float erGain = 0.0f, lateGain = 1.0f, buildSamples = 0.0f, echoSamples = 0.0f, echoGain = 0.0f;
    float shimmerIn = 0.0f, shimmerTank = 0.0f, bloomFeedback = 0.0f, bloomMix = 0.0f;
    float choirMix = 0.0f, ringMix = 0.0f, grit = 0.0f, levelTrim = 1.0f;
    bool voice2 = false;
};

// ---------------------------------------------------------------------------
// PLATE: a figure-eight tank (two allpass-delay loops feeding each other).
// ---------------------------------------------------------------------------
class PlateMachine final : public MachineBase
{
public:
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

private:
    float sr = 48000.0f, scale = 1.0f, scaleTarget = 1.0f;
    std::array<AllpassDelay, 4> inDiff;
    std::array<AllpassDelay, 2> modAp, decayAp;
    std::array<DelayLine, 4> delays;    // left 1, left 2, right 1, right 2
    std::array<OnePole, 2> damp, lowSplit;
    OnePole bandwidth;
    QuadLfo lfoL, lfoR;
    float decay = 0.5f, lowExtra = 0.0f, inG1 = 0.75f, inG2 = 0.625f, decG = 0.5f;
    float depth = 0.0f, rate = 1.0f, drive = 0.0f, driveNorm = 1.0f, levelTrim = 1.0f;
    float feedL = 0.0f, feedR = 0.0f;
    bool infinite = false;
};

// ---------------------------------------------------------------------------
// SPRING: dispersive spring loops, one to three of them.
// ---------------------------------------------------------------------------
class SpringMachine final : public MachineBase
{
public:
    static constexpr int kStages = 64;
    static constexpr int kMaxStretch = 16;
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

private:
    struct Spring
    {
        // Stretched allpass sections: y = a x + x[-K] - a y[-K].
        std::array<std::array<float, kMaxStretch>, kStages> xh {}, yh {};
        int pos = 0;
        DelayLine loop;
        OnePole lp, hp;
        float len = 1000.0f, g = 0.8f, fb = 0.0f, pan = 0.0f;
        AllpassDelay smear;
        float smearLen = 100.0f;
    };
    float disperse (Spring&, float x);

    float sr = 48000.0f;
    std::array<Spring, 3> springs;
    int count = 2, stretch = 5, stages = 48;
    float a = 0.6f, dwell = 0.0f, dwellNorm = 1.0f, smearG = 0.0f, levelTrim = 1.0f;
    OnePole outLp;
    bool infinite = false;
};

// ---------------------------------------------------------------------------
// REFLECTIONS: an image-source small room with a movable source.
// ---------------------------------------------------------------------------
class ReflectionsMachine final : public MachineBase
{
public:
    static constexpr int kMaxTaps = 320;
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

    // Where the dry sound sits, -1..1, for the engine's dry pan.
    float dryPan() const noexcept { return pan; }

private:
    struct Tap { float delay; float gain; bool dull; };
    struct TapSet { std::array<Tap, kMaxTaps> l, r; int nl = 0, nr = 0; float norm = 1.0f, lowNorm = 1.0f; };
    void compute (TapSet&, float x, float y, float size, int shape);

    float sr = 48000.0f;
    DelayLine line;
    std::array<TapSet, 2> sets;
    int current = 0;
    float fade = 1.0f, fadeStep = 0.0f;   // 1 = all `current`
    float lastX = -1.0f, lastY = -1.0f, lastSize = -1.0f;
    int lastShape = -1;
    float pan = 0.0f, erGain = 1.0f, tailGain = 0.4f, wanderDepth = 0.0f, wanderOffset = 0.0f;
    SmoothRandom wander;
    Rng rng { 13 };
    std::array<OnePole, 2> dullLp, brightLp, lowCut, lowSplit, highSplit;
    Tank tail;
};

// ---------------------------------------------------------------------------
// NONLINEAR: a shaped bank of taps with feedback, and a late tank.
// ---------------------------------------------------------------------------
class NonlinearMachine final : public MachineBase
{
public:
    static constexpr int kTaps = 56;
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

    static float shape (int s, float x);   // envelope 0..1 over the TIME window

private:
    float sr = 48000.0f;
    DelayLine line;
    std::array<Diffuser, 2> diff;
    std::array<float, kTaps> tapTime {}, tapTarget {}, tapJitter {}, gainL {}, gainR {};
    std::array<OnePole, 2> lp;
    std::array<float, 2> last {};
    QuadLfo lfo;
    Tank late;
    float feedback = 0.0f, lateGain = 0.0f, levelTrim = 1.0f, depth = 0.0f;
    bool first = true;
};

// ---------------------------------------------------------------------------
// TAPE: multi-head tape echo with wow, smeared, into a small room.
// ---------------------------------------------------------------------------
class TapeMachine final : public MachineBase
{
public:
    static constexpr int kMaxHeads = 6;
    void prepare (float sampleRate) override;
    void reset() override;
    void set (const Controls&, int blockSamples) override;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) override;

private:
    float sr = 48000.0f;
    DelayLine tape;
    std::array<Diffuser, 2> smear;
    std::array<float, kMaxHeads> frac {};
    int heads = 3;
    bool scatter = false;
    Smoother time;
    QuadLfo wow, flutter;
    SmoothRandom drift;
    Rng rng { 23 };
    OnePole fbLp, fbHp, outLpL, outLpR;
    Tank room;
    float feedback = 0.0f, wowDepth = 0.0f, roomGain = 0.0f, last = 0.0f;
    bool infinite = false;
};
} // namespace lathe::rvb
