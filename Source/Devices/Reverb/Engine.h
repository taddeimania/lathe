// Engine.h — the reverb's signal path, without JUCE:
//
//   input -> (swell, DRY mode) -> pre-delay -> the type's machine -> LOW END,
//   MID, LOW CUT, HIGH CUT -> (lo-fi, smash, swell) -> duck and gate -> width
//   -> mixed with the dry signal -> output.
//
// HOLD keeps the tail going for ever: INFINITE still lets new sound in,
// FREEZE shuts the input so only what's there plays on.
#pragma once

#include "Machines.h"

namespace lathe::rvb
{
struct EngineSettings
{
    int   type = TConcertHall;
    float decay = 3.0f;           // seconds
    float size = 0.55f;           // 0..1
    float predelayMs = 20.0f;     // the processor converts synced values to ms
    float diffusion = 0.8f;       // 0..1
    float early = 0.45f;          // 0..1
    float a = 0.45f, b = 0.35f, c = 0.0f;
    int   mode = 0;

    float tone = 0.5f;            // 0..1
    float lowEnd = 0.0f;          // -10..10
    float midDb = 0.0f;           // -12..12
    float lowCutHz = 20.0f;       // 20 = off
    float highCutHz = 20000.0f;   // 20000 = off

    float modDepth = 0.3f;        // 0..1
    float modRate = 0.6f;         // Hz

    float duck = 0.0f;            // 0..1
    float gateDb = -80.0f;        // -80 = off
    float releaseMs = 300.0f;     // for DUCK and GATE

    bool  hold = false;
    int   holdMode = 0;           // 0 INFINITE, 1 FREEZE

    float inputDb = 0.0f;
    float mix = 0.3f;             // 0 dry .. 0.5 both full .. 1 wet
    float width = 1.0f;           // 0..1.5
    float outputDb = 0.0f;
};

class Engine
{
public:
    static constexpr int kBlock = 32;

    void prepare (double sampleRate, int maxBlock);
    void reset();

    // In-place safe: every input sample is read before that output sample is written.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings&);

    // Peaks since the last take (for meters), and live state for the panel.
    float takeInputPeak()  { const float p = inPeak; inPeak = 0.0f; return p; }
    float takeOutputPeak() { const float p = outPeak; outPeak = 0.0f; return p; }
    float takeWetPeak()    { const float p = wetPeak; wetPeak = 0.0f; return p; }
    float duckReductionDb() const noexcept { return duckDb; }
    bool  gateOpen() const noexcept { return gateGain > 0.5f; }
    float swellLevel() const noexcept { return swellEnv; }
    int   activeType() const noexcept { return active; }

    // The knobs as the machines see them (also used by the probe and tests).
    static Controls resolve (const EngineSettings&, float sampleRate);

private:
    MachineBase* machineFor (int type);
    void processBlock (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings&);
    float swellTick (float level, const EngineSettings&);

    float sr = 48000.0f;
    SpaceMachine space;
    PlateMachine plate;
    SpringMachine spring;
    ReflectionsMachine reflections;
    NonlinearMachine nonlinear;
    TapeMachine tapeEcho;

    int active = -1, wanted = -1;
    float switchGain = 1.0f;
    bool switching = false, primed = false;

    DelayLine preL, preR;
    Smoother predelay, inGain, outGain, dryGain, wetGain, widthS, sendGain;
    std::array<DcBlocker, 2> dc;
    std::array<Biquad, 2> lowShelf, mid, lowCut, lowCut2, highCut, highCut2;
    float lastLowEnd = 99.0f, lastMid = 99.0f, lastLowCut = -1.0f, lastHighCut = -1.0f;

    // Dynamics.
    EnvFollower duckEnv, gateEnv;
    float duckDb = 0.0f, gateGain = 1.0f, gateHold = 0.0f;
    float lastRelease = -1.0f;

    // Swell.
    EnvFollower swellFast, swellSlow;
    float swellEnv = 1.0f, swellPos = 1.0f, sinceTrigger = 1.0e9f;
    bool swellArmed = true, swellFalling = false;

    // Lo-fi and smash.
    float holdL = 0.0f, holdR = 0.0f, holdCount = 0.0f;
    DelayLine wobbleL, wobbleR;
    QuadLfo wobbleLfo;
    SmoothRandom wobbleDrift;
    Rng rng { 3 };
    std::array<OnePole, 2> lofiLp;
    Compressor smash;

    std::array<float, kBlock> sendL {}, sendR {}, wetL {}, wetR {}, dryL {}, dryR {};
    float inPeak = 0.0f, outPeak = 0.0f, wetPeak = 0.0f;
};
} // namespace lathe::rvb
