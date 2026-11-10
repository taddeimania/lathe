// Master.h — the mastering engine's signal path, without JUCE:
//
//   in -> GAIN (set by LEARN) -> TONE -> GLUE -> COLOR -> WIDTH -> DRIVE -> LIMIT -> out
//
// STYLE picks a recipe, MASTER scales it, TARGET sets loudness and ceiling.
// LEARN listens to the song and sets GAIN (so the stages see a predictable
// level) and DRIVE (so the output lands on the target loudness).
// COMPARE plays the untouched input, delayed to line up and turned to the
// same loudness as the master. Meters follow BS.1770.
#pragma once

#include "Dsp.h"
#include "Loudness.h"
#include "Styles.h"

#include <array>

namespace lathe::mst
{
struct MasterSettings
{
    int   style = Gentle;
    int   target = Streaming;
    float amount = 0.5f;          // MASTER, 0..1
    float trimDb = 0.0f;          // PUSH: louder or quieter than the target
    float inputGainDb = 0.0f;     // learned
    float driveOffsetDb = 14.0f;  // learned: the drive into LIMIT is this plus the
                                  // target loudness, so changing TARGET moves the
                                  // level with it
    std::array<bool, NumStages> on { true, true, true, true, true };
    bool  compare = false;
    bool  mono = false;
    int   learnToggles = 0;       // each change starts or stops LEARN
    int   meterResets = 0;        // each change clears integrated loudness and peak holds
};

struct LearnResult
{
    enum Code { None, Done, TooShort };
    int   code = None;
    float inputGainDb = 0.0f, driveOffsetDb = 14.0f;
};

// What each stage is doing, for the panel. Written by process().
struct Telemetry
{
    std::array<float, 4> toneDb {};       // low, mud, presence, air
    float glueGrDb = 0.0f;                // gain reduction now
    float glueRatio = 1.0f, glueThresholdDb = 0.0f;
    float colorDrive = 0.0f;              // 0..1
    float colorDb = 0.0f;                 // peak reduction from saturation, dB
    float widthPct = 100.0f, monoBassHz = 20.0f;
    float limitGrDb = 0.0f;               // gain reduction now
    float ceilingDb = -1.0f;
    float driveDb = 0.0f, inputGainDb = 0.0f;
    float inPeak = 0.0f, outPeak = 0.0f;  // linear, max since last read
    float momentary = -70.0f, shortTerm = -70.0f, integrated = -70.0f, truePeakDb = -100.0f;
    float inShortTerm = -70.0f;
    float matchDb = 0.0f;
    bool  learning = false;
    float learnSeconds = 0.0f;
};

class Master
{
public:
    void prepare (double sampleRate);
    void reset();
    int  latency() const noexcept { return latencySamples; }

    // Stereo in place. Any block size.
    void process (float* l, float* r, int n, const MasterSettings&);

    const Telemetry& telemetry() const noexcept { return tm; }
    void clearPeaks() { tm.inPeak = tm.outPeak = 0.0f; }

    // A finished LEARN, once. Code None if there's nothing new.
    LearnResult takeLearnResult() { auto r = result; result = {}; return r; }
    bool learning() const noexcept { return isLearning; }

    // Meters, read directly by tests.
    const LoudnessMeter& outputMeter() const { return outMeter; }
    const LoudnessMeter& inputMeter() const  { return inMeter; }

private:
    static constexpr int kStep = 32;   // control rate

    void updateControls (const MasterSettings&);
    void processChunk (float* l, float* r, int n, const MasterSettings&);
    void onHop();
    void startLearn();
    void finishLearn (bool manual);

    double sr = 48000.0;
    int latencySamples = 0;

    // --- TONE ---
    struct ToneCh { Biquad rumble, low, mud, presence, air;
                    float tick (float x) { return air.tick (presence.tick (mud.tick (low.tick (rumble.tick (x))))); }
                    void reset() { rumble.reset(); low.reset(); mud.reset(); presence.reset(); air.reset(); } };
    ToneCh toneL, toneR;
    std::array<float, 4> toneShown { 99, 99, 99, 99 };

    // --- GLUE ---
    Biquad scL, scR;
    double glueEnv = 0.0;
    float  glueGr = 0.0f;
    float  envCoef = 0.0f, attCoef = 0.0f, relCoef = 0.0f;
    float  ratio = 1.0f, thresholdDb = 0.0f, makeupDb = 0.0f;
    static constexpr float kKnee = 6.0f;
    float staticGr (float levelDb) const;

    // --- COLOR ---
    float satG = 0.0f;
    float colorPeakIn = 0.0f, colorPeakOut = 0.0f;

    // --- WIDTH ---
    Biquad sideHp;
    float  sideGain = 1.0f, sideHz = 0.0f;

    // --- LIMIT ---
    struct Limiter
    {
        void prepare (double sr);
        void reset();
        // Returns the limited, driven sample pair, and the undriven pair
        // delayed by the same amount (for bypass).
        void tick (float l, float r, float drive, float ceiling, float& outL, float& outR, float& rawL, float& rawR);
        int latency() const { return TruePeak4x::kDelay + window - 1; }

        TruePeak4x tpl, tpr;
        std::vector<float> dl, dr, rl, rr;       // ring buffers
        std::vector<float> box;
        std::vector<float> dqVal;                // moving minimum: ascending values...
        std::vector<long>  dqIdx;                // ...and when each arrived
        int dqHead = 0, dqCount = 0;
        long t = 0;
        int size = 1, w = 0, window = 1, gw = 0;
        double boxSum = 0;
        float hold = 1.0f, relCoef = 0.001f, lastGain = 1.0f;
        int refresh = 0;
    } limiter;
    float limitGainMin = 1.0f, ceilingLin = 0.89f, targetLufs = -14.0f;

    // --- smoothing ---
    Smoother smAmount, smGain, smDrive, smMatch;
    std::array<Smoother, NumStages> smOn;
    Smoother smCompare, smMono;

    // --- metering, compare ---
    LoudnessMeter inMeter, outMeter;
    FixedDelay dryDL, dryDR;                     // input delayed by the latency
    float matchDb = 0.0f;
    int seenResets = 0;

    // --- LEARN ---
    bool isLearning = false, pendingAck = false;
    int  seenLearnToggles = 0, ackHops = 0;
    int  heardHops = 0, silentHops = 0, learnHops = 0;
    float learnGain = 0.0f, learnDrive = 0.0f, startGain = 0.0f, startDrive = 0.0f;
    static constexpr int kBins = 800;
    std::array<double, kBins> lInE {}, lOutE {};
    std::array<int, kBins> lCount {};
    LearnResult result;

    float appliedGainDb = 0.0f, appliedDriveDb = 0.0f;   // what's in use this chunk
    float lastSetGain = 0.0f, lastSetDrive = 0.0f;
    bool  limitOnNow = true, firstBlock = true;
    // Per-sample ramps across a control step.
    float rGain = 1, rDrive = 1, rMatch = 1, rCompare = 0, rMono = 0;
    std::array<float, NumStages> rOn {};

    Telemetry tm;
};
} // namespace lathe::mst
