#include "Chords.h"

namespace lathe
{
namespace chords
{
namespace
{
    int pc (int pitch)   { return ((pitch % 12) + 12) % 12; }

    const std::vector<int> majorSteps { 0, 2, 4, 5, 7, 9, 11 };

    /** The seven-note scale a key's chords come from: the scale itself, or a pentatonic's (or
        the blues's) parent. */
    const std::vector<int>& parentSteps (Scale s)
    {
        if (s == Scale::majorPentatonic)
            return keys::intervals (Scale::major);
        if (s == Scale::minorPentatonic || s == Scale::blues)
            return keys::intervals (Scale::minor);
        return keys::intervals (s);
    }

    int letterOf (const String& name)
    {
        static const String order ("CDEFGAB");
        return jmax (0, order.indexOfChar (name[0]));
    }

    String degreeSymbol (const char* utf8)   { return String::fromUTF8 (utf8); }
    const char* dimSign  = "\xc2\xb0";   // °
    const char* halfDim  = "\xc3\xb8";   // ø

    bool isUpper (Quality q)
    {
        return q != Quality::minor && q != Quality::diminished && q != Quality::minor7
            && q != Quality::halfDiminished7 && q != Quality::diminished7 && q != Quality::minorMajor7;
    }

    String numeralFor (int degree, int interval, Quality q)
    {
        static const char* upper[] { "I", "II", "III", "IV", "V", "VI", "VII" };
        String accidental;
        const int diff = interval - majorSteps[(size_t) degree];
        if (diff < 0)        accidental = String::repeatedString ("b", -diff);
        else if (diff > 0)   accidental = String::repeatedString ("#", diff);
        String roman = upper[degree];
        if (! isUpper (q))
            roman = roman.toLowerCase();
        String tail;
        switch (q)
        {
            case Quality::diminished:       tail = degreeSymbol (dimSign); break;
            case Quality::augmented:        tail = "+"; break;
            case Quality::dominant7:        tail = "7"; break;
            case Quality::major7:           tail = "maj7"; break;
            case Quality::minor7:           tail = "7"; break;
            case Quality::halfDiminished7:  tail = degreeSymbol (halfDim) + "7"; break;
            case Quality::diminished7:      tail = degreeSymbol (dimSign) + "7"; break;
            case Quality::minorMajor7:      tail = "(maj7)"; break;
            case Quality::augmentedMajor7:  tail = "+maj7"; break;
            case Quality::sus2:             tail = "sus2"; break;
            case Quality::sus4:             tail = "sus4"; break;
            case Quality::power:            tail = "5"; break;
            case Quality::major:
            case Quality::minor:            break;
        }
        return accidental + roman + tail;
    }

    std::optional<Quality> triadQuality (int third, int fifth)
    {
        if (third == 4 && fifth == 7) return Quality::major;
        if (third == 3 && fifth == 7) return Quality::minor;
        if (third == 3 && fifth == 6) return Quality::diminished;
        if (third == 4 && fifth == 8) return Quality::augmented;
        return {};
    }

    std::optional<Quality> seventhQuality (Quality triad, int seventh)
    {
        if (triad == Quality::major && seventh == 11)       return Quality::major7;
        if (triad == Quality::major && seventh == 10)       return Quality::dominant7;
        if (triad == Quality::minor && seventh == 10)       return Quality::minor7;
        if (triad == Quality::minor && seventh == 11)       return Quality::minorMajor7;
        if (triad == Quality::diminished && seventh == 10)  return Quality::halfDiminished7;
        if (triad == Quality::diminished && seventh == 9)   return Quality::diminished7;
        if (triad == Quality::augmented && seventh == 11)   return Quality::augmentedMajor7;
        return {};
    }

    //==========================================================================
    // Chord shapes for a guitar in standard tuning, as the open chord they come from (the
    // shape's name, its root there, and the frets low E first; X = not played). Movable shapes
    // slide up the neck, the open strings becoming a barre. Some shapes have a note a fret
    // behind the root (written -1), so they only work moved up.
    constexpr int X = -100;
    struct Shape
    {
        const char* name;
        int openRoot;
        std::array<int, 6> frets;
        bool movable;
    };

    const std::vector<Shape>& shapesFor (Quality q)
    {
        static const std::map<Quality, std::vector<Shape>> table {
            { Quality::major, {
                { "E shape", 4, { 0, 2, 2, 1, 0, 0 }, true },
                { "A shape", 9, { X, 0, 2, 2, 2, 0 }, true },
                { "C shape", 0, { X, 3, 2, 0, 1, 0 }, true },
                { "D shape", 2, { X, X, 0, 2, 3, 2 }, true },
                { "G shape", 7, { 3, 2, 0, 0, 0, 3 }, false } } },
            { Quality::minor, {
                { "Em shape", 4, { 0, 2, 2, 0, 0, 0 }, true },
                { "Am shape", 9, { X, 0, 2, 2, 1, 0 }, true },
                { "Dm shape", 2, { X, X, 0, 2, 3, 1 }, true } } },
            { Quality::dominant7, {
                { "E7 shape", 4, { 0, 2, 0, 1, 0, 0 }, true },
                { "A7 shape", 9, { X, 0, 2, 0, 2, 0 }, true },
                { "C7 shape", 0, { X, 3, 2, 3, 1, 0 }, true },
                { "D7 shape", 2, { X, X, 0, 2, 1, 2 }, true },
                { "G7 shape", 7, { 3, 2, 0, 0, 0, 1 }, false },
                { "B7 shape", 11, { X, 2, 1, 2, 0, 2 }, false } } },
            { Quality::major7, {
                { "Emaj7 shape", 4, { 0, X, 1, 1, 0, X }, true },
                { "Amaj7 shape", 9, { X, 0, 2, 1, 2, 0 }, true },
                { "Dmaj7 shape", 2, { X, X, 0, 2, 2, 2 }, true },
                { "Cmaj7 shape", 0, { X, 3, 2, 0, 0, 0 }, false },
                { "Fmaj7 shape", 5, { X, X, 3, 2, 1, 0 }, false } } },
            { Quality::minor7, {
                { "Em7 shape", 4, { 0, 2, 0, 0, 0, 0 }, true },
                { "Am7 shape", 9, { X, 0, 2, 0, 1, 0 }, true },
                { "Dm7 shape", 2, { X, X, 0, 2, 1, 1 }, true } } },
            { Quality::halfDiminished7, {
                { "E shape", 4, { 0, X, 0, 0, -1, X }, true },
                { "A shape", 9, { X, 0, 1, 0, 1, X }, true },
                { "D shape", 2, { X, X, 0, 1, 1, 1 }, true } } },
            { Quality::diminished, {
                { "A shape", 9, { X, 0, 1, 2, 1, X }, true },
                { "E shape", 4, { 0, 1, 2, 0, X, X }, true },
                { "D shape", 2, { X, X, 0, 1, 3, 1 }, true } } },
            { Quality::diminished7, {
                { "A shape", 9, { X, 0, 1, -1, 1, X }, true },
                { "E shape", 4, { 0, X, -1, 0, -1, X }, true },
                { "D shape", 2, { X, X, 0, 1, 0, 1 }, true } } },
            { Quality::augmented, {
                { "A shape", 9, { X, 0, 3, 2, 2, X }, true },
                { "E shape", 4, { 0, 3, 2, 1, X, X }, true },
                { "C shape", 0, { X, 3, 2, 1, 1, 0 }, true },
                { "D shape", 2, { X, X, 0, 3, 3, 2 }, true } } },
            { Quality::augmentedMajor7, {
                { "E shape", 4, { 0, X, 1, 1, 1, X }, true },
                { "A shape", 9, { X, 0, X, 1, 2, 1 }, true } } },
            { Quality::minorMajor7, {
                { "Em shape", 4, { 0, 2, 1, 0, 0, 0 }, true },
                { "Am shape", 9, { X, 0, 2, 1, 1, 0 }, true },
                { "Dm shape", 2, { X, X, 0, 2, 2, 1 }, true } } },
            { Quality::sus2, {
                { "A shape", 9, { X, 0, 2, 2, 0, 0 }, true },
                { "D shape", 2, { X, X, 0, 2, 3, 0 }, true } } },
            { Quality::sus4, {
                { "E shape", 4, { 0, 2, 2, 2, 0, 0 }, true },
                { "A shape", 9, { X, 0, 2, 2, 3, 0 }, true },
                { "D shape", 2, { X, X, 0, 2, 3, 3 }, true } } },
            { Quality::power, {
                { "Root on E", 4, { 0, 2, 2, X, X, X }, true },
                { "Root on A", 9, { X, 0, 2, 2, X, X }, true },
                { "Root on D", 2, { X, X, 0, 2, 3, X }, true } } },
        };
        static const std::vector<Shape> none;
        auto it = table.find (q);
        return it != table.end() ? it->second : none;
    }

    const std::array<int, 6>& tuning()
    {
        static const std::array<int, 6> t { 40, 45, 50, 55, 59, 64 };
        return t;
    }

    /** Where the index finger lies across the strings: the lowest fret, if two or more strings
        are stopped there, none is open and no string in between is left out. */
    void findBarre (Voicing& v)
    {
        if (v.usesOpenStrings())
            return;
        const int low = v.lowestFret();
        int from = -1, to = -1, count = 0;
        for (int s = 0; s < (int) v.frets.size(); ++s)
            if (v.frets[(size_t) s] == low)
            {
                if (from < 0) from = s;
                to = s;
                ++count;
            }
        if (count < 2)
            return;
        for (int s = from; s <= to; ++s)
            if (v.frets[(size_t) s] < 0)
                return;
        v.barreFret = low;
        v.barreFrom = from;
        v.barreTo = to;
    }

    int fingersFor (const Voicing& v)
    {
        int n = 0;
        for (int s = 0; s < (int) v.frets.size(); ++s)
        {
            const int f = v.frets[(size_t) s];
            if (f > 0 && ! (v.barreFret > 0 && f == v.barreFret && s >= v.barreFrom && s <= v.barreTo))
                ++n;
        }
        return n + (v.barreFret > 0 ? 1 : 0);
    }

    /** Whether a voicing has every note of the chord. A 7th may leave out its (perfect) 5th,
        as the open C7 does. */
    bool hasEveryTone (const Voicing& v, const Chord& chord)
    {
        const auto& steps = intervals (chord.quality);
        for (size_t i = 0; i < steps.size(); ++i)
        {
            if (steps.size() == 4 && steps[i] == 7)
                continue;
            const int t = pc (chord.root + steps[i]);
            bool found = false;
            for (auto p : v.pitches())
                found = found || pc (p) == t;
            if (! found)
                return false;
        }
        return true;
    }

    String inversionName (int bassTone)
    {
        static const char* names[] { "Root position", "1st inversion", "2nd inversion", "3rd inversion" };
        return names[jlimit (0, 3, bassTone)];
    }
}

//==============================================================================
const std::vector<int>& intervals (Quality q)
{
    static const std::map<Quality, std::vector<int>> table {
        { Quality::major, { 0, 4, 7 } },            { Quality::minor, { 0, 3, 7 } },
        { Quality::diminished, { 0, 3, 6 } },       { Quality::augmented, { 0, 4, 8 } },
        { Quality::dominant7, { 0, 4, 7, 10 } },    { Quality::major7, { 0, 4, 7, 11 } },
        { Quality::minor7, { 0, 3, 7, 10 } },       { Quality::halfDiminished7, { 0, 3, 6, 10 } },
        { Quality::diminished7, { 0, 3, 6, 9 } },   { Quality::minorMajor7, { 0, 3, 7, 11 } },
        { Quality::augmentedMajor7, { 0, 4, 8, 11 } },
        { Quality::sus2, { 0, 2, 7 } },             { Quality::sus4, { 0, 5, 7 } },
        { Quality::power, { 0, 7 } },
    };
    return table.at (q);
}

String suffix (Quality q)
{
    switch (q)
    {
        case Quality::major:            return {};
        case Quality::minor:            return "m";
        case Quality::diminished:       return "dim";
        case Quality::augmented:        return "aug";
        case Quality::dominant7:        return "7";
        case Quality::major7:           return "maj7";
        case Quality::minor7:           return "m7";
        case Quality::halfDiminished7:  return "m7b5";
        case Quality::diminished7:      return "dim7";
        case Quality::minorMajor7:      return "m(maj7)";
        case Quality::augmentedMajor7:  return "maj7#5";
        case Quality::sus2:             return "sus2";
        case Quality::sus4:             return "sus4";
        case Quality::power:            return "5";
    }
    return {};
}

String toneLabel (Quality q, int semitones)
{
    switch (pc (semitones))
    {
        case 0:  return "R";
        case 2:  return "2";
        case 3:  return "b3";
        case 4:  return "3";
        case 5:  return "4";
        case 6:  return "b5";
        case 7:  return "5";
        case 8:  return "#5";
        case 9:  return q == Quality::diminished7 ? "bb7" : "6";
        case 10: return "b7";
        case 11: return "7";
        default: return "?";
    }
}

String variantName (Variant v)
{
    switch (v)
    {
        case Variant::triad:      return "Triad";
        case Variant::seventh:    return "7th";
        case Variant::dominant7:  return "Dom 7";
        case Variant::sus2:       return "sus2";
        case Variant::sus4:       return "sus4";
        case Variant::power:      return "Power";
    }
    return {};
}

std::vector<String> toneNames (const Chord& c, bool flats)
{
    // Each tone a letter on from the root: the 3rd two letters up, the 5th four, the 7th six.
    const int rootLetter = letterOf (c.name);
    std::vector<String> out;
    for (auto i : intervals (c.quality))
    {
        int letters = 0;
        if (i == 2)                   letters = 1;
        else if (i == 3 || i == 4)    letters = 2;
        else if (i == 5)              letters = 3;
        else if (i >= 6 && i <= 8)    letters = 4;
        else if (i >= 9)              letters = 6;
        out.push_back (i == 0 ? c.name.substring (0, c.name.length() - suffix (c.quality).length())
                              : keys::letterName (c.root + i, rootLetter + letters, flats));
    }
    return out;
}

std::vector<int> tones (const Chord& c)
{
    std::vector<int> out;
    for (auto i : intervals (c.quality))
        out.push_back (pc (c.root + i));
    return out;
}

std::vector<Degree> diatonic (int root, Scale scale)
{
    root = pc (root);
    const auto& steps = parentSteps (scale);
    const bool flats = keys::usesFlats (root, scale);
    const int rootLetter = letterOf (keys::noteName (root, flats));
    auto inScale = [&] (int interval)
    {
        return std::find (steps.begin(), steps.end(), pc (interval)) != steps.end();
    };

    std::vector<Degree> out;
    for (int d = 0; d < 7; ++d)
    {
        auto at = [&] (int k) { return steps[(size_t) ((d + k) % 7)]; };
        const int step = at (0);
        const int third = pc (at (2) - step), fifth = pc (at (4) - step), seventh = pc (at (6) - step);
        const auto triad = triadQuality (third, fifth);
        if (! triad)
            continue;

        Degree deg;
        deg.degree = d;
        deg.root = pc (root + step);
        deg.numeral = numeralFor (d, step, *triad);
        const auto name = keys::letterName (deg.root, rootLetter + d, flats);
        auto add = [&] (Quality q, Variant v)
        {
            Chord c;
            c.root = deg.root;
            c.quality = q;
            c.variant = v;
            c.name = name + suffix (q);
            c.numeral = numeralFor (d, step, q);
            deg.chords.push_back (c);
        };

        add (*triad, Variant::triad);
        if (auto q7 = seventhQuality (*triad, seventh))
            add (*q7, Variant::seventh);
        // The blues is played on dominant 7ths: I7, IV7 and V7.
        if (scale == Scale::blues && (d == 0 || d == 3 || d == 4))
            add (Quality::dominant7, Variant::dominant7);
        const bool perfectFifth = fifth == 7;
        if (perfectFifth && inScale (step + 2))
            add (Quality::sus2, Variant::sus2);
        if (perfectFifth && inScale (step + 5))
            add (Quality::sus4, Variant::sus4);
        if (perfectFifth)
            add (Quality::power, Variant::power);
        out.push_back (std::move (deg));
    }
    return out;
}

//==============================================================================
int Voicing::lowestFret() const
{
    int low = 0;
    for (auto f : frets)
        if (f > 0 && (low == 0 || f < low))
            low = f;
    return low;
}

int Voicing::highestFret() const
{
    int high = 0;
    for (auto f : frets)
        high = jmax (high, f);
    return high;
}

int Voicing::bassString() const
{
    for (int s = 0; s < (int) frets.size(); ++s)
        if (frets[(size_t) s] >= 0)
            return s;
    return -1;
}

bool Voicing::usesOpenStrings() const
{
    return std::find (frets.begin(), frets.end(), 0) != frets.end();
}

std::vector<int> Voicing::pitches() const
{
    if (instrument == Instrument::keyboard)
        return keys;
    const auto& strings = fretboard::instruments()[(size_t) instrument].strings;
    std::vector<int> out;
    for (size_t s = 0; s < frets.size() && s < strings.size(); ++s)
        if (frets[s] >= 0)
            out.push_back (strings[s] + frets[s]);
    return out;
}

namespace
{
    /** Shapes on a four-string instrument, found by trying every fret within reach: the
        ukulele plays all four strings (its G is tuned up, so any note can be lowest); the bass
        has the root on its lowest string played, and leaves out what it doesn't need. */
    std::vector<Voicing> searchFretted (const Chord& chord, Instrument instrument)
    {
        const auto& inst = fretboard::instruments()[(size_t) instrument];
        const int n = (int) inst.strings.size();
        const bool uke = instrument == Instrument::ukulele;
        const auto& steps = intervals (chord.quality);
        // A power chord has only two notes: on a ukulele, strings may be left out for it too.
        const bool allStrings = uke && steps.size() > 2;
        const auto chordTones = tones (chord);
        // What must sound: every note (a 7th may leave out its 5th); on bass, the root, 3rd and 7th.
        std::vector<int> needed;
        for (auto i : steps)
        {
            if (steps.size() == 4 && i == 7)
                continue;
            if (! uke && i == 7 && steps.size() == 3 && (steps[1] == 3 || steps[1] == 4))
                continue;
            needed.push_back (pc (chord.root + i));
        }
        struct Candidate { Voicing v; float score; };
        std::vector<Candidate> found;
        std::vector<int> f ((size_t) n, -1);
        std::function<void (int)> search = [&] (int s)
        {
            if (s == n)
            {
                Voicing v;
                v.instrument = instrument;
                v.frets = f;
                const auto ps = v.pitches();
                if ((int) ps.size() < (allStrings ? n : 2))
                    return;
                int lo = 99, hi = 0, played = 0, gaps = 0;
                bool started = false;
                for (auto fr : f)
                {
                    if (fr < 0) { gaps += started ? 1 : 0; continue; }
                    started = true;
                    ++played;
                    if (fr > 0) { lo = jmin (lo, fr); hi = jmax (hi, fr); }
                }
                if (hi > 0 && hi - lo > 3)
                    return;
                for (auto t : needed)
                    if (std::none_of (ps.begin(), ps.end(), [t] (int p) { return pc (p) == t; }))
                        return;
                if (! uke && pc (ps.front()) != chord.root)
                    return;
                findBarre (v);
                if (fingersFor (v) > 4)
                    return;
                // Fewer fingers and a smaller stretch read easier; on bass, fewer notes ring clearer.
                const float score = (float) fingersFor (v) + 0.5f * (float) (hi > 0 ? hi - lo : 0)
                                  + (uke ? 0.0f : 0.6f * (float) played + 1.5f * (float) gaps);
                found.push_back ({ v, score });
                return;
            }
            if (! allStrings)
            {
                f[(size_t) s] = -1;
                search (s + 1);
            }
            for (int fret = 0; fret <= inst.frets; ++fret)
            {
                const int p = pc (inst.strings[(size_t) s] + fret);
                if (std::find (chordTones.begin(), chordTones.end(), p) == chordTones.end())
                    continue;
                // Keep the hand within four frets of the lowest fretted note so far.
                bool reach = true;
                for (int k = 0; k < s; ++k)
                    if (f[(size_t) k] > 0 && fret > 0 && std::abs (f[(size_t) k] - fret) > 3)
                        reach = false;
                if (! reach)
                    continue;
                f[(size_t) s] = fret;
                search (s + 1);
            }
            f[(size_t) s] = -1;
        };
        search (0);

        // The best for each place on the neck: open shapes first, then by fret, a couple of
        // frets apart so each one is somewhere new.
        std::stable_sort (found.begin(), found.end(), [] (const Candidate& a, const Candidate& b)
        {
            const bool ao = a.v.usesOpenStrings() && a.v.highestFret() <= 4, bo = b.v.usesOpenStrings() && b.v.highestFret() <= 4;
            if (ao != bo) return ao;
            if (ao) return a.score < b.score;
            if (a.v.lowestFret() != b.v.lowestFret()) return a.v.lowestFret() < b.v.lowestFret();
            return a.score < b.score;
        });
        static const StringArray bassNames { "E", "A", "D", "G" };
        std::vector<Voicing> out;
        int opens = 0, lastFret = -10;
        for (auto& c : found)
        {
            auto v = c.v;
            const bool open = v.usesOpenStrings() && v.highestFret() <= 4;
            if (open)
            {
                if (opens >= (uke ? 2 : 2))
                    continue;
                ++opens;
                v.group = Voicing::shape;
                v.name = "Open";
            }
            else
            {
                if (v.usesOpenStrings() || v.lowestFret() < lastFret + 2 || out.size() >= 7)
                    continue;
                lastFret = v.lowestFret();
                v.group = Voicing::upTheNeck;
                v.name = "Fret " + String (v.lowestFret());
            }
            if (uke)
                v.detail = v.barreFret > 0 ? "Barre at fret " + String (v.barreFret) : (open ? "Open position" : "Fret " + String (v.lowestFret()));
            else
                v.detail = "Root on the " + bassNames[v.bassString()] + " string";
            out.push_back (v);
        }
        return out;
    }

    /** Keyboard voicings, within C3 to B4: the chord close together in each inversion, around
        middle C, then spread: the root low, the rest close together an octave or more above. */
    std::vector<Voicing> keyboardVoicings (const Chord& chord)
    {
        const auto& steps = intervals (chord.quality);
        const int n = (int) steps.size();
        const auto names = toneNames (chord, true);
        static const char* inversions[] { "Root position", "1st inversion", "2nd inversion", "3rd inversion" };
        // "C4 E4 G4", each note spelled as the chord spells it.
        auto describe = [&] (const std::vector<int>& keys)
        {
            String d;
            for (auto k : keys)
                for (int i = 0; i < n; ++i)
                    if (pc (chord.root + steps[(size_t) i]) == pc (k))
                    {
                        d << (d.isEmpty() ? "" : " ") << names[(size_t) i] << (k / 12 - 1);
                        break;
                    }
            return d;
        };
        // The tones from index `from` on, stacked upward from `bass`.
        auto stack = [&] (int bass, int from, int count)
        {
            std::vector<int> keys;
            int p = bass;
            for (int k = 0; k < count; ++k)
            {
                const int want = pc (chord.root + steps[(size_t) ((from + k) % n)]);
                while (pc (p) != want) ++p;
                keys.push_back (p);
                ++p;
            }
            return keys;
        };

        std::vector<Voicing> out;
        for (int inv = 0; inv < n; ++inv)
        {
            Voicing v;
            v.instrument = Instrument::keyboard;
            v.keys = stack (55, inv, n);   // from G3 up, so they sit around middle C
            if (v.keys.back() > fretboard::keyboardHigh)
                for (auto& k : v.keys) k -= 12;
            v.group = Voicing::shape;
            v.name = n == 2 ? (inv == 0 ? "Root and 5th" : "5th and root") : inversions[inv];
            v.detail = describe (v.keys);
            out.push_back (v);
        }
        if (n >= 3)
        {
            Voicing v;
            v.instrument = Instrument::keyboard;
            const int low = fretboard::keyboardLow + pc (chord.root - fretboard::keyboardLow);
            v.keys = stack (low + 12, 1, n - 1);
            v.keys.insert (v.keys.begin(), low);
            if (v.keys.back() <= fretboard::keyboardHigh)
            {
                v.group = Voicing::upTheNeck;
                v.name = "Spread";
                v.detail = describe (v.keys);
                out.push_back (v);
            }
        }
        return out;
    }
}

std::vector<Voicing> voicings (const Chord& chord, Instrument instrument)
{
    if (instrument == Instrument::keyboard)
        return keyboardVoicings (chord);
    if (instrument != Instrument::guitar)
        return searchFretted (chord, instrument);

    std::vector<Voicing> shapes, neck;
    auto seen = [&] (const std::vector<int>& f)
    {
        for (auto* list : { &shapes, &neck })
            for (auto& v : *list)
                if (v.frets == f)
                    return true;
        return false;
    };

    // The shapes, slid to where their root is the chord's.
    for (auto& shape : shapesFor (chord.quality))
    {
        const int t = pc (chord.root - shape.openRoot);
        std::vector<int> positions;
        if (shape.movable)
            positions = t == 0 ? std::vector<int> { 0, 12 } : std::vector<int> { t, t + 12 };
        else if (t == 0)
            positions = { 0 };
        int added = 0;
        for (int pos : positions)
        {
            // An open shape moved up a whole octave is only worth it at the 12th fret.
            if (pos >= 12 && added > 0 && pos != 12)
                continue;
            Voicing v;
            v.frets.assign (6, -1);
            bool ok = true;
            for (int s = 0; s < 6; ++s)
            {
                const int f = shape.frets[(size_t) s];
                v.frets[(size_t) s] = f == X ? -1 : f + pos;
                if (f != X && (f + pos < 0 || f + pos > guitarFrets))
                    ok = false;
            }
            if (! ok || seen (v.frets))
                continue;
            v.group = Voicing::shape;
            if (pos == 0)
            {
                v.name = "Open";
                v.detail = "Open position";
            }
            else
            {
                v.name = shape.name;
                v.detail = "Fret " + String (v.lowestFret());
            }
            findBarre (v);
            // Moved up, a shape that needs a fifth finger leaves its top string out (C7, say).
            if (fingersFor (v) > 4)
            {
                auto fewer = v;
                fewer.frets[5] = -1;
                fewer.barreFret = 0;
                fewer.barreFrom = fewer.barreTo = -1;
                findBarre (fewer);
                if (fingersFor (fewer) > 4 || ! hasEveryTone (fewer, chord) || seen (fewer.frets))
                    continue;
                v = fewer;
            }
            shapes.push_back (v);
            ++added;
        }
    }
    std::stable_sort (shapes.begin(), shapes.end(), [] (const Voicing& a, const Voicing& b)
    {
        const bool ao = a.name == "Open", bo = b.name == "Open";
        if (ao != bo)
            return ao;
        return a.lowestFret() < b.lowestFret();
    });

    // Up the neck: close voicings on adjacent strings (triads on three, 7ths on four), one for
    // each inversion on each string set, the lowest from the 2nd fret up.
    const auto chordTones = tones (chord);
    const int n = (int) chordTones.size();
    if (n >= 3)
    {
        const std::vector<std::vector<int>> sets = n == 3 ? std::vector<std::vector<int>> { { 3, 4, 5 }, { 2, 3, 4 } }
                                                          : std::vector<std::vector<int>> { { 2, 3, 4, 5 }, { 1, 2, 3, 4 } };
        static const StringArray stringNames { "E", "A", "D", "G", "B", "E" };
        for (auto& set : sets)
        {
            std::map<int, Voicing> best;   // by the tone in the bass
            std::vector<int> f ((size_t) n, 0);
            std::function<void (int)> search = [&] (int i)
            {
                if (i == n)
                {
                    int lo = 99, hi = 0;
                    std::vector<int> used;
                    for (int k = 0; k < n; ++k)
                    {
                        lo = jmin (lo, f[(size_t) k]);
                        hi = jmax (hi, f[(size_t) k]);
                        used.push_back (pc (tuning()[(size_t) set[(size_t) k]] + f[(size_t) k]));
                    }
                    if (lo < 2 || hi - lo > 3)
                        return;
                    auto sorted = used;
                    std::sort (sorted.begin(), sorted.end());
                    if (std::adjacent_find (sorted.begin(), sorted.end()) != sorted.end())
                        return;   // every tone once
                    const int bassTone = (int) (std::find (chordTones.begin(), chordTones.end(), used[0]) - chordTones.begin());
                    auto it = best.find (bassTone);
                    if (it != best.end() && it->second.lowestFret() <= lo)
                        return;
                    Voicing v;
                    v.frets.assign (6, -1);
                    for (int k = 0; k < n; ++k)
                        v.frets[(size_t) set[(size_t) k]] = f[(size_t) k];
                    v.group = Voicing::upTheNeck;
                    String on;
                    for (int s : set)
                        on << (on.isEmpty() ? "" : " ") << stringNames[s];
                    v.name = (n == 3 ? "Triad on " : "On ") + on;
                    v.detail = inversionName (bassTone) + ", fret " + String (lo);
                    best[bassTone] = v;
                    return;
                }
                for (int fret = 1; fret <= guitarFrets; ++fret)
                {
                    const int p = pc (tuning()[(size_t) set[(size_t) i]] + fret);
                    if (std::find (chordTones.begin(), chordTones.end(), p) == chordTones.end())
                        continue;
                    f[(size_t) i] = fret;
                    search (i + 1);
                }
            };
            search (0);
            for (auto& [tone, v] : best)
                if (! seen (v.frets))
                {
                    findBarre (v);
                    neck.push_back (v);
                }
        }
        std::stable_sort (neck.begin(), neck.end(), [] (const Voicing& a, const Voicing& b)
        {
            return a.lowestFret() < b.lowestFret();
        });
    }

    shapes.insert (shapes.end(), neck.begin(), neck.end());
    return shapes;
}
} // namespace chords
} // namespace lathe
