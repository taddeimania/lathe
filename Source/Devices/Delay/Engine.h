// Engine.h — the delay's signal path, without JUCE.
//
//   input -> (swell) -> send -> [ write chain: FILTER, GRIT, SMEAR and the
//   type's own colour ] -> delay line -> the type's reader (one echo, taps,
//   heads, a second delay, backwards slices, pitched slices) -> feedback back
//   into the write chain (REPEATS, TAPER, DUCK REPEATS)
//   -> echoes: the type's post stage (tremolo, sweeping filter, speaker)
//   -> LOW CUT, HIGH CUT -> DUCK -> WIDTH -> mixed with the dry sound.
//
// Everything in the write chain is inside the loop, so the echoes change a
// little more each time round, as on a real machine. INFINITE closes the input
// and keeps what's in the line going round for ever, untouched.
#pragma once

#include "Dsp.h"
#include "Types.h"

namespace lathe::dly
{
struct EngineSettings
{
    int   type = TDigital;
    float timeMs = 400.0f;        // the processor converts synced and tapped values to ms
    float repeats = 0.4f;         // 0..1.1; past 1 it runs away (softly)
    float taper = 0.0f;           // 0..1: later repeats fall away faster
    int   stereo = 0;             // 0 STEREO, 1 PING-PONG, 2 MONO

    float a = 0.5f, b = 0.0f, c = 0.0f;
    int   mode = 0;

    float filter = 0.0f;          // 0..1
    float grit = 0.0f;            // 0..1
    float smear = 0.0f;           // 0..1
    float lowCutHz = 20.0f;       // 20 = off
    float highCutHz = 20000.0f;   // 20000 = off

    float speed = 0.3f;           // 0..1 (see speedHz)
    float depth = 0.1f;           // 0..1

    float duck = 0.0f;            // 0..1
    float releaseMs = 250.0f;
    bool  duckRepeats = false;

    bool  hold = false;           // INFINITE

    float inputDb = 0.0f;
    float mix = 0.35f;            // 0 dry .. 0.5 both full .. 1 wet
    float width = 1.0f;           // 0..1.5
    float outputDb = 0.0f;
};

// MOTION SPEED as Hz.
inline float speedHz (float v) { return expMap (v, 0.05f, 10.0f); }

class Engine
{
public:
    static constexpr int kBlock = 32;

    void prepare (double sampleRate, int maxBlock);
    void reset();

    // In-place safe: every input sample is read before that output sample is written.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings&);

    float takeInputPeak()  { const float p = inPeak; inPeak = 0.0f; return p; }
    float takeOutputPeak() { const float p = outPeak; outPeak = 0.0f; return p; }
    float takeWetPeak()    { const float p = wetPeak; wetPeak = 0.0f; return p; }
    float duckReductionDb() const noexcept { return gainToDb (duckGain); }
    float delayMs() const noexcept { return timeS.value * 1000.0f / sr; }

    // How the line is read for the type (shared with the tests).
    static float effectiveTimeMs (const EngineSettings& s) { return timeFor (s.type, s.mode, s.timeMs); }

private:
    struct Channel
    {
        DelayLine line, line2, pitch;
        // Write chain.
        Biquad voiceLp, voiceLp2, voiceHp, gritLp;
        std::array<AllpassDelay, 3> smear;
        std::array<AllpassDelay, 4> cloud;
        DcBlocker dc, dc2;
        // Tape.
        Biquad tapeBump, tapeAge, tapeAge2, tapeBias, tapeLow;
        OnePole hissTone;
        // Bucket.
        Biquad bbdIn, bbdIn2, bbdOut, bbdOut2;
        EnvFollower compEnv, expEnv;
        float bbdHeld = 0.0f;
        // Lo-fi.
        float lofiHeld = 0.0f;
        Biquad shapeHp, shapeLp, shapePeak;
        // Post.
        Svf sweep;
        Biquad lowCut, lowCut2, highCut, highCut2;
        // Feedback.
        EnvFollower taperEnv;
        float fb = 0.0f, fb2 = 0.0f;
    };

    void updateControls (const EngineSettings&);
    void processBlock (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings&);
    float writeChain (Channel&, int ch, float x);
    float readMain (Channel&, int ch, float delaySamples);

    float sr = 48000.0f;
    std::array<Channel, 2> chans;
    int maxMain = 1, maxSecond = 1;

    Smoother timeS, inGain, outGain, dryGain, wetGain, widthS, holdS, fbS, fb2S, lvl2S;
    int lastType = -1, lastMode = -1, curType = TDigital, curMode = 0, curStereo = 0;
    bool primed = false, gainsPrimed = false;

    // Control-rate values.
    float gritDrive = 1.0f, gritBias = 0.0f, smearG = 0.0f, smearScale = 1.0f;
    bool voiceOn = false, gritOn = false, smearOn = false;
    float modMs = 0.0f, modHz = 0.5f, wowMs = 0.0f, flutterMs = 0.0f, crinkle = 0.0f;
    float lowCutLast = -1.0f, highCutLast = -1.0f;
    float taperK = 0.0f;
    float duckTarget = 1.0f, fbDuckTarget = 1.0f;
    float bbdCutLast = -1.0f, bbdClock = 10000.0f, bbdLoss = 0.0f, comp = 0.0f, hiss = 0.0f;
    float tapeLevel = 1.0f, tapeDrive = 1.0f;
    float lofiRate = 48000.0f, lofiQ = 0.0f, shapeDrive = 0.0f, vinyl = 0.0f;
    int shapeLast = -1;
    float cloudG = 0.0f, cloudMix = 0.0f, cloudScale = 1.0f;
    float ratio2 = 1.0f;
    float lfoRatio = 1.0f, tremDepth = 0.0f, sweepCentre = 1000.0f, sweepOct = 0.0f, sweepQ = 0.7f;
    float swellRise = 1000.0f, revSmooth = 0.5f;
    float iceRatio = 2.0f, iceGrain = 1000.0f, iceBlend = 0.5f, iceDuty = 1.0f;
    float offset = 0.0f;

    static constexpr int kMaxTaps = 16;
    struct TapGain { float at = 1.0f, gl = 0.0f, gr = 0.0f; };
    std::array<TapGain, kMaxTaps> taps {};
    int numTaps = 0;
    float tapNorm = 1.0f;

    // Modulation.
    SmoothRandom modDrift, wowDrift, crinkleDrift;
    float modPhase = 0.0f, flutterPhase = 0.0f;
    float crinkleGain = 1.0f, crinkleTarget = 1.0f, crinkleLeft = 0.0f;
    Rng rng { 5 };

    // Type-specific state.
    float revPhase = 0.0f;          // Reverse: samples into the slice
    float icePhase = 0.0f;          // Ice: 0..1 through a grain
    float lfoPhase = 0.0f;          // Trem / Filter
    float randomA = 0.0f, randomB = 0.0f, sweepV = 0.0f, tremGain = 1.0f;
    float lofiPhase = 0.0f, bbdPhase = 0.0f;
    bool bbdTick = false, lofiTick = false;
    OnePole crackleTone;
    float crackleEnv = 0.0f;
    float typeFade = 1.0f;

    // Dynamics and swell.
    EnvFollower duckEnv, playEnv, sweepEnv, onsetFast, onsetSlow, wetEnv, peakEnv;
    float duckGain = 1.0f, fbDuck = 1.0f;
    float swellGain = 1.0f;
    bool swellArmed = true, revArmed = true, swellFalling = false;

    float inPeak = 0.0f, outPeak = 0.0f, wetPeak = 0.0f;
};
} // namespace lathe::dly
