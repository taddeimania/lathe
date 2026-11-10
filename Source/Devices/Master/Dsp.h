// Dsp.h — small building blocks for the mastering engine: biquads, smoothers, a fixed
// delay and a 4x interpolator for true-peak detection. No JUCE, no
// allocation after prepare().
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace lathe::mst
{
constexpr double kPi = 3.14159265358979323846;

inline float clampf (float x, float lo, float hi) { return std::min (hi, std::max (lo, x)); }
inline float dbToGain (float db)                  { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)                   { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// ---------------------------------------------------------------------------
// Biquad, transposed direct form II with double state so low shelves stay
// clean at high sample rates. Coefficients from the RBJ cookbook.
// ---------------------------------------------------------------------------
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    float tick (float xf)
    {
        const double x = xf;
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
    void reset() { z1 = z2 = 0; }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
    }

    void flat() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void highPass (double hz, double q, double sr)
    {
        const double w = 2 * kPi * std::min (hz, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void peak (double hz, double q, double db, double sr)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2 * kPi * std::min (hz, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    // Shelves with slope S = 1 written as Q = 1/sqrt(2).
    void lowShelf (double hz, double db, double sr, double q = 0.7071)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2 * kPi * std::min (hz, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        const double s = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
             (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }

    void highShelf (double hz, double db, double sr, double q = 0.7071)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2 * kPi * std::min (hz, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        const double s = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
             (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }

    // Magnitude in dB at `hz`, for drawing and tests.
    double responseDb (double hz, double sr) const
    {
        const double w = 2 * kPi * hz / sr;
        const double cr = std::cos (w), ci = -std::sin (w), c2r = std::cos (2 * w), c2i = -std::sin (2 * w);
        const double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        const double dr = 1 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return 10 * std::log10 ((nr * nr + ni * ni) / std::max (1e-30, dr * dr + di * di));
    }
};

// ---------------------------------------------------------------------------
// One-pole smoother.
// ---------------------------------------------------------------------------
struct Smoother
{
    float value = 0.0f, target = 0.0f, coeff = 0.01f;
    void setTime (float ms, float sr, int step = 1)
    {
        coeff = 1.0f - std::exp (-(float) step / std::max (1.0f, ms * 0.001f * sr));
    }
    void snap (float v) { value = target = v; }
    float next()        { value += coeff * (target - value); return value; }
};

// ---------------------------------------------------------------------------
// Fixed integer delay.
// ---------------------------------------------------------------------------
class FixedDelay
{
public:
    void prepare (int delaySamples)
    {
        delay = std::max (0, delaySamples);
        buf.assign ((size_t) delay + 1, 0.0f);
        w = 0;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    float tick (float x)
    {
        if (delay == 0)
            return x;
        buf[(size_t) w] = x;
        w = (w + 1) % (int) buf.size();
        return buf[(size_t) w];   // the oldest sample: `delay` behind
    }
    int length() const noexcept { return delay; }

private:
    std::vector<float> buf;
    int delay = 0, w = 0;
};

// ---------------------------------------------------------------------------
// 4x interpolator for true-peak detection (ITU-R BS.1770 annex 2 style):
// a 49-tap Kaiser-windowed sinc, run as four polyphase branches. Its group
// delay is exactly 6 input samples, so the four outputs for input n are the
// signal at n-6, n-5.75, n-5.5 and n-5.25.
// ---------------------------------------------------------------------------
class TruePeak4x
{
public:
    static constexpr int kTaps = 49;
    static constexpr int kHist = 13;     // ceil(49 / 4)
    static constexpr int kDelay = 6;

    TruePeak4x() { design(); }

    void reset() { std::fill (std::begin (hist), std::end (hist), 0.0f); pos = 0; }

    // Pushes one sample and returns the largest absolute value of the four
    // interpolated points it completes.
    float push (float x)
    {
        // The history is stored twice over so a window never wraps.
        pos = (pos + kHist - 1) % kHist;
        hist[pos] = hist[pos + kHist] = x;
        const float* hx = hist + pos;   // hx[k] = x[n - k]
        float m = 0.0f;
        for (int p = 0; p < 4; ++p)
        {
            float acc = 0.0f;
            const float* h = phase[p];
            for (int k = 0; k < kHist; ++k)
                acc += h[k] * hx[k];
            m = std::max (m, std::fabs (acc));
        }
        return m;
    }

private:
    void design()
    {
        // Cutoff a little under the original Nyquist; beta 7 gives ~70 dB.
        const double beta = 7.0, fc = 0.5 * 0.9 / 4.0;   // cycles per upsampled sample
        auto bessel0 = [] (double x)
        {
            double sum = 1, term = 1;
            for (int k = 1; k < 30; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
            return sum;
        };
        double h[kTaps];
        const double mid = (kTaps - 1) / 2.0;
        for (int n = 0; n < kTaps; ++n)
        {
            const double t = n - mid;
            const double sinc = 2 * n == kTaps - 1 ? 2 * fc : std::sin (2 * kPi * fc * t) / (kPi * t);
            const double r = t / mid;
            const double win = bessel0 (beta * std::sqrt (std::max (0.0, 1 - r * r))) / bessel0 (beta);
            h[n] = sinc * win;
        }
        // Each branch sums to one so DC passes at unity.
        for (int p = 0; p < 4; ++p)
        {
            double s = 0;
            for (int k = 0; k < kHist; ++k)
                if (p + 4 * k < kTaps) s += h[p + 4 * k];
            for (int k = 0; k < kHist; ++k)
                phase[p][k] = (p + 4 * k < kTaps) ? (float) (h[p + 4 * k] / s) : 0.0f;
        }
    }

    float phase[4][kHist] {};
    float hist[2 * kHist] {};
    int pos = 0;
};
} // namespace lathe::mst
