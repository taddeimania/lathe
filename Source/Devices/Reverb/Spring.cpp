// Spring.cpp — the SPRING machine. Each spring is a loop: a long chain of
// stretched allpass sections (which delay low frequencies more than high
// ones, giving the chirp and "boing" of a real spring), a delay for the
// spring's length, and the losses of the tank.
#include "Machines.h"

namespace lathe::rvb
{
namespace
{
constexpr float kLenMs[3] { 33.7f, 41.3f, 52.9f };
constexpr float kChirpHz = 4300.0f;
} // namespace

void SpringMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    stretch = std::max (1, std::min (kMaxStretch, (int) std::lround (sr / (2.0f * kChirpHz))));
    for (int i = 0; i < 3; ++i)
    {
        auto& s = springs[(size_t) i];
        s.loop.prepare ((int) (kLenMs[i] * 1.8f * 0.001f * sr) + 16);
        s.smear.prepare ((int) (0.02f * sr));
        s.smearLen = (5.1f + 1.7f * (float) i) * 0.001f * sr;
    }
    outLp.setCutoff (7000.0f, sr);
    reset();
}

void SpringMachine::reset()
{
    for (auto& s : springs)
    {
        for (auto& h : s.xh) h.fill (0.0f);
        for (auto& h : s.yh) h.fill (0.0f);
        s.pos = 0;
        s.loop.reset();
        s.lp.reset();
        s.hp.reset();
        s.smear.reset();
        s.fb = 0.0f;
    }
    outLp.reset();
}

void SpringMachine::set (const Controls& c, int)
{
    infinite = c.infinite;
    count = 1 + std::max (0, std::min (2, c.mode));
    static const float pans[3][3] { { 0.0f, 0.0f, 0.0f }, { -0.6f, 0.6f, 0.0f }, { -0.8f, 0.0f, 0.8f } };

    dwell = c.a;
    dwellNorm = 1.0f + 14.0f * dwell * dwell;
    a = -(0.5f + 0.24f * c.b);   // negative: lows arrive later, the falling "pew" of a real spring
    stages = 32 + (int) std::lround (32.0f * c.diffusion);
    smearG = 0.15f + 0.45f * c.diffusion;

    const float lenScale = clampf (0.6f + 0.4f * c.sizeScale, 0.7f, 1.4f);
    const float lpHz = infinite ? 16000.0f : clampf (c.toneHz * 0.7f, 2000.0f, 9000.0f);
    const float hpHz = infinite ? 15.0f : clampf (140.0f * std::pow (2.0f, -c.lowEnd / 5.0f), 30.0f, 600.0f);
    float energy = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        auto& s = springs[(size_t) i];
        s.len = kLenMs[i] * lenScale * 0.001f * sr;
        s.g = infinite ? 1.0f : decayGain (s.len, c.decay * 1.8f, sr);   // the loop filters take their share
        s.lp.setCutoff (lpHz, sr);
        s.hp.setCutoff (hpHz, sr);
        s.pan = pans[count - 1][i];
        energy += 1.0f / std::max (1.0e-3f, 1.0f - s.g * s.g);
    }
    outLp.setCutoff (std::min (12000.0f, c.toneHz), sr);
    levelTrim = 1.1f / std::sqrt (energy / 3.0f) * std::sqrt (3.0f / (float) count) * 2.2f
              * clampf (std::pow (std::max (0.1f, c.decay) / 2.0f, 0.18f), 0.7f, 1.6f);
}

float SpringMachine::disperse (Spring& s, float x)
{
    const int p = s.pos;
    for (int j = 0; j < stages; ++j)
    {
        auto& xh = s.xh[(size_t) j];
        auto& yh = s.yh[(size_t) j];
        const float y = a * x + xh[(size_t) p] - a * yh[(size_t) p];
        xh[(size_t) p] = x;
        yh[(size_t) p] = y;
        x = y;
    }
    s.pos = (p + 1) % stretch;
    return x;
}

void SpringMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float x = 0.5f * (inL[i] + inR[i]);
        if (dwell > 0.001f)
        {
            // The tank's driver: a little asymmetry for the even harmonics of a tube.
            const float bias = 0.12f * dwell;
            x = (fastTanh (x * dwellNorm + bias) - fastTanh (bias)) / dwellNorm * (1.0f + 1.2f * dwell);
        }
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < count; ++k)
        {
            auto& s = springs[(size_t) k];
            const float d = disperse (s, x + s.fb);
            s.loop.push (d);
            const float y = s.loop.read (s.len);
            s.fb = softLimit (s.g * s.hp.hp (s.lp.lp (y)), 1.2f, 2.5f);
            const float o = s.smear.tick (y, s.smearLen, smearG);
            const float pl = std::sqrt (0.5f * (1.0f - s.pan)), pr = std::sqrt (0.5f * (1.0f + s.pan));
            l += o * pl;
            r += o * pr;
        }
        outL[i] = l * levelTrim;
        outR[i] = r * levelTrim;
    }
}
} // namespace lathe::rvb
