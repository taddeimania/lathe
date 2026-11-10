// Nonlinear.cpp — the NONLINEAR machine: a dense bank of taps spread over
// TIME whose levels follow a SHAPE (rising, flat, bell...) instead of a
// natural decay, with feedback around it and a normal tail behind it.
#include "Machines.h"

namespace lathe::rvb
{
namespace
{
constexpr float kMaxSeconds = 2.1f;
}

float NonlinearMachine::shape (int s, float x)
{
    x = clampf (x, 0.0f, 1.0f);
    float e = 1.0f;
    switch (s)
    {
        case 0: e = std::sqrt (x) * (1.0f - 0.3f * x); break;                       // SWOOSH: fast rise, then eases
        case 1: e = std::exp (4.6f * (x - 1.0f)); break;                            // REVERSE: -40 dB up to full
        case 2: e = 0.08f + 0.92f * x; break;                                       // RAMP
        case 3: e = 1.0f; break;                                                    // GATE
        case 4: { const float u = (x - 0.5f) / 0.2f; e = std::exp (-u * u); break; } // GAUSS
        case 5: { const float u = (x - 0.5f) / 0.22f; e = 1.0f - 0.88f * std::exp (-u * u); break; } // BOUNCE
        default: break;
    }
    // Soft edges so a flat shape doesn't click.
    const float in = clampf (x / 0.03f, 0.0f, 1.0f), out = clampf ((1.0f - x) / 0.04f, 0.0f, 1.0f);
    return e * in * out;
}

void NonlinearMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    line.prepare ((int) (kMaxSeconds * sr) + 64);
    diff[0].prepare (sr, 40.0f, 61);
    diff[1].prepare (sr, 40.0f, 67);
    Rng r (91);
    for (int k = 0; k < kTaps; ++k)
        tapJitter[(size_t) k] = r.bi();
    late.prepare (sr, 260.0f);
    reset();
}

void NonlinearMachine::reset()
{
    line.reset();
    for (auto& d : diff) d.reset();
    for (auto& f : lp) f.reset();
    late.reset();
    last.fill (0.0f);
    first = true;
}

void NonlinearMachine::set (const Controls& c, int blockSamples)
{
    const float timeMs = expMap (c.a, 40.0f, 2000.0f);
    const float T = std::min (timeMs * 0.001f * sr, kMaxSeconds * sr - 0.01f * sr);
    const int s = std::max (0, std::min (5, c.mode));

    float energy = 0.0f;
    Rng signs (17);
    for (int k = 0; k < kTaps; ++k)
    {
        const size_t i = (size_t) k;
        const float pos = ((float) k + 0.5f + 0.42f * tapJitter[i]) / (float) kTaps;
        tapTarget[i] = std::max (2.0f, T * pos);
        const float e = shape (s, pos) * (signs.uni() < 0.5f ? -1.0f : 1.0f);
        const float p = ((k & 1) ? 0.75f : -0.75f) * (0.6f + 0.4f * std::fabs (tapJitter[i]));
        gainL[i] = e * std::sqrt (0.5f * (1.0f - p));
        gainR[i] = e * std::sqrt (0.5f * (1.0f + p));
        energy += e * e;
    }
    const float norm = 1.0f / std::sqrt (std::max (1.0e-3f, energy));
    for (int k = 0; k < kTaps; ++k)
    {
        gainL[(size_t) k] *= norm * 1.25f;
        gainR[(size_t) k] *= norm * 1.25f;
    }
    // Glide the taps when TIME moves.
    const float glide = first ? 1.0f : 1.0f - std::exp (-(float) blockSamples / (0.08f * sr));
    for (int k = 0; k < kTaps; ++k)
        tapTime[(size_t) k] += glide * (tapTarget[(size_t) k] - tapTime[(size_t) k]);
    first = false;

    feedback = c.infinite ? 0.79f : c.b * 0.74f;   // the taps sum to about 1.25x
    lateGain = c.c * 1.4f;
    depth = c.modDepth * c.modDepth * 0.002f * sr;
    const float g = 0.72f * c.diffusion;
    const float dl = 3.0f + 9.0f * std::min (1.0f, timeMs / 600.0f);
    diff[0].set (c.diffusion < 0.02f ? 0 : 6, dl, g);
    diff[1].set (c.diffusion < 0.02f ? 0 : 6, dl * 1.07f, g);
    for (auto& f : lp) f.setCutoff (std::min (18000.0f, c.toneHz * 1.3f), sr);

    Tank::Params p;
    p.minMs = 24.0f * c.sizeScale;
    p.maxMs = 105.0f * c.sizeScale;
    p.rt60 = c.decay;
    p.lowMult = c.lowMult;
    p.hfRatio = c.hfRatio;
    p.hfHz = c.dampHz;
    p.modDepthMs = c.modDepth * c.modDepth * 1.5f;
    p.modRateHz = c.modRate;
    p.infinite = c.infinite;
    late.set (p, blockSamples);
    levelTrim = 1.0f;
}

void NonlinearMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const float fb = softLimit (feedback * 0.5f * (last[0] + last[1]), 0.8f, 1.5f);
        const float x = 0.5f * (inL[i] + inR[i]) + fb;
        line.push (diff[0].tick (diff[1].tick (x)));

        const float m = depth > 0.0f ? depth * (1.0f + lfo.tick (0.7f, sr)) * 0.5f : 0.0f;
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < kTaps; ++k)
        {
            const size_t t = (size_t) k;
            const float off = m * (float) ((k % 5) - 2) * 0.5f;
            const float v = line.readLinear (tapTime[t] + off);
            l += gainL[t] * v;
            r += gainR[t] * v;
        }
        l = lp[0].lp (l);
        r = lp[1].lp (r);
        last[0] = l;
        last[1] = r;

        float tl = 0.0f, tr = 0.0f;
        late.tick (l, r, tl, tr);
        outL[i] = l * levelTrim + tl * lateGain;
        outR[i] = r * levelTrim + tr * lateGain;
    }
}
} // namespace lathe::rvb
