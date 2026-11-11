#include "MixAnalysis.h"

namespace lathe::assistant
{
const MixAnalysis::Band MixAnalysis::bands[kBands] = {
    { "sub (20-60 Hz)",          20.0f,    60.0f },
    { "bass (60-250 Hz)",        60.0f,   250.0f },
    { "low mids (250-500 Hz)",  250.0f,   500.0f },
    { "mids (500 Hz-2 kHz)",    500.0f,  2000.0f },
    { "upper mids (2-4 kHz)",  2000.0f,  4000.0f },
    { "presence (4-6 kHz)",    4000.0f,  6000.0f },
    { "air (6-20 kHz)",        6000.0f, 20000.0f },
};

namespace
{
double toDb (double power)                { return power > 1.0e-12 ? 10.0 * std::log10 (power) : -120.0; }
double rounded (double v, double step)    { return std::round (v / step) * step; }
var obj()                                 { return var (new DynamicObject()); }
void set (var& o, const Identifier& k, const var& v)   { o.getDynamicObject()->setProperty (k, v); }

// A window counts as playing above this mean square (-60 dBFS).
constexpr float kActive = 1.0e-6f;
// In a band, a track takes part when it carries at least this share of the mix's power there.
constexpr float kPartOfBand = 0.1f;
} // namespace

MixAnalysis::MixAnalysis (std::vector<Tap> tapList, double sr, double perBar, double first, int64 lengthSamples, int sectionLength)
    : sampleRate (sr), samplesPerBar (perBar), firstBar (first), length (lengthSamples),
      sectionBars (jmax (1, sectionLength)),
      numBars (jmax (1, (int) std::ceil ((double) lengthSamples / perBar - 1.0e-6)))
{
    for (auto& t : tapList)
    {
        auto s = std::make_unique<TapState>();
        s->tap = t;
        s->l.reserve (kWindow);
        s->r.reserve (kWindow);
        s->barEnergy.assign ((size_t) numBars, 0.0);
        s->barLoudness.assign ((size_t) numBars, 0.0);
        s->meter = std::make_unique<mst::LoudnessMeter>();
        s->meter->prepare (sr);
        s->barSamples.assign ((size_t) numBars, 0);
        taps.push_back (std::move (s));
    }
    hann.resize (kWindow);
    for (int i = 0; i < kWindow; ++i)
        hann[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) kWindow);
    // K-weighting for this sample rate (RBJ biquads, as most BS.1770 meters build it).
    {
        const double fc = 1500.0, gain = 4.0, q = 1.0 / std::sqrt (2.0);
        const double A = std::pow (10.0, gain / 40.0), w = MathConstants<double>::twoPi * fc / sr;
        const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q), sa = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1) - (A - 1) * c + sa;
        kCoeffs[0][0] = A * ((A + 1) + (A - 1) * c + sa) / a0;
        kCoeffs[0][1] = -2.0 * A * ((A - 1) + (A + 1) * c) / a0;
        kCoeffs[0][2] = A * ((A + 1) + (A - 1) * c - sa) / a0;
        kCoeffs[0][3] = 2.0 * ((A - 1) - (A + 1) * c) / a0;
        kCoeffs[0][4] = ((A + 1) - (A - 1) * c - sa) / a0;
    }
    {
        const double fc = 38.0, q = 0.5, w = MathConstants<double>::twoPi * fc / sr;
        const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        kCoeffs[1][0] = (1.0 + c) / 2.0 / a0;
        kCoeffs[1][1] = -(1.0 + c) / a0;
        kCoeffs[1][2] = (1.0 + c) / 2.0 / a0;
        kCoeffs[1][3] = -2.0 * c / a0;
        kCoeffs[1][4] = (1.0 - alpha) / a0;
    }
    scratch.resize (kWindow);
    twiddles.resize (kWindow / 2);
    for (int k = 0; k < kWindow / 2; ++k)
        twiddles[(size_t) k] = std::polar (1.0, -MathConstants<double>::twoPi * k / kWindow);
    bitReverse.resize (kWindow);
    for (int i = 0; i < kWindow; ++i)
    {
        int r = 0;
        for (int b = 0; b < kWindowOrder; ++b)
            if (i & (1 << b))
                r |= 1 << (kWindowOrder - 1 - b);
        bitReverse[(size_t) i] = r;
    }

    mixMeter = std::make_unique<mst::LoudnessMeter>();
    mixMeter->prepare (sr);
    mixMeter->setTruePeak (true);
    const int sections = (numBars + sectionBars - 1) / sectionBars;
    for (int i = 0; i < sections; ++i)
    {
        sectionMeters.push_back (std::make_unique<mst::LoudnessMeter>());
        sectionMeters.back()->prepare (sr);
    }
}

MixAnalysis::~MixAnalysis() = default;

double MixAnalysis::kWeight (TapState& s, int ch, double x) const
{
    for (int stage = 0; stage < 2; ++stage)
    {
        const auto* k = kCoeffs[stage];
        auto* z = s.kState[ch][stage];
        const double y = k[0] * x + z[0];
        z[0] = k[1] * x - k[3] * y + z[1];
        z[1] = k[2] * x - k[4] * y;
        x = y;
    }
    return x;
}

void MixAnalysis::transform (std::vector<std::complex<double>>& x) const
{
    for (int i = 0; i < kWindow; ++i)
        if (const int j = bitReverse[(size_t) i]; i < j)
            std::swap (x[(size_t) i], x[(size_t) j]);
    for (int size = 2; size <= kWindow; size <<= 1)
    {
        const int half = size / 2, step = kWindow / size;
        for (int start = 0; start < kWindow; start += size)
            for (int k = 0; k < half; ++k)
            {
                const auto t = twiddles[(size_t) (k * step)] * x[(size_t) (start + k + half)];
                x[(size_t) (start + k + half)] = x[(size_t) (start + k)] - t;
                x[(size_t) (start + k)] += t;
            }
    }
}

void MixAnalysis::add (int index, const AudioBuffer<float>& buffer, int start, int count, int64 timelineSample)
{
    if (index < 0 || index >= (int) taps.size() || count <= 0)
        return;
    auto& s = *taps[(size_t) index];
    const float* l = buffer.getReadPointer (0, start);
    const float* r = buffer.getReadPointer (buffer.getNumChannels() > 1 ? 1 : 0, start);

    // Only the bars asked about: not the tail after them.
    const int64 inRange = jlimit ((int64) 0, (int64) count, length - timelineSample);
    if (inRange <= 0)
        return;
    count = (int) inRange;

    if (! s.tap.isMix)
        s.meter->process (l, r, count);
    if (s.tap.isMix)
    {
        mixMeter->process (l, r, count);
        // Each section's own loudness: split the block where a section starts.
        const double perSection = samplesPerBar * sectionBars;
        int done = 0;
        while (done < count)
        {
            const int64 at = timelineSample + done;
            const int section = jlimit (0, (int) sectionMeters.size() - 1, (int) ((double) at / perSection));
            const int64 sectionEnd = (int64) std::ceil ((section + 1) * perSection);
            const int n = (int) jlimit ((int64) 1, (int64) (count - done), sectionEnd - at);
            sectionMeters[(size_t) section]->process (l + done, r + done, n);
            done += n;
        }
    }

    for (int i = 0; i < count; ++i)
    {
        const float a = l[i], b = r[i];
        const auto bar = (size_t) jlimit (0, numBars - 1, (int) ((double) (timelineSample + i) / samplesPerBar));
        s.barEnergy[bar] += (double) a * a + (double) b * b;
        ++s.barSamples[bar];
        const double ka = kWeight (s, 0, a), kb = kWeight (s, 1, b);
        s.kSum += ka * ka + kb * kb;
        s.barLoudness[bar] += ka * ka + kb * kb;
        s.sumL += (double) a * a;
        s.sumR += (double) b * b;
        s.sumX += (double) a * b;
        const float m = jmax (std::abs (a), std::abs (b));
        s.peak = jmax (s.peak, m);
        if (m > 1.0f)
            ++s.overs;
        if (s.windowStart < 0)
            s.windowStart = timelineSample + i;
        s.l.push_back (a);
        s.r.push_back (b);
        if ((int) s.l.size() == kWindow)
            finishWindow (s);
    }
    s.samples += count;
}

void MixAnalysis::finishWindow (TapState& s) const
{
    const int n = (int) s.l.size();
    if (n == 0)
        return;

    Window w;
    double timePower = 0.0;
    for (int i = 0; i < n; ++i)
        timePower += (double) s.l[(size_t) i] * s.l[(size_t) i] + (double) s.r[(size_t) i] * s.r[(size_t) i];
    timePower /= 2.0 * n;

    // Both channels in one complex transform: left as the real part, right as the imaginary.
    std::fill (scratch.begin(), scratch.end(), std::complex<double>());
    for (int i = 0; i < n; ++i)
        scratch[(size_t) i] = { (double) s.l[(size_t) i] * hann[(size_t) i], (double) s.r[(size_t) i] * hann[(size_t) i] };
    transform (scratch);

    double pl[kBands] {}, pr[kBands] {}, px[kBands] {};
    const double binHz = sampleRate / kWindow;
    int band = 0;
    for (int k = 1; k <= kWindow / 2; ++k)
    {
        const double hz = k * binHz;
        if (hz < bands[0].low)
            continue;
        while (band < kBands && hz >= bands[band].high)
            ++band;
        if (band >= kBands)
            break;
        // Pull the two channels apart: L(k) = (Z(k) + conj Z(N-k)) / 2, R(k) = (Z(k) - conj Z(N-k)) / 2i.
        const auto z = scratch[(size_t) k], zc = std::conj (scratch[(size_t) ((kWindow - k) % kWindow)]);
        const auto L = (z + zc) * 0.5, R = (z - zc) * std::complex<double> (0.0, -0.5);
        pl[band] += std::norm (L);
        pr[band] += std::norm (R);
        px[band] += (L * std::conj (R)).real();
    }
    for (int b = 0; b < kBands; ++b)
    {
        s.bandL[b] += pl[b];
        s.bandR[b] += pr[b];
        s.bandX[b] += px[b];
        w.bandPower[b] = (float) ((pl[b] + pr[b]) * 0.5);
        w.power += w.bandPower[b];
    }

    const int64 middle = s.windowStart + n / 2;
    w.bar = jlimit (0, numBars - 1, (int) ((double) middle / samplesPerBar));
    w.timePower = (float) timePower;
    if (timePower <= kActive)
        w.power = 0.0f, std::fill (std::begin (w.bandPower), std::end (w.bandPower), 0.0f);   // silence is silence
    s.windows.push_back (w);

    s.l.clear();
    s.r.clear();
    s.windowStart = -1;
}

std::vector<bool> MixAnalysis::TapState::barActive() const
{
    // A bar counts as played when the track is above -60 dBFS over at least a 16th of it.
    std::vector<bool> on (barEnergy.size(), false);
    for (size_t i = 0; i < barEnergy.size(); ++i)
        on[i] = barSamples[i] > 0 && barEnergy[i] / 2.0 > kActive * (double) barSamples[i] / 16.0;
    return on;
}

String MixAnalysis::barRanges (const std::vector<bool>& active) const
{
    StringArray parts;
    for (int i = 0; i < (int) active.size(); ++i)
    {
        if (! active[(size_t) i])
            continue;
        int j = i;
        while (j + 1 < (int) active.size() && active[(size_t) (j + 1)])
            ++j;
        const int a = (int) firstBar + i, b = (int) firstBar + j;
        parts.add (a == b ? String (a) : String (a) + "-" + String (b));
        i = j;
    }
    return parts.joinIntoString (", ");
}

var MixAnalysis::bandBalance (const TapState& s) const
{
    double total = 0.0;
    for (int b = 0; b < kBands; ++b)
        total += s.bandL[b] + s.bandR[b];
    var o = obj();
    for (int b = 0; b < kBands; ++b)
        set (o, bands[b].name, rounded (toDb ((s.bandL[b] + s.bandR[b]) / jmax (1.0e-20, total)), 0.1));
    return o;
}

float MixAnalysis::loudestBandShare (const TapState& s, int& best) const
{
    double total = 0.0, top = -1.0;
    best = 0;
    for (int b = 0; b < kBands; ++b)
    {
        const double p = s.bandL[b] + s.bandR[b];
        total += p;
        if (p > top)
            top = p, best = b;
    }
    return total > 0.0 ? (float) (top / total) : 0.0f;
}

namespace
{
/** Stereo, from summed squares and the cross term: correlation, how wide (side over mid) and
    which way it leans. */
var stereoOf (double l, double r, double x)
{
    var o = obj();
    if (l + r <= 1.0e-12)
        return o;
    const double mid = (l + r + 2.0 * x) * 0.25, side = jmax (0.0, (l + r - 2.0 * x) * 0.25);
    const double width = toDb (side) - toDb (mid);
    set (o, "correlation", rounded (x / std::sqrt (jmax (1.0e-20, l * r)), 0.01));
    if (width < -40.0)
        set (o, "width", "mono");
    else
        set (o, "side_vs_mid_db", rounded (width, 0.1));
    const double lean = toDb (l) - toDb (r);
    if (std::abs (lean) >= 0.5)
        set (o, "leans", (lean > 0 ? "left by " : "right by ") + String (std::abs (lean), 1) + " dB");
    return o;
}
} // namespace

var MixAnalysis::tapReport (const TapState& s, const TapState* mix) const
{
    var o = obj();
    set (o, "name", s.tap.name);
    if (! s.tap.isMix)
        set (o, "kind", s.tap.isReturn ? "return" : "track");
    if (s.peak <= 0.0f)
    {
        set (o, "silent", true);
        return o;
    }
    set (o, "peak_dbfs", rounded (Decibels::gainToDecibels (s.peak, -120.0f), 0.1));
    if (s.overs > 0)
        set (o, "samples_over_0dbfs", s.overs);

    const double meanSquare = (s.sumL + s.sumR) / (2.0 * jmax ((int64) 1, s.samples));
    if (! s.tap.isMix)
    {
        set (o, "plays_in_bars", barRanges (s.barActive()));
        // Its level while it plays, not averaged with its rests.
        double playing = 0.0;
        int windows = 0;
        for (auto& w : s.windows)
            if (w.timePower > kActive)
                playing += w.timePower, ++windows;
        if (windows > 0)
            set (o, "level_while_playing_dbfs", rounded (toDb (playing / windows), 0.1));
        if (s.meter->hasIntegrated())
            set (o, "loudness_lufs", rounded (s.meter->integrated(), 0.1));
        if (mix != nullptr)
        {
            // By loudness (K-weighted, as a loudness meter hears it), not raw energy, which the
            // bass always wins: bright parts sound louder than their energy says.
            double tracks = 0.0;
            for (auto& t : taps)
                if (! t->tap.isMix)
                    tracks += t->kSum;
            set (o, "share_of_loudness_percent", rounded (100.0 * s.kSum / jmax (1.0e-20, tracks), 1.0));
        }
        int band = 0;
        const float share = loudestBandShare (s, band);
        set (o, "most_energy_in", String (bands[band].name) + ", " + String (roundToInt (share * 100.0f)) + "%");
    }
    else
    {
        set (o, "rms_dbfs", rounded (toDb (meanSquare), 0.1));
        set (o, "crest_db", rounded (Decibels::gainToDecibels (s.peak, -120.0f) - toDb (meanSquare), 0.1));
        set (o, "true_peak_dbtp", rounded (jmax (-120.0f, mixMeter->truePeakDb()), 0.1));
        set (o, "integrated_lufs", mixMeter->hasIntegrated() ? var (rounded (mixMeter->integrated(), 0.1)) : var ("too quiet to measure"));
        set (o, "band_balance_db", bandBalance (s));
    }

    var stereo = stereoOf (s.sumL, s.sumR, s.sumX);
    if (s.tap.isMix)
    {
        // The low end on its own: wide or out-of-phase bass is what goes wrong on mono speakers.
        const double l = s.bandL[0] + s.bandL[1], r = s.bandR[0] + s.bandR[1], x = s.bandX[0] + s.bandX[1];
        if (l + r > 0.0)
            set (stereo, "correlation_below_250_hz", rounded (x / std::sqrt (jmax (1.0e-20, l * r)), 0.01));
    }
    set (o, "stereo", stereo);
    return o;
}

var MixAnalysis::report()
{
    for (auto& s : taps)
        if ((int) s->l.size() >= kWindow / 4)
            finishWindow (*s);

    const TapState* mix = nullptr;
    for (auto& s : taps)
        if (s->tap.isMix)
            mix = s.get();

    var out = obj();
    set (out, "bars", String ((int) firstBar) + " to " + String ((int) firstBar + numBars));
    set (out, "seconds", rounded ((double) length / sampleRate, 0.1));
    if (mix != nullptr)
        set (out, "mix", tapReport (*mix, mix));

    // Sections: how loud, and who's loudest.
    if (mix != nullptr)
    {
        Array<var> sections;
        for (int sec = 0; sec < (int) sectionMeters.size(); ++sec)
        {
            const int a = sec * sectionBars, b = jmin (numBars, a + sectionBars) - 1;
            var o = obj();
            set (o, "bars", String ((int) firstBar + a) + (b > a ? "-" + String ((int) firstBar + b) : String()));
            auto& m = *sectionMeters[(size_t) sec];
            set (o, "loudness_lufs", m.hasIntegrated() ? var (rounded (m.integrated(), 0.1)) : var ("silent"));
            std::vector<std::pair<double, String>> who;
            double all = 0.0;
            for (auto& t : taps)
            {
                if (t->tap.isMix || t->tap.isReturn)
                    continue;
                double e = 0.0;
                for (int bar = a; bar <= b; ++bar)
                    e += t->barLoudness[(size_t) bar];
                all += e;
                if (e > 0.0)
                    who.push_back ({ e, t->tap.name });
            }
            std::sort (who.begin(), who.end(), [] (auto& x, auto& y) { return x.first > y.first; });
            StringArray loudest;
            for (size_t i = 0; i < who.size() && i < 3; ++i)
                loudest.add (who[i].second + " " + String (roundToInt (100.0 * who[i].first / jmax (1.0e-20, all))) + "%");
            if (! loudest.isEmpty())
                set (o, "loudest_tracks", loudest.joinIntoString (", "));
            sections.add (o);
        }
        set (out, "sections", sections);
    }

    Array<var> trackList;
    for (auto& s : taps)
        if (! s->tap.isMix)
            trackList.add (tapReport (*s, mix));
    set (out, "tracks", trackList);

    // Masking: two tracks loud in the same band at the same time, within 6 dB of each other.
    if (mix != nullptr)
    {
        struct Clash { double score; var info; };
        std::vector<Clash> clashes;
        const auto& mw = mix->windows;
        for (size_t i = 0; i < taps.size(); ++i)
            for (size_t j = i + 1; j < taps.size(); ++j)
            {
                auto& a = *taps[i];
                auto& b = *taps[j];
                if (a.tap.isMix || b.tap.isMix || a.tap.isReturn || b.tap.isReturn || a.peak <= 0.0f || b.peak <= 0.0f)
                    continue;
                const size_t n = jmin (mw.size(), a.windows.size(), b.windows.size());
                Clash best { 0.0, {} };
                for (int band = 0; band < kBands; ++band)
                {
                    int either = 0, both = 0;
                    double diff = 0.0, bandEnergy = 0.0, mixEnergy = 0.0;
                    for (size_t w = 0; w < n; ++w)
                    {
                        const float m = mw[w].bandPower[band];
                        mixEnergy += mw[w].power;
                        if (m <= 0.0f || m < mw[w].power * 1.0e-3f)
                            continue;   // the band hardly counts in this stretch of the mix
                        const float pa = a.windows[w].bandPower[band], pb = b.windows[w].bandPower[band];
                        const bool inA = pa >= m * kPartOfBand, inB = pb >= m * kPartOfBand;
                        if (inA || inB)
                            ++either;
                        if (inA && inB)
                        {
                            const double d = toDb (pa) - toDb (pb);
                            if (std::abs (d) <= 6.0)
                            {
                                ++both;
                                diff += d;
                                bandEnergy += m;
                            }
                        }
                    }
                    if (both < 4 || either == 0)
                        continue;
                    const double share = (double) both / either;
                    if (share < 0.3)
                        continue;
                    const double score = share * bandEnergy / jmax (1.0e-20, mixEnergy);
                    if (score > best.score)
                    {
                        var o = obj();
                        Array<var> names;
                        names.add (a.tap.name);
                        names.add (b.tap.name);
                        set (o, "tracks", names);
                        set (o, "band", bands[band].name);
                        set (o, "percent_of_the_time", roundToInt (100.0 * share));
                        const double avg = diff / both;
                        set (o, "louder", std::abs (avg) < 0.5 ? String ("about even")
                                                               : (avg > 0 ? a.tap.name : b.tap.name) + " by " + String (std::abs (avg), 1) + " dB");
                        set (o, "band_share_of_mix_percent", rounded (100.0 * bandEnergy / jmax (1.0e-20, mixEnergy), 1.0));
                        best = { score, o };
                    }
                }
                if (best.score > 0.0)
                    clashes.push_back (best);
            }
        std::sort (clashes.begin(), clashes.end(), [] (auto& x, auto& y) { return x.score > y.score; });
        Array<var> list;
        for (size_t i = 0; i < clashes.size() && i < 6; ++i)
            list.add (clashes[i].info);
        set (out, "masking", list);
    }

    // Plain notes on what's measurably off. Taste is the assistant's job, not Lathe's.
    StringArray notes;
    if (mix != nullptr && mix->peak > 0.0f)
    {
        if (mix->overs > 0)
            notes.add ("The mix goes over 0 dBFS " + String (mix->overs) + " times: it will clip in a 16 or 24-bit file.");
        const float tp = mixMeter->truePeakDb();
        if (tp > -1.0f)
            notes.add ("The mix's true peak is " + String (tp, 1) + " dBTP. Streaming services ask for -1 dBTP or lower.");
        const double l = mix->bandL[0] + mix->bandL[1], r = mix->bandR[0] + mix->bandR[1], x = mix->bandX[0] + mix->bandX[1];
        if (l + r > 0.0)
        {
            const double c = x / std::sqrt (jmax (1.0e-20, l * r));
            if (c < 0.5)
                notes.add ("Below 250 Hz the mix is wide or out of phase (correlation " + String (c, 2) + "): the low end can thin out in mono, on phones and club systems.");
        }
        const double c = mix->sumX / std::sqrt (jmax (1.0e-20, mix->sumL * mix->sumR));
        if (c < 0.0)
            notes.add ("The mix's correlation is " + String (c, 2) + ": a lot of it cancels in mono. Check for a track with one side flipped.");
        const double lean = toDb (mix->sumL) - toDb (mix->sumR);
        if (std::abs (lean) > 1.5)
            notes.add ("The mix leans " + String (lean > 0 ? "left" : "right") + " by " + String (std::abs (lean), 1) + " dB.");
    }
    for (auto& s : taps)
        if (! s->tap.isMix && s->overs > 0)
            notes.add (s->tap.name + " goes over 0 dBFS before the master. Lathe doesn't clip there, but plug-ins after it may not like it.");
    set (out, "notes", notes);
    return out;
}
} // namespace lathe::assistant
