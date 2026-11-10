// Space.cpp — the SPACE machine: early reflections, diffusers and a 16-line
// tank, plus the extras some types add (cloud, bloom, shimmer, choir, tuned
// combs, cathedral echo).
#include "Machines.h"

#include <cstring>

namespace lathe::rvb
{
namespace
{
constexpr float kMaxLineMs = 600.0f;
constexpr float kMaxDiffMs = 240.0f;
constexpr float kMaxBloomMs = 420.0f;
constexpr float kMaxErMs = 520.0f;

bool hasBuild (const TypeInfo& t) { return std::strcmp (t.charLabel[0], "BUILD") == 0; }
} // namespace

void SpaceMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    tank.prepare (sr, kMaxLineMs);
    diff[0].prepare (sr, kMaxDiffMs, 11);
    diff[1].prepare (sr, kMaxDiffMs * 1.08f, 29);
    bloom[0].prepare (sr, kMaxBloomMs, 41);
    bloom[1].prepare (sr, kMaxBloomMs * 1.07f, 53);
    early.prepare (sr, kMaxErMs);
    buildLineL.prepare ((int) (0.4f * sr));
    buildLineR.prepare ((int) (0.4f * sr));
    echoLine.prepare ((int) (1.3f * sr));
    for (auto& s : shifters) s.prepare (sr);
    choir.prepare (sr);
    combs.prepare (sr);
    for (int c = 0; c < 2; ++c)
    {
        shimmerHp[(size_t) c].highpass (140.0, 0.6, sr);
        shimmerLp[(size_t) c].lowpass (8500.0, 0.6, sr);
        echoLp[(size_t) c].setCutoff (3500.0f, sr);
    }
    shimmerEnv.set (5.0f, 400.0f, sr);
    reset();
}

void SpaceMachine::reset()
{
    tank.reset();
    for (auto& d : diff) d.reset();
    for (auto& d : bloom) d.reset();
    early.reset();
    buildLineL.reset();
    buildLineR.reset();
    echoLine.reset();
    for (auto& s : shifters) s.reset();
    choir.reset();
    combs.reset();
    for (auto& f : shimmerHp) f.reset();
    for (auto& f : shimmerLp) f.reset();
    for (auto& f : echoLp) f.reset();
    shimmerEnv.reset();
    tankOut.fill (0.0f);
    bloomFb.fill (0.0f);
}

void SpaceMachine::set (const Controls& c, int blockSamples)
{
    const auto& info = *c.info;
    const auto& v = info.voicing;
    features = v.features;
    const float s = c.sizeScale;

    // Early reflections and the gap before the tail.
    const bool rooms = hasBuild (info);
    const float distance = rooms ? c.b : 0.0f;
    early.configure (v.erPattern, v.erMs * s, distance);
    early.setTone (std::min (c.toneHz * 1.4f, 16000.0f));
    erGain = v.erGain * c.early * (1.0f - 0.55f * distance) * 1.4f;
    buildSamples = rooms ? c.a * (15.0f + 85.0f * s) * 0.001f * sr : 0.0f;
    lateGain = 1.0f + 0.5f * distance;

    // The tank.
    Tank::Params p;
    p.minMs = v.lineMinMs * s;
    p.maxMs = v.lineMaxMs * s;
    p.rt60 = c.decay * 1.2f;   // the early part and the HF losses make it read short otherwise
    p.lowMult = c.lowMult;
    p.hfRatio = c.hfRatio;
    p.hfHz = c.dampHz;
    p.xoverHz = 220.0f;
    const float wobble = (features & FeatLofi) ? c.b : 0.0f;
    p.modDepthMs = (c.modDepth * c.modDepth * 2.2f + wobble * 3.0f) * v.modScale;
    p.modRateHz = c.modRate * (1.0f - 0.6f * wobble);
    p.infinite = c.infinite;
    tank.set (p, blockSamples);

    // Input diffusion.
    const int stages = c.diffusion < 0.02f ? 0 : v.diffusers;
    if (features & FeatCloud)
    {
        const float haze = (features & FeatShimmer) ? 0.45f : c.a;
        const float smearK = (features & FeatShimmer) ? 0.6f : c.b;
        const float len = v.diffuserMs * s * (0.3f + 1.2f * smearK);
        const float g = 0.5f + 0.28f * c.diffusion;
        for (auto& d : diff) d.set (stages, len, g, haze * 3.0f + c.modDepth * 0.8f, 0.25f + haze * 1.2f);
    }
    else
    {
        const float g = 0.75f * c.diffusion;
        for (auto& d : diff) d.set (stages, v.diffuserMs * s, g);
    }

    // Bloom.
    if (features & FeatBloom)
    {
        const float len = (40.0f + 300.0f * c.a) * std::min (1.2f, s);
        for (auto& d : bloom) d.set (5, len, 0.62f + 0.12f * c.diffusion, 0.8f * c.modDepth, 0.4f + 0.3f * c.modRate);
        bloomFeedback = c.b * 0.8f;
        bloomMix = 0.35f;
    }

    // Shimmer.
    if (features & FeatShimmer)
    {
        const int s1 = shiftSemis100 (c.b, false), s2 = shiftSemis100 (c.c, true);
        shifters[0].setShift (s1);
        shifters[1].setShift (s1);
        voice2 = s2 > -100000;
        if (voice2) { shifters[2].setShift (s2); shifters[3].setShift (s2); }
        const float split = voice2 ? 0.72f : 1.0f;
        shimmerIn = (c.mode != 1 ? c.a : 0.0f) * split;
        shimmerTank = (c.mode != 0 ? c.a * 0.62f : 0.0f) * split;
    }

    // Choir and tuned combs.
    if (features & FeatChoir)
    {
        choir.set (c.mode, c.a, c.b, 0.3f + c.modDepth, blockSamples);
        choirMix = c.a;
    }
    if (features & FeatResonant)
    {
        combs.set (c.mode, chordIndex (c.b), std::min (c.infinite ? 60.0f : c.decay * 0.9f, 40.0f), c.toneHz);
        ringMix = c.a;
    }

    // Cathedral echo.
    if (features & FeatEcho)
    {
        echoSamples = (v.erMs * s * 1.6f + 110.0f) * 0.001f * sr;
        echoGain = c.c * 0.7f;
    }
    else
        echoGain = 0.0f;

    grit = (features & FeatLofi) ? c.c : 0.0f;

    // Longer tails sound a little louder, as in a real space.
    levelTrim = clampf (std::pow (std::max (0.1f, c.decay) / 2.0f, 0.18f), 0.7f, 1.6f);
}

void SpaceMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const bool cloud = features & FeatCloud, bloomOn = features & FeatBloom, shimmer = features & FeatShimmer;
    const bool choirOn = features & FeatChoir, ring = features & FeatResonant;
    const bool echoOn = echoGain > 0.0f;
    (void) cloud;

    for (int i = 0; i < n; ++i)
    {
        float xl = inL[i], xr = inR[i];

        // Grit: drive into the tank.
        if (grit > 0.0f)
        {
            const float d = 1.0f + grit * 6.0f;
            xl = fastTanh (xl * d) / (1.0f + grit * 2.0f);
            xr = fastTanh (xr * d) / (1.0f + grit * 2.0f);
        }

        float el = 0.0f, er = 0.0f;
        early.tick (0.5f * (xl + xr), el, er);

        // BUILD: hold the tail back behind the early reflections.
        buildLineL.push (xl);
        buildLineR.push (xr);
        if (buildSamples > 1.0f)
        {
            xl = buildLineL.read (buildSamples);
            xr = buildLineR.read (buildSamples);
        }

        float echoL = 0.0f, echoR = 0.0f;
        if (echoOn)
        {
            echoLine.push (0.5f * (inL[i] + inR[i]));
            const float e = echoLine.read (echoSamples) * echoGain;
            echoL = echoLp[0].lp (e);
            echoR = echoLp[1].lp (echoLine.read (echoSamples * 1.03f) * echoGain);
        }

        float dl = diff[0].tick (xl + 0.3f * el + 0.5f * echoL);
        float dr = diff[1].tick (xr + 0.3f * er + 0.5f * echoR);

        float bl = 0.0f, br = 0.0f;
        if (bloomOn)
        {
            bl = bloom[0].tick (dl + bloomFeedback * bloomFb[1]);
            br = bloom[1].tick (dr + bloomFeedback * bloomFb[0]);
            bloomFb[0] = softLimit (bl, 0.8f, 1.5f);
            bloomFb[1] = softLimit (br, 0.8f, 1.5f);
            dl = bl;
            dr = br;
        }

        if (shimmer)
        {
            // What the shifters hear: the input, the tail, or both.
            const float envIn = shimmerEnv.tick (0.5f * (tankOut[0] + tankOut[1]));
            const float agc = envIn > 0.35f ? 0.35f / envIn : 1.0f;
            const float srcL = shimmerIn * xl + shimmerTank * agc * tankOut[0];
            const float srcR = shimmerIn * xr + shimmerTank * agc * tankOut[1];
            float sl = shifters[0].tick (srcL), sr_ = shifters[1].tick (srcR);
            if (voice2)
            {
                sl += shifters[2].tick (srcR);
                sr_ += shifters[3].tick (srcL);
            }
            sl = shimmerLp[0].tick (shimmerHp[0].tick (sl));
            sr_ = shimmerLp[1].tick (shimmerHp[1].tick (sr_));
            dl += softLimit (sl, 0.6f, 1.0f);
            dr += softLimit (sr_, 0.6f, 1.0f);
        }

        float tl, tr;
        tank.tick (dl, dr, tl, tr);
        tankOut[0] = tl;
        tankOut[1] = tr;

        if (choirOn)
        {
            float cl, cr;
            choir.tick (tl, tr, cl, cr);
            // Formants ring hard when the music lines up with them: keep the peaks in hand.
            cl = softLimit (cl * (0.25f + 0.3f * choirMix), 0.3f, 0.7f);
            cr = softLimit (cr * (0.25f + 0.3f * choirMix), 0.3f, 0.7f);
            tl = tl * (1.0f - 0.75f * choirMix) + cl;
            tr = tr * (1.0f - 0.75f * choirMix) + cr;
        }
        if (ring)
        {
            float cl, cr;
            combs.tick (0.5f * inL[i] + 0.4f * tl, 0.5f * inR[i] + 0.4f * tr, cl, cr);
            // Strings ring hardest when the music is in their key.
            cl = softLimit (cl * ringMix * 1.4f, 0.3f, 0.7f);
            cr = softLimit (cr * ringMix * 1.4f, 0.3f, 0.7f);
            tl = tl * (1.0f - 0.7f * ringMix) + cl;
            tr = tr * (1.0f - 0.7f * ringMix) + cr;
        }

        const float late = lateGain * levelTrim;
        float ol = erGain * el + late * tl + echoL;
        float orr = erGain * er + late * tr + echoR;
        if (bloomOn)
        {
            ol += bloomMix * bl;
            orr += bloomMix * br;
        }
        outL[i] = ol;
        outR[i] = orr;
    }
}
} // namespace lathe::rvb
