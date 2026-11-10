// Blocks.h — the parts the reverb's reverb machines are built from: a 16-line
// feedback delay network, allpass diffusers, early reflection patterns, a
// pitch shifter for shimmer, a vowel formant bank and a tuned comb bank.
// No JUCE, no allocation after prepare().
#pragma once

#include "Dsp.h"

namespace lathe::rvb
{
// ---------------------------------------------------------------------------
// Tank: a 16-line feedback delay network with a Hadamard matrix, three-band
// decay (lows, mids and highs each with their own decay time), modulated line
// lengths and an infinite mode. Output is normalised so that its energy is
// roughly the input's, whatever the decay time.
// ---------------------------------------------------------------------------
class Tank
{
public:
    static constexpr int N = 16;
    struct Params
    {
        float minMs = 20.0f, maxMs = 80.0f;   // line lengths, already scaled by SIZE
        float rt60 = 2.0f;                    // mid-band decay
        float lowMult = 1.0f;                 // RT below the crossover = rt60 * lowMult
        float hfRatio = 0.4f;                 // RT above hfHz = rt60 * hfRatio
        float hfHz = 4000.0f;                 // where the highs start to die faster
        float xoverHz = 220.0f;
        float modDepthMs = 0.3f, modRateHz = 0.5f;
        bool infinite = false;
    };

    void prepare (float sampleRate, float maxLineMs);
    void reset();
    void set (const Params&, int blockSamples);
    void tick (float inL, float inR, float& outL, float& outR);

private:
    std::array<DelayLine, N> lines;
    std::array<OnePole, N> xover, hfSplit;
    std::array<float, N> len {}, lenTarget {}, gain {}, lowExtra {}, highExtra {}, mod {}, modStep {}, phase {}, rate {};
    std::array<SmoothRandom, N> wander;
    Rng rng { 77 };
    float sr = 48000.0f, maxLen = 1000.0f, normGain = 0.3f, depth = 0.0f, lenCoeff = 0.01f;
    bool infinite = false, first = true;
};

// ---------------------------------------------------------------------------
// A chain of up to 8 allpass diffusers, optionally modulated.
// ---------------------------------------------------------------------------
class Diffuser
{
public:
    static constexpr int kMax = 8;
    void prepare (float sampleRate, float maxMs, uint32_t seed);
    void reset();
    // stages 0..8; longestMs at the first stage; g the allpass gain (0..0.8).
    void set (int stages, float longestMs, float g, float modDepthMs = 0.0f, float modRateHz = 0.5f);
    float tick (float x);

private:
    std::array<AllpassDelay, kMax> ap;
    std::array<float, kMax> len {}, target {}, jitter {};
    std::array<QuadLfo, kMax> lfo;
    int stages = 0;
    float g = 0.6f, depth = 0.0f, rate = 0.5f, sr = 48000.0f, maxLen = 100.0f;
    bool first = true;
};

// ---------------------------------------------------------------------------
// Early reflections: a stereo pattern of taps read from one mono line.
// ---------------------------------------------------------------------------
class EarlyTaps
{
public:
    static constexpr int kMaxTaps = 28;
    void prepare (float sampleRate, float maxMs);
    void reset();
    // pattern 1..4 (see Voicing::erPattern); windowMs scaled by SIZE;
    // distance 0..1 bunches the taps later and flatter.
    void configure (int pattern, float windowMs, float distance);
    void setTone (float lpHz);
    void tick (float in, float& outL, float& outR);
    float readDelayed (float ms) const { return line.readLinear (ms * 0.001f * sr); }

private:
    struct Tap { float delay = 0.0f, gain = 0.0f; };
    DelayLine line;
    std::array<Tap, kMaxTaps> left {}, right {};
    int count = 0, lastPattern = -1;
    float lastWindow = -1.0f, lastDistance = -1.0f, sr = 48000.0f, norm = 1.0f;
    OnePole lpL, lpR, hpL, hpR;
};

// ---------------------------------------------------------------------------
// Pitch shifter: two crossfading read heads on a short delay line.
// ---------------------------------------------------------------------------
class PitchShifter
{
public:
    void prepare (float sampleRate);
    void reset();
    void setShift (int cents);   // -2400..2400
    float tick (float x);

private:
    DelayLine line;
    float phase = 0.0f, step = 0.0f, window = 2000.0f, sr = 48000.0f;
};

// ---------------------------------------------------------------------------
// Vowel formants: up to six "singers", each three band-passes, drifting.
// ---------------------------------------------------------------------------
class FormantBank
{
public:
    static constexpr int kVoices = 6;
    void prepare (float sampleRate);
    void reset();
    // vowel 0..6 (AH, OH, OO, EE, AH-OH, OH-OO, RANDOM); resonance, voices, drift 0..1.
    void set (int vowel, float resonance, float voices, float drift, int blockSamples);
    void tick (float inL, float inR, float& outL, float& outR);

private:
    struct Singer
    {
        std::array<Svf, 3> f;
        SmoothRandom drift;
        float offset = 1.0f, pan = 0.0f;
    };
    std::array<Singer, kVoices> singers;
    Rng rng { 31 };
    float sr = 48000.0f, morph = 0.0f, morphPhase = 0.0f, gain = 1.0f, q = 6.0f;
    std::array<float, 3> current { 730.0f, 1090.0f, 2440.0f }, target { 730.0f, 1090.0f, 2440.0f };
    int active = 4, holdLeft = 0;
};

// ---------------------------------------------------------------------------
// Tuned combs: a bank of up to 12 damped combs on the notes of a chord.
// ---------------------------------------------------------------------------
class CombBank
{
public:
    static constexpr int kMax = 12;
    void prepare (float sampleRate);
    void reset();
    void set (int rootPc, int chord, float rt60, float toneHz);
    void tick (float inL, float inR, float& outL, float& outR);

private:
    struct Comb { DelayLine d; OnePole damp; float delay = 100.0f, g = 0.9f, pan = 0.0f, norm = 0.3f; };
    std::array<Comb, kMax> combs;
    int count = 0, lastRoot = -1, lastChord = -1;
    float sr = 48000.0f, norm = 0.3f;
};
} // namespace lathe::rvb
