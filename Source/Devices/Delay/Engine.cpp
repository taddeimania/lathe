#include "Engine.h"

namespace lathe::dly
{
namespace
{
constexpr float kMainSeconds = 6.3f;     // TIME up to 4 s, plus OFFSET; reverse +8VA reads 3x a 2 s slice
constexpr float kSecondSeconds = 8.3f;   // Dual's TIME 2 at x2
constexpr float kTapeHeads[3] { 0.36f, 0.68f, 1.0f };

// Which tape heads each HEADS choice turns on.
constexpr bool kHeadsOn[5][3] { { false, false, true }, { true, true, false }, { false, true, true },
                                { true, false, true }, { true, true, true } };

float smoothstep01 (float x)
{
    x = clampf (x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

struct Shape { float hp, hpQ, lp, lpQ, peak, peakQ, peakDb, drive; };
const Shape kShapes[] {
    { 20.0f, 0.7f, 20000.0f, 0.7f, 1000.0f, 1.0f, 0.0f, 0.0f },     // OFF
    { 90.0f, 0.7f, 5200.0f, 0.9f, 1800.0f, 0.8f, 3.0f, 0.0f },      // AMP
    { 350.0f, 1.2f, 3000.0f, 1.5f, 1200.0f, 1.2f, 6.0f, 0.0f },     // VICTROLA
    { 220.0f, 0.9f, 3800.0f, 1.0f, 2000.0f, 1.0f, 4.0f, 0.0f },     // RADIO
    { 600.0f, 1.0f, 3200.0f, 2.0f, 1600.0f, 1.5f, 9.0f, 0.6f },     // MEGAPHONE
    { 400.0f, 1.2f, 3400.0f, 1.4f, 1000.0f, 1.0f, 2.0f, 0.2f },     // PHONE
    { 700.0f, 1.0f, 2600.0f, 1.6f, 1400.0f, 1.2f, 5.0f, 0.5f },     // INTERCOM
    { 500.0f, 1.0f, 2900.0f, 2.2f, 2200.0f, 1.4f, 8.0f, 0.9f },     // WALKIE
};
} // namespace

void Engine::prepare (double sampleRate, int)
{
    sr = (float) sampleRate;
    maxMain = (int) (kMainSeconds * sr) + 64;
    maxSecond = (int) (kSecondSeconds * sr) + 64;
    for (auto& c : chans)
    {
        c.line.prepare (maxMain);
        c.line2.prepare (maxSecond);
        c.pitch.prepare ((int) (0.9f * sr));
        for (auto& a : c.smear) a.prepare ((int) (0.04f * sr));
        for (auto& a : c.cloud) a.prepare ((int) (0.13f * sr));
        c.dc.prepare (sr);
        c.dc2.prepare (sr);
        c.hissTone.setCutoff (5000.0f, sr);
        c.compEnv.set (4.0f, 70.0f, sr);
        c.expEnv.set (6.0f, 110.0f, sr);
        c.taperEnv.set (2.0f, 60.0f, sr);
    }
    crackleTone.setCutoff (2500.0f, sr);

    timeS.setTime (120.0f, sr);
    for (auto* s : { &inGain, &outGain, &dryGain, &wetGain, &widthS, &fbS, &fb2S, &lvl2S })
        s->setTime (25.0f, sr);
    holdS.setTime (30.0f, sr);

    duckEnv.set (3.0f, 250.0f, sr);
    playEnv.set (2.0f, 250.0f, sr);
    onsetFast.set (1.0f, 25.0f, sr);
    onsetSlow.set (40.0f, 200.0f, sr);
    sweepEnv.set (5.0f, 160.0f, sr);
    wetEnv.set (2.0f, 120.0f, sr);
    peakEnv.set (1.0f, 1500.0f, sr);

    lastType = lastMode = -1;
    primed = gainsPrimed = false;
    lowCutLast = highCutLast = bbdCutLast = -1.0f;
    shapeLast = -1;
    reset();
}

void Engine::reset()
{
    for (auto& c : chans)
    {
        c.line.reset(); c.line2.reset(); c.pitch.reset();
        for (auto& a : c.smear) a.reset();
        for (auto& a : c.cloud) a.reset();
        for (auto* b : { &c.voiceLp, &c.voiceLp2, &c.voiceHp, &c.gritLp, &c.tapeBump, &c.tapeAge, &c.tapeAge2, &c.tapeBias,
                         &c.tapeLow, &c.bbdIn, &c.bbdIn2, &c.bbdOut, &c.bbdOut2, &c.shapeHp, &c.shapeLp, &c.shapePeak,
                         &c.lowCut, &c.lowCut2, &c.highCut, &c.highCut2 })
            b->reset();
        c.dc.reset(); c.dc2.reset(); c.hissTone.reset(); c.sweep.reset();
        c.compEnv.reset(); c.expEnv.reset(); c.taperEnv.reset();
        c.bbdHeld = c.lofiHeld = c.fb = c.fb2 = 0.0f;
    }
    for (auto* e : { &duckEnv, &playEnv, &onsetFast, &onsetSlow, &sweepEnv, &wetEnv, &peakEnv })
        e->reset();
    modDrift.reset(); wowDrift.reset(); crinkleDrift.reset();
    crinkleGain = crinkleTarget = 1.0f;
    crinkleLeft = 0.0f;
    revPhase = icePhase = lfoPhase = lofiPhase = bbdPhase = 0.0f;
    randomA = randomB = 0.0f;
    crackleEnv = 0.0f;
    crackleTone.reset();
    duckGain = fbDuck = swellGain = 1.0f;
    swellArmed = revArmed = true;
    swellFalling = false;
    tremGain = 1.0f;
    typeFade = 1.0f;
    timeS.snap (timeS.target);
}

// ---------------------------------------------------------------------------
// Control rate
// ---------------------------------------------------------------------------
void Engine::updateControls (const EngineSettings& s)
{
    const int type = clampi (s.type, 0, NumTypes - 1);
    if (type != lastType)
    {
        if (lastType >= 0)
            typeFade = 0.0f;   // hide the change of reader under a quick fade
        timeS.setTime (type == TTape ? 320.0f : type == TBucket ? 200.0f : 120.0f, sr);
        lastType = type;
        bbdCutLast = -1.0f;
    }
    curType = type;
    curMode = s.mode;
    curStereo = clampi (s.stereo, 0, 2);

    // TIME, held in the type's range and inside the line.
    float ms = Engine::effectiveTimeMs (s);
    float target = ms * 0.001f * sr;
    const float room = (float) (maxMain - 64) - 0.012f * sr;
    if (type == TReverse)
    {
        static const float r[] { 1.0f, 0.5f, 2.0f };
        target = std::min (target, room / (1.0f + r[clampi (s.mode, 0, 2)]));
    }
    else if (type == TDigital)
        target = std::min (target, room / 1.26f);
    timeS.target = clampf (target, 2.0f, room);
    if (! primed)
    {
        timeS.snap (timeS.target);
        primed = true;
    }

    inGain.target = dbToGain (s.inputDb);
    outGain.target = dbToGain (s.outputDb);
    const float mix = clampf (s.mix, 0.0f, 1.0f);
    dryGain.target = mix <= 0.5f ? 1.0f : 2.0f * (1.0f - mix);
    wetGain.target = mix >= 0.5f ? 1.0f : 2.0f * mix;
    widthS.target = clampf (s.width, 0.0f, 1.5f);
    holdS.target = s.hold ? 1.0f : 0.0f;
    if (! gainsPrimed)
    {
        for (auto* sm : { &inGain, &outGain, &dryGain, &wetGain, &widthS, &holdS })
            sm->snap (sm->target);
        gainsPrimed = true;
    }

    // FILTER's voicing for the types that don't give it another job.
    const bool voiced = type != TTape && type != TBucket && type != TFilter;
    const float f = clampf (s.filter, 0.0f, 1.0f);
    voiceOn = voiced && f > 0.015f;
    if (voiceOn)
    {
        float lp, hp;
        if (f <= 0.5f) { lp = expMap (f / 0.5f, 20000.0f, 2300.0f); hp = 20.0f; }
        else
        {
            const float t = (f - 0.5f) / 0.5f;
            lp = expMap (t, 2300.0f, 6000.0f);
            hp = expMap (t, 20.0f, 520.0f);
        }
        for (auto& c : chans)
        {
            c.voiceLp.lowpass (lp, 0.62, sr);
            c.voiceHp.highpass (hp, 0.62, sr);
        }
    }

    // GRIT.
    const float g = clampf (s.grit, 0.0f, 1.0f);
    gritOn = type != TTape && g > 0.01f;
    gritDrive = 1.0f + 7.0f * g * g;
    gritBias = 0.18f * g;
    if (gritOn)
        for (auto& c : chans) c.gritLp.lowpass (expMap (g, 17000.0f, 5500.0f), 0.6, sr);

    // SMEAR.
    smearOn = s.smear > 0.01f;
    smearG = 0.68f * clampf (s.smear, 0.0f, 1.0f);
    smearScale = (1.0f + 1.6f * s.smear) * 0.001f * sr;

    // REPEATS and TAPER.
    float rep = clampf (s.repeats, 0.0f, 1.1f);
    bbdLoss = type == TBucket ? g : 0.0f;
    if (type == TBucket) rep *= 1.0f - 0.22f * bbdLoss;
    fbS.target = rep;
    taperK = 1.6f * clampf (s.taper, 0.0f, 1.0f);

    // Dynamics: DUCK and DUCK REPEATS, from the dry input's envelopes.
    duckEnv.set (3.0f, std::max (10.0f, s.releaseMs), sr);
    playEnv.set (2.0f, std::max (10.0f, s.releaseMs), sr);
    {
        const float envDb = gainToDb (duckEnv.env);
        const float amount = clampf ((envDb + 50.0f) / 36.0f, 0.0f, 1.0f);
        duckTarget = dbToGain (-clampf (s.duck, 0.0f, 1.0f) * 32.0f * amount);
        const float playing = clampf ((gainToDb (playEnv.env) + 54.0f) / 18.0f, 0.0f, 1.0f);
        fbDuckTarget = s.duckRepeats ? 1.0f - 0.97f * playing : 1.0f;
    }

    // MOTION.
    if (type == TTape)
    {
        modMs = 0.0f;
        wowMs = 3.5f * s.depth * s.depth;
        flutterMs = 0.12f * s.depth;
        crinkle = clampf (s.speed, 0.0f, 1.0f);
    }
    else
    {
        modMs = 6.0f * s.depth * s.depth;
        modHz = speedHz (s.speed);
        wowMs = flutterMs = crinkle = 0.0f;
    }

    // Tape.
    if (type == TTape)
    {
        const float age = f, bias = g;
        tapeLevel = 1.03f - 0.33f * bias * bias;
        tapeDrive = 1.1f + 2.4f * bias;
        hiss = 0.006f * s.b * s.b;
        for (auto& c : chans)
        {
            c.tapeBump.lowShelf (95.0, 2.5, sr);
            c.tapeAge.lowpass (expMap (age, 15000.0f, 2400.0f), 0.65, sr);
            c.tapeAge2.lowpass (expMap (age, 18000.0f, 4500.0f), 0.55, sr);
            c.tapeBias.peak (5500.0, 0.6, lerpf (4.5f, -7.0f, bias), sr);
            c.tapeLow.highpass (expMap (s.a, 25.0f, 700.0f), 0.6, sr);
        }
    }

    // Bucket brigade: the clock slows as the time grows, and the filters close.
    if (type == TBucket)
    {
        const float stages = s.mode == 1 ? 8192.0f : 4096.0f;
        const float dSec = std::max (0.002f, timeS.value / sr);
        bbdClock = stages / (2.0f * dSec);
        const float scale = std::pow (2.0f, -(f - 0.5f) * 3.0f);
        const float cut = clampf (0.36f * bbdClock * scale, 500.0f, 0.45f * sr);
        if (std::abs (cut - bbdCutLast) > 0.01f * cut)
        {
            bbdCutLast = cut;
            for (auto& c : chans)
            {
                c.bbdIn.lowpass (cut, 0.54, sr);   c.bbdIn2.lowpass (cut, 1.31, sr);
                c.bbdOut.lowpass (cut, 0.54, sr);  c.bbdOut2.lowpass (cut, 1.31, sr);
            }
        }
        comp = clampf (s.a, 0.0f, 1.0f);
        hiss = 0.004f * s.b * s.b;
    }

    // Lo-fi.
    if (type == TLofi)
    {
        lofiRate = expMap (s.a, 750.0f, 48000.0f);
        const float bits = 4.0f + 20.0f * clampf (s.b, 0.0f, 1.0f);
        lofiQ = bits < 23.5f ? std::pow (2.0f, bits - 1.0f) : 0.0f;
        const int shape = clampi (s.mode, 0, 7);
        if (shape != shapeLast)
        {
            shapeLast = shape;
            const auto& sh = kShapes[shape];
            for (auto& c : chans)
            {
                c.shapeHp.highpass (sh.hp, sh.hpQ, sr);
                c.shapeLp.lowpass (sh.lp, sh.lpQ, sr);
                c.shapePeak.peak (sh.peak, sh.peakQ, sh.peakDb, sr);
            }
            shapeDrive = sh.drive;
        }
        vinyl = clampf (s.c, 0.0f, 1.0f);
    }

    // Cloud.
    cloudG = 0.45f + 0.3f * clampf (s.a, 0.0f, 1.0f);
    cloudMix = clampf (s.a, 0.0f, 1.0f);
    cloudScale = (0.4f + 1.6f * clampf (s.b, 0.0f, 1.0f)) * 0.001f * sr;

    // LOW CUT and HIGH CUT on the echoes.
    if (std::abs (s.lowCutHz - lowCutLast) > 0.1f)
    {
        lowCutLast = s.lowCutHz;
        for (auto& c : chans)
        {
            c.lowCut.highpass (s.lowCutHz, 0.54, sr);
            c.lowCut2.highpass (s.lowCutHz, 1.31, sr);
        }
    }
    if (std::abs (s.highCutHz - highCutLast) > 1.0f)
    {
        highCutLast = s.highCutHz;
        for (auto& c : chans)
        {
            c.highCut.lowpass (s.highCutHz, 0.54, sr);
            c.highCut2.lowpass (s.highCutHz, 1.31, sr);
        }
    }

    // Dual.
    {
        const auto& r = dualRatios();
        ratio2 = r[(size_t) ratioIndex (s.a, (int) r.size())].value;
        fb2S.target = clampf (s.b, 0.0f, 1.0f);
        lvl2S.target = clampf (s.c, 0.0f, 1.0f);
    }

    // Pattern taps.
    if (type == TPattern)
    {
        const auto& pats = patterns();
        const auto& pat = pats[(size_t) clampi (s.mode, 0, (int) pats.size() - 1)];
        const float lean = (clampf (s.b, 0.0f, 1.0f) - 0.5f) * 2.0f;
        const float ex = std::pow (2.0f, lean * 1.2f);
        const float spread = clampf (s.a, 0.0f, 1.0f);
        numTaps = 0;
        float energy = 0.0f;
        for (const auto& t : pat.taps)
        {
            if (numTaps >= kMaxTaps) break;
            auto& tp = taps[(size_t) numTaps++];
            tp.at = std::pow (clampf (t.at, 0.0f, 1.0f), ex);
            const float pan = clampf (t.pan * spread, -1.0f, 1.0f);
            const float ang = (pan + 1.0f) * 0.25f * kPi;
            tp.gl = t.level * std::cos (ang) * 1.41421356f;
            tp.gr = t.level * std::sin (ang) * 1.41421356f;
            energy += t.level * t.level;
        }
        tapNorm = 1.0f / std::max (1.0f, std::sqrt (energy));
    }

    // Trem and Filter rates, and the filter's sweep.
    {
        const auto& r = lfoRatios();
        lfoRatio = r[(size_t) ratioIndex (type == TFilter ? s.c : s.b, (int) r.size())].value;
        tremDepth = clampf (s.a, 0.0f, 1.0f);
        sweepCentre = expMap (f, 120.0f, 8000.0f);
        sweepOct = 3.0f * clampf (s.a, 0.0f, 1.0f);
        sweepQ = 0.6f + 12.0f * s.b * s.b;
    }

    // Swell, Reverse and Ice.
    swellRise = expMap (s.a, 30.0f, 4000.0f) * 0.001f * sr;
    revSmooth = clampf (s.a, 0.0f, 1.0f);
    iceRatio = std::pow (2.0f, (float) iceSemis (s.mode) / 12.0f);
    iceGrain = expMap (s.a, 15.0f, 400.0f) * 0.001f * sr;
    iceBlend = clampf (s.b, 0.0f, 1.0f);
    iceDuty = 1.0f - 0.85f * clampf (s.c, 0.0f, 1.0f);

    offset = type == TDigital ? (clampf (s.a, 0.0f, 1.0f) - 0.5f) * 0.5f : 0.0f;
}

// ---------------------------------------------------------------------------
// The write chain: everything the echoes go through on their way into the
// line, every time round.
// ---------------------------------------------------------------------------
float Engine::writeChain (Channel& c, int ch, float x)
{
    if (voiceOn)
        x = c.voiceHp.tick (c.voiceLp.tick (x));

    if (gritOn)
    {
        const float y = fastTanh (gritDrive * x + gritBias) - fastTanh (gritBias);
        x = c.gritLp.tick (y / std::pow (gritDrive, 0.75f));
    }

    if (smearOn)
    {
        static const float lens[] { 3.1f, 5.3f, 8.9f };
        for (int i = 0; i < 3; ++i)
            x = c.smear[(size_t) i].tick (x, lens[i] * smearScale, smearG);
    }

    switch (curType)
    {
        case TTape:
        {
            x = c.tapeBump.tick (x);
            x = c.tapeBias.tick (x);
            x = fastTanh (x * tapeDrive) / tapeDrive * 1.15f;
            x = c.tapeAge2.tick (c.tapeAge.tick (x)) * tapeLevel;
            x = c.tapeLow.tick (x);
            if (hiss > 0.0f) x += hiss * c.hissTone.lp (rng.bi());
            break;
        }
        case TBucket:
        {
            if (comp > 0.001f)
            {
                const float e = c.compEnv.tick (x);
                x *= clampf (std::pow (std::max (e, 1.0e-4f) / 0.25f, -0.5f * comp), 0.2f, 8.0f);
            }
            x = c.bbdIn2.tick (c.bbdIn.tick (x));
            if (bbdLoss > 0.001f)
            {
                // Loss: the chips saturate, and at low clock rates the steps show.
                const float y = fastTanh (x * (1.0f + 2.5f * bbdLoss)) / (1.0f + 1.5f * bbdLoss);
                if (bbdTick) c.bbdHeld = y;
                x = lerpf (y, c.bbdHeld, 0.75f * bbdLoss);
            }
            if (hiss > 0.0f) x += hiss * c.hissTone.lp (rng.bi());
            break;
        }
        case TLofi:
        {
            if (lofiTick || lofiRate >= sr * 0.999f) c.lofiHeld = x;
            x = c.lofiHeld;
            if (lofiQ > 0.0f) x = std::round (x * lofiQ) / lofiQ;
            break;
        }
        case TCloud:
        {
            static const float lens[] { 7.7f, 13.1f, 19.3f, 29.9f };
            float d = x;
            for (int i = 0; i < 4; ++i)
            {
                const float wob = 1.0f + 0.006f * std::sin (kTwoPi * (modPhase + 0.21f * (float) (i + 1) + 0.5f * (float) ch));
                d = c.cloud[(size_t) i].tick (d, lens[i] * cloudScale * wob, cloudG);
            }
            x = lerpf (x, d, cloudMix);
            break;
        }
        default: break;
    }

    x = softLimit (x, 0.9f, 1.3f);
    return c.dc2.tick (x);
}

float Engine::readMain (Channel& c, int, float d)
{
    return c.line.read (clampf (d, 1.0f, (float) (maxMain - 8)));
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
void Engine::process (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings& s)
{
    int done = 0;
    while (done < n)
    {
        const int len = std::min (kBlock, n - done);
        updateControls (s);
        processBlock (inL + done, inR + done, outL + done, outR + done, len, s);
        done += len;
    }
    // Recover from anything that went non-finite.
    if (! std::isfinite (outL[n - 1]) || ! std::isfinite (outR[n - 1]))
    {
        reset();
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
    }
}

void Engine::processBlock (const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings&)
{
    auto& L = chans[0];
    auto& R = chans[1];
    const int type = curType;
    const float fadeStep = 1.0f / (0.015f * sr);
    const float modSamples = modMs * 0.001f * sr;
    const float wowSamples = wowMs * 0.001f * sr, flutterSamples = flutterMs * 0.001f * sr;
    const float duckStep = 1.0f - std::exp (-1.0f / (0.004f * sr));
    const float tremSmooth = 1.0f - std::exp (-1.0f / (0.0015f * sr));
    const int heads = clampi (curMode, 0, 4);
    int numHeads = 0;
    for (bool on : kHeadsOn[heads]) numHeads += on ? 1 : 0;

    for (int i = 0; i < n; ++i)
    {
        const float ig = inGain.next();
        float xl = inL[i] * ig, xr = inR[i] * ig;
        inPeak = std::max (inPeak, std::max (std::fabs (xl), std::fabs (xr)));
        const float mono = 0.5f * (xl + xr);

        duckEnv.tick (mono);
        playEnv.tick (mono);
        peakEnv.tick (mono);
        duckGain += duckStep * (duckTarget - duckGain);
        fbDuck += duckStep * (fbDuckTarget - fbDuck);

        // Onsets, for Swell and Reverse.
        bool onset = false;
        if (type == TSwell || type == TReverse)
        {
            const float fa = onsetFast.tick (mono), sl = onsetSlow.tick (mono);
            if (swellArmed && fa > 1.9f * sl + 2.0e-4f && fa > 0.003f) { onset = true; swellArmed = false; }
            else if (! swellArmed && fa < 1.15f * sl) swellArmed = true;
        }

        float dryL = xl, dryR = xr;
        float sendL = xl, sendR = xr;
        if (type == TSwell)
        {
            // A new note: dip quickly (no click), then rise over RISE.
            if (onset) swellFalling = true;
            if (swellFalling)
            {
                swellGain -= 1.0f / (0.004f * sr);
                if (swellGain <= 0.0f) { swellGain = 0.0f; swellFalling = false; }
            }
            else
                swellGain = std::min (1.0f, swellGain + 1.0f / std::max (1.0f, swellRise));
            const float g = swellGain * swellGain;
            sendL *= g; sendR *= g;
            if (curMode == 1) { dryL *= g; dryR *= g; }
        }

        switch (curStereo)
        {
            case 1: sendL = 0.5f * (sendL + sendR); sendR = 0.0f; break;   // PING-PONG
            case 2: sendL = sendR = 0.5f * (sendL + sendR); break;          // MONO
            default: break;
        }

        const float h = holdS.next();
        const float open = 1.0f - h;
        sendL *= open; sendR *= open;

        // --- modulation ---------------------------------------------------------
        const float D = timeS.next();
        float mL = 0.0f, mR = 0.0f;
        modPhase += modHz / sr;
        modPhase -= std::floor (modPhase);
        if (modSamples > 0.0f)
        {
            const float drift = modDrift.tick (modHz * 0.37f + 0.05f, sr, rng);
            mL = modSamples * (1.0f + 0.8f * std::sin (kTwoPi * modPhase) + 0.2f * drift);
            mR = modSamples * (1.0f + 0.8f * std::sin (kTwoPi * (modPhase + 0.25f)) - 0.2f * drift);
        }
        float crk = 1.0f;
        if (type == TTape)
        {
            const float wow = wowDrift.tick (0.9f, sr, rng);
            flutterPhase += 7.3f / sr;
            flutterPhase -= std::floor (flutterPhase);
            const float fl = std::sin (kTwoPi * flutterPhase);
            mL = wowSamples * (1.0f + wow) + flutterSamples * (1.0f + fl);
            mR = wowSamples * (1.0f + 0.92f * wow) + flutterSamples * (1.0f + std::sin (kTwoPi * (flutterPhase + 0.1f)));
            if (crinkle > 0.01f)
            {
                if (crinkleLeft > 0.0f) { if ((crinkleLeft -= 1.0f) <= 0.0f) crinkleTarget = 1.0f; }
                else if (rng.uni() < crinkle * 2.5f / sr)
                {
                    crinkleTarget = 1.0f - (0.2f + 0.65f * crinkle) * rng.uni();
                    crinkleLeft = (0.006f + 0.045f * rng.uni()) * sr;
                }
                crinkleGain += 0.02f * (crinkleTarget - crinkleGain);
                crk = crinkleGain * (1.0f - 0.07f * crinkle * (1.0f + crinkleDrift.tick (14.0f, sr, rng)));
            }
        }

        // Clocks for the sample-and-hold stages.
        bbdTick = false;
        lofiTick = false;
        if (type == TBucket) { bbdPhase += bbdClock / sr; if (bbdPhase >= 1.0f) { bbdPhase -= std::floor (bbdPhase); bbdTick = true; } }
        if (type == TLofi)   { lofiPhase += lofiRate / sr; if (lofiPhase >= 1.0f) { lofiPhase -= std::floor (lofiPhase); lofiTick = true; } }

        // --- read the line --------------------------------------------------------
        // y: what goes round again. w: what you hear. raw: the plain echo, for INFINITE.
        float yL = 0.0f, yR = 0.0f, wL = 0.0f, wR = 0.0f;
        const float rawL = L.line.read (D), rawR = R.line.read (D);

        switch (type)
        {
            case TDigital:
            {
                const float dl = D * (1.0f + std::max (0.0f, -offset)) + mL;
                const float dr = D * (1.0f + std::max (0.0f, offset)) + mR;
                yL = wL = readMain (L, 0, dl);
                yR = wR = readMain (R, 1, dr);
                break;
            }
            case TPattern:
            {
                yL = readMain (L, 0, D + mL);
                yR = readMain (R, 1, D + mR);
                for (int k = 0; k < numTaps; ++k)
                {
                    const auto& tp = taps[(size_t) k];
                    const float d = std::max (1.0f, D * tp.at);
                    const float v = 0.5f * (L.line.read (d + mL) + R.line.read (d + mR));
                    wL += tp.gl * v;
                    wR += tp.gr * v;
                }
                wL *= tapNorm; wR *= tapNorm;
                break;
            }
            case TTape:
            {
                float sl = 0.0f, sr_ = 0.0f;
                for (int k = 0; k < 3; ++k)
                    if (kHeadsOn[heads][k])
                    {
                        sl += readMain (L, 0, D * kTapeHeads[k] + mL);
                        sr_ += readMain (R, 1, D * kTapeHeads[k] + mR);
                    }
                const float nh = (float) std::max (1, numHeads);
                yL = crk * sl / nh;  yR = crk * sr_ / nh;
                wL = crk * sl / std::sqrt (nh);  wR = crk * sr_ / std::sqrt (nh);
                break;
            }
            case TBucket:
            {
                yL = L.bbdOut2.tick (L.bbdOut.tick (readMain (L, 0, D + mL)));
                yR = R.bbdOut2.tick (R.bbdOut.tick (readMain (R, 1, D + mR)));
                if (comp > 0.001f)
                {
                    const float k = 0.5f * comp / (1.0f - 0.5f * comp);
                    const float el = L.expEnv.tick (yL), er = R.expEnv.tick (yR);
                    yL *= clampf (std::pow (std::max (el, 1.0e-4f) / 0.25f, k), 0.05f, 4.0f);
                    yR *= clampf (std::pow (std::max (er, 1.0e-4f) / 0.25f, k), 0.05f, 4.0f);
                }
                wL = yL; wR = yR;
                break;
            }
            case TReverse:
            {
                static const float rates[] { 1.0f, 0.5f, 2.0f };
                const float r = rates[clampi (curMode, 0, 2)];
                const float T = std::max (8.0f, D);
                if (onset && revArmed && wetEnv.env < 2.0e-3f) revPhase = 0.0f;
                revArmed = wetEnv.env < 2.0e-3f || revArmed;
                revPhase += 1.0f;
                if (revPhase >= T) revPhase -= T * std::floor (revPhase / T);
                const float p1 = revPhase, p2 = std::fmod (revPhase + 0.5f * T, T);
                const float fade = lerpf (0.03f, 0.5f, revSmooth) * T;
                const float w1 = smoothstep01 (std::min (p1, T - p1) / fade);
                const float w2 = revSmooth * smoothstep01 (std::min (p2, T - p2) / fade);
                const float d1 = 1.0f + (1.0f + r) * p1, d2 = 1.0f + (1.0f + r) * p2;
                yL = wL = w1 * L.line.read (d1) + w2 * L.line.read (d2);
                yR = wR = w1 * R.line.read (d1) + w2 * R.line.read (d2);
                break;
            }
            case TIce:
            {
                const float el = readMain (L, 0, D + mL), er = readMain (R, 1, D + mR);
                L.pitch.push (el);
                R.pitch.push (er);
                const float G = std::max (16.0f, iceGrain);
                icePhase += std::fabs (iceRatio - 1.0f) / G;
                icePhase -= std::floor (icePhase);
                float pl = 0.0f, pr = 0.0f;
                for (int k = 0; k < 2; ++k)
                {
                    float ph = icePhase + 0.5f * (float) k;
                    ph -= std::floor (ph);
                    const float env = ph < iceDuty ? std::pow (std::sin (kPi * ph / iceDuty), 2.0f) : 0.0f;
                    if (env <= 0.0f) continue;
                    const float d = 2.0f + (iceRatio > 1.0f ? G * (1.0f - ph) : G * ph);
                    pl += env * L.pitch.read (d);
                    pr += env * R.pitch.read (d);
                }
                yL = wL = lerpf (el, pl, iceBlend);
                yR = wR = lerpf (er, pr, iceBlend);
                break;
            }
            default:   // Dual, Lo-Fi, Trem, Filter, Swell, Cloud
                yL = wL = readMain (L, 0, D + mL);
                yR = wR = readMain (R, 1, D + mR);
                break;
        }

        // --- feedback ------------------------------------------------------------
        const float g = fbS.next() * fbDuck;
        float tgL = 1.0f, tgR = 1.0f;
        if (taperK > 0.0f)
        {
            const float thr = std::max (peakEnv.env * 0.35f, 1.0e-4f);
            const float el = L.taperEnv.tick (yL), er = R.taperEnv.tick (yR);
            if (el < thr) tgL = std::pow (el / thr, taperK);
            if (er < thr) tgR = std::pow (er / thr, taperK);
        }
        const float fbL = yL * g * tgL, fbR = yR * g * tgR;
        const float intoL = sendL + (curStereo == 1 ? fbR : fbL);
        const float intoR = sendR + (curStereo == 1 ? fbL : fbR);
        float writeL = writeChain (L, 0, intoL);
        float writeR = writeChain (R, 1, intoR);
        if (h > 0.0f)
        {
            writeL = open * writeL + h * (curStereo == 1 ? rawR : rawL);
            writeR = open * writeR + h * (curStereo == 1 ? rawL : rawR);
        }
        L.line.push (writeL);
        R.line.push (writeR);

        // Dual's second delay.
        if (type == TDual)
        {
            const float g2 = fb2S.next(), l2 = lvl2S.next();
            const float d2 = clampf (D * ratio2, 2.0f, (float) (maxSecond - 16));
            const float al = L.line2.read (d2 + mR), ar = R.line2.read (d2 + mL);
            const float raw2L = L.line2.read (d2), raw2R = R.line2.read (d2);
            float in2L, in2R;
            if (curMode == 0) { in2L = yL; in2R = yR; }                       // SERIES
            else { in2L = in2R = 0.5f * (sendL + sendR); }                     // PARALLEL
            float w2L = softLimit (in2L + g2 * al, 0.9f, 1.3f), w2R = softLimit (in2R + g2 * ar, 0.9f, 1.3f);
            if (h > 0.0f) { w2L = open * w2L + h * raw2L; w2R = open * w2R + h * raw2R; }
            L.line2.push (w2L);
            R.line2.push (w2R);
            if (curMode == 0) { wL = yL + l2 * al; wR = yR + l2 * ar; }
            else { wL = 0.5f * (yL + yR); wR = l2 * 0.5f * (al + ar); }
        }

        // --- the echoes' post stage ---------------------------------------------------
        if (type == TTrem || type == TFilter)
        {
            lfoPhase += lfoRatio / std::max (8.0f, D);
            if (lfoPhase >= 1.0f)
            {
                lfoPhase -= std::floor (lfoPhase);
                randomA = randomB;
                randomB = rng.bi();
            }
        }
        if (type == TTrem)
        {
            const float p = lfoPhase;
            float u;
            switch (clampi (curMode, 0, 4))
            {
                case 1:  u = std::fabs (1.0f - 2.0f * p); break;                                       // TRIANGLE
                case 2:  u = 0.5f + 0.5f * fastTanh (6.0f * std::cos (kTwoPi * p)) / fastTanh (3.0f); break; // SQUARE
                case 3:  u = 1.0f - p; break;                                                           // SAW
                case 4:  u = p; break;                                                                  // RAMP
                default: u = 0.5f + 0.5f * std::cos (kTwoPi * p); break;                               // SINE
            }
            const float target = 1.0f - tremDepth * (1.0f - clampf (u, 0.0f, 1.0f));
            tremGain += tremSmooth * (target - tremGain);
            wL *= tremGain; wR *= tremGain;
        }
        else if (type == TFilter)
        {
            const float p = lfoPhase;
            float v;
            switch (clampi (curMode, 0, 6))
            {
                case 1:  v = 1.0f - 4.0f * std::fabs (p - 0.5f); break;                               // TRIANGLE
                case 2:  v = fastTanh (6.0f * std::sin (kTwoPi * p)) / fastTanh (3.0f); break;        // SQUARE
                case 3:  v = 2.0f * p - 1.0f; break;                                                  // RAMP
                case 4:  v = 1.0f - 2.0f * p; break;                                                  // SAW
                case 5:  v = lerpf (randomA, randomB, smoothstep01 (p)); break;                       // RANDOM
                case 6:  v = clampf ((gainToDb (sweepEnv.tick (mono)) + 46.0f) / 18.0f, 0.0f, 2.0f) - 1.0f; break; // ENVELOPE
                default: v = std::sin (kTwoPi * p); break;                                            // SINE
            }
            sweepV += 0.01f * (v - sweepV);
            const float hz = sweepCentre * std::pow (2.0f, sweepOct * sweepV);
            L.sweep.set (hz, sweepQ, sr);
            R.sweep.set (hz, sweepQ, sr);
            L.sweep.tick (wL); R.sweep.tick (wR);
            const float comp_ = 1.0f / (1.0f + 0.06f * sweepQ);
            wL = L.sweep.lp * comp_; wR = R.sweep.lp * comp_;
        }
        else if (type == TLofi)
        {
            // Vinyl: the first half crackles with the echoes, the second all the time.
            if (vinyl > 0.005f)
            {
                const float e = wetEnv.env;
                const float amt = vinyl < 0.5f ? vinyl * 2.0f * clampf (e * 10.0f, 0.0f, 1.0f) : (vinyl - 0.5f) * 2.0f + 0.4f;
                if (rng.uni() < (4.0f + 26.0f * amt) / sr) crackleEnv = (0.05f + 0.25f * rng.uni() * rng.uni()) * amt;
                crackleEnv *= 0.985f;
                const float crackle = crackleEnv * rng.bi() - crackleTone.lp (crackleEnv * rng.bi());
                const float surface = 0.0025f * amt * rng.bi();
                wL += crackle + surface;
                wR += crackle - surface;
            }
            wL = L.shapePeak.tick (L.shapeLp.tick (L.shapeHp.tick (wL)));
            wR = R.shapePeak.tick (R.shapeLp.tick (R.shapeHp.tick (wR)));
            if (shapeDrive > 0.0f)
            {
                const float k = 1.0f + 5.0f * shapeDrive;
                wL = fastTanh (wL * k) / std::sqrt (k);
                wR = fastTanh (wR * k) / std::sqrt (k);
            }
        }

        // LOW CUT and HIGH CUT.
        if (lowCutLast > 20.5f)
        {
            wL = L.lowCut2.tick (L.lowCut.tick (wL));
            wR = R.lowCut2.tick (R.lowCut.tick (wR));
        }
        if (highCutLast < 19900.0f)
        {
            wL = L.highCut2.tick (L.highCut.tick (wL));
            wR = R.highCut2.tick (R.highCut.tick (wR));
        }

        // Type changes hide under a short fade.
        if (typeFade < 1.0f)
        {
            typeFade = std::min (1.0f, typeFade + fadeStep);
            const float tf = typeFade * typeFade;
            wL *= tf; wR *= tf;
        }

        wetEnv.tick (0.5f * (wL + wR));
        wL *= duckGain; wR *= duckGain;
        wetPeak = std::max (wetPeak, std::max (std::fabs (wL), std::fabs (wR)));

        // WIDTH, MIX, OUTPUT.
        const float wd = widthS.next();
        const float m = 0.5f * (wL + wR), sd = 0.5f * (wL - wR) * wd;
        wL = m + sd; wR = m - sd;
        const float dg = dryGain.next(), wg = wetGain.next(), og = outGain.next();
        const float oL = (dryL * dg + wL * wg) * og;
        const float oR = (dryR * dg + wR * wg) * og;
        outL[i] = oL;
        outR[i] = oR;
        outPeak = std::max (outPeak, std::max (std::fabs (oL), std::fabs (oR)));
    }
}
} // namespace lathe::dly
