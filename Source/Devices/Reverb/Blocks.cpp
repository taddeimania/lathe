#include "Blocks.h"
#include "Types.h"

namespace lathe::rvb
{
// ===========================================================================
// Tank
// ===========================================================================
namespace
{
// Where each line sits between the shortest and longest length (0..1). The
// order is shuffled so the left and right inputs (even and odd lines) each
// get a spread of lengths.
constexpr float kLinePos[Tank::N] { 0.00f, 0.53f, 0.27f, 0.80f, 0.13f, 0.66f, 0.40f, 0.93f,
                                    0.07f, 0.60f, 0.33f, 0.87f, 0.20f, 0.73f, 0.47f, 1.00f };
// Small offsets so no two lines share a common factor.
constexpr float kLineNudge[Tank::N] { 1.000f, 1.013f, 0.991f, 1.007f, 1.019f, 0.987f, 1.003f, 0.995f,
                                      1.011f, 0.983f, 1.017f, 0.997f, 1.005f, 0.989f, 1.009f, 0.993f };
// Signs for injecting and tapping the lines, so left and right decorrelate.
constexpr float kInSign[Tank::N]  {  1, -1,  1,  1, -1,  1, -1, -1,  1,  1, -1,  1, -1, -1,  1, -1 };
constexpr float kOutL[Tank::N]    {  1,  1, -1,  1, -1, -1,  1,  1, -1,  1,  1, -1,  1, -1, -1,  1 };
constexpr float kOutR[Tank::N]    {  1, -1,  1,  1,  1, -1, -1,  1,  1,  1, -1, -1, -1,  1, -1, -1 };
} // namespace

void Tank::prepare (float sampleRate, float maxLineMs)
{
    sr = sampleRate;
    maxLen = maxLineMs * 0.001f * sr;
    for (auto& l : lines)
        l.prepare ((int) (maxLen + 0.02f * sr));
    for (int i = 0; i < N; ++i)
    {
        phase[(size_t) i] = (float) i / (float) N;
        rate[(size_t) i] = 0.62f + 0.76f * std::fmod ((float) i * 0.381966f, 1.0f);
    }
    lenCoeff = 1.0f - std::exp (-1.0f / (0.25f * sr));
    reset();
}

void Tank::reset()
{
    for (auto& l : lines) l.reset();
    for (auto& x : xover) x.reset();
    for (auto& x : hfSplit) x.reset();
    for (auto& w : wander) w.reset();
    mod.fill (0.0f);
    modStep.fill (0.0f);
    first = true;
}

void Tank::set (const Params& p, int blockSamples)
{
    infinite = p.infinite;
    const float lo = std::max (2.0f, p.minMs), hi = std::max (lo + 1.0f, p.maxMs);
    const float depthTarget = std::max (0.0f, p.modDepthMs) * 0.001f * sr;
    depth = depthTarget;

    float meanLen = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const size_t k = (size_t) i;
        lenTarget[k] = std::min (maxLen, expMap (kLinePos[k], lo, hi) * kLineNudge[k] * 0.001f * sr);
        if (first) len[k] = lenTarget[k];
        else len[k] += (1.0f - std::pow (1.0f - lenCoeff, (float) blockSamples)) * (lenTarget[k] - len[k]);
        len[k] = std::max (len[k], depth + 8.0f);
        meanLen += len[k];

        // Three bands: lows below the crossover, highs above hfHz, each with
        // its own gain per pass so each decays in its own time.
        const float g = infinite ? 1.0f : decayGain (len[k], p.rt60, sr);
        const float gl = infinite ? 1.0f : decayGain (len[k], std::min (90.0f, p.rt60 * p.lowMult), sr);
        const float gh = infinite ? 1.0f : decayGain (len[k], p.rt60 * clampf (p.hfRatio, 0.03f, 1.0f), sr);
        gain[k] = g;
        lowExtra[k] = gl - g;
        highExtra[k] = gh - g;
        xover[k].setCutoff (p.xoverHz, sr);
        hfSplit[k].setCutoff (p.hfHz, sr);

        // Modulation: a slow sine per line plus a wandering offset, both
        // computed at block rate and ramped across the block.
        phase[k] += p.modRateHz * rate[k] * (float) blockSamples / sr;
        phase[k] -= std::floor (phase[k]);
        const float w = wander[k].tick (p.modRateHz * 0.7f * (float) blockSamples, sr, rng);
        const float m = depth * (0.5f + 0.32f * std::sin (kTwoPi * phase[k]) + 0.18f * w);
        modStep[k] = first ? 0.0f : (m - mod[k]) / (float) std::max (1, blockSamples);
        if (first) mod[k] = m;
    }
    meanLen /= (float) N;
    const float gbar = decayGain (meanLen, p.rt60, sr);
    normGain = std::sqrt (std::max (1.0e-4f, 1.0f - gbar * gbar));
    first = false;
}

void Tank::tick (float inL, float inR, float& outL, float& outR)
{
    std::array<float, N> y;
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const size_t k = (size_t) i;
        mod[k] += modStep[k];
        const float v = lines[k].read (len[k] - mod[k]);
        const float lowPart = xover[k].lp (v);
        const float highPart = v - hfSplit[k].lp (v);
        const float d = gain[k] * v + lowExtra[k] * lowPart + highExtra[k] * highPart;
        y[k] = d;
        l += kOutL[k] * d;
        r += kOutR[k] * d;
    }
    hadamard (y);
    constexpr float inScale = 0.35355339f;   // sqrt(2 / N): each input reaches eight lines
    for (int i = 0; i < N; ++i)
    {
        const size_t k = (size_t) i;
        float w = y[k] + kInSign[k] * ((i & 1) ? inR : inL) * inScale;
        w = softLimit (w, 1.5f, 3.0f);
        lines[k].push (w);
    }
    const float s = normGain * 0.25f * 1.41421356f;
    outL = l * s;
    outR = r * s;
}

// ===========================================================================
// Diffuser
// ===========================================================================
namespace
{
constexpr float kDiffRatio[Diffuser::kMax] { 1.0f, 0.77f, 0.61f, 0.47f, 0.37f, 0.29f, 0.21f, 0.15f };
}

void Diffuser::prepare (float sampleRate, float maxMs, uint32_t seed)
{
    sr = sampleRate;
    maxLen = maxMs * 0.001f * sr;
    Rng r (seed);
    for (int i = 0; i < kMax; ++i)
    {
        ap[(size_t) i].prepare ((int) (maxLen * kDiffRatio[i] * 1.15f + 0.01f * sr + 8));
        jitter[(size_t) i] = 0.9f + 0.2f * r.uni();
        lfo[(size_t) i].phase = r.uni();
    }
    reset();
}

void Diffuser::reset()
{
    for (auto& a : ap) a.reset();
    first = true;
}

void Diffuser::set (int s, float longestMs, float gain, float modDepthMs, float modRateHz)
{
    stages = std::max (0, std::min (kMax, s));
    g = clampf (gain, 0.0f, 0.85f);
    depth = modDepthMs * 0.001f * sr;
    rate = modRateHz;
    for (int i = 0; i < kMax; ++i)
    {
        const size_t k = (size_t) i;
        target[k] = clampf (longestMs * 0.001f * sr * kDiffRatio[i] * jitter[k], 2.0f + depth, maxLen * kDiffRatio[i] * 1.1f);
        len[k] = first ? target[k] : len[k] + 0.05f * (target[k] - len[k]);
    }
    first = false;
}

float Diffuser::tick (float x)
{
    if (depth > 0.0f)
    {
        for (int i = 0; i < stages; ++i)
        {
            const size_t k = (size_t) i;
            const float m = depth * (0.5f + 0.5f * lfo[k].tick (rate * (0.7f + 0.11f * (float) i), sr));
            x = ap[k].tick (x, len[k] - m, g);
        }
        return x;
    }
    for (int i = 0; i < stages; ++i)
        x = ap[(size_t) i].tick (x, len[(size_t) i], g);
    return x;
}

// ===========================================================================
// EarlyTaps
// ===========================================================================
void EarlyTaps::prepare (float sampleRate, float maxMs)
{
    sr = sampleRate;
    line.prepare ((int) (maxMs * 0.001f * sr) + 64);
    hpL.setCutoff (90.0f, sr);
    hpR.setCutoff (90.0f, sr);
    reset();
}

void EarlyTaps::reset()
{
    line.reset();
    lpL.reset(); lpR.reset(); hpL.reset(); hpR.reset();
}

void EarlyTaps::setTone (float lpHz)
{
    lpL.setCutoff (lpHz, sr);
    lpR.setCutoff (lpHz * 1.06f, sr);
}

void EarlyTaps::configure (int pattern, float windowMs, float distance)
{
    if (pattern == lastPattern && std::abs (windowMs - lastWindow) < 0.003f * lastWindow
        && std::abs (distance - lastDistance) < 0.003f)
        return;
    lastPattern = pattern;
    lastWindow = windowMs;
    lastDistance = distance;

    static const int counts[] { 0, 12, 18, 22, 26 };
    count = counts[std::max (0, std::min (4, pattern))];
    // Rooms gather early; halls leave a gap, then a dense build.
    static const float firstFrac[] { 0.0f, 0.06f, 0.08f, 0.14f, 0.18f };
    static const float curve[] { 1.0f, 0.9f, 0.85f, 0.75f, 0.7f };
    const float f0 = firstFrac[std::max (0, std::min (4, pattern))] + 0.25f * distance;
    const float cv = curve[std::max (0, std::min (4, pattern))];
    const float maxDelay = (float) line.maxDelay() - 4.0f;

    float energy = 0.0f;
    for (int side = 0; side < 2; ++side)
    {
        auto& taps = side == 0 ? left : right;
        Rng r (side == 0 ? 1301u + (uint32_t) pattern : 2711u + (uint32_t) pattern);
        for (int i = 0; i < count; ++i)
        {
            const float u = ((float) i + 0.3f + 0.4f * r.uni()) / (float) count;
            const float t = f0 + (1.0f - f0) * std::pow (u, cv);
            const float ms = std::max (1.0f, windowMs * t);
            // Gains fall with time; distance flattens the fall.
            const float fall = std::exp (-(2.2f - 1.6f * distance) * t);
            const float sign = r.uni() < 0.5f ? -1.0f : 1.0f;
            taps[(size_t) i].delay = std::min (maxDelay, ms * 0.001f * sr);
            taps[(size_t) i].gain = sign * fall * (0.55f + 0.45f * r.uni());
            energy += taps[(size_t) i].gain * taps[(size_t) i].gain;
        }
    }
    norm = count > 0 ? 1.0f / std::sqrt (std::max (1.0e-3f, energy * 0.5f)) : 0.0f;
}

void EarlyTaps::tick (float in, float& outL, float& outR)
{
    line.push (in);
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        l += left[(size_t) i].gain * line.readLinear (left[(size_t) i].delay);
        r += right[(size_t) i].gain * line.readLinear (right[(size_t) i].delay);
    }
    outL = hpL.hp (lpL.lp (l * norm));
    outR = hpR.hp (lpR.lp (r * norm));
}

// ===========================================================================
// PitchShifter
// ===========================================================================
void PitchShifter::prepare (float sampleRate)
{
    sr = sampleRate;
    line.prepare ((int) (0.2f * sr));
    window = 0.075f * sr;
    reset();
}

void PitchShifter::reset()
{
    line.reset();
    phase = 0.0f;
}

void PitchShifter::setShift (int cents)
{
    const float ratio = std::pow (2.0f, (float) cents / 1200.0f);
    // Tiny shifts want a long window so the heads don't flutter.
    window = (std::abs (cents) < 50 ? 0.14f : 0.075f) * sr;
    step = (1.0f - ratio) / window;
}

float PitchShifter::tick (float x)
{
    line.push (x);
    phase += step;
    phase -= std::floor (phase);
    const float p2 = phase + 0.5f - std::floor (phase + 0.5f);
    const float base = 0.003f * sr;
    const float a = line.read (base + phase * window);
    const float b = line.read (base + p2 * window);
    const float wa = std::sin (kPi * phase), wb = std::sin (kPi * p2);
    return a * wa + b * wb;
}

// ===========================================================================
// FormantBank
// ===========================================================================
namespace
{
constexpr float kVowels[4][3] {
    { 730.0f, 1090.0f, 2440.0f },   // AH
    { 570.0f, 840.0f, 2410.0f },    // OH
    { 300.0f, 870.0f, 2240.0f },    // OO
    { 270.0f, 2290.0f, 3010.0f } }; // EE
constexpr float kFormantGain[3] { 1.0f, 0.7f, 0.4f };
}

void FormantBank::prepare (float sampleRate)
{
    sr = sampleRate;
    for (int v = 0; v < kVoices; ++v)
    {
        auto& s = singers[(size_t) v];
        s.offset = 1.0f + 0.07f * ((float) v - 2.5f) / 2.5f;
        s.pan = (v % 2 == 0 ? -1.0f : 1.0f) * (0.25f + 0.12f * (float) v);
    }
    reset();
}

void FormantBank::reset()
{
    for (auto& s : singers)
    {
        for (auto& f : s.f) f.reset();
        s.drift.reset();
    }
    holdLeft = 0;
}

void FormantBank::set (int vowel, float resonance, float voices, float drift, int blockSamples)
{
    q = 2.5f + resonance * resonance * 16.0f;
    active = 1 + (int) std::lround (voices * (float) (kVoices - 1));
    gain = std::pow (q, 0.3f) * 1.1f / std::sqrt ((float) active);

    // Pick the target vowel.
    if (vowel <= 3)
        target = { kVowels[vowel][0], kVowels[vowel][1], kVowels[vowel][2] };
    else if (vowel <= 5)
    {
        const int a = vowel == 4 ? 0 : 1, b = vowel == 4 ? 1 : 2;
        morphPhase += 0.12f * (float) blockSamples / sr;
        morphPhase -= std::floor (morphPhase);
        morph = 0.5f - 0.5f * std::cos (kTwoPi * morphPhase);
        for (int i = 0; i < 3; ++i)
            target[(size_t) i] = lerpf (kVowels[a][i], kVowels[b][i], morph);
    }
    else
    {
        holdLeft -= blockSamples;
        if (holdLeft <= 0)
        {
            const int v = (int) (rng.uni() * 3.999f);
            target = { kVowels[v][0], kVowels[v][1], kVowels[v][2] };
            holdLeft = (int) ((1.2f + 2.5f * rng.uni()) * sr);
        }
    }
    // Glide towards it.
    const float glide = 1.0f - std::exp (-(float) blockSamples / (0.25f * sr));
    for (int i = 0; i < 3; ++i)
        current[(size_t) i] += glide * (target[(size_t) i] - current[(size_t) i]);

    for (int v = 0; v < active; ++v)
    {
        auto& s = singers[(size_t) v];
        const float d = s.drift.tick (0.35f * (float) blockSamples, sr, rng);
        const float spread = 1.0f + (s.offset - 1.0f) * (0.4f + voices);
        const float wobble = 1.0f + 0.035f * drift * d;
        for (int i = 0; i < 3; ++i)
            s.f[(size_t) i].set (current[(size_t) i] * spread * wobble, q * (1.0f - 0.15f * (float) i), sr);
    }
}

void FormantBank::tick (float inL, float inR, float& outL, float& outR)
{
    float l = 0.0f, r = 0.0f;
    for (int v = 0; v < active; ++v)
    {
        auto& s = singers[(size_t) v];
        const float x = (v & 1) ? inR : inL;
        float y = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            s.f[(size_t) i].tick (x);
            y += s.f[(size_t) i].bp * kFormantGain[i];
        }
        const float pl = std::sqrt (0.5f * (1.0f - s.pan)), pr = std::sqrt (0.5f * (1.0f + s.pan));
        l += y * pl;
        r += y * pr;
    }
    outL = l * gain;
    outR = r * gain;
}

// ===========================================================================
// CombBank
// ===========================================================================
void CombBank::prepare (float sampleRate)
{
    sr = sampleRate;
    for (auto& c : combs)
        c.d.prepare ((int) (sr / 30.0f) + 8);
    reset();
}

void CombBank::reset()
{
    for (auto& c : combs) { c.d.reset(); c.damp.reset(); }
}

void CombBank::set (int rootPc, int chord, float rt60, float toneHz)
{
    if (rootPc != lastRoot || chord != lastChord)
    {
        lastRoot = rootPc;
        lastChord = chord;
        const auto& semis = chordSemis (chord);
        count = 0;
        // The chord in three octaves from C2.
        for (int oct = 0; oct < 3 && count < kMax; ++oct)
            for (int s : semis)
            {
                if (count >= kMax) break;
                if (oct > 0 && s == 12) continue;   // the octave is the next root
                const float note = 36.0f + (float) rootPc + (float) s + 12.0f * (float) oct;
                const float hz = 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f);
                auto& c = combs[(size_t) count];
                c.delay = sr / hz;
                c.pan = (count % 2 == 0 ? -1.0f : 1.0f) * (0.2f + 0.6f * (float) oct / 2.0f);
                ++count;
            }
        norm = 0.6f / std::sqrt ((float) std::max (1, count));
    }
    for (int i = 0; i < count; ++i)
    {
        auto& c = combs[(size_t) i];
        c.g = decayGain (c.delay, rt60, sr);
        c.norm = std::sqrt (std::max (1.0e-4f, 1.0f - c.g * c.g));
        c.damp.setCutoff (toneHz, sr);
    }
}

void CombBank::tick (float inL, float inR, float& outL, float& outR)
{
    float l = 0.0f, r = 0.0f;
    const float x = 0.5f * (inL + inR);
    for (int i = 0; i < count; ++i)
    {
        auto& c = combs[(size_t) i];
        const float y = c.d.read (c.delay);
        c.d.push (x + c.g * c.damp.lp (y));
        const float pl = std::sqrt (0.5f * (1.0f - c.pan)), pr = std::sqrt (0.5f * (1.0f + c.pan));
        l += y * pl * c.norm;
        r += y * pr * c.norm;
    }
    outL = l * norm;
    outR = r * norm;
}
} // namespace lathe::rvb
