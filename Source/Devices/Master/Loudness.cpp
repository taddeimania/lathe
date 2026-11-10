#include "Loudness.h"

namespace lathe::mst
{
void LoudnessMeter::prepare (double sampleRate)
{
    sr = sampleRate;
    // K-weighting: the BS.1770 high shelf and RLB high-pass, re-derived for
    // this sample rate the way libebur128 does it, so 48 kHz gives the
    // standard's own coefficients.
    for (auto* k : { &kl, &kr })
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            k->shelf.set ((Vh + Vb * K / Q + K * K), 2.0 * (K * K - Vh), (Vh - Vb * K / Q + K * K),
                          a0, 2.0 * (K * K - 1.0), (1.0 - K / Q + K * K));
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            k->hp.set (a0, -2.0 * a0, a0, a0, 2.0 * (K * K - 1.0), (1.0 - K / Q + K * K));
        }
    }
    hopLen = std::max (1, (int) std::lround (sr * 0.1));
    reset();
}

void LoudnessMeter::reset()
{
    kl.reset(); kr.reset();
    tpl.reset(); tpr.reset();
    inHop = 0; hopSum = 0; hopCount = 0; hopWrite = 0;
    hopEnergy.fill (0.0);
    tpRecent = 0.0f;
    resetIntegrated();
}

void LoudnessMeter::resetIntegrated()
{
    binEnergy.fill (0.0);
    binCount.fill (0);
    gatedBlocks = 0;
    tpHold = 0.0f;
}

int LoudnessMeter::process (const float* l, const float* r, int n)
{
    int completed = 0;
    for (int i = 0; i < n; ++i)
    {
        const float a = kl.tick (l[i]), b = kr.tick (r[i]);
        hopSum += (double) a * a + (double) b * b;

        if (truePeakOn)
        {
            const float p = std::max (tpl.push (l[i]), tpr.push (r[i]));
            tpHold = std::max (tpHold, p);
            tpRecent = std::max (tpRecent, p);
        }

        if (++inHop >= hopLen)
        {
            hopEnergy[(size_t) hopWrite] = hopSum / hopLen;
            hopWrite = (hopWrite + 1) % (int) hopEnergy.size();
            hopSum = 0;
            inHop = 0;
            ++hopCount;
            ++completed;

            if (hopCount >= 4)
            {
                const double e = meanOfLast (4);
                const float lufs = toLufs (e);
                if (lufs > kSilence)
                {
                    const int bin = std::clamp ((int) ((lufs - kSilence) * 10.0f), 0, kBins - 1);
                    binEnergy[(size_t) bin] += e;
                    binCount[(size_t) bin] += 1;
                    ++gatedBlocks;
                }
            }
        }
    }
    return completed;
}

double LoudnessMeter::meanOfLast (int count) const
{
    const int have = std::min (count, hopCount);
    if (have <= 0)
        return 0.0;
    double s = 0;
    for (int k = 1; k <= have; ++k)
        s += hopEnergy[(size_t) ((hopWrite - k + (int) hopEnergy.size()) % (int) hopEnergy.size())];
    // Until the window has filled, the missing hops count as silence, as
    // they would on a meter that had been running.
    return s / count;
}

float LoudnessMeter::integrated() const
{
    if (gatedBlocks == 0)
        return kSilence;
    double sum = 0;
    int count = 0;
    for (int b = 0; b < kBins; ++b) { sum += binEnergy[(size_t) b]; count += binCount[(size_t) b]; }
    const float relGate = toLufs (sum / count) - 10.0f;

    double gs = 0;
    int gc = 0;
    for (int b = 0; b < kBins; ++b)
    {
        // A bin straddling the gate is judged by its mean.
        if (binCount[(size_t) b] == 0) continue;
        if (toLufs (binEnergy[(size_t) b] / binCount[(size_t) b]) >= relGate)
        {
            gs += binEnergy[(size_t) b];
            gc += binCount[(size_t) b];
        }
    }
    return gc > 0 ? toLufs (gs / gc) : kSilence;
}
} // namespace lathe::mst
