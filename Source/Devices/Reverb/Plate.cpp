// Plate.cpp — the PLATE machine: four input diffusers into a figure-eight
// tank of two allpass-delay loops, after the well-known plate topology
// published by Jon Dattorro (1997). Lengths are given at 29761 Hz and scaled.
#include "Machines.h"

namespace lathe::rvb
{
namespace
{
constexpr float kRef = 29761.0f;
constexpr float kInLen[4] { 142.0f, 107.0f, 379.0f, 277.0f };
constexpr float kModAp[2] { 672.0f, 908.0f };
constexpr float kDecayAp[2] { 1800.0f, 2656.0f };
constexpr float kDelay[4] { 4453.0f, 3720.0f, 4217.0f, 3163.0f };   // L1, L2, R1, R2
constexpr float kExcursion = 16.0f;
constexpr float kMaxScale = 1.6f;
} // namespace

void PlateMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    const float f = sr / kRef * kMaxScale;
    for (int i = 0; i < 4; ++i)
        inDiff[(size_t) i].prepare ((int) (kInLen[i] * f) + 8);
    for (int i = 0; i < 2; ++i)
    {
        modAp[(size_t) i].prepare ((int) ((kModAp[i] + kExcursion * 3.0f) * f) + 8);
        decayAp[(size_t) i].prepare ((int) (kDecayAp[i] * f) + 8);
        lowSplit[(size_t) i].setCutoff (200.0f, sr);
    }
    for (int i = 0; i < 4; ++i)
        delays[(size_t) i].prepare ((int) (kDelay[i] * f) + 8);
    lfoR.phase = 0.25f;
    reset();
}

void PlateMachine::reset()
{
    for (auto& a : inDiff) a.reset();
    for (auto& a : modAp) a.reset();
    for (auto& a : decayAp) a.reset();
    for (auto& d : delays) d.reset();
    for (auto& d : damp) d.reset();
    for (auto& d : lowSplit) d.reset();
    bandwidth.reset();
    feedL = feedR = 0.0f;
    scale = scaleTarget;
}

void PlateMachine::set (const Controls& c, int blockSamples)
{
    infinite = c.infinite;
    const float base = c.mode == 1 ? 0.56f : 1.0f;   // SMALL or LARGE
    scaleTarget = base * clampf (0.55f + 0.45f * c.sizeScale, 0.6f, 1.45f);
    scale += (1.0f - std::exp (-(float) blockSamples / (0.3f * sr))) * (scaleTarget - scale);

    const float k = sr / kRef * scale;
    const float seg = 5322.0f * k;   // average stretch between decay multipliers
    decay = infinite ? 1.0f : decayGain (seg, c.decay, sr);
    const float gLow = infinite ? 1.0f : decayGain (seg, std::min (90.0f, c.decay * c.lowMult), sr);
    lowExtra = gLow - decay;

    const float dampHz = infinite ? 20000.0f : clampf (c.toneHz * (c.mode == 1 ? 1.25f : 1.0f), 800.0f, 19000.0f);
    damp[0].setCutoff (dampHz, sr);
    damp[1].setCutoff (dampHz, sr);
    bandwidth.setCutoff (std::min (19000.0f, c.toneHz * 1.8f), sr);

    const float d = 0.25f + 0.75f * c.diffusion;
    inG1 = 0.75f * d;
    inG2 = 0.625f * d;
    decG = 0.3f + 0.25f * c.diffusion;

    depth = kExcursion * k * (0.1f + 1.6f * c.modDepth);
    rate = 0.35f + c.modRate;

    drive = c.a;
    driveNorm = 1.0f + 7.0f * drive;

    const float gbar = decayGain (seg * 2.0f, c.decay, sr);
    levelTrim = std::sqrt (std::max (1.0e-4f, 1.0f - gbar * gbar)) * 1.9f * (c.mode == 1 ? 1.15f : 1.0f)
              * clampf (std::pow (std::max (0.1f, c.decay) / 2.0f, 0.18f), 0.7f, 1.6f);
}

void PlateMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const float k = sr / kRef * scale;
    const float in1 = kInLen[0] * k, in2 = kInLen[1] * k, in3 = kInLen[2] * k, in4 = kInLen[3] * k;
    const float mL = kModAp[0] * k, mR = kModAp[1] * k;
    const float aL = kDecayAp[0] * k, aR = kDecayAp[1] * k;
    const float dL1 = kDelay[0] * k, dL2 = kDelay[1] * k, dR1 = kDelay[2] * k, dR2 = kDelay[3] * k;
    auto tap = [k] (const DelayLine& d, float at) { return d.readInt ((int) (at * k)); };

    for (int i = 0; i < n; ++i)
    {
        float x = 0.5f * (inL[i] + inR[i]);
        if (drive > 0.001f)
            x = fastTanh (x * driveNorm) / driveNorm * (1.0f + 0.6f * drive);
        x = bandwidth.lp (x);
        x = inDiff[0].tick (x, in1, inG1);
        x = inDiff[1].tick (x, in2, inG1);
        x = inDiff[2].tick (x, in3, inG2);
        x = inDiff[3].tick (x, in4, inG2);

        const float modL = depth * (1.0f + lfoL.tick (rate, sr));
        const float modR = depth * (1.0f + lfoR.tick (rate * 1.09f, sr));

        // Left half.
        float l = modAp[0].tick (x + feedR, mL + modL, -0.7f);
        delays[0].push (l);
        l = delays[0].read (dL1);
        {
            const float dmp = damp[0].lp (l);
            l = dmp * decay + lowSplit[0].lp (l) * lowExtra;
        }
        l = decayAp[0].tick (l, aL, decG);
        delays[1].push (l);
        const float endL = delays[1].read (dL2) * decay;

        // Right half.
        float r = modAp[1].tick (x + feedL, mR + modR, -0.7f);
        delays[2].push (r);
        r = delays[2].read (dR1);
        {
            const float dmp = damp[1].lp (r);
            r = dmp * decay + lowSplit[1].lp (r) * lowExtra;
        }
        r = decayAp[1].tick (r, aR, decG);
        delays[3].push (r);
        const float endR = delays[3].read (dR2) * decay;

        feedL = softLimit (endL, 1.5f, 3.0f);
        feedR = softLimit (endR, 1.5f, 3.0f);

        const auto& apL = decayAp[0].d;
        const auto& apR = decayAp[1].d;
        float yl = tap (delays[2], 266.0f) + tap (delays[2], 2974.0f) - tap (apR, 1913.0f) + tap (delays[3], 1996.0f)
                 - tap (delays[0], 1990.0f) - tap (apL, 187.0f) - tap (delays[1], 1066.0f);
        float yr = tap (delays[0], 353.0f) + tap (delays[0], 3627.0f) - tap (apL, 1228.0f) + tap (delays[1], 2673.0f)
                 - tap (delays[2], 2111.0f) - tap (apR, 335.0f) - tap (delays[3], 121.0f);
        outL[i] = yl * 0.6f * levelTrim;
        outR[i] = yr * 0.6f * levelTrim;
    }
}
} // namespace lathe::rvb
