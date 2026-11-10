// Reflections.cpp — the REFLECTIONS machine. The room's walls, floor and
// ceiling are mirrored to find where every reflection of the source comes
// from (up to fifth order: a few hundred of them), and each one reaches each
// ear at its own time and level. Moving the source recalculates them, with a
// short crossfade. A small tank adds the room's tail.
#include "Machines.h"

namespace lathe::rvb
{
namespace
{
constexpr float kSpeed = 343.0f;
constexpr int kOrder = 5;
constexpr float kListenerZ = 1.35f, kSourceZ = 1.5f, kEar = 0.09f;
} // namespace

void ReflectionsMachine::prepare (float sampleRate)
{
    sr = sampleRate;
    line.prepare ((int) (0.45f * sr));
    tail.prepare (sr, 90.0f);
    for (auto& f : lowCut) f.setCutoff (45.0f, sr);
    for (auto& f : lowSplit) f.setCutoff (220.0f, sr);
    for (auto& f : highSplit) f.setCutoff (220.0f, sr);
    reset();
}

void ReflectionsMachine::reset()
{
    line.reset();
    tail.reset();
    for (auto& f : dullLp) f.reset();
    for (auto& f : brightLp) f.reset();
    for (auto& f : lowCut) f.reset();
    for (auto& f : lowSplit) f.reset();
    for (auto& f : highSplit) f.reset();
    wander.reset();
    lastX = lastY = lastSize = -1.0f;
    lastShape = -1;
    fade = 1.0f;
    fadeStep = 0.0f;
}

void ReflectionsMachine::compute (TapSet& ts, float x, float y, float size, int shape)
{
    const float area = 9.3f + size * (93.0f - 9.3f);   // 100 to 1000 sq ft
    const float aspect = shape == 1 ? 1.618f : shape == 2 ? 1.0f / 1.618f : 1.0f;
    const float W = std::sqrt (area * aspect), D = area / W, H = 2.5f + 1.0f * size;

    const float lx = 0.5f * W, ly = std::min (1.2f, 0.2f * D);
    const float sx = 0.4f + x * (W - 0.8f);
    const float sy = ly + 0.7f + y * std::max (0.2f, D - ly - 1.1f);

    const float d0 = std::sqrt ((sx - lx) * (sx - lx) + (sy - ly) * (sy - ly) + (kSourceZ - kListenerZ) * (kSourceZ - kListenerZ));
    const float wall = 0.78f;
    const float maxDelay = (float) line.maxDelay() - 0.01f * sr;

    ts.nl = ts.nr = 0;
    float energy = 0.0f;
    for (int i = -kOrder; i <= kOrder; ++i)
        for (int j = -kOrder; j <= kOrder; ++j)
        {
            const int o2 = std::abs (i) + std::abs (j);
            if (o2 > kOrder) continue;
            const float ix = (float) i * W + ((i & 1) ? (W - sx) : sx);
            const float iy = (float) j * D + ((j & 1) ? (D - sy) : sy);
            for (int kz = 0; kz < 3; ++kz)
            {
                const int order = o2 + (kz > 0 ? 1 : 0);
                if (order == 0 || order > kOrder) continue;
                const float iz = kz == 0 ? kSourceZ : kz == 1 ? -kSourceZ : 2.0f * H - kSourceZ;
                const float dy = iy - ly, dz = iz - kListenerZ;
                for (int ear = 0; ear < 2; ++ear)
                {
                    const float ex = lx + (ear == 0 ? -kEar : kEar);
                    const float dx = ix - ex;
                    const float d = std::sqrt (dx * dx + dy * dy + dz * dz);
                    float g = (d0 / std::max (0.3f, d)) * std::pow (wall, (float) order);
                    // The head shades the far ear.
                    const bool far = (ear == 0 && ix > lx + 0.2f) || (ear == 1 && ix < lx - 0.2f);
                    if (far) g *= 0.62f;
                    const float delay = clampf ((d - d0) / kSpeed * sr + 0.002f * sr, 1.0f, maxDelay);
                    Tap t { delay, (order & 1) ? -g : g, far || order > 1 };
                    if (ear == 0 && ts.nl < kMaxTaps) ts.l[(size_t) ts.nl++] = t;
                    if (ear == 1 && ts.nr < kMaxTaps) ts.r[(size_t) ts.nr++] = t;
                    energy += g * g;
                }
            }
        }
    // Physically the reflections get as loud as the direct sound at the back
    // of the room, and hundreds of them add up to far more. Keep the total in
    // hand: it still grows towards the back, but gently. Highs add up by
    // energy; lows add up in phase (the plain sum of the gains), so the two
    // bands get their own level.
    const float target = std::sqrt (0.45f + 0.75f * y);
    const float spread = std::sqrt (std::max (1.0e-4f, 0.5f * energy));
    // The low band's level is the strongest response among a few low
    // frequencies, where the taps add with their phases.
    float coherent = spread;
    for (float hz : { 40.0f, 60.0f, 85.0f, 120.0f, 170.0f })
        for (int ear = 0; ear < 2; ++ear)
        {
            const auto& taps = ear == 0 ? ts.l : ts.r;
            const int count = ear == 0 ? ts.nl : ts.nr;
            double re = 0.0, im = 0.0;
            const double w = 2.0 * 3.141592653589793 * hz / sr;
            for (int t = 0; t < count; ++t)
            {
                re += taps[(size_t) t].gain * std::cos (w * taps[(size_t) t].delay);
                im -= taps[(size_t) t].gain * std::sin (w * taps[(size_t) t].delay);
            }
            coherent = std::max (coherent, (float) std::sqrt (re * re + im * im));
        }
    ts.norm = target / spread * 2.4f;
    ts.lowNorm = target / coherent * 0.9f;
}

void ReflectionsMachine::set (const Controls& c, int blockSamples)
{
    const float x = c.a, y = c.b;
    const int shape = std::max (0, std::min (2, c.mode));
    const bool moved = std::abs (x - lastX) > 0.002f || std::abs (y - lastY) > 0.002f
                    || std::abs (c.size - lastSize) > 0.002f || shape != lastShape;
    if (moved && fade >= 1.0f)
    {
        const int next = 1 - current;
        compute (sets[(size_t) next], x, y, c.size, shape);
        if (lastShape < 0)
        {
            current = next;   // first time: no fade
            fade = 1.0f;
        }
        else
        {
            current = next;
            fade = 0.0f;
            fadeStep = 1.0f / (0.03f * sr);
        }
        lastX = x; lastY = y; lastSize = c.size; lastShape = shape;
    }

    pan = clampf ((x - 0.5f) * 1.6f, -1.0f, 1.0f);
    erGain = c.early * 1.7f;
    tailGain = 0.35f + 0.45f * y;
    wanderDepth = c.c * 0.004f * sr;

    for (int ch = 0; ch < 2; ++ch)
    {
        brightLp[(size_t) ch].setCutoff (std::min (18000.0f, c.toneHz * 1.6f), sr);
        dullLp[(size_t) ch].setCutoff (std::max (700.0f, c.toneHz * 0.55f), sr);
    }

    Tank::Params p;
    const float s = 0.6f + 0.9f * c.size;
    p.minMs = 7.0f * s;
    p.maxMs = 33.0f * s;
    p.rt60 = c.decay;
    p.lowMult = c.lowMult;
    p.hfRatio = c.hfRatio;
    p.hfHz = c.dampHz;
    p.modDepthMs = c.modDepth * c.modDepth * 0.8f;
    p.modRateHz = c.modRate;
    p.infinite = c.infinite;
    tail.set (p, blockSamples);
}

void ReflectionsMachine::process (const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const auto& cur = sets[(size_t) current];
    const auto& old = sets[(size_t) (1 - current)];
    for (int i = 0; i < n; ++i)
    {
        line.push (0.5f * (inL[i] + inR[i]));
        const float off = 1.0f + wanderDepth * (1.0f + wander.tick (0.35f, sr, rng));

        auto sum = [&] (const std::array<Tap, kMaxTaps>& taps, int count, float& bright, float& dull)
        {
            for (int t = 0; t < count; ++t)
            {
                const auto& tp = taps[(size_t) t];
                const float v = tp.gain * line.readLinear (tp.delay + off);
                if (tp.dull) dull += v; else bright += v;
            }
        };
        // Each set's sums at its high-band and low-band levels, crossfaded.
        float bl = 0, dl = 0, br = 0, dr = 0;
        sum (cur.l, cur.nl, bl, dl);
        sum (cur.r, cur.nr, br, dr);
        float hbl = bl * cur.norm, hdl = dl * cur.norm, hbr = br * cur.norm, hdr = dr * cur.norm;
        float loL = (bl + dl) * cur.lowNorm, loR = (br + dr) * cur.lowNorm;
        if (fade < 1.0f)
        {
            float obl = 0, odl = 0, obr = 0, odr = 0;
            sum (old.l, old.nl, obl, odl);
            sum (old.r, old.nr, obr, odr);
            const float f = fade, g = 1.0f - fade;
            hbl = hbl * f + obl * old.norm * g; hdl = hdl * f + odl * old.norm * g;
            hbr = hbr * f + obr * old.norm * g; hdr = hdr * f + odr * old.norm * g;
            loL = loL * f + (obl + odl) * old.lowNorm * g;
            loR = loR * f + (obr + odr) * old.lowNorm * g;
            fade = std::min (1.0f, fade + fadeStep);
        }

        const float el = (highSplit[0].hp (brightLp[0].lp (hbl) + dullLp[0].lp (hdl)) + lowCut[0].hp (lowSplit[0].lp (loL))) * 0.32f;
        const float er = (highSplit[1].hp (brightLp[1].lp (hbr) + dullLp[1].lp (hdr)) + lowCut[1].hp (lowSplit[1].lp (loR))) * 0.32f;

        float tl, tr;
        tail.tick (el, er, tl, tr);
        outL[i] = erGain * el + tailGain * tl;
        outR[i] = erGain * er + tailGain * tr;
    }
}
} // namespace lathe::rvb
