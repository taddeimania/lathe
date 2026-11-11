#pragma once

#include "../Common.h"
#include <complex>
#include "../Devices/Master/Loudness.h"

namespace lathe::assistant
{
//==============================================================================
/** What analyze_mix hears: the mix and each track's own signal, rendered offline, measured
    the way someone mixing would check them. Feed it with add() (any thread, one at a time),
    then ask for report().

    Every tap is cut into windows of kWindow samples. For each window and each frequency
    band it keeps the power of the left and right channels and their cross term (an FFT),
    which gives level, frequency balance, stereo width per band, and which tracks share a
    band at the same time (masking). */
class MixAnalysis
{
public:
    struct Tap
    {
        String name;
        bool isMix = false;
        bool isReturn = false;
    };

    /** sampleRate; samplesPerBar to turn time into bars; firstBar = the bar the render starts at;
        lengthSamples = how long the render is (without any tail); sectionBars = how many bars
        each "sections" entry covers. */
    MixAnalysis (std::vector<Tap> taps, double sampleRate, double samplesPerBar, double firstBar,
                 int64 lengthSamples, int sectionBars);
    ~MixAnalysis();

    void add (int tap, const AudioBuffer<float>&, int start, int count, int64 timelineSample);

    /** The JSON the assistant gets. Call once, after the last add(). */
    var report();

    struct Band { const char* name; float low, high; };
    static constexpr int kBands = 7;
    static const Band bands[kBands];
    static constexpr int kWindowOrder = 12;            // 4096 samples
    static constexpr int kWindow = 1 << kWindowOrder;

private:
    struct Window
    {
        float bandPower[kBands] {};   // mean of L and R power, per band
        float power = 0.0f;           // all bands
        float timePower = 0.0f;       // mean square, both channels
        int bar = 0;                  // 0-based, from the start of the render
    };
    struct TapState
    {
        Tap tap;
        std::vector<float> l, r;               // the window being filled
        int64 windowStart = -1;                // timeline sample where it starts
        std::vector<Window> windows;
        double bandL[kBands] {}, bandR[kBands] {}, bandX[kBands] {};   // whole-render sums
        double sumL = 0.0, sumR = 0.0, sumX = 0.0;                    // time domain
        float peak = 0.0f;
        int64 overs = 0;                       // samples above 0 dBFS
        int64 samples = 0;
        std::vector<double> barEnergy;         // sum of squares (both channels) in each bar
        std::vector<double> barLoudness;       // the same, K-weighted (what loudness meters hear)
        double kSum = 0.0;                     // K-weighted sum of squares, both channels
        double kState[2][2][2] {};             // K-weighting filters: [channel][stage][z1, z2]
        std::unique_ptr<mst::LoudnessMeter> meter;   // its own loudness (LUFS)
        std::vector<int64> barSamples;
        std::vector<bool> barActive() const;
    };

    void finishWindow (TapState&) const;
    var tapReport (const TapState&, const TapState* mix) const;
    var bandBalance (const TapState&) const;
    String barRanges (const std::vector<bool>&) const;
    float loudestBandShare (const TapState&, int& band) const;

    std::vector<std::unique_ptr<TapState>> taps;
    double sampleRate, samplesPerBar, firstBar;
    int64 length;
    int sectionBars, numBars;
    std::vector<float> hann;
    // K-weighting (ITU-R BS.1770): a high shelf, then a high-pass. b0, b1, b2, a1, a2 for each stage.
    double kCoeffs[2][5] {};
    double kWeight (TapState&, int channel, double x) const;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> bitReverse;
    mutable std::vector<std::complex<double>> scratch;
    /** In place, radix 2, kWindow points. */
    void transform (std::vector<std::complex<double>>&) const;

    // The mix's loudness: whole, and for each section.
    std::unique_ptr<mst::LoudnessMeter> mixMeter;
    std::vector<std::unique_ptr<mst::LoudnessMeter>> sectionMeters;
};
} // namespace lathe::assistant
