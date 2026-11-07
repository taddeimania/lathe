#pragma once

#include "../Common.h"

namespace lathe
{
//==============================================================================
// The built-in channel EQ every track, return and the master has. Six fixed
// bands, like a console strip: low cut, low shelf, two bells, high shelf, high cut.
// Settings are atomics the audio thread reads live; the filter state lives in
// EqProcessor, which only the audio (or export) thread touches.
//==============================================================================

enum class EqType { lowCut, lowShelf, bell, highShelf, highCut };

struct EqBand
{
    EqType type = EqType::bell;
    std::atomic<bool> on { true };
    std::atomic<float> freq { 1000.0f }, gainDb { 0.0f }, q { 0.7f };

    bool isCut() const noexcept      { return type == EqType::lowCut || type == EqType::highCut; }
    bool hasGain() const noexcept    { return ! isCut(); }
    bool hasQ() const noexcept       { return type == EqType::bell; }
    /** True when the band changes the sound (on, and not a 0 dB boost/cut). */
    bool isActive() const noexcept   { return on.load() && (isCut() || std::abs (gainDb.load()) >= 0.05f); }
};

struct ChannelEq
{
    static constexpr int numBands = 6;
    static constexpr float minFreq = 20.0f, maxFreq = 20000.0f, maxGain = 18.0f;

    ChannelEq();

    std::atomic<bool> enabled { true };
    EqBand bands[numBands];

    /** Back to flat: cuts off, shelves and bells at 0 dB, default frequencies. */
    void reset();
    bool isFlat() const;
    void copyFrom (const ChannelEq&);

    /** Short name for a band ("Low cut", "Bell 1", ...). */
    static String bandName (int index);

    var toVar() const;
    void fromVar (const var&);

    /** Response of the whole EQ at a frequency, in dB, as the processor would apply it. */
    float responseDb (double freqHz, double sampleRate) const;
};

//==============================================================================
/** One second-order section (RBJ cookbook), transposed direct form II. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad make (EqType, double freq, double gainDb, double q, double sampleRate, int stage = 0);
    double magnitude (double freq, double sampleRate) const;
};

/** Audio-thread state for one ChannelEq (stereo). */
class EqProcessor
{
public:
    /** Processes channels 0 and 1 in place. Recomputes coefficients only when a setting moved. */
    void process (const ChannelEq&, AudioBuffer<float>&, int numSamples, double sampleRate);
    void resetState();

private:
    struct BandState
    {
        bool active = false;
        float freq = 0, gain = 0, q = 0;
        Biquad stage[2];          // cuts use two sections (24 dB/oct)
        int numStages = 0;
        double z[2][2][2] {};     // [stage][channel][delay]
    };

    BandState state[ChannelEq::numBands];
    double lastRate = 0.0;
};
} // namespace lathe
