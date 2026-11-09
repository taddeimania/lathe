#include "ChannelEq.h"
#include <complex>

namespace lathe
{
namespace
{
struct BandDefault { EqType type; bool on; float freq, q; };

constexpr BandDefault kDefaults[ChannelEq::numBands] = {
    { EqType::lowCut,    false,    80.0f, 0.7f },
    { EqType::lowShelf,  true,    120.0f, 0.7f },
    { EqType::bell,      true,    400.0f, 1.0f },
    { EqType::bell,      true,   2500.0f, 1.0f },
    { EqType::highShelf, true,   8000.0f, 0.7f },
    { EqType::highCut,   false, 16000.0f, 0.7f },
};

// Q per section for a 4th-order Butterworth made of two biquads.
constexpr double kCutQ[2] = { 0.54119610, 1.30656296 };
} // namespace

//==============================================================================
ChannelEq::ChannelEq()
{
    for (int i = 0; i < numBands; ++i)
        bands[i].type = kDefaults[i].type;
    reset();
}

void ChannelEq::reset()
{
    enabled = true;
    for (int i = 0; i < numBands; ++i)
    {
        bands[i].on = kDefaults[i].on;
        bands[i].freq = kDefaults[i].freq;
        bands[i].gainDb = 0.0f;
        bands[i].q = kDefaults[i].q;
    }
}

bool ChannelEq::isFlat() const
{
    for (auto& b : bands)
        if (b.isActive())
            return false;
    return true;
}

void ChannelEq::copyFrom (const ChannelEq& other)
{
    enabled = other.enabled.load();
    for (int i = 0; i < numBands; ++i)
    {
        bands[i].on = other.bands[i].on.load();
        bands[i].freq = other.bands[i].freq.load();
        bands[i].gainDb = other.bands[i].gainDb.load();
        bands[i].q = other.bands[i].q.load();
    }
}

String ChannelEq::bandName (int index)
{
    static const char* names[] = { "Low cut", "Low shelf", "Bell 1", "Bell 2", "High shelf", "High cut" };
    return isPositiveAndBelow (index, numBands) ? String (names[index]) : String();
}

var ChannelEq::toVar() const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("on", enabled.load());
    Array<var> list;
    for (auto& b : bands)
    {
        DynamicObject::Ptr bo = new DynamicObject();
        bo->setProperty ("on", b.on.load());
        bo->setProperty ("freq", b.freq.load());
        bo->setProperty ("gain", b.gainDb.load());
        bo->setProperty ("q", b.q.load());
        list.add (var (bo.get()));
    }
    o->setProperty ("bands", list);
    return var (o.get());
}

void ChannelEq::fromVar (const var& v)
{
    reset();
    if (! v.isObject())
        return;

    enabled = v.hasProperty ("on") ? (bool) v["on"] : true;
    const var list = v["bands"];
    if (auto* arr = list.getArray())
    {
        for (int i = 0; i < numBands && i < arr->size(); ++i)
        {
            const var& b = arr->getReference (i);
            bands[i].on = (bool) b["on"];
            bands[i].freq = jlimit (minFreq, maxFreq, (float) (double) b["freq"]);
            bands[i].gainDb = jlimit (-maxGain, maxGain, (float) (double) b["gain"]);
            bands[i].q = jlimit (0.1f, 18.0f, b.hasProperty ("q") ? (float) (double) b["q"] : kDefaults[i].q);
        }
    }
}

float ChannelEq::responseDb (double freqHz, double sampleRate) const
{
    if (! enabled.load())
        return 0.0f;

    double mag = 1.0;
    for (auto& b : bands)
    {
        if (! b.isActive())
            continue;

        const int stages = b.isCut() ? 2 : 1;
        for (int s = 0; s < stages; ++s)
            mag *= Biquad::make (b.type, b.freq.load(), b.gainDb.load(), b.q.load(), sampleRate, s).magnitude (freqHz, sampleRate);
    }
    return (float) Decibels::gainToDecibels (mag, -120.0);
}

//==============================================================================
Biquad Biquad::make (EqType type, double freq, double gainDb, double q, double sr, int stage)
{
    freq = jlimit (10.0, sr * 0.49, freq);
    if (type == EqType::lowCut || type == EqType::highCut)
        q = kCutQ[jlimit (0, 1, stage)];
    else if (type != EqType::bell)
        q = 0.7071;
    q = jmax (0.05, q);

    const double w0 = MathConstants<double>::twoPi * freq / sr;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * q);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double sqA2alpha = 2.0 * std::sqrt (A) * alpha;

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type)
    {
        case EqType::lowCut:
            b0 = (1 + cw) / 2;  b1 = -(1 + cw);  b2 = (1 + cw) / 2;
            a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
            break;
        case EqType::highCut:
            b0 = (1 - cw) / 2;  b1 = 1 - cw;     b2 = (1 - cw) / 2;
            a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
            break;
        case EqType::bell:
            b0 = 1 + alpha * A; b1 = -2 * cw;    b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw;    a2 = 1 - alpha / A;
            break;
        case EqType::lowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sqA2alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sqA2alpha);
            a0 = (A + 1) + (A - 1) * cw + sqA2alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sqA2alpha;
            break;
        case EqType::highShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sqA2alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sqA2alpha);
            a0 = (A + 1) - (A - 1) * cw + sqA2alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sqA2alpha;
            break;
    }

    Biquad bq;
    bq.b0 = b0 / a0;  bq.b1 = b1 / a0;  bq.b2 = b2 / a0;
    bq.a1 = a1 / a0;  bq.a2 = a2 / a0;
    return bq;
}

double Biquad::magnitude (double freq, double sr) const
{
    const double w = MathConstants<double>::twoPi * freq / sr;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    const auto num = b0 + b1 * z1 + b2 * z2;
    const auto den = 1.0 + a1 * z1 + a2 * z2;
    return std::abs (num / den);
}

//==============================================================================
void EqProcessor::resetState()
{
    for (auto& s : state)
    {
        s.active = false;
        std::memset (s.z, 0, sizeof (s.z));
    }
}

void EqProcessor::process (const ChannelEq& eq, AudioBuffer<float>& buffer, int n, double sr)
{
    if (! eq.enabled.load() || sr <= 0.0 || buffer.getNumChannels() < 2)
    {
        if (lastRate != 0.0)
        {
            resetState();
            lastRate = 0.0;
        }
        return;
    }

    if (! exactlyEqual (sr, lastRate))
    {
        resetState();
        lastRate = sr;
    }

    for (int i = 0; i < ChannelEq::numBands; ++i)
    {
        auto& band = eq.bands[i];
        auto& st = state[i];

        if (! band.isActive())
        {
            if (st.active)
            {
                st.active = false;
                std::memset (st.z, 0, sizeof (st.z));
            }
            continue;
        }

        const float f = band.freq.load(), g = band.gainDb.load(), q = band.q.load();
        if (! st.active || ! exactlyEqual (f, st.freq) || ! exactlyEqual (g, st.gain) || ! exactlyEqual (q, st.q))
        {
            st.numStages = band.isCut() ? 2 : 1;
            for (int s = 0; s < st.numStages; ++s)
                st.stage[s] = Biquad::make (band.type, f, g, q, sr, s);
            st.freq = f;  st.gain = g;  st.q = q;
            st.active = true;
        }

        for (int s = 0; s < st.numStages; ++s)
        {
            const auto& c = st.stage[s];
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                double z1 = st.z[s][ch][0], z2 = st.z[s][ch][1];
                for (int k = 0; k < n; ++k)
                {
                    const double x = d[k];
                    const double y = c.b0 * x + z1;
                    z1 = c.b1 * x - c.a1 * y + z2;
                    z2 = c.b2 * x - c.a2 * y;
                    d[k] = (float) y;
                }
                // Flush denormals that survive on silent input.
                st.z[s][ch][0] = std::abs (z1) < 1.0e-20 ? 0.0 : z1;
                st.z[s][ch][1] = std::abs (z2) < 1.0e-20 ? 0.0 : z2;
            }
        }
    }
}
} // namespace lathe
