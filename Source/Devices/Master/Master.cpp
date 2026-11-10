#include "Master.h"

namespace lathe::mst
{
// ===========================================================================
// Limiter: true-peak lookahead. Each driven sample pair goes through the 4x
// interpolator; the gain that would bring the highest interpolated point to
// the ceiling goes into a moving minimum one window long, then a release,
// then a moving average one window long. With the audio delayed by the
// interpolator's delay plus the window, the gain is fully down by the time
// the peak arrives, and it ramps rather than steps.
// ===========================================================================
void Master::Limiter::prepare (double rate)
{
    window = std::max (1, (int) std::lround (0.0015 * rate));
    size = latency() + 1;
    dl.assign ((size_t) size, 0.0f); dr = dl; rl = dl; rr = dl;
    box.assign ((size_t) window, 1.0f);
    dqVal.assign ((size_t) window + 1, 1.0f);
    dqIdx.assign ((size_t) window + 1, 0);
    reset();
}

void Master::Limiter::reset()
{
    tpl.reset(); tpr.reset();
    std::fill (dl.begin(), dl.end(), 0.0f); std::fill (dr.begin(), dr.end(), 0.0f);
    std::fill (rl.begin(), rl.end(), 0.0f); std::fill (rr.begin(), rr.end(), 0.0f);
    std::fill (box.begin(), box.end(), 1.0f);
    dqHead = dqCount = 0;
    t = 0;
    boxSum = window;
    hold = lastGain = 1.0f;
    w = gw = refresh = 0;
}

void Master::Limiter::tick (float l, float r, float drive, float ceiling, float& outL, float& outR, float& rawL, float& rawR)
{
    const float a = l * drive, b = r * drive;
    w = (w + 1) % size;
    dl[(size_t) w] = a; dr[(size_t) w] = b;
    rl[(size_t) w] = l; rr[(size_t) w] = r;

    float p = std::max (tpl.push (a), tpr.push (b));
    const int i6 = (w - TruePeak4x::kDelay + size) % size;
    p = std::max ({ p, std::fabs (dl[(size_t) i6]), std::fabs (dr[(size_t) i6]) });

    // Moving minimum of the wanted gain over the last `window` samples.
    const float g = p > ceiling ? ceiling / p : 1.0f;
    const int cap = window + 1;
    while (dqCount > 0 && dqVal[(size_t) ((dqHead + dqCount - 1) % cap)] >= g)
        --dqCount;
    dqVal[(size_t) ((dqHead + dqCount) % cap)] = g;
    dqIdx[(size_t) ((dqHead + dqCount) % cap)] = t;
    ++dqCount;
    while (dqIdx[(size_t) dqHead] <= t - window)
    {
        dqHead = (dqHead + 1) % cap;
        --dqCount;
    }
    ++t;
    const float gmin = dqVal[(size_t) dqHead];

    if (gmin < hold) hold = gmin;
    else             hold += (gmin - hold) * relCoef;

    boxSum += hold - box[(size_t) gw];
    box[(size_t) gw] = hold;
    gw = (gw + 1) % window;
    if (++refresh >= 8192)
    {
        refresh = 0;
        boxSum = 0;
        for (float v : box) boxSum += v;
    }
    const float s = std::min (1.0f, (float) (boxSum / window));
    lastGain = s;

    const int o = (w + 1) % size;   // the oldest: latency() behind
    outL = dl[(size_t) o] * s;
    outR = dr[(size_t) o] * s;
    rawL = rl[(size_t) o];
    rawR = rr[(size_t) o];
}

// ===========================================================================
void Master::prepare (double sampleRate)
{
    sr = sampleRate;
    limiter.prepare (sr);
    latencySamples = limiter.latency();
    dryDL.prepare (latencySamples);
    dryDR.prepare (latencySamples);
    inMeter.prepare (sr);
    inMeter.setTruePeak (false);
    outMeter.prepare (sr);

    const float fsr = (float) sr;
    smAmount.setTime (60.0f, fsr, kStep);
    smGain.setTime (60.0f, fsr, kStep);
    smDrive.setTime (60.0f, fsr, kStep);
    smMatch.setTime (150.0f, fsr, kStep);
    for (auto& s : smOn) s.setTime (25.0f, fsr, kStep);
    smCompare.setTime (25.0f, fsr, kStep);
    smMono.setTime (25.0f, fsr, kStep);

    envCoef = 1.0f - std::exp (-1.0f / (0.010f * fsr));
    for (auto* f : { &scL, &scR })
        f->highPass (90.0, 0.7071, sr);
    for (auto* t : { &toneL, &toneR })
        t->rumble.highPass (kRumbleHz, 0.7071, sr);
    toneShown = { 99, 99, 99, 99 };
    sideHz = 0.0f;
    reset();
}

void Master::reset()
{
    toneL.reset(); toneR.reset();
    scL.reset(); scR.reset(); sideHp.reset();
    glueEnv = 0.0; glueGr = 0.0f;
    limiter.reset();
    dryDL.reset(); dryDR.reset();
    inMeter.reset(); outMeter.reset();
    matchDb = 0.0f;
    firstBlock = true;
    tm = {};
}

float Master::staticGr (float levelDb) const
{
    const float over = levelDb - thresholdDb;
    const float slope = 1.0f - 1.0f / ratio;
    if (over <= -kKnee * 0.5f) return 0.0f;
    if (over < kKnee * 0.5f)   return slope * (over + kKnee * 0.5f) * (over + kKnee * 0.5f) / (2.0f * kKnee);
    return slope * over;
}

// ---------------------------------------------------------------------------
void Master::process (float* l, float* r, int n, const MasterSettings& s)
{
    if (firstBlock)
    {
        smAmount.snap (s.amount);
        smGain.snap (s.inputGainDb);
        smDrive.snap (s.driveOffsetDb + targetSpec (s.target).lufs + s.trimDb);
        for (int i = 0; i < NumStages; ++i) smOn[(size_t) i].snap (s.on[(size_t) i] ? 1.0f : 0.0f);
        smCompare.snap (s.compare ? 1.0f : 0.0f);
        smMono.snap (s.mono ? 1.0f : 0.0f);
        smMatch.snap (1.0f);
        rGain = dbToGain (s.inputGainDb);
        rDrive = dbToGain (s.driveOffsetDb + targetSpec (s.target).lufs + s.trimDb);
        rMatch = 1.0f;
        rCompare = smCompare.value;
        rMono = smMono.value;
        for (int i = 0; i < NumStages; ++i) rOn[(size_t) i] = smOn[(size_t) i].value;
        lastSetGain = s.inputGainDb;
        lastSetDrive = s.driveOffsetDb;
        firstBlock = false;
    }

    if (s.meterResets != seenResets)
    {
        seenResets = s.meterResets;
        outMeter.resetIntegrated();
        inMeter.resetIntegrated();
    }
    if (s.learnToggles != seenLearnToggles)
    {
        seenLearnToggles = s.learnToggles;
        if (isLearning) finishLearn (true);
        else            startLearn();
    }

    for (int start = 0; start < n; start += kStep)
    {
        const int len = std::min (kStep, n - start);
        updateControls (s);
        processChunk (l + start, r + start, len, s);
    }
}

void Master::updateControls (const MasterSettings& s)
{
    lastSetGain = s.inputGainDb;
    lastSetDrive = s.driveOffsetDb;

    smAmount.target = clampf (s.amount, 0.0f, 1.0f);
    const float amount = smAmount.next();
    const auto& rec = recipe (s.style);
    const auto& ts = targetSpec (s.target);

    const bool useLearned = isLearning || pendingAck;
    smGain.target = useLearned ? learnGain : s.inputGainDb;
    smDrive.target = (useLearned ? learnDrive : s.driveOffsetDb) + ts.lufs + s.trimDb;
    for (int i = 0; i < NumStages; ++i)
        smOn[(size_t) i].target = s.on[(size_t) i] ? 1.0f : 0.0f;
    smCompare.target = s.compare ? 1.0f : 0.0f;
    smMono.target = s.mono ? 1.0f : 0.0f;
    limitOnNow = s.on[LimitStage];

    // TONE
    const std::array<float, 4> want { rec.lowDb * amount, rec.mudDb * amount, rec.presenceDb * amount, rec.airDb * amount };
    bool changed = false;
    for (int b = 0; b < 4; ++b)
        changed = changed || std::fabs (want[(size_t) b] - toneShown[(size_t) b]) > 0.005f;
    if (changed)
    {
        toneShown = want;
        for (auto* t : { &toneL, &toneR })
        {
            t->low.lowShelf (kLowHz, want[0], sr);
            t->mud.peak (kMudHz, 0.9, want[1], sr);
            t->presence.peak (kPresenceHz, 0.8, want[2], sr);
            t->air.highShelf (kAirHz, want[3], sr);
        }
    }

    // GLUE
    ratio = 1.0f + (rec.ratio - 1.0f) * amount;
    thresholdDb = -12.0f - 12.0f * rec.depth * std::pow (amount, 0.6f);
    attCoef = 1.0f - std::exp (-(1.0f) / (rec.attackMs * 0.001f * (float) sr));
    relCoef = 1.0f - std::exp (-(1.0f) / (rec.releaseMs * 0.001f * (float) sr));
    makeupDb = 0.5f * staticGr (kReferenceLufs - 2.0f);

    // COLOR
    satG = 2.2f * rec.drive * amount;

    // WIDTH
    sideGain = 1.0f + rec.width * amount;
    const float hz = ts.monoBassHz > 0.0f ? ts.monoBassHz : 20.0f + 80.0f * amount;
    if (std::fabs (hz - sideHz) > 0.5f)
    {
        sideHz = hz;
        sideHp.highPass (hz, 0.7071, sr);
    }

    // LIMIT
    limiter.relCoef = 1.0f - std::exp (-1.0f / (rec.limitReleaseMs * 0.001f * (float) sr));
    ceilingLin = dbToGain (ts.ceilingDb);
    targetLufs = ts.lufs;

    // Telemetry that doesn't depend on the audio.
    const float tOn = s.on[ToneStage] ? 1.0f : 0.0f;
    for (int b = 0; b < 4; ++b) tm.toneDb[(size_t) b] = want[(size_t) b] * tOn;
    tm.glueRatio = ratio;
    tm.glueThresholdDb = thresholdDb;
    tm.colorDrive = s.on[ColorStage] ? rec.drive * amount : 0.0f;
    tm.widthPct = s.on[WidthStage] ? sideGain * 100.0f : 100.0f;
    tm.monoBassHz = s.on[WidthStage] ? hz : 0.0f;
    tm.ceilingDb = ts.ceilingDb;
}

void Master::processChunk (float* l, float* r, int n, const MasterSettings&)
{
    // Ramp targets for this chunk.
    const float gainDb = smGain.next(), driveDb = smDrive.next();
    appliedGainDb = gainDb;
    appliedDriveDb = driveDb;
    const float g1 = dbToGain (gainDb), d1 = dbToGain (driveDb);
    smMatch.target = dbToGain (matchDb);
    const float m1 = smMatch.next();
    std::array<float, NumStages> on1 {};
    for (int i = 0; i < NumStages; ++i) on1[(size_t) i] = smOn[(size_t) i].next();
    const float c1 = smCompare.next(), mo1 = smMono.next();

    const float inv = 1.0f / (float) n;
    const float g0 = rGain, d0 = rDrive, m0 = rMatch, c0 = rCompare, mo0 = rMono;
    const auto on0 = rOn;

    float rawL[kStep], rawR[kStep], outL[kStep], outR[kStep];
    float minLimit = 1.0f, maxGr = 0.0f, inPk = 0.0f, outPk = 0.0f, cIn = 0.0f, cOut = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float t = (float) (i + 1) * inv;
        const float g = g0 + (g1 - g0) * t, d = d0 + (d1 - d0) * t;
        const float mTone = on0[0] + (on1[0] - on0[0]) * t, mGlue = on0[1] + (on1[1] - on0[1]) * t,
                    mColor = on0[2] + (on1[2] - on0[2]) * t, mWidth = on0[3] + (on1[3] - on0[3]) * t,
                    mLimit = on0[4] + (on1[4] - on0[4]) * t;

        float inL = l[i], inR = r[i];
        if (! std::isfinite (inL)) inL = 0.0f;
        if (! std::isfinite (inR)) inR = 0.0f;
        rawL[i] = inL; rawR[i] = inR;
        inPk = std::max (inPk, std::max (std::fabs (inL), std::fabs (inR)));
        const float dryL = dryDL.tick (inL), dryR = dryDR.tick (inR);

        float xl = inL * g, xr = inR * g;

        // TONE
        {
            const float yl = toneL.tick (xl), yr = toneR.tick (xr);
            xl += (yl - xl) * mTone;
            xr += (yr - xr) * mTone;
        }

        // GLUE
        {
            const float sl = scL.tick (xl), sr2 = scR.tick (xr);
            glueEnv += envCoef * (0.5 * ((double) sl * sl + (double) sr2 * sr2) - glueEnv);
            const float lvl = 10.0f * std::log10 ((float) glueEnv + 1.0e-12f);
            const float want = staticGr (lvl);
            glueGr += (want > glueGr ? attCoef : relCoef) * (want - glueGr);
            maxGr = std::max (maxGr, glueGr * mGlue);
            const float gg = dbToGain (makeupDb - glueGr);
            xl += (xl * gg - xl) * mGlue;
            xr += (xr * gg - xr) * mGlue;
        }

        // COLOR
        if (satG > 1.0e-3f)
        {
            cIn = std::max (cIn, std::max (std::fabs (xl), std::fabs (xr)));
            const float yl = std::tanh (satG * xl) / satG, yr = std::tanh (satG * xr) / satG;
            xl += (yl - xl) * mColor;
            xr += (yr - xr) * mColor;
            cOut = std::max (cOut, std::max (std::fabs (xl), std::fabs (xr)));
        }

        // WIDTH
        {
            const float mid = 0.5f * (xl + xr), side = 0.5f * (xl - xr);
            const float s2 = sideHp.tick (side) * sideGain;
            const float wl = mid + s2, wr = mid - s2;
            xl += (wl - xl) * mWidth;
            xr += (wr - xr) * mWidth;
        }

        // DRIVE + LIMIT
        float ol, orr, ul, ur;
        limiter.tick (xl, xr, d, ceilingLin, ol, orr, ul, ur);
        minLimit = std::min (minLimit, limiter.lastGain);
        float yl = ul + (ol - ul) * mLimit, yr = ur + (orr - ur) * mLimit;
        if (mLimit > 0.999f)
        {
            yl = clampf (yl, -ceilingLin, ceilingLin);
            yr = clampf (yr, -ceilingLin, ceilingLin);
        }
        outL[i] = yl; outR[i] = yr;
        outPk = std::max (outPk, std::max (std::fabs (yl), std::fabs (yr)));

        // COMPARE: the original at the master's loudness.
        const float c = c0 + (c1 - c0) * t;
        const float mg = m0 + (m1 - m0) * t;
        float fl = yl + (dryL * mg - yl) * c;
        float fr = yr + (dryR * mg - yr) * c;

        // MONO
        const float mo = mo0 + (mo1 - mo0) * t;
        const float mono = 0.5f * (fl + fr);
        fl += (mono - fl) * mo;
        fr += (mono - fr) * mo;

        l[i] = fl;
        r[i] = fr;
    }

    rGain = g1; rDrive = d1; rMatch = m1; rCompare = c1; rMono = mo1; rOn = on1;

    // Display values fall back slowly so the panel can read them.
    const float fall = 18.0f * (float) n / (float) sr;   // dB per chunk
    tm.glueGrDb = std::max (maxGr, tm.glueGrDb - fall);
    const float limGr = limitOnNow ? -gainToDb (minLimit) : 0.0f;
    tm.limitGrDb = std::max (limGr, tm.limitGrDb - fall);
    const float cDb = (cIn > 1.0e-4f && cOut > 0.0f) ? -gainToDb (cOut / cIn) : 0.0f;
    tm.colorDb = std::max (std::max (0.0f, cDb), tm.colorDb - fall);
    tm.inPeak = std::max (tm.inPeak, inPk);
    tm.outPeak = std::max (tm.outPeak, outPk);
    tm.inputGainDb = appliedGainDb;
    tm.driveDb = limitOnNow ? appliedDriveDb : 0.0f;

    inMeter.process (rawL, rawR, n);
    if (outMeter.process (outL, outR, n) > 0)
        onHop();
}

// ---------------------------------------------------------------------------
void Master::onHop()
{
    tm.momentary = outMeter.momentary();
    tm.shortTerm = outMeter.shortTerm();
    tm.integrated = outMeter.integrated();
    tm.truePeakDb = outMeter.truePeakDb();
    tm.inShortTerm = inMeter.shortTerm();

    // COMPARE gain: follow the short-term difference between master and original.
    if (tm.shortTerm > -60.0f && tm.inShortTerm > -60.0f)
    {
        const float want = clampf (tm.shortTerm - tm.inShortTerm, -30.0f, 30.0f);
        matchDb += 0.25f * (want - matchDb);
    }
    tm.matchDb = matchDb;

    if (pendingAck && ! isLearning)
    {
        if ((std::fabs (lastSetGain - learnGain) < 0.01f && std::fabs (lastSetDrive - learnDrive) < 0.01f) || ++ackHops > 30)
            pendingAck = false;
    }

    tm.learning = isLearning;
    if (! isLearning)
        return;

    ++learnHops;
    const double ein = inMeter.blockEnergy();
    const float lin = LoudnessMeter::toLufs (ein);
    if (lin < -60.0f) ++silentHops;
    else { ++heardHops; silentHops = 0; }
    tm.learnSeconds = (float) heardHops * 0.1f;

    if (inMeter.hops() >= 4 && lin > LoudnessMeter::kSilence)
    {
        const int bin = std::clamp ((int) ((lin - LoudnessMeter::kSilence) * 10.0f), 0, kBins - 1);
        const float applied = appliedGainDb + (limitOnNow ? appliedDriveDb : 0.0f);
        const float lnorm = LoudnessMeter::toLufs (outMeter.blockEnergy()) - applied;
        lInE[(size_t) bin] += ein;
        lOutE[(size_t) bin] += LoudnessMeter::toEnergy (lnorm);
        lCount[(size_t) bin] += 1;
    }

    // Gated integrated loudness of the input, and of the output normalised
    // by the gain that was in use, over the same blocks.
    double sum = 0; int cnt = 0;
    for (int b = 0; b < kBins; ++b) { sum += lInE[(size_t) b]; cnt += lCount[(size_t) b]; }
    if (cnt > 0 && heardHops >= 10)
    {
        const float gate = LoudnessMeter::toLufs (sum / cnt) - 10.0f;
        double ie = 0, oe = 0; int gc = 0;
        for (int b = 0; b < kBins; ++b)
        {
            const int c = lCount[(size_t) b];
            if (c == 0 || LoudnessMeter::toLufs (lInE[(size_t) b] / c) < gate) continue;
            ie += lInE[(size_t) b]; oe += lOutE[(size_t) b]; gc += c;
        }
        if (gc > 0)
        {
            const float iin = LoudnessMeter::toLufs (ie / gc);
            const float inorm = LoudnessMeter::toLufs (oe / gc);
            const float newGain = clampf (kReferenceLufs - iin, -24.0f, 24.0f);
            // inorm already excludes the gain in use; the drive this asks for
            // is relative to the new input gain and leaves PUSH out.
            const float newDrive = clampf (targetLufs - inorm - newGain, -6.0f, 24.0f) - targetLufs;
            learnGain += 0.5f * (newGain - learnGain);
            learnDrive += 0.5f * (newDrive - learnDrive);
        }
    }

    if ((heardHops >= 100 && silentHops >= 30) || learnHops >= 6000)
        finishLearn (false);
}

void Master::startLearn()
{
    isLearning = true;
    pendingAck = false;
    heardHops = silentHops = learnHops = 0;
    lInE.fill (0.0); lOutE.fill (0.0); lCount.fill (0);
    startGain = learnGain = lastSetGain;
    startDrive = learnDrive = lastSetDrive;
    inMeter.resetIntegrated();
    outMeter.resetIntegrated();
    tm.learning = true;
    tm.learnSeconds = 0.0f;
}

void Master::finishLearn (bool)
{
    isLearning = false;
    tm.learning = false;
    pendingAck = true;
    ackHops = 0;
    if (heardHops < 50)
    {
        learnGain = startGain;
        learnDrive = startDrive;
        result = { LearnResult::TooShort, startGain, startDrive };
        return;
    }
    learnGain = std::round (learnGain * 10.0f) / 10.0f;
    learnDrive = std::round (learnDrive * 10.0f) / 10.0f;
    result = { LearnResult::Done, learnGain, learnDrive };
}
} // namespace lathe::mst
