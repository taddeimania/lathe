#include "Notation.h"
#include <algorithm>
#include <cmath>
#include <map>

using namespace juce;

namespace lathe::notation
{
namespace
{
constexpr double eps = 1.0e-6;

struct Drum { const char* name; int pitch; const char* aliases; };
const Drum kDrums[] = {
    { "kick", 36, "bd" }, { "rim", 37, "rs" }, { "snare", 38, "sd" }, { "clap", 39, "cp" },
    { "hat", 42, "hh chh" }, { "phat", 44, "ph" }, { "ohat", 46, "oh ohh" },
    { "ltom", 41, "lt" }, { "mtom", 45, "mt" }, { "htom", 48, "ht" },
    { "crash", 49, "cr" }, { "ride", 51, "rd" }, { "tamb", 54, "" }, { "cowbell", 56, "cb" },
};

/** 1-9 in a grid: nine even steps up to 127. */
int tierVelocity (int digit)  { return roundToInt (127.0 * digit / 9.0); }

bool nearInt (double v)       { return std::abs (v - std::round (v)) < eps; }

String number (double v, int decimals)
{
    auto s = String (v, decimals);
    if (s.containsChar ('.'))
        s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    return s == "-0" ? String ("0") : s;
}

//==============================================================================
// Parsing

struct Mods
{
    int velocity = -1;              // -1: not set
    bool hasOffset = false;
    double offsetMs = 0.0, offsetPercent = 0.0;
    bool percent = false;
    double length = -1.0;           // beats; -1: not set
    int times = 1;

    /** This item's own settings, falling back to the group's. */
    Mods over (const Mods& outer) const
    {
        Mods m = *this;
        if (m.velocity < 0)       m.velocity = outer.velocity;
        if (! m.hasOffset)        { m.hasOffset = outer.hasOffset; m.offsetMs = outer.offsetMs; m.offsetPercent = outer.offsetPercent; m.percent = outer.percent; }
        if (m.length < 0.0)       m.length = outer.length;
        m.times = times;
        return m;
    }
};

struct Item
{
    enum class Kind { note, rest, hold, group, alt } kind = Kind::rest;
    int pitch = -1;
    int defaultVelocity = 100;
    std::vector<std::vector<Item>> parallel;   // group: sequences played at once
    std::vector<Item> alternatives;            // alt: one per bar
    Mods mods;
};

struct Feel
{
    double swing = 50.0, swingUnit = 0.25;   // percent, beats
    double humanizeMs = 0.0;
    int humanizeVel = 0;
    Random random { 1 };
};

class LineParser
{
public:
    LineParser (const String& text, int lanePitch) : s (text), lane (lanePitch) {}

    /** A bar's items, or an error in `error`. */
    std::vector<Item> parseBar()
    {
        auto items = sequence (0, false);
        skipSpaces();
        if (error.isEmpty() && i < s.length())
            fail ("unexpected '" + String::charToString (s[i]) + "'");
        return items;
    }

    String error;

private:
    const String& s;
    int i = 0;
    int lane;

    void fail (const String& message) { if (error.isEmpty()) error = message; }
    void skipSpaces()                 { while (i < s.length() && CharacterFunctions::isWhitespace (s[i])) ++i; }
    static bool isBreak (juce_wchar c){ return CharacterFunctions::isWhitespace (c) || String ("[]<>,|").containsChar (c); }

    std::vector<Item> sequence (juce_wchar closer, bool commasEnd)
    {
        std::vector<Item> items;
        for (;;)
        {
            skipSpaces();
            if (! error.isEmpty() || i >= s.length())
                break;
            const auto c = s[i];
            if (c == closer || (commasEnd && c == ','))
                break;
            if (c == ']' || c == '>' || c == ',')
            {
                fail ("unexpected '" + String::charToString (c) + "'");
                break;
            }
            items.push_back (item());
        }
        return items;
    }

    Item item()
    {
        Item it;
        const auto c = s[i];
        if (c == '[')
        {
            ++i;
            it.kind = Item::Kind::group;
            for (;;)
            {
                auto seq = sequence (']', true);
                if (seq.empty())
                    fail ("empty [ ]");
                it.parallel.push_back (std::move (seq));
                if (! error.isEmpty())
                    return it;
                if (i < s.length() && s[i] == ',') { ++i; continue; }
                break;
            }
            if (i >= s.length() || s[i] != ']')
            {
                fail ("a [ isn't closed");
                return it;
            }
            ++i;
            it.mods = suffix();
            return it;
        }
        if (c == '<')
        {
            ++i;
            it.kind = Item::Kind::alt;
            it.alternatives = sequence ('>', false);
            if (i >= s.length() || s[i] != '>')
            {
                fail ("a < isn't closed");
                return it;
            }
            if (it.alternatives.empty())
                fail ("empty < >");
            for (auto& a : it.alternatives)
                if (a.kind == Item::Kind::hold)
                    fail ("_ can't be one of the < > choices");
            ++i;
            it.mods = suffix();
            return it;
        }

        const int start = i;
        while (i < s.length() && ! isBreak (s[i]))
            ++i;
        word (s.substring (start, i), it);
        return it;
    }

    /** Suffixes right after ] or >. */
    Mods suffix()
    {
        const int start = i;
        while (i < s.length() && ! isBreak (s[i]))
            ++i;
        Mods m;
        const auto text = s.substring (start, i);
        if (text.isNotEmpty())
            if (auto e = mods (text, m); e.isNotEmpty())
                fail (e);
        return m;
    }

    void word (const String& w, Item& it)
    {
        if (w == "~") { it.kind = Item::Kind::rest; return; }
        if (w == "_") { it.kind = Item::Kind::hold; return; }

        // The name: a note name (letter, #/b, octave), a MIDI number, or a word (x X o, drum names).
        int k = 0;
        const int n = w.length();
        String name;
        if (n > 0 && String ("ABCDEFGabcdefg").containsChar (w[0]))
        {
            int j = 1;
            while (j < n && (w[j] == '#' || w[j] == 'b'))
                ++j;
            int d = j;
            if (d < n && w[d] == '-')
                ++d;
            if (d < n && CharacterFunctions::isDigit (w[d]))
            {
                while (d < n && CharacterFunctions::isDigit (w[d]))
                    ++d;
                k = d;
            }
        }
        if (k == 0 && n > 0 && CharacterFunctions::isDigit (w[0]))
            while (k < n && CharacterFunctions::isDigit (w[k]))
                ++k;
        if (k == 0)
        {
            while (k < n && (CharacterFunctions::isLetter (w[k]) || w[k] == '_'))
                ++k;
            if (k > 0 && ! (k == 1 && String ("xXo").containsChar (w[0])))
                while (k < n && CharacterFunctions::isDigit (w[k]))   // "q3": shown whole in the error
                    ++k;
        }
        name = w.substring (0, k);
        if (name.isEmpty())
        {
            fail ("\"" + w + "\" isn't a note");
            return;
        }

        it.kind = Item::Kind::note;
        if (name == "x" || name == "X" || name == "o")
        {
            if (lane < 0)
            {
                fail ("\"" + name + "\" is a hit, but this line has no drum or pitch label to play (like \"snare:\")");
                return;
            }
            it.pitch = lane;
            it.defaultVelocity = name == "X" ? 120 : name == "o" ? 45 : 100;
        }
        else
        {
            it.pitch = parsePitchName (name);
            if (it.pitch < 0)
                it.pitch = drumPitch (name);
            if (it.pitch < 0)
            {
                fail ("\"" + name + "\" isn't a note (C3 is middle C; drums: kick, snare, hat...)");
                return;
            }
        }
        if (k < n)
            if (auto e = mods (w.substring (k), it.mods); e.isNotEmpty())
                fail ("\"" + w + "\": " + e);
    }

    /** :velocity  +/-Nms or +/-N%  =length  *times */
    static String mods (const String& t, Mods& m)
    {
        int j = 0;
        const int n = t.length();
        auto readNumber = [&] (bool allowDecimal) -> String
        {
            const int from = j;
            while (j < n && (CharacterFunctions::isDigit (t[j]) || (allowDecimal && t[j] == '.')))
                ++j;
            return t.substring (from, j);
        };
        while (j < n)
        {
            const auto c = t[j++];
            if (c == ':')
            {
                const auto v = readNumber (false);
                if (v.isEmpty() || v.getIntValue() < 1 || v.getIntValue() > 127)
                    return "velocity after : must be 1-127";
                m.velocity = v.getIntValue();
            }
            else if (c == '+' || c == '-')
            {
                const auto v = readNumber (true);
                if (v.isEmpty())
                    return "a number must follow " + String::charToString (c);
                const double amount = (c == '-' ? -1.0 : 1.0) * v.getDoubleValue();
                if (t.substring (j).startsWith ("ms"))       { m.offsetMs = amount; m.percent = false; j += 2; }
                else if (t.substring (j).startsWith ("%"))   { m.offsetPercent = amount; m.percent = true; j += 1; }
                else return "say ms or % after " + String::charToString (c) + v;
                m.hasOffset = true;
            }
            else if (c == '=')
            {
                const auto v = readNumber (true);
                if (v.isEmpty() || ! (v.getDoubleValue() > 0.0))
                    return "length after = must be more than 0 beats";
                m.length = v.getDoubleValue();
            }
            else if (c == '*')
            {
                const auto v = readNumber (false);
                if (v.isEmpty() || v.getIntValue() < 1 || v.getIntValue() > 64)
                    return "repeat after * must be 1-64";
                m.times = v.getIntValue();
            }
            else
            {
                return "unexpected '" + String::charToString (c) + "' (after a note: :velocity, +/-ms or %, =length, *times)";
            }
        }
        return {};
    }
};

/** Puts one line's notes in out. */
class Emitter
{
public:
    Emitter (const Context& c, Feel& f, std::vector<Note>& o) : ctx (c), feel (f), out (o) {}

    void note (int pitch, int defaultVelocity, double t, double slot, double barStart, const Mods& m)
    {
        if (t >= ctx.spanBeats - eps)
            return;
        double start = t;
        // Swing: notes exactly on the off-beat of the swing unit move later.
        if (std::abs (feel.swing - 50.0) > eps)
        {
            const double u = (t - barStart) / feel.swingUnit;
            if (nearInt (u) && ((int) std::llround (u)) % 2 == 1)
                start += 2.0 * feel.swingUnit * feel.swing / 100.0 - feel.swingUnit;
        }
        if (m.hasOffset)
            start += m.percent ? m.offsetPercent / 100.0 * slot : m.offsetMs / 1000.0 * ctx.tempo / 60.0;
        else if (feel.humanizeMs > 0.0)
            start += (feel.random.nextDouble() * 2.0 - 1.0) * feel.humanizeMs / 1000.0 * ctx.tempo / 60.0;

        int velocity = m.velocity >= 0 ? m.velocity : defaultVelocity;
        if (m.velocity < 0 && feel.humanizeVel > 0)
            velocity += feel.random.nextInt (2 * feel.humanizeVel + 1) - feel.humanizeVel;

        Note n;
        n.pitch = pitch;
        n.start = jlimit (0.0, jmax (0.0, ctx.spanBeats - eps), start);
        n.length = m.length > 0.0 ? m.length : slot;
        n.velocity = jlimit (1, 127, velocity);
        if (m.length <= 0.0)
            last.push_back (out.size());   // a hold after it can lengthen it
        out.push_back (n);
    }

    /** Lays a sequence out over [t0, t0 + dur). */
    void sequence (const std::vector<Item>& items, double t0, double dur, double barStart, int bar, const Mods& outer)
    {
        if (items.empty())
            return;
        const double d = dur / (double) items.size();
        for (size_t k = 0; k < items.size(); ++k)
            item (items[k], t0 + (double) k * d, d, barStart, bar, outer);
    }

    void item (const Item& it, double t, double d, double barStart, int bar, const Mods& outer)
    {
        switch (it.kind)
        {
            case Item::Kind::rest:
                last.clear();
                return;
            case Item::Kind::hold:
                for (auto index : last)
                    out[index].length += d;
                return;
            case Item::Kind::note:
            {
                const auto m = it.mods.over (outer);
                const double sub = d / m.times;
                std::vector<size_t> made;
                for (int r = 0; r < m.times; ++r)
                {
                    last.clear();
                    note (it.pitch, it.defaultVelocity, t + r * sub, sub, barStart, m);
                    made = last;
                }
                last = made;
                return;
            }
            case Item::Kind::group:
            {
                const auto m = it.mods.over (outer);
                Mods inner = m;
                inner.times = 1;
                const double sub = d / m.times;
                for (int r = 0; r < m.times; ++r)
                {
                    std::vector<size_t> ends;
                    for (auto& seq : it.parallel)
                    {
                        last.clear();
                        sequence (seq, t + r * sub, sub, barStart, bar, inner);
                        ends.insert (ends.end(), last.begin(), last.end());
                    }
                    last = ends;
                }
                return;
            }
            case Item::Kind::alt:
            {
                const auto& chosen = it.alternatives[(size_t) bar % it.alternatives.size()];
                const auto m = it.mods.over (outer);
                Mods inner = m;
                inner.times = 1;
                const double sub = d / m.times;
                for (int r = 0; r < m.times; ++r)
                    item (chosen, t + r * sub, sub, barStart, bar, inner);
                return;
            }
        }
    }

    std::vector<size_t> last;   // the notes a hold would lengthen

private:
    const Context& ctx;
    Feel& feel;
    std::vector<Note>& out;
};

bool isGridChar (juce_wchar c)      { return String (".-xXo123456789").containsChar (c); }

/** A run of two or more step characters, and nothing but steps, spaces and bar lines. */
bool looksLikeGrid (const String& body)
{
    int run = 0, longest = 0;
    for (auto c : body)
    {
        if (isGridChar (c))
            longest = jmax (longest, ++run);
        else if (c == ' ' || c == '\t' || c == '|')
            run = 0;
        else
            return false;
    }
    return longest >= 2;
}

bool isHitWord (const String& w)
{
    if (w.isEmpty() || ! String ("xXo").containsChar (w[0]))
        return false;
    return w.length() == 1 || String (":+-=*").containsChar (w[1]);
}

/** A word that can start a token line by itself. */
bool isToken (const String& w)
{
    if (w.isEmpty())
        return false;
    if (String ("~_[<").containsChar (w[0]) || isHitWord (w))
        return true;
    String name = w;
    for (int j = 0; j < w.length(); ++j)
        if (String (":=*").containsChar (w[j])) { name = w.substring (0, j); break; }
    // A name with an offset (c3+8ms): strip a trailing +/- part that ends in ms or %.
    for (int j = 1; j < name.length(); ++j)
        if ((name[j] == '+' || name[j] == '-') && (name.endsWith ("ms") || name.endsWith ("%")) && CharacterFunctions::isDigit (name[j - 1]))
        {
            name = name.substring (0, j);
            break;
        }
    return parsePitchName (name) >= 0 || drumPitch (name) >= 0;
}

int defaultGrid (double beatsPerBar)
{
    const double n = beatsPerBar * 4.0;
    return nearInt (n) ? (int) std::llround (n) : (int) std::llround (beatsPerBar * 8.0);
}
} // namespace

//==============================================================================
int drumPitch (const String& name)
{
    const auto n = name.trim().toLowerCase();
    if (n.isEmpty())
        return -1;
    for (auto& d : kDrums)
        if (n == d.name || StringArray::fromTokens (d.aliases, " ", "").contains (n))
            return d.pitch;
    return -1;
}

String drumName (int pitch)
{
    for (auto& d : kDrums)
        if (d.pitch == pitch)
            return d.name;
    return {};
}

int parsePitchName (const String& text)
{
    auto s = text.trim();
    if (s.isEmpty())
        return -1;
    if (s.containsOnly ("0123456789"))
        return s.length() <= 3 && s.getIntValue() <= 127 ? s.getIntValue() : -1;

    static const int semis[] { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    const auto letter = CharacterFunctions::toUpperCase (s[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int pitch = semis[letter - 'A'];
    int i = 1;
    for (; i < s.length() && (s[i] == '#' || s[i] == 'b'); ++i)
        pitch += s[i] == '#' ? 1 : -1;
    const auto octave = s.substring (i);
    const auto digits = octave.startsWith ("-") ? octave.substring (1) : octave;
    if (digits.isEmpty() || ! digits.containsOnly ("0123456789") || digits.length() > 2)
        return -1;
    pitch += (octave.getIntValue() + 2) * 12;
    return pitch >= 0 && pitch <= 127 ? pitch : -1;
}

String pitchName (int pitch)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return String (names[pitch % 12]) + String (pitch / 12 - 2);
}

//==============================================================================
Result parse (const String& text, const Context& ctx, std::vector<Note>& out)
{
    out.clear();
    std::vector<Note> notes;
    Feel feel;
    int grid = defaultGrid (ctx.beatsPerBar);
    const double bpb = ctx.beatsPerBar;
    bool anyPart = false;

    const auto lines = StringArray::fromLines (text);
    for (int ln = 0; ln < lines.size(); ++ln)
    {
        auto line = lines[ln];
        // # starts a comment at the start of a line or after a space (C#3 is a note).
        for (int c = 0; c < line.length(); ++c)
            if (line[c] == '#' && (c == 0 || CharacterFunctions::isWhitespace (line[c - 1])))
            {
                line = line.substring (0, c);
                break;
            }
        line = line.trim();
        if (line.isEmpty())
            continue;

        const auto where = "Line " + String (ln + 1);
        auto words = StringArray::fromTokens (line, " \t", "");
        words.removeEmptyStrings();
        const auto first = words[0].toLowerCase();

        // Settings
        if (first == "grid" && words.size() == 2 && words[1].containsOnly ("0123456789"))
        {
            grid = words[1].getIntValue();
            if (grid < 1 || grid > 192)
                return Result::fail (where + ": grid must be 1-192 steps per bar.");
            continue;
        }
        if (first == "swing")
        {
            if (words.size() >= 2 && words[1].equalsIgnoreCase ("off")) { feel.swing = 50.0; continue; }
            const auto p = words[1].trimCharactersAtEnd ("%");
            if (words.size() < 2 || words.size() > 3 || ! p.containsOnly ("0123456789.") || p.isEmpty()
                || p.getDoubleValue() < 50.0 || p.getDoubleValue() > 80.0)
                return Result::fail (where + ": swing is a percentage from 50 (straight) to 80, then 16 or 8 (which notes): \"swing 58%\", \"swing 62% 8\".");
            feel.swing = p.getDoubleValue();
            feel.swingUnit = 0.25;
            if (words.size() == 3)
            {
                if (words[2] == "8")        feel.swingUnit = 0.5;
                else if (words[2] != "16")  return Result::fail (where + ": swing's second number is 16 or 8.");
            }
            continue;
        }
        if (first == "humanize")
        {
            if (words.size() == 2 && words[1].equalsIgnoreCase ("off")) { feel.humanizeMs = 0.0; feel.humanizeVel = 0; continue; }
            double ms = 0.0;
            int vel = 0, seed = 1;
            for (int w = 1; w < words.size(); ++w)
            {
                const auto x = words[w].toLowerCase();
                if (x.endsWith ("ms") && x.dropLastCharacters (2).containsOnly ("0123456789."))       ms = x.dropLastCharacters (2).getDoubleValue();
                else if (x.endsWith ("vel") && x.dropLastCharacters (3).containsOnly ("0123456789")) vel = x.dropLastCharacters (3).getIntValue();
                else if (x == "seed" && w + 1 < words.size() && words[w + 1].containsOnly ("0123456789")) seed = words[++w].getIntValue();
                else return Result::fail (where + ": humanize takes amounts like \"6ms 8vel seed 3\" (or \"off\").");
            }
            if (ms < 0.0 || ms > 100.0 || vel < 0 || vel > 64)
                return Result::fail (where + ": humanize up to 100ms and 64vel.");
            feel.humanizeMs = ms;
            feel.humanizeVel = vel;
            feel.random.setSeed (seed);
            continue;
        }

        // The label, if there is one.
        String label, body = line;
        int lane = -1;
        const auto rest = line.substring (words[0].length()).trim();
        bool labelled = false;
        if (words[0].endsWith (":"))
        {
            label = words[0].dropLastCharacters (1);
            labelled = true;
        }
        else if (looksLikeGrid (rest) || ! isToken (words[0]))
        {
            label = words[0];
            labelled = true;
        }
        else if (! isHitWord (words[0]))
        {
            for (int w = 1; w < words.size() && ! labelled; ++w)
                labelled = isHitWord (words[w]);
            if (labelled)
                label = words[0];
        }
        if (labelled)
        {
            body = rest;
            lane = parsePitchName (label);
            if (lane < 0)
                lane = drumPitch (label);
        }
        const auto at = where + (label.isNotEmpty() ? " (" + label + ")" : String());
        if (body.isEmpty())
            return Result::fail (at + ": nothing after the label.");

        Emitter emit (ctx, feel, notes);
        anyPart = true;

        if (looksLikeGrid (body))
        {
            if (lane < 0)
                return Result::fail (at + ": \"" + label + "\" isn't a note or drum name, so the grid has nothing to play (kick, snare, hat, C3...).");
            // Steps, bar by bar.
            std::vector<String> bars;
            if (body.containsChar ('|'))
            {
                for (auto& b : StringArray::fromTokens (body, "|", ""))
                    bars.push_back (b.removeCharacters (" \t"));
                for (size_t b = 0; b < bars.size(); ++b)
                    if (bars[b].length() != grid)
                        return Result::fail (at + ", bar " + String ((int) b + 1) + ": " + String (bars[b].length()) + " steps, but grid is "
                                             + String (grid) + ". Each bar between | needs exactly " + String (grid) + ".");
            }
            else
            {
                const auto all = body.removeCharacters (" \t");
                if (all.length() % grid != 0)
                    return Result::fail (at + ": " + String (all.length()) + " steps isn't a whole number of bars (grid is " + String (grid)
                                         + " a bar). Add steps, or set grid first.");
                for (int b = 0; b < all.length() / grid; ++b)
                    bars.push_back (all.substring (b * grid, (b + 1) * grid));
            }
            const int lineBars = (int) bars.size();
            if (lineBars * bpb > ctx.spanBeats + eps)
                return Result::fail (at + ": the line is " + String (lineBars) + " bars, longer than the " + number (ctx.spanBeats / bpb, 2)
                                     + " bars it's written over. Shorten it or widen the range.");
            const double step = bpb / grid;
            for (size_t b = 0; b < bars.size(); ++b)
                for (int s = 0; s < grid; ++s)
                {
                    const auto c = bars[b][s];
                    if (! isGridChar (c))
                        return Result::fail (at + ", bar " + String ((int) b + 1) + ", step " + String (s + 1)
                                             + ": expected . x X o or 1-9, got '" + String::charToString (c) + "'.");
                }
            for (int loop = 0; loop * lineBars * bpb < ctx.spanBeats - eps; ++loop)
                for (size_t b = 0; b < bars.size(); ++b)
                {
                    const double barStart = ((double) loop * lineBars + (double) b) * bpb;
                    for (int s = 0; s < grid; ++s)
                    {
                        const auto c = bars[b][s];
                        if (c == '.' || c == '-')
                            continue;
                        const int velocity = c == 'x' ? 100 : c == 'X' ? 120 : c == 'o' ? 45 : tierVelocity ((int) (c - '0'));
                        emit.note (lane, velocity, barStart + s * step, step, barStart, Mods());
                    }
                }
            continue;
        }

        // A token line: bars between |, each divided evenly among its tokens.
        std::vector<std::vector<Item>> bars;
        auto segments = StringArray::fromTokens (body, "|", "");
        for (int b = 0; b < segments.size(); ++b)
        {
            if (segments[b].trim().isEmpty())
                return Result::fail (at + ", bar " + String (b + 1) + ": empty (use ~ for a bar's rest).");
            LineParser p (segments[b], lane);
            auto items = p.parseBar();
            if (p.error.isNotEmpty())
                return Result::fail (at + (segments.size() > 1 ? ", bar " + String (b + 1) : String()) + ": " + p.error + ".");
            if (items.empty())
                return Result::fail (at + ", bar " + String (b + 1) + ": empty (use ~ for a bar's rest).");
            if (items.front().kind == Item::Kind::hold && b == 0)
                return Result::fail (at + ": a line can't start with _ (it holds the note before it).");
            bars.push_back (std::move (items));
        }
        const int lineBars = (int) bars.size();
        if (lineBars * bpb > ctx.spanBeats + eps)
            return Result::fail (at + ": the line is " + String (lineBars) + " bars, longer than the " + number (ctx.spanBeats / bpb, 2)
                                 + " bars it's written over. Shorten it or widen the range.");
        for (int loop = 0; loop * lineBars * bpb < ctx.spanBeats - eps; ++loop)
            for (size_t b = 0; b < bars.size(); ++b)
            {
                const int bar = loop * lineBars + (int) b;
                const double barStart = bar * bpb;
                if (barStart >= ctx.spanBeats - eps)
                    break;
                emit.sequence (bars[b], barStart, bpb, barStart, loop, Mods());   // <a b> takes turns each time round
            }
    }

    if (! anyPart)
        return Result::fail ("The pattern has no notes in it (only settings or comments).");
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    out = std::move (notes);
    return Result::ok();
}

//==============================================================================
// Formatting
namespace
{
String velocitySuffix (int velocity, int defaultVelocity)
{
    return velocity == defaultVelocity ? String() : ":" + String (velocity);
}

/** One token line, bar by bar, for notes in [0, bars * bpb). lane >= 0: the notes are hits on it. */
StringArray tokenBars (const std::vector<Note>& notes, const Context& ctx, int bars, int lane)
{
    const double bpb = ctx.beatsPerBar;
    StringArray out;
    for (int b = 0; b < bars; ++b)
    {
        const double barStart = b * bpb;
        std::vector<Note> in;
        for (auto& n : notes)
            if (n.start >= barStart - eps && n.start < barStart + bpb - eps)
                in.push_back (n);
        if (in.empty())
        {
            out.add ("~");
            continue;
        }

        // The fewest equal steps that every note starts on, and ends on too where it ends in the bar
        // (so lengths are holds, not =); failing that, just the starts; 16ths (with offsets) if none fits.
        int slots = 0;
        for (bool ends : { true, false })
        {
            for (double perBeat : { 0.25, 0.5, 1.0, 2.0, 4.0, 3.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0 })
            {
                const double m = bpb * perBeat;
                if (! nearInt (m) || m < 1.0)
                    continue;
                const int M = (int) std::llround (m);
                bool fits = true;
                for (auto& n : in)
                {
                    fits = fits && nearInt ((n.start - barStart) / (bpb / M));
                    const double end = n.start + n.length - barStart;
                    if (ends && end <= bpb + eps)
                        fits = fits && nearInt (end / (bpb / M));
                }
                if (fits) { slots = M; break; }
            }
            if (slots > 0)
                break;
        }
        if (slots == 0)
            slots = defaultGrid (bpb);
        const double d = bpb / slots;

        std::vector<std::vector<Note>> at ((size_t) slots);
        for (auto& n : in)
        {
            const int k = jlimit (0, slots - 1, (int) std::floor ((n.start - barStart) / d + 0.5));
            at[(size_t) k].push_back (n);
        }

        StringArray tokens;
        int heldUntil = -1;
        for (int k = 0; k < slots; ++k)
        {
            auto& stack = at[(size_t) k];
            if (stack.empty())
            {
                tokens.add (k <= heldUntil ? "_" : "~");
                continue;
            }
            // Holds when every note in the step lasts the same whole number of steps, all inside the bar.
            int hold = 0;
            {
                const double steps = stack.front().length / d;
                bool same = nearInt (steps) && steps >= 1.0 - eps;
                for (auto& n : stack)
                    same = same && std::abs (n.length - stack.front().length) < eps;
                const int h = same ? (int) std::llround (steps) - 1 : -1;
                bool clear = same && k + h < slots;
                for (int j = k + 1; clear && j <= k + h; ++j)
                    clear = at[(size_t) j].empty();
                hold = clear ? h : -1;
            }
            if (hold > 0)
                heldUntil = k + hold;
            else
                heldUntil = -1;

            StringArray members;
            for (auto& n : stack)
            {
                String t;
                int defaultVelocity = 100;
                if (lane >= 0)
                {
                    t = n.velocity == 120 ? "X" : n.velocity == 45 ? "o" : "x";
                    defaultVelocity = n.velocity == 120 ? 120 : n.velocity == 45 ? 45 : 100;
                }
                else
                {
                    t = pitchName (n.pitch);
                }
                t << velocitySuffix (n.velocity, defaultVelocity);
                const double offset = (n.start - (barStart + k * d)) / d * 100.0;
                if (std::abs (offset) > 0.0005)
                    t << (offset > 0 ? "+" : "-") << number (std::abs (offset), 3) << "%";
                if (hold < 0)
                    t << "=" << number (n.length, 6);
                members.add (t);
            }
            tokens.add (members.size() == 1 ? members[0] : "[" + members.joinIntoString (",") + "]");
        }
        // A whole bar of one held note or one hit reads better short: "c3" instead of "c3 _ _ _".
        out.add (tokens.joinIntoString (" "));
    }
    return out;
}

/** A grid line for one lane if every hit is on one grid, lasts a step and uses a grid velocity. */
bool gridBars (const std::vector<Note>& notes, const Context& ctx, int bars, int stepsPerBar, StringArray& out)
{
    const double bpb = ctx.beatsPerBar;
    const double step = bpb / stepsPerBar;
    std::vector<String> cells ((size_t) bars, String::repeatedString (".", stepsPerBar));
    for (auto& n : notes)
    {
        if (! nearInt (n.start / step) || std::abs (n.length - step) > eps)
            return false;
        const auto index = (int) std::llround (n.start / step);
        const int b = index / stepsPerBar, s = index % stepsPerBar;
        if (b >= bars || cells[(size_t) b][s] != '.')
            return false;
        String c;
        if (n.velocity == 100)       c = "x";
        else if (n.velocity == 120)  c = "X";
        else if (n.velocity == 45)   c = "o";
        else
            for (int d = 1; d <= 9 && c.isEmpty(); ++d)
                if (tierVelocity (d) == n.velocity)
                    c = String (d);
        if (c.isEmpty())
            return false;
        cells[(size_t) b] = cells[(size_t) b].replaceSection (s, 1, c);
    }
    // Spaces every beat, to read like a drum machine.
    const double perBeat = stepsPerBar / bpb;
    const int group = nearInt (perBeat) && perBeat >= 2.0 ? (int) std::llround (perBeat) : stepsPerBar;
    out.clear();
    for (auto& bar : cells)
    {
        String spaced;
        for (int s = 0; s < bar.length(); ++s)
        {
            if (s > 0 && s % group == 0)
                spaced << " ";
            spaced << bar[s];
        }
        out.add (spaced);
    }
    return true;
}

/** The shortest loop of bars that the line repeats (lines loop to fill the range). */
StringArray shortestLoop (const StringArray& bars)
{
    for (int p = 1; p < bars.size(); ++p)
    {
        bool repeats = true;
        for (int b = p; b < bars.size() && repeats; ++b)
            repeats = bars[b] == bars[b % p];
        if (repeats)
        {
            StringArray loop;
            for (int b = 0; b < p; ++b)
                loop.add (bars[b]);
            return loop;
        }
    }
    return bars;
}
} // namespace

String format (std::vector<Note> notes, const Context& ctx, bool drums)
{
    const double bpb = ctx.beatsPerBar;
    const int bars = jmax (1, (int) std::ceil (ctx.spanBeats / bpb - eps));
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b)
                      { return a.start < b.start - eps || (std::abs (a.start - b.start) <= eps && a.pitch > b.pitch); });

    StringArray lines;
    const int normalGrid = defaultGrid (bpb);
    int currentGrid = normalGrid;

    if (drums)
    {
        std::map<int, std::vector<Note>> lanes;
        for (auto& n : notes)
            lanes[n.pitch].push_back (n);
        int width = 0;
        for (auto& [pitch, list] : lanes)
            width = jmax (width, (drumName (pitch).isNotEmpty() ? drumName (pitch) : pitchName (pitch)).length() + 1);

        for (auto& [pitch, list] : lanes)
        {
            const auto name = drumName (pitch).isNotEmpty() ? drumName (pitch) : pitchName (pitch);
            StringArray cells;
            int chosen = 0;
            std::vector<int> tries { currentGrid };
            for (double perBeat : { 4.0, 3.0, 6.0, 8.0, 2.0, 1.0, 12.0, 16.0, 24.0 })
                if (nearInt (bpb * perBeat))
                    tries.push_back ((int) std::llround (bpb * perBeat));
            for (int g : tries)
                if (gridBars (list, ctx, bars, g, cells)) { chosen = g; break; }

            if (chosen > 0)
            {
                if (chosen != currentGrid)
                {
                    lines.add ("grid " + String (chosen));
                    currentGrid = chosen;
                }
                lines.add (name.paddedRight (' ', width + 1) + shortestLoop (cells).joinIntoString (" | "));
            }
            else
            {
                lines.add ((name + ":").paddedRight (' ', width + 1) + shortestLoop (tokenBars (list, ctx, bars, pitch)).joinIntoString (" | "));
            }
        }
        return lines.joinIntoString ("\n");
    }

    // Voices: a note joins the first line whose notes have ended, or as a chord with notes that
    // start and end with it.
    struct Voice { std::vector<Note> notes; double end = 0.0, lastStart = -1.0, lastLength = 0.0; };
    std::vector<Voice> voices;
    for (auto& n : notes)
    {
        Voice* home = nullptr;
        for (auto& v : voices)
            if ((std::abs (v.lastStart - n.start) <= eps && std::abs (v.lastLength - n.length) <= eps) || v.end <= n.start + eps)
            {
                home = &v;
                break;
            }
        if (home == nullptr)
            home = &voices.emplace_back();
        home->notes.push_back (n);
        home->end = jmax (home->end, n.start + n.length);
        home->lastStart = n.start;
        home->lastLength = n.length;
    }
    for (auto& v : voices)
        lines.add (shortestLoop (tokenBars (v.notes, ctx, bars, -1)).joinIntoString (" | "));
    return lines.joinIntoString ("\n");
}

//==============================================================================
String guide()
{
    return R"(Lathe note notation (write_notes "pattern", get_notes answers in it)

A pattern is lines; # starts a comment. It fills from_bar to to_bar: every line LOOPS to fill
that range (a one-bar line plays in every bar). repeat still repeats the whole range.
Pitches: note names, C3 = 60 = middle C (F#2, Bb1, C-1), or MIDI numbers. Drum names:
kick(bd) 36, rim(rs) 37, snare(sd) 38, clap(cp) 39, hat(hh) 42, phat 44, ohat(oh) 46,
ltom 41, mtom 45, htom 48, crash 49, ride 51, tamb 54, cowbell(cb) 56.

GRID LINES: a lane (drum or pitch), then one character per step.
  grid 16                       steps per bar from here on (default: 16ths; 12 = 8th triplets in 4/4)
  kick   x... ..x. x... ....
  snare  .... x... .... x..o
  hat    8.6. 9.6. 8.6. 9.67
  . or -  rest      x  hit (velocity 100)    X  accent (120)    o  ghost (45)
  1-9     hit at 14, 28, 42, 56, 71, 85, 99, 113, 127
  Spaces are ignored. | marks bars (then each bar needs exactly "grid" steps). A hit lasts one step.

TOKEN LINES: for melodies, chords and exact values. Optional label ending in ":"; if it's a
drum or pitch, x X o in the line play it. | separates bars; each bar is split evenly among its
tokens (no | = a one-bar line).
  bass:  c2:110 ~ [c2:80 c2:70-6ms] ~ | f1:105=2 _ ~ g1:90+4ms
  keys:  [c3,e3,g3] _ _ ~ | <[a2,c3,e3] [f2,a2,c3]> _ _ ~
  snare: ~ x:96+6ms ~ x:100+8ms
  c3 F#2 36 snare  a note          x X o  a hit on the label's lane
  ~       rest                     _      hold: the note before lasts one more step
  [a b]   share one step           [a,b]  together (chord)    [a b, c d]  two sequences at once
  <a b>   a the 1st time the line plays, b the 2nd... (a one-bar line: bar by bar)   a*3  three times in its step

SUFFIXES on any note, hit or group, no spaces, in this order:
  :96      velocity 1-127 (default 100)
  +8ms -5ms   later/earlier in milliseconds;  +20% -10%  as a share of the note's own step
  =1.5     length in beats (default: its step plus holds)
  *3       repeat within the step
  e.g. c3:96+8ms=1.5   [c3,e3,g3]:80 (applies to notes inside that don't set their own)

FEEL (applies to the lines after it):
  swing 58%        every 2nd 16th moved to 58% of its 8th (50 = straight);  swing 62% 8 for 8ths
  humanize 6ms 8vel seed 3   small random timing/velocity; same seed = same notes. swing off / humanize off

TIPS: one line per drum is easiest. A 4-bar part over 16 bars: write the 4 bars, it loops.
Use a token line only where you need exact timing, lengths or velocities. get_notes shows a
part in this notation; edit it and write it back over the same bars. Mistakes are reported
with the line, bar and step, and nothing is written until the whole pattern is right.)";
}
} // namespace lathe::notation
