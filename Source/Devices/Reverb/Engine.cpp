#include "Engine.h"

namespace lathe::rvb
{
namespace
{
constexpr float kMaxPredelayMs = 2100.0f;

// Level trims so every type sits at about the same loudness (pink noise in,
// 100% wet, starting settings: about -5 dB). Measured, in TypeIndex order.
constexpr float kTrimDb[NumTypes] {
     0.5f,  2.1f,  1.1f,  1.9f,  1.6f,  3.0f,  2.7f,    // rooms
     2.6f,  1.8f, -0.4f, -0.6f,                          // halls
    -8.0f, -5.9f, -9.8f,                                 // plates
    -7.4f, -8.2f,                                        // springs
     1.4f,  0.2f,  2.4f,  1.6f,  0.5f, -0.3f,  2.9f,     // ambient
     0.8f,  0.9f,  0.6f,  0.6f,  1.1f,  6.2f };          // special
} // namespace

Controls Engine::resolve (const EngineSettings& s, float sampleRate)
{
    Controls c;
    c.type = std::max (0, std::min (NumTypes - 1, s.type));
    c.info = &typeInfo (c.type);
    const auto& v = c.info->voicing;
    c.sr = sampleRate;
    c.decay = decayFor (c.type, s.decay);
    c.size = clampf (s.size, 0.0f, 1.0f);
    c.sizeScale = expMap (c.size, 0.5f, 2.0f);
    c.diffusion = clampf (s.diffusion, 0.0f, 1.0f);
    c.early = clampf (s.early, 0.0f, 1.0f);
    c.a = clampf (s.a, 0.0f, 1.0f);
    c.b = clampf (s.b, 0.0f, 1.0f);
    c.c = clampf (s.c, 0.0f, 1.0f);
    const int modes = (int) c.info->modeNames.size();
    c.mode = modes > 0 ? std::max (0, std::min (modes - 1, s.mode)) : 0;

    c.tone = clampf (s.tone, 0.0f, 1.0f);
    c.hfRatio = clampf (0.06f * std::pow (1.0f / 0.06f, std::pow (c.tone, 0.75f)) / v.darkness, 0.03f, 1.0f);
    c.toneHz = clampf (expMap (c.tone, 1500.0f, 18000.0f) / v.darkness, 800.0f, 19000.0f);
    c.dampHz = clampf (expMap (c.tone, 900.0f, 9000.0f) / v.darkness, 500.0f, 12000.0f);
    c.lowEnd = clampf (s.lowEnd + v.lowBias, -10.0f, 10.0f);
    c.lowMult = std::pow (2.0f, c.lowEnd / 5.0f);

    c.modDepth = clampf (s.modDepth, 0.0f, 1.0f);
    c.modRate = clampf (s.modRate, 0.02f, 10.0f);
    c.infinite = s.hold;
    return c;
}

void Engine::prepare (double sampleRate, int)
{
    sr = (float) sampleRate;
    space.prepare (sr);
    plate.prepare (sr);
    spring.prepare (sr);
    reflections.prepare (sr);
    nonlinear.prepare (sr);
    tapeEcho.prepare (sr);

    preL.prepare ((int) (kMaxPredelayMs * 0.001f * sr) + 64);
    preR.prepare ((int) (kMaxPredelayMs * 0.001f * sr) + 64);
    predelay.setTime (180.0f, sr);
    inGain.setTime (20.0f, sr);
    outGain.setTime (20.0f, sr);
    dryGain.setTime (20.0f, sr);
    wetGain.setTime (20.0f, sr);
    widthS.setTime (30.0f, sr);
    sendGain.setTime (25.0f, sr);
    for (auto& d : dc) d.prepare (sr);

    wobbleL.prepare ((int) (0.03f * sr));
    wobbleR.prepare ((int) (0.03f * sr));
    smash.prepare (1.0f, 110.0f, sr);
    smash.thresholdDb = -36.0f;
    smash.ratio = 12.0f;
    swellFast.set (1.0f, 40.0f, sr);
    swellSlow.set (40.0f, 260.0f, sr);
    gateEnv.set (0.5f, 50.0f, sr);
    reset();
}

void Engine::reset()
{
    space.reset();
    plate.reset();
    spring.reset();
    reflections.reset();
    nonlinear.reset();
    tapeEcho.reset();
    preL.reset();
    preR.reset();
    for (auto& d : dc) d.reset();
    for (auto* arr : { &lowShelf, &mid, &lowCut, &lowCut2, &highCut, &highCut2 })
        for (auto& f : *arr) f.reset();
    lastLowEnd = lastMid = 99.0f;
    lastLowCut = lastHighCut = -1.0f;
    duckEnv.reset();
    gateEnv.reset();
    duckDb = 0.0f;
    gateGain = 1.0f;
    gateHold = 0.0f;
    swellFast.reset();
    swellSlow.reset();
    swellEnv = 1.0f;
    swellPos = 1.0f;
    swellArmed = true;
    swellFalling = false;
    sinceTrigger = 1.0e9f;
    holdL = holdR = holdCount = 0.0f;
    wobbleL.reset();
    wobbleR.reset();
    wobbleDrift.reset();
    for (auto& f : lofiLp) f.reset();
    smash.reset();
    active = wanted = -1;
    switching = false;
    switchGain = 1.0f;
    inPeak = outPeak = wetPeak = 0.0f;
    primed = false;
}

MachineBase* Engine::machineFor (int type)
{
    switch (typeInfo (type).voicing.machine)
    {
        case Machine::Space:       return &space;
        case Machine::Plate:       return &plate;
        case Machine::Spring:      return &spring;
        case Machine::Reflections: return &reflections;
        case Machine::Nonlinear:   return &nonlinear;
        case Machine::Tape:        return &tapeEcho;
    }
    return &space;
}

void Engine::process (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings& s)
{
    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int len = std::min (kBlock, n - pos);
        processBlock (inL + pos, inR + pos, outL + pos, outR + pos, len, s);
    }
}

float Engine::swellTick (float level, const EngineSettings& s)
{
    const float rise = expMap (s.a, 20.0f, 4000.0f) * 0.001f * sr;
    const float thr = dbToGain (-22.0f - 32.0f * s.b);
    const float f = swellFast.tick (level), sl = swellSlow.tick (level);
    sinceTrigger += 1.0f;

    // A new note: loud enough, and clearly louder than a moment ago.
    const bool onset = f > thr && f > sl * 1.9f + 1.0e-5f;
    if (onset && (swellArmed || (f > sl * 3.0f && sinceTrigger > 0.15f * sr)))
    {
        swellArmed = false;
        swellFalling = swellEnv > 0.001f;
        swellPos = 0.0f;
        sinceTrigger = 0.0f;
    }
    if (! swellArmed && f < thr * 0.5f)
        swellArmed = true;

    if (swellFalling)
    {
        swellEnv -= 1.0f / (0.012f * sr);
        if (swellEnv <= 0.0f) { swellEnv = 0.0f; swellFalling = false; }
    }
    else if (swellPos < 1.0f)
    {
        swellPos = std::min (1.0f, swellPos + 1.0f / std::max (1.0f, rise));
        const float sn = std::sin (0.5f * kPi * swellPos);
        swellEnv = sn * sn;
    }
    return swellEnv;
}

void Engine::processBlock (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings& s)
{
    // --- type switching: fade the wet out, reset the new machine, fade in ---------
    const int type = std::max (0, std::min (NumTypes - 1, s.type));
    if (active < 0)
    {
        active = wanted = type;
        machineFor (active)->reset();
    }
    else if (type != wanted)
    {
        wanted = type;
        switching = true;
    }

    EngineSettings sa = s;
    sa.type = active;   // while switching, the old machine plays out
    const Controls c = resolve (sa, sr);
    MachineBase* m = machineFor (active);
    m->set (c, n);
    const auto& info = *c.info;
    const unsigned feat = info.voicing.features;

    // --- block-rate targets ---------------------------------------------------------
    inGain.target = dbToGain (s.inputDb);
    outGain.target = dbToGain (s.outputDb);
    const float mx = clampf (s.mix, 0.0f, 1.0f);
    dryGain.target = mx < 0.5f ? 1.0f : std::sin ((1.0f - mx) * kPi);
    wetGain.target = mx < 0.5f ? std::sin (mx * kPi) : 1.0f;
    widthS.target = clampf (s.width, 0.0f, 1.5f);
    sendGain.target = (s.hold && s.holdMode == 1) ? 0.0f : 1.0f;
    const float pdMs = clampf (s.predelayMs, 0.0f, kMaxPredelayMs - 20.0f);
    predelay.target = pdMs * 0.001f * sr;
    if (! primed)
    {
        // First block after a reset: start where the knobs are, not from zero.
        primed = true;
        for (auto* sm : { &predelay, &inGain, &outGain, &dryGain, &wetGain, &widthS, &sendGain })
            sm->snap (sm->target);
    }
    const bool swell = (feat & FeatSwell) != 0;
    const bool swellDry = swell && c.mode == 1;
    const bool reflect = info.voicing.machine == Machine::Reflections;
    const float pan = reflect ? reflections.dryPan() : 0.0f;

    // Post EQ, recomputed only when it moves.
    const float lowEndCut = std::min (0.0f, c.lowEnd) * 1.0f;
    if (std::abs (lowEndCut - lastLowEnd) > 0.01f)
    {
        lastLowEnd = lowEndCut;
        for (auto& f : lowShelf) { if (lowEndCut < -0.01f) f.lowShelf (220.0, lowEndCut, sr); else f.flat(); }
    }
    if (std::abs (s.midDb - lastMid) > 0.01f)
    {
        lastMid = s.midDb;
        for (auto& f : mid) { if (std::abs (s.midDb) > 0.05f) f.peak (900.0, 0.6, s.midDb, sr); else f.flat(); }
    }
    if (std::abs (s.lowCutHz - lastLowCut) > 0.05f)
    {
        lastLowCut = s.lowCutHz;
        for (int ch = 0; ch < 2; ++ch)
        {
            if (s.lowCutHz > 20.5f) { lowCut[(size_t) ch].highpass (s.lowCutHz, 0.541, sr); lowCut2[(size_t) ch].highpass (s.lowCutHz, 1.307, sr); }
            else { lowCut[(size_t) ch].flat(); lowCut2[(size_t) ch].flat(); }
        }
    }
    if (std::abs (s.highCutHz - lastHighCut) > 1.0f)
    {
        lastHighCut = s.highCutHz;
        for (int ch = 0; ch < 2; ++ch)
        {
            if (s.highCutHz < 19900.0f) { highCut[(size_t) ch].lowpass (s.highCutHz, 0.541, sr); highCut2[(size_t) ch].lowpass (s.highCutHz, 1.307, sr); }
            else { highCut[(size_t) ch].flat(); highCut2[(size_t) ch].flat(); }
        }
    }
    if (std::abs (s.releaseMs - lastRelease) > 0.5f)
    {
        lastRelease = s.releaseMs;
        duckEnv.set (4.0f, std::max (10.0f, s.releaseMs), sr);
    }

    // --- in: input gain, swell, pre-delay ------------------------------------------------
    std::array<float, kBlock> dyn {}, sw {};
    for (int i = 0; i < n; ++i)
    {
        const float g = inGain.next();
        float l = inL[i] * g, r = inR[i] * g;
        const float level = std::max (std::fabs (l), std::fabs (r));
        inPeak = std::max (inPeak, level);

        if (swell) swellTick (level, s);
        else swellEnv = 1.0f;
        if (swellDry) { l *= swellEnv; r *= swellEnv; }
        dryL[(size_t) i] = l;
        dryR[(size_t) i] = r;

        const float send = sendGain.next();
        preL.push (dc[0].tick (l) * send);
        preR.push (dc[1].tick (r) * send);
        const float d = 1.0f + predelay.next();   // read (1) is the sample just written
        sendL[(size_t) i] = preL.read (d);
        sendR[(size_t) i] = preR.read (d);

        // Ducking and gating listen to the input.
        if (s.duck > 0.001f)
        {
            const float env = duckEnv.tick (level);
            duckDb = s.duck * 30.0f * clampf ((gainToDb (env) + 45.0f) / 35.0f, 0.0f, 1.0f);
        }
        else
            duckDb = 0.0f;
        if (s.gateDb > -79.5f)
        {
            const float env = gateEnv.tick (level);
            if (gainToDb (env) > s.gateDb)
            {
                gateHold = 0.04f * sr;
                gateGain += (1.0f - gateGain) * (1.0f - std::exp (-1.0f / (0.002f * sr)));
            }
            else if (gateHold > 0.0f)
                gateHold -= 1.0f;
            else
                gateGain *= std::exp (-6.9f / (std::max (10.0f, s.releaseMs) * 0.001f * sr));
        }
        else
            gateGain = 1.0f;
        // The per-sample dynamics, for the second pass.
        dyn[(size_t) i] = dbToGain (-duckDb) * gateGain;
        sw[(size_t) i] = swellEnv;
    }

    // --- the reverb -----------------------------------------------------------------------
    auto& ml = wetL;
    auto& mr = wetR;
    m->process (sendL.data(), sendR.data(), ml.data(), mr.data(), n);

    // A machine that blew up starts again rather than poisoning the host.
    if (! std::isfinite (ml[0]) || ! std::isfinite (mr[(size_t) (n - 1)]))
    {
        m->reset();
        ml.fill (0.0f);
        mr.fill (0.0f);
    }

    // --- out: post EQ, character, dynamics, width, mix --------------------------------------
    const bool lofi = (feat & FeatLofi) != 0, smashOn = (feat & FeatSmash) != 0 && c.c > 0.001f;
    const float holdN = 1.0f + std::round (c.a * c.a * 15.0f);
    const float bits = 16.0f - c.a * 10.0f;
    const float q = std::pow (2.0f, bits - 1.0f);
    if (lofi)
    {
        lofiLp[0].setCutoff (std::max (2500.0f, 9000.0f - 5000.0f * c.a), sr);
        lofiLp[1].setCutoff (std::max (2500.0f, 9000.0f - 5000.0f * c.a), sr);
    }
    const float makeup = dbToGain (15.0f);
    const float trim = dbToGain (kTrimDb[active]);

    for (int i = 0; i < n; ++i)
    {
        float l = ml[(size_t) i] * trim, r = mr[(size_t) i] * trim;
        l = highCut2[0].tick (highCut[0].tick (lowCut2[0].tick (lowCut[0].tick (mid[0].tick (lowShelf[0].tick (l))))));
        r = highCut2[1].tick (highCut[1].tick (lowCut2[1].tick (lowCut[1].tick (mid[1].tick (lowShelf[1].tick (r))))));

        if (lofi)
        {
            if (c.a > 0.01f)
            {
                holdCount += 1.0f;
                if (holdCount >= holdN) { holdCount = 0.0f; holdL = l; holdR = r; }
                l = std::round (holdL * q) / q;
                r = std::round (holdR * q) / q;
            }
            l = lofiLp[0].lp (l);
            r = lofiLp[1].lp (r);
            wobbleL.push (l);
            wobbleR.push (r);
            const float w = c.b * 0.004f * sr * (1.0f + 0.7f * wobbleLfo.tick (0.55f, sr) + 0.3f * wobbleDrift.tick (0.4f, sr, rng));
            l = wobbleL.read (2.0f + w);
            r = wobbleR.read (2.0f + w * 1.05f);
        }
        if (smashOn)
        {
            const float g = smash.gain (std::max (std::fabs (l), std::fabs (r)));
            const float k = c.c;
            l = l * (1.0f - k) + l * g * makeup * k;
            r = r * (1.0f - k) + r * g * makeup * k;
        }
        if (swell && ! swellDry)
        {
            l *= sw[(size_t) i];
            r *= sw[(size_t) i];
        }

        // Duck/gate and the type-switch fade.
        if (switching)
        {
            switchGain -= 1.0f / (0.015f * sr);
            if (switchGain <= 0.0f)
            {
                switchGain = 0.0f;
                switching = false;
                active = wanted;
                machineFor (active)->reset();
            }
        }
        else if (switchGain < 1.0f)
            switchGain = std::min (1.0f, switchGain + 1.0f / (0.01f * sr));
        const float wd = dyn[(size_t) i] * switchGain;
        l *= wd;
        r *= wd;

        const float wdt = widthS.next();
        const float mm = 0.5f * (l + r), ss = 0.5f * (l - r) * wdt;
        l = mm + ss;
        r = mm - ss;

        const float wg = wetGain.next(), dg = dryGain.next(), og = outGain.next();
        l *= wg;
        r *= wg;
        wetPeak = std::max (wetPeak, std::max (std::fabs (l), std::fabs (r)));

        float xl = dryL[(size_t) i], xr = dryR[(size_t) i];
        if (reflect && std::abs (pan) > 0.001f)
        {
            // Equal-power pan of the middle; the sides narrow as it moves off centre.
            const float m = 0.5f * (xl + xr), sd = 0.5f * (xl - xr) * (1.0f - std::abs (pan));
            xl = m * std::sqrt (1.0f - pan) + sd;
            xr = m * std::sqrt (1.0f + pan) - sd;
        }
        const float ol = (xl * dg + l) * og, orr = (xr * dg + r) * og;
        outL[i] = ol;
        outR[i] = orr;
        outPeak = std::max (outPeak, std::max (std::fabs (ol), std::fabs (orr)));
    }
}
} // namespace lathe::rvb
