// Dsp.h — small building blocks for the reverb's reverbs: filters, delay lines,
// smoothers, envelope followers, noise and soft clipping. No JUCE, and no
// allocation after prepare().
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lathe::rvb
{
inline constexpr float kPi    = 3.14159265358979f;
inline constexpr float kTwoPi = 6.28318530717959f;

inline float clampf (float x, float lo, float hi) { return std::min (hi, std::max (lo, x)); }
inline float lerpf (float a, float b, float t)     { return a + (b - a) * t; }
inline float dbToGain (float db)                   { return db <= -99.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)                    { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
// x in 0..1 mapped exponentially from lo to hi (both > 0).
inline float expMap (float x, float lo, float hi)  { return lo * std::pow (hi / lo, clampf (x, 0.0f, 1.0f)); }

// Gain per pass for a loop `loopSamples` long that falls 60 dB in `rt60` seconds.
inline float decayGain (float loopSamples, float rt60, float sr)
{
    return std::pow (10.0f, -3.0f * loopSamples / (std::max (0.01f, rt60) * sr));
}

// Padé tanh: monotonic and saturating at +-1.
inline float fastTanh (float x)
{
    x = clampf (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Leaves the signal alone below `knee`, then bends it smoothly towards `ceiling`.
inline float softLimit (float x, float knee = 0.8f, float ceiling = 1.0f)
{
    const float a = std::fabs (x);
    if (a <= knee)
        return x;
    const float room = ceiling - knee;
    const float y = knee + room * fastTanh ((a - knee) / room);
    return x < 0.0f ? -y : y;
}

// ---------------------------------------------------------------------------
// Noise and wandering control signals
// ---------------------------------------------------------------------------
struct Rng
{
    uint32_t s = 0x9e3779b9u;
    explicit Rng (uint32_t seed = 1) : s (seed * 2654435761u + 1u) { if (s == 0) s = 1; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni()     { return (float) (next() >> 8) * (1.0f / 16777216.0f); }   // 0..1
    float bi()      { return uni() * 2.0f - 1.0f; }                              // -1..1
};

// Random values joined by smooth curves.
struct SmoothRandom
{
    float phase = 1.0f, a = 0.0f, b = 0.0f;
    float tick (float hz, float sr, Rng& rng)
    {
        phase += hz / sr;
        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);
            a = b;
            b = rng.bi();
        }
        const float t = phase * phase * (3.0f - 2.0f * phase);
        return a + (b - a) * t;
    }
    void reset() { phase = 1.0f; a = b = 0.0f; }
};

// Sine and cosine together from a rotating phasor; cheap per-sample LFO.
struct QuadLfo
{
    float phase = 0.0f;
    float tick (float hz, float sr)
    {
        phase += hz / sr;
        phase -= std::floor (phase);
        return std::sin (kTwoPi * phase);
    }
};

// ---------------------------------------------------------------------------
// Smoothing and envelopes
// ---------------------------------------------------------------------------
struct Smoother
{
    float value = 0.0f, target = 0.0f, coeff = 0.01f;
    void setTime (float ms, float sr) { coeff = 1.0f - std::exp (-1.0f / std::max (1.0f, ms * 0.001f * sr)); }
    void snap (float v)               { value = target = v; }
    float next()                      { value += coeff * (target - value); return value; }
    // Advance n samples at once (for control-rate smoothing).
    float advance (int n)             { value += (1.0f - std::pow (1.0f - coeff, (float) n)) * (target - value); return value; }
};

struct EnvFollower
{
    float env = 0.0f, att = 0.1f, rel = 0.001f;
    void set (float attackMs, float releaseMs, float sr)
    {
        att = 1.0f - std::exp (-1.0f / std::max (1.0f, attackMs * 0.001f * sr));
        rel = 1.0f - std::exp (-1.0f / std::max (1.0f, releaseMs * 0.001f * sr));
    }
    float tick (float x)
    {
        const float a = std::fabs (x);
        env += (a > env ? att : rel) * (a - env);
        return env;
    }
    void reset() { env = 0.0f; }
};

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------
struct OnePole
{
    float a = 1.0f, z = 0.0f;
    void setCutoff (float hz, float sr) { a = 1.0f - std::exp (-kTwoPi * clampf (hz, 1.0f, sr * 0.49f) / sr); }
    float lp (float x) { z += a * (x - z); return z; }
    float hp (float x) { return x - lp (x); }
    void reset() { z = 0.0f; }
};

// Jot's absorptive filter for a feedback loop: gain `g` at DC falling to
// g * ratio-of-decay at Nyquist. y = g(1-b) x + b y[-1].
struct Absorb
{
    float b = 0.0f, gain = 1.0f, z = 0.0f;
    // `g` is the loop gain at mid frequencies; `hfRatio` = RT(high) / RT(mid), 0.05..1.
    void set (float g, float hfRatio)
    {
        hfRatio = clampf (hfRatio, 0.03f, 1.0f);
        const float bb = 0.5756463f * std::log10 (std::max (1.0e-6f, g)) * (1.0f - 1.0f / (hfRatio * hfRatio));
        b = clampf (bb, 0.0f, 0.995f);
        gain = g * (1.0f - b);
    }
    float tick (float x) { z = gain * x + b * z; return z; }
    void reset() { z = 0.0f; }
};

// Topology-preserving state-variable filter (Zavalishin).
struct Svf
{
    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;

    void set (float hz, float q, float sr)
    {
        hz = clampf (hz, 5.0f, sr * 0.45f);
        g  = std::tan (kPi * hz / sr);
        k  = 1.0f / std::max (0.05f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick (float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    void reset() { ic1 = ic2 = lp = bp = hp = 0.0f; }
};

// Biquad, transposed direct form II, RBJ designs.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0.0f; }
    float tick (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0);
        a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
    }
    void flat() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void lowpass (double fc, double q, double sr)
    {
        fc = std::min (fc, sr * 0.48);
        const double w = 2.0 * 3.141592653589793 * fc / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set ((1 - c) * 0.5, 1 - c, (1 - c) * 0.5, 1 + al, -2 * c, 1 - al);
    }
    void highpass (double fc, double q, double sr)
    {
        fc = std::min (fc, sr * 0.48);
        const double w = 2.0 * 3.141592653589793 * fc / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set ((1 + c) * 0.5, -(1 + c), (1 + c) * 0.5, 1 + al, -2 * c, 1 - al);
    }
    void bandpass (double fc, double q, double sr)   // 0 dB at the centre
    {
        fc = std::min (fc, sr * 0.48);
        const double w = 2.0 * 3.141592653589793 * fc / sr, al = std::sin (w) / (2.0 * q);
        set (al, 0, -al, 1 + al, -2 * std::cos (w), 1 - al);
    }
    void peak (double fc, double q, double db, double sr)
    {
        fc = std::min (fc, sr * 0.48);
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * 3.141592653589793 * fc / sr;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf (double fc, double db, double sr)
    {
        fc = std::min (fc, sr * 0.48);
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * 3.141592653589793 * fc / sr;
        const double c = std::cos (w), al = std::sin (w) / 2.0 * std::sqrt (2.0), sa = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
             (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
};

struct DcBlocker
{
    float x1 = 0.0f, y1 = 0.0f, r = 0.995f;
    void prepare (float sr) { r = 1.0f - kTwoPi * 10.0f / sr; }
    float tick (float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0.0f; }
};

struct Allpass1
{
    float z = 0.0f;
    float tick (float x, float a) { const float y = a * x + z; z = x - a * y; return y; }
    void reset() { z = 0.0f; }
};

// ---------------------------------------------------------------------------
// Delay line with a power-of-two buffer.
// ---------------------------------------------------------------------------
class DelayLine
{
public:
    void prepare (int maxSamples)
    {
        int size = 16;
        while (size < maxSamples + 8)
            size <<= 1;
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        w = 0;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    void push (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
    int  size() const noexcept { return mask + 1; }
    int  maxDelay() const noexcept { return mask - 4; }

    // Hermite interpolation, `delay` samples behind the newest write (>= 1).
    float read (float delay) const
    {
        delay = clampf (delay, 1.0f, (float) (mask - 4));
        const float pos = (float) w - delay;
        const int   i   = (int) std::floor (pos);
        const float f   = pos - (float) i;
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    // Linear interpolation: cheaper, for taps that barely move.
    float readLinear (float delay) const
    {
        delay = clampf (delay, 1.0f, (float) (mask - 4));
        const float pos = (float) w - delay;
        const int   i   = (int) std::floor (pos);
        const float f   = pos - (float) i;
        return at (i) + f * (at (i + 1) - at (i));
    }
    float readInt (int delay) const { return at (w - std::max (1, std::min (delay, mask - 4))); }

private:
    float at (int i) const { return buf[(size_t) (i & mask)]; }
    std::vector<float> buf { std::vector<float> (16, 0.0f) };
    int mask = 15, w = 0;
};

// A Schroeder allpass around a delay line: the building block of diffusers.
struct AllpassDelay
{
    DelayLine d;
    void prepare (int maxSamples) { d.prepare (maxSamples); }
    void reset() { d.reset(); }
    float tick (float x, float delay, float g)
    {
        const float v = d.read (delay);
        const float u = x + g * v;
        d.push (u);
        return v - g * u;
    }
    float tickInt (float x, int delay, float g)
    {
        const float v = d.readInt (delay);
        const float u = x + g * v;
        d.push (u);
        return v - g * u;
    }
};

// Feed-forward compressor gain computer with a soft knee.
struct Compressor
{
    EnvFollower env;
    float thresholdDb = -20.0f, ratio = 4.0f, kneeDb = 6.0f;
    void prepare (float attackMs, float releaseMs, float sr) { env.set (attackMs, releaseMs, sr); }
    float gain (float detector)
    {
        const float lvl = gainToDb (env.tick (detector));
        const float over = lvl - thresholdDb;
        float red = 0.0f;
        if (over > kneeDb * 0.5f)
            red = over * (1.0f - 1.0f / ratio);
        else if (over > -kneeDb * 0.5f)
        {
            const float t = over + kneeDb * 0.5f;
            red = (1.0f - 1.0f / ratio) * t * t / (2.0f * kneeDb);
        }
        return dbToGain (-red);
    }
    void reset() { env.reset(); }
};

// In-place fast Walsh-Hadamard transform of N (power of two) values,
// normalised to keep energy.
template <size_t N>
inline void hadamard (std::array<float, N>& v)
{
    for (size_t h = 1; h < N; h <<= 1)
        for (size_t i = 0; i < N; i += h << 1)
            for (size_t j = i; j < i + h; ++j)
            {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    const float s = 1.0f / std::sqrt ((float) N);
    for (auto& x : v) x *= s;
}
} // namespace lathe::rvb
