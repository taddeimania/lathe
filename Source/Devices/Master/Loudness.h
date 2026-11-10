// Loudness.h — a loudness meter after ITU-R BS.1770-4 / EBU R128:
// K-weighting, momentary (400 ms), short-term (3 s), gated integrated
// loudness, and true peak by 4x interpolation. Stereo, channel weights 1.
//
// Everything advances in 100 ms hops. A gated 400 ms block is taken every
// hop (75% overlap) and filed in a 0.1 LU histogram, so integrated loudness
// over any length of programme needs no allocation.
#pragma once

#include "Dsp.h"
#include <array>

namespace lathe::mst
{
class LoudnessMeter
{
public:
    static constexpr float kSilence = -70.0f;   // the absolute gate; also "nothing yet"

    void prepare (double sampleRate);
    void reset();                // everything
    void resetIntegrated();      // integrated loudness and the true-peak hold
    void setTruePeak (bool on) { truePeakOn = on; }   // skip it where it isn't shown

    // Feed audio. Returns the number of 100 ms hops that completed.
    int process (const float* l, const float* r, int n);

    float momentary() const  { return toLufs (meanOfLast (4)); }
    float shortTerm() const  { return toLufs (meanOfLast (30)); }
    float integrated() const;                 // kSilence until a block passes the gate
    bool  hasIntegrated() const { return gatedBlocks > 0; }
    float truePeakDb() const { return gainToDb (tpHold); }     // since the last reset
    float recentTruePeak() { const float p = tpRecent; tpRecent = 0.0f; return p; }   // linear, then cleared

    // Energy (mean square, both channels summed) of the 400 ms block that
    // ended with the latest hop. For the learn and compare logic.
    double blockEnergy() const { return meanOfLast (4); }
    double shortEnergy() const { return meanOfLast (30); }
    int    hops() const        { return hopCount; }

    static float toLufs (double energy) { return energy <= 0 ? -200.0f : (float) (-0.691 + 10.0 * std::log10 (energy)); }
    static double toEnergy (float lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

private:
    double meanOfLast (int count) const;

    struct KFilter
    {
        Biquad shelf, hp;
        float tick (float x) { return hp.tick (shelf.tick (x)); }
        void reset() { shelf.reset(); hp.reset(); }
    };

    double sr = 48000.0;
    KFilter kl, kr;
    TruePeak4x tpl, tpr;

    int hopLen = 4800, inHop = 0, hopCount = 0;
    double hopSum = 0;
    std::array<double, 32> hopEnergy {};     // ring of the last 32 hops
    int hopWrite = 0;

    // Integrated: histogram of gated block loudness, -70 .. +10 LUFS in 0.1 LU bins.
    static constexpr int kBins = 800;
    std::array<double, kBins> binEnergy {};
    std::array<int, kBins> binCount {};
    int gatedBlocks = 0;

    float tpHold = 0.0f, tpRecent = 0.0f;
    bool truePeakOn = true;
};
} // namespace lathe::mst
