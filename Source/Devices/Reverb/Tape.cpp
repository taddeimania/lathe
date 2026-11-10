// Tape.cpp — the TAPE machine: one tape loop read by three, four or six
// playback heads, with wow and flutter, feedback through the worn tone of the
// tape, diffusers that smear the repeats, and a small room behind.
#include "Machines.h"

namespace lathe::rvb
{
namespace
{
constexpr float kMaxSeconds = 1.5f;
// Head positions as fractions of TIME. EVEN spacing is computed.
constexpr float kScatter[3][6] {
    { 0.23f, 0.58f, 1.0f, 0, 0, 0 },
    { 0.17f, 0.45f, 0.68f, 1.0f, 0, 0 },
    { 0.11f, 0.29f, 0.38f, 0.57f, 0.81f, 1.0f } };
} // namespace

void TapeMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    tape.prepare ((int) (kMaxSeconds * 1.08f * sr) + 64);
    smear[0].prepare (sr, 40.0f, 101);
    smear[1].prepare (sr, 43.0f, 103);
    time.setTime (220.0f, sr);
    time.snap (0.4f * sr);
    flutter.phase = 0.3f;
    room.prepare (sr, 140.0f);
    reset();
}

void TapeMachine::reset()
{
    tape.reset();
    for (auto& s : smear) s.reset();
    fbLp.reset(); fbHp.reset(); outLpL.reset(); outLpR.reset();
    drift.reset();
    room.reset();
    last = 0.0f;
}

void TapeMachine::set (const Controls& c, int blockSamples)
{
    infinite = c.infinite;
    const float ms = expMap (c.a, 60.0f, 1500.0f);
    time.target = ms * 0.001f * sr;

    const int m = std::max (0, std::min (6, c.mode));
    static const int counts[] { 1, 3, 4, 6, 3, 4, 6 };
    heads = counts[m];
    scatter = m >= 4;
    for (int h = 0; h < heads; ++h)
        frac[(size_t) h] = scatter ? kScatter[heads == 3 ? 0 : heads == 4 ? 1 : 2][h] : (float) (h + 1) / (float) heads;

    feedback = infinite ? 1.0f : c.b * 1.1f;
    wowDepth = c.c;
    const float g = 0.7f * c.diffusion;
    const float len = 5.0f + 26.0f * c.diffusion;
    smear[0].set (c.diffusion < 0.02f ? 0 : 4, len, g);
    smear[1].set (c.diffusion < 0.02f ? 0 : 4, len * 1.1f, g);

    fbLp.setCutoff (clampf (c.toneHz * 0.55f, 1400.0f, 12000.0f), sr);
    fbHp.setCutoff (clampf (110.0f * std::pow (2.0f, -c.lowEnd / 5.0f), 30.0f, 500.0f), sr);
    outLpL.setCutoff (std::min (16000.0f, c.toneHz), sr);
    outLpR.setCutoff (std::min (16000.0f, c.toneHz * 1.05f), sr);

    roomGain = c.early * 0.9f;
    Tank::Params p;
    p.minMs = 13.0f * c.sizeScale;
    p.maxMs = 58.0f * c.sizeScale;
    p.rt60 = c.decay;
    p.lowMult = c.lowMult;
    p.hfRatio = c.hfRatio;
    p.hfHz = c.dampHz;
    p.modDepthMs = 0.3f;
    p.modRateHz = 0.5f;
    p.infinite = c.infinite;
    room.set (p, blockSamples);
}

void TapeMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const float norm = 1.25f / std::sqrt ((float) heads);
    for (int i = 0; i < n; ++i)
    {
        const float T = time.next();
        const float w = wowDepth * (0.0045f * wow.tick (0.55f, sr) + 0.0011f * flutter.tick (6.3f, sr)
                                    + 0.0025f * drift.tick (0.3f, sr, rng));

        float l = 0.0f, r = 0.0f, fbSrc = 0.0f;
        for (int h = 0; h < heads; ++h)
        {
            const float v = tape.read (std::max (2.0f, T * frac[(size_t) h] * (1.0f + w)));
            const float p = heads == 1 ? 0.0f : -0.8f + 1.6f * (float) h / (float) (heads - 1);
            l += v * std::sqrt (0.5f * (1.0f - p));
            r += v * std::sqrt (0.5f * (1.0f + p));
            if (h == heads - 1) fbSrc += v;
            if (scatter && heads > 1 && h == heads - 2) fbSrc += v;
        }
        if (scatter) fbSrc *= 0.6f;

        // Feedback through the tape's tone, saturating gently past unity.
        float fb = fbHp.hp (fbLp.lp (fbSrc * feedback));
        fb = softLimit (fb, 0.5f, 1.1f);
        tape.push (0.5f * (inL[i] + inR[i]) + fb);

        l = outLpL.lp (smear[0].tick (l * norm));
        r = outLpR.lp (smear[1].tick (r * norm));

        float tl = 0.0f, tr = 0.0f;
        room.tick (l, r, tl, tr);
        outL[i] = l + tl * roomGain;
        outR[i] = r + tr * roomGain;
    }
}
} // namespace lathe::rvb
