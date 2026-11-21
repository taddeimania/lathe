// Convert to MIDI (see AudioToMidi.h). The melody side reads the pitch with the Tuner's YIN every
// 5 ms and cuts the pitch line into notes; the drum side measures three bands every 2.5 ms and
// takes a hit wherever one of them jumps up.
#include "AudioToMidi.h"
#include "../Devices/Tuner.h"

namespace lathe::audiotomidi
{
namespace
{
float median (std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    const auto mid = v.begin() + (long) (v.size() / 2);
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}

float toDb (double power) { return (float) (10.0 * std::log10 (power + 1.0e-12)); }

/** Velocity from how far under the loudest a note is: the loudest is 1, `range` dB under is the floor. */
float velocityFor (float db, float loudestDb, float range)
{
    return jlimit (0.12f, 1.0f, 1.0f + (db - loudestDb) / range);
}

bool keepGoing (const Progress& p, double amount)
{
    return ! p || p (jlimit (0.0, 1.0, amount));
}
} // namespace

//==============================================================================
std::vector<Found> melody (const float* x, int n, double sr, const Progress& progress)
{
    std::vector<Found> out;
    if (x == nullptr || n <= 0 || sr <= 0.0)
        return out;

    // Down to about 11-12 kHz first: plenty for the pitches of a voice or a bass (the detector looks
    // up to 1.5 kHz), and the detector's work grows with the square of the window.
    const int step = jmax (1, (int) std::floor (sr / 11025.0));
    const double fs = sr / step;
    std::vector<float> y;
    {
        IIRFilter a, b;
        a.setCoefficients (IIRCoefficients::makeLowPass (sr, 0.42 * fs));
        b.setCoefficients (IIRCoefficients::makeLowPass (sr, 0.42 * fs));
        y.reserve ((size_t) (n / step + 1));
        for (int i = 0; i < n; ++i)
        {
            const float v = b.processSingleSampleRaw (a.processSingleSampleRaw (x[i]));
            if (i % step == 0)
                y.push_back (v);
        }
    }
    const int m = (int) y.size();
    const int frame = roundToInt (fs * 0.085);   // the detector looks at the first half: ~42 ms, down to ~24 Hz
    const int hop = jmax (1, roundToInt (fs * 0.005));
    if (m < frame)
        return out;
    const int frames = (m - frame) / hop + 1;
    auto timeOf = [&] (int f) { return (f * hop + frame / 4) / fs; };   // the middle of what the detector looked at

    // How loud each moment is, over 20 ms around where the detector looks.
    std::vector<float> level ((size_t) frames, 0.0f);
    const int half = jmax (1, roundToInt (fs * 0.01));
    float loudest = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        const int c = f * hop + frame / 4;
        double sum = 0.0;
        int count = 0;
        for (int i = jmax (0, c - half); i < jmin (m, c + half); ++i, ++count)
            sum += (double) y[(size_t) i] * y[(size_t) i];
        level[(size_t) f] = count > 0 ? (float) std::sqrt (sum / count) : 0.0f;
        loudest = jmax (loudest, level[(size_t) f]);
    }
    if (loudest < 1.0e-3f)
        return out;   // silence
    const float gate = jmax (1.0e-3f, loudest * 0.01f);   // 40 dB under the loudest

    // The pitch (as a MIDI note number with a fraction) where there's one; -1 where there isn't.
    std::vector<float> pitch ((size_t) frames, -1.0f), work;
    for (int f = 0; f < frames; ++f)
    {
        if (f % 256 == 0 && ! keepGoing (progress, 0.9 * f / frames))
            return {};
        if (level[(size_t) f] < gate)
            continue;
        const auto r = devices::tuner::detect (y.data() + (size_t) f * hop, frame, fs, work, 40.0f, 1500.0f);
        if (r.hz > 0.0f && r.clarity >= 0.8f)   // breaths and consonants have no clear pitch
            pitch[(size_t) f] = 69.0f + 12.0f * std::log2 (r.hz / 440.0f);
    }

    // The detector sometimes lands an octave (or two) off for a moment: put a frame that's a
    // whole octave away from the pitch around it back in line.
    {
        const int around = jmax (2, roundToInt (0.1 * fs / hop));
        auto fixed = pitch;
        for (int f = 0; f < frames; ++f)
        {
            if (pitch[(size_t) f] < 0.0f)
                continue;
            std::vector<float> near;
            for (int k = jmax (0, f - around); k <= jmin (frames - 1, f + around); ++k)
                if (k != f && pitch[(size_t) k] >= 0.0f)
                    near.push_back (pitch[(size_t) k]);
            if (near.size() < 4)
                continue;
            const float off = pitch[(size_t) f] - median (near);
            for (float octaves : { -24.0f, -12.0f, 12.0f, 24.0f })
                if (std::abs (off - octaves) < 0.7f)
                    fixed[(size_t) f] = pitch[(size_t) f] - octaves;
        }
        pitch.swap (fixed);
    }

    // A running median over 25 ms, so a single frame that jumped an octave doesn't make a note.
    {
        auto smoothed = pitch;
        for (int f = 0; f < frames; ++f)
        {
            if (pitch[(size_t) f] < 0.0f)
                continue;
            std::vector<float> near;
            for (int k = jmax (0, f - 2); k <= jmin (frames - 1, f + 2); ++k)
                if (pitch[(size_t) k] >= 0.0f)
                    near.push_back (pitch[(size_t) k]);
            if (near.size() >= 3)
                smoothed[(size_t) f] = median (near);
        }
        pitch.swap (smoothed);
    }

    // Cut the pitch line into notes.
    const int minFrames = jmax (2, roundToInt (0.07 * fs / hop));     // shorter than 70 ms: not a note
    const int gapFrames = jmax (1, roundToInt (0.02 * fs / hop));     // a 20 ms break ends one
    const int moveFrames = jmax (1, roundToInt (0.04 * fs / hop));    // a new pitch held 40 ms starts one
    const int lookBack = jmax (2, roundToInt (0.04 * fs / hop));
    struct Current { bool on = false; int start = 0; std::vector<float> pitches; } cur;
    std::vector<float> moved;   // frames at a new pitch, waiting to see if it holds
    int silentRun = 0;

    auto finish = [&] (int end)
    {
        if (cur.on && end - cur.start >= minFrames && ! cur.pitches.empty())
        {
            float peak = 0.0f;
            for (int f = cur.start; f < end; ++f)
                peak = jmax (peak, level[(size_t) f]);
            Found note;
            note.pitch = jlimit (0, 127, roundToInt (median (cur.pitches)));
            note.start = timeOf (cur.start);
            note.length = timeOf (end) - note.start;
            note.velocity = peak;   // a level for now; made a velocity at the end
            out.push_back (note);
        }
        cur = {};
    };
    auto begin = [&] (int f, std::vector<float> pitches)
    {
        cur.on = true;
        cur.start = f;
        cur.pitches = std::move (pitches);
    };

    for (int f = 0; f < frames; ++f)
    {
        const float p = pitch[(size_t) f];
        if (! cur.on)
        {
            if (p >= 0.0f)
                begin (f, { p });
            continue;
        }
        if (p < 0.0f)
        {
            if (++silentRun >= gapFrames)
            {
                finish (f - silentRun + 1);
                moved.clear();
                silentRun = 0;
            }
            continue;
        }
        silentRun = 0;

        // Played again: the sound jumps back up after dipping, at the same pitch.
        float recent = level[(size_t) f];
        for (int k = jmax (cur.start, f - lookBack); k < f; ++k)
            recent = jmin (recent, level[(size_t) k]);
        if (f - cur.start >= minFrames && level[(size_t) f] > 2.5f * recent && level[(size_t) f] > 3.0f * gate)
        {
            finish (f);
            moved.clear();
            begin (f, { p });
            continue;
        }

        const float held = median (cur.pitches);
        if (std::abs (p - held) > 0.8f)
        {
            moved.push_back (p);
            if ((int) moved.size() >= moveFrames)
            {
                const int at = f - (int) moved.size() + 1;
                finish (at);
                begin (at, moved);
                moved.clear();
            }
            continue;
        }
        for (auto q : moved)          // it came back: those were part of this note after all
            cur.pitches.push_back (q);
        moved.clear();
        cur.pitches.push_back (p);
    }
    finish (frames - silentRun);

    // A blip far under or over the rest of the part (a breath that sounded like a low note, the
    // bass bleeding into a vocal mic) isn't part of the melody: drop short notes more than an
    // octave and a half from the middle of the notes around them.
    {
        std::vector<Found> kept;
        for (size_t i = 0; i < out.size(); ++i)
        {
            std::vector<float> around;
            for (size_t k = 0; k < out.size(); ++k)
                if (k != i && std::abs (out[k].start - out[i].start) < 5.0)
                    for (int w = 0; w < jmax (1, (int) (out[k].length / 0.05)); ++w)   // longer notes count for more
                        around.push_back ((float) out[k].pitch);
            const bool blip = out[i].length < 0.15 && around.size() >= 6 && std::abs (out[i].pitch - median (around)) > 18.0f;
            if (! blip)
                kept.push_back (out[i]);
        }
        out.swap (kept);
    }

    float loudestNote = 0.0f;
    for (auto& note : out)
        loudestNote = jmax (loudestNote, note.velocity);
    for (auto& note : out)
        note.velocity = velocityFor (toDb ((double) note.velocity * note.velocity), toDb ((double) loudestNote * loudestNote), 36.0f);
    keepGoing (progress, 1.0);
    return out;
}

//==============================================================================
std::vector<Found> drums (const float* x, int n, double sr, bool oneDrum, const Progress& progress)
{
    std::vector<Found> out;
    if (x == nullptr || n <= 0 || sr <= 0.0)
        return out;

    // Three bands: the kick's thump (under 120 Hz), a snare's body (150-400 Hz, which a hi-hat
    // hardly has) and the cymbals' and snare wires' hiss (over 5 kHz).
    enum { low, mid, high, all, numBands };
    IIRFilter lowA, lowB, midA, midB, highA, highB;
    lowA.setCoefficients (IIRCoefficients::makeLowPass (sr, 120.0));
    lowB.setCoefficients (IIRCoefficients::makeLowPass (sr, 120.0));
    midA.setCoefficients (IIRCoefficients::makeBandPass (sr, 245.0, 1.0));
    midB.setCoefficients (IIRCoefficients::makeBandPass (sr, 245.0, 1.0));
    const double highCut = jmin (5000.0, sr * 0.35);
    highA.setCoefficients (IIRCoefficients::makeHighPass (sr, highCut));
    highB.setCoefficients (IIRCoefficients::makeHighPass (sr, highCut));

    const int hop = jmax (1, roundToInt (sr * 0.0025));
    const int frames = n / hop;
    if (frames < 8)
        return out;
    std::vector<std::array<float, numBands>> energy ((size_t) frames);   // mean power in each hop
    for (int f = 0; f < frames; ++f)
    {
        if (f % 4096 == 0 && ! keepGoing (progress, 0.6 * f / frames))
            return {};
        std::array<double, numBands> sum {};
        for (int i = f * hop; i < (f + 1) * hop; ++i)
        {
            const float v = x[i];
            const float l = lowB.processSingleSampleRaw (lowA.processSingleSampleRaw (v));
            const float md = midB.processSingleSampleRaw (midA.processSingleSampleRaw (v));
            const float h = highB.processSingleSampleRaw (highA.processSingleSampleRaw (v));
            sum[low] += (double) l * l;
            sum[mid] += (double) md * md;
            sum[high] += (double) h * h;
            sum[all] += (double) v * v;
        }
        for (int b = 0; b < numBands; ++b)
            energy[(size_t) f][(size_t) b] = (float) (sum[(size_t) b] / hop);
    }

    std::array<float, numBands> peakDb;
    peakDb.fill (-240.0f);
    std::vector<std::array<float, numBands>> db ((size_t) frames);
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < numBands; ++b)
        {
            db[(size_t) f][(size_t) b] = toDb (energy[(size_t) f][(size_t) b]);
            peakDb[(size_t) b] = jmax (peakDb[(size_t) b], db[(size_t) f][(size_t) b]);
        }
    if (peakDb[all] < -60.0f)
        return out;   // silence

    // How sharply the sound jumps up in each hop: in any band that's loud enough to count, what
    // comes next against the average just before. Against an average, not a single earlier hop:
    // a real drum's ring and a cymbal's wash flicker by several dB from hop to hop, and a rise out
    // of one of those dips isn't a hit. The kick's and snare's bands look longer (10 ms against
    // 30 ms): a 60 Hz wave takes 16 ms, so 5 ms of it flickers on its own. The hiss is quick
    // (5 ms against 15 ms).
    const int back = 3;
    std::vector<float> rise ((size_t) frames, 0.0f);
    for (int f = 13; f + 4 <= frames; ++f)
        for (int b = 0; b < all; ++b)
        {
            const int nowHops = b == high ? 2 : 4, wasHops = b == high ? 6 : 12;
            double now = 0.0, was = 0.0;
            for (int k = 0; k < nowHops; ++k) now += energy[(size_t) (f + k)][(size_t) b];
            for (int k = 1; k <= wasHops; ++k) was += energy[(size_t) (f - 1 - k)][(size_t) b];
            const float nowDb = toDb (now / nowHops);
            if (nowDb > peakDb[(size_t) b] - 45.0f)
                rise[(size_t) f] = jmax (rise[(size_t) f], nowDb - toDb (was / wasHops));
        }

    const float threshold = 9.0f;                          // dB
    const int near = jmax (1, roundToInt (0.01 * sr / hop));    // the biggest within 10 ms
    const int minGap = jmax (1, roundToInt (0.03 * sr / hop));  // and not within 30 ms of the last
    const int after = jmax (2, roundToInt (0.04 * sr / hop));   // what a hit is: its first 40 ms
    const int before = jmax (2, roundToInt (0.02 * sr / hop));  //   against the 20 ms before it

    struct Hit { int frame; int drum; std::array<float, numBands> power; float fresh; };
    std::vector<Hit> hits;
    int last = -minGap;
    for (int f = back; f < frames; ++f)
    {
        const float r = rise[(size_t) f];
        if (r < threshold || f - last < minGap || db[(size_t) f][all] < peakDb[all] - 45.0f)
            continue;
        bool biggest = true;
        for (int k = jmax (0, f - near); k <= jmin (frames - 1, f + near) && biggest; ++k)
            if (rise[(size_t) k] > r || (rise[(size_t) k] == r && k < f))
                biggest = false;
        if (! biggest)
            continue;
        last = f;

        // What's new in each band: its first 40 ms, less what was already ringing before it.
        std::array<float, numBands> fresh {}, peak {};
        for (int b = 0; b < numBands; ++b)
        {
            double was = 0.0;
            int count = 0;
            for (int k = jmax (0, f - back - before); k < f - back; ++k, ++count)
                was += energy[(size_t) k][(size_t) b];
            was = count > 0 ? was / count : 0.0;
            double now = 0.0;
            for (int k = f - 1; k < jmin (frames, f + after); ++k)
            {
                now += energy[(size_t) k][(size_t) b];
                peak[(size_t) b] = jmax (peak[(size_t) b], energy[(size_t) k][(size_t) b]);
            }
            fresh[(size_t) b] = (float) jmax (0.0, now - was * (after + 1));
        }
        // Which drum: a kick is mostly low; a hi-hat or a cymbal is mostly hiss; a snare is
        // mostly body. A hit can be two at once: a hat or a snare on a kick
        // shows as high or middle energy near the loudest there is in those bands.
        const float l = fresh[low], md = fresh[mid], h = fresh[high];
        const float total = l + md + h;
        if (total <= 0.0f)
            continue;
        auto closeToLoudest = [&] (int band, float within) { return toDb (peak[(size_t) band]) > peakDb[(size_t) band] - within; };
        auto add = [&] (int drum) { hits.push_back ({ f, drum, peak, total }); };
        // A hat or cymbal under a kick is swamped in power, but still adds a little real top (a
        // few tenths of a percent of what's new; a kick's beater click adds next to none) at
        // about a cymbal's loudness.
        const bool hatToo = h >= 0.003f * total && closeToLoudest (high, 12.0f);
        if (l >= 0.5f * total)
        {
            add (kick);
            // A kick's own boom puts a fifth of what's new into the snare's band; a snare on top
            // of it makes that a third or more.
            if (md >= 0.25f * total && closeToLoudest (mid, 12.0f)) add (snare);
            else if (hatToo) add (hihat);
        }
        // A real snare is nearly all body over its wires' hiss; a crash or a ride, bright as it is,
        // still rings a little in the snare's band.
        else if (md < 0.25f * (md + h) && l < 0.3f * h)
            add (hihat);
        else
            add (snare);
    }
    // A click, the room or a breath 30 dB under the loudest hit isn't a hit (ghost notes are
    // nearer 20 dB under).
    float loudestHit = 0.0f;
    for (auto& hit : hits)
        loudestHit = jmax (loudestHit, hit.fresh);
    hits.erase (std::remove_if (hits.begin(), hits.end(), [loudestHit] (const Hit& hit) { return hit.fresh < loudestHit * 1.0e-3f; }), hits.end());

    // One drum found twice within 45 ms (the stick, then the drum) is one hit: the louder one's.
    {
        const int merge = jmax (1, roundToInt (0.045 * sr / hop));
        std::vector<Hit> merged;
        for (auto& hit : hits)
        {
            auto same = std::find_if (merged.rbegin(), merged.rend(), [&] (const Hit& m) { return m.drum == hit.drum; });
            if (same != merged.rend() && hit.frame - same->frame < merge)
            {
                if (hit.fresh > same->fresh)
                    *same = hit;
                continue;
            }
            merged.push_back (hit);
        }
        hits.swap (merged);
    }
    keepGoing (progress, 0.9);

    // One drum: every hit is the drum most of them sound like.
    if (oneDrum && ! hits.empty())
    {
        std::map<int, int> count;
        for (auto& h : hits) ++count[h.drum];
        int drum = snare, most = 0;
        for (auto& [d, c] : count) if (c > most) { drum = d; most = c; }
        std::vector<Hit> one;
        for (auto& h : hits)
            if (one.empty() || one.back().frame != h.frame)
                one.push_back ({ h.frame, drum, h.power, h.fresh });
        hits.swap (one);
    }

    // Velocity from each drum's own band, against that drum's loudest hit.
    auto bandOf = [oneDrum] (int drum) { return oneDrum ? (int) all : drum == kick ? (int) low : drum == hihat ? (int) high : (int) mid; };
    std::map<int, float> loudest;
    for (auto& h : hits)
        loudest[h.drum] = jmax (loudest.count (h.drum) ? loudest[h.drum] : -240.0f, toDb (h.power[(size_t) bandOf (h.drum)]));

    // Lost in the tail: a soft "hit" no louder in its drum's band than that band already was in the
    // last quarter second is the room, a ringing drum or the cymbals' wash moving, not a new hit
    // (real hits stand 15 dB and more over what came before). Loud ones stay, however close.
    {
        const int recent = jmax (1, roundToInt (0.25 * sr / hop)), skip = 4;
        hits.erase (std::remove_if (hits.begin(), hits.end(), [&] (const Hit& h)
        {
            const int b = bandOf (h.drum);
            const float level = toDb (h.power[(size_t) b]);
            if (level > loudest[h.drum] - 8.0f || rise[(size_t) h.frame] >= 15.0f)
                return false;
            float was = -240.0f;
            for (int k = jmax (0, h.frame - recent); k < h.frame - skip; ++k)
                was = jmax (was, db[(size_t) k][(size_t) b]);
            return level < was;
        }), hits.end());
    }
    for (auto& h : hits)
    {
        Found note;
        note.pitch = h.drum;
        note.start = jmax (0.0, (h.frame - back + 1) * (double) hop / sr);   // where the jump began
        note.length = 0.1;
        note.velocity = velocityFor (toDb (h.power[(size_t) bandOf (h.drum)]), loudest[h.drum], 30.0f);
        out.push_back (note);
    }
    std::sort (out.begin(), out.end(), [] (const Found& a, const Found& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
    keepGoing (progress, 1.0);
    return out;
}

//==============================================================================
Result convertClip (const Clip& clip, Kind kind, double tempo, AudioFormatManager& formats, const Progress& progress)
{
    Result result;
    if (clip.kind != ClipKind::audio || tempo <= 0.0)
    {
        result.error = "That isn't an audio clip.";
        return result;
    }
    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (clip.file));
    if (reader == nullptr)
    {
        result.error = "Couldn't read " + clip.file.getFileName() + ".";
        return result;
    }
    const double sr = reader->sampleRate;
    const double samplesPerBeat = sr * 60.0 / tempo;
    const int64 from = jmax ((int64) 0, clip.sourceOffset);
    const int64 wanted = (int64) std::llround (clip.length * samplesPerBeat);
    const int length = (int) jmin ((int64) std::numeric_limits<int>::max() / 4, jmin (wanted, reader->lengthInSamples - from));
    if (length <= 0)
    {
        result.error = "The clip has no sound in it.";
        return result;
    }
    const int channels = (int) jlimit (1u, 2u, reader->numChannels);
    AudioBuffer<float> buffer (channels, length);
    reader->read (&buffer, 0, length, from, true, channels > 1);
    if (channels > 1)
    {
        buffer.addFrom (0, 0, buffer, 1, 0, length);
        buffer.applyGain (0, 0, length, 0.5f);
    }

    const auto found = kind == Kind::melody ? melody (buffer.getReadPointer (0), length, sr, progress)
                                            : drums (buffer.getReadPointer (0), length, sr, kind == Kind::oneDrum, progress);
    if (progress && ! progress (1.0))
    {
        result.cancelled = true;
        return result;
    }
    const double beatsPerSecond = tempo / 60.0;
    for (auto& f : found)
    {
        Note note;
        note.pitch = f.pitch;
        note.start = f.start * beatsPerSecond;
        note.length = jmax (1.0 / 64.0, f.length * beatsPerSecond);
        note.velocity = f.velocity;
        if (note.start >= clip.length)
            continue;
        note.length = jmin (note.length, clip.length - note.start);
        result.notes.push_back (note);
    }
    if (result.notes.empty())
        result.error = kind == Kind::melody ? "No notes found: Convert to MIDI hears one note at a time (a voice, a bass, a lead), not chords."
                                            : "No drum hits found.";
    return result;
}

std::shared_ptr<Track> place (Edit& edit, const Track& source, const std::vector<std::pair<Clip, std::vector<Note>>>& converted)
{
    auto track = edit.addTrack (TrackKind::midi, source.name + " MIDI");
    auto& list = edit.tracks;
    list.erase (std::remove (list.begin(), list.end(), track), list.end());
    auto at = std::find_if (list.begin(), list.end(), [&] (auto& t) { return t->id == source.id; });
    list.insert (at == list.end() ? list.end() : at + 1, track);
    for (auto& [audio, notes] : converted)
    {
        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::midi;
        c.name = audio.name;
        c.start = audio.start;
        c.length = audio.length;
        c.notes = notes;
        track->clips.push_back (c);
    }
    std::sort (track->clips.begin(), track->clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
    return track;
}

String kindName (Kind k)
{
    return k == Kind::melody ? "melody" : k == Kind::drums ? "drums" : "one_drum";
}

bool parseKind (const String& s, Kind& out)
{
    const auto t = s.trim().toLowerCase().replaceCharacter (' ', '_').replaceCharacter ('-', '_');
    if (t == "melody") { out = Kind::melody; return true; }
    if (t == "drums")  { out = Kind::drums; return true; }
    if (t == "one_drum" || t == "drum") { out = Kind::oneDrum; return true; }
    return false;
}
} // namespace lathe::audiotomidi
