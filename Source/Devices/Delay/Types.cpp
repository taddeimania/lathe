#include "Types.h"
#include "Dsp.h"

#include <cctype>
#include <cstdio>

namespace lathe::dly
{
namespace
{
TypeInfo make (const char* name, const char* category, const char* blurb, const char* keywords, float minMs, float maxMs,
               Starting start)
{
    TypeInfo t;
    t.name = name;
    t.category = category;
    t.blurb = blurb;
    t.keywords = keywords;
    t.minMs = minMs;
    t.maxMs = maxMs;
    t.start = start;
    t.charLabel[0] = t.charLabel[1] = t.charLabel[2] = "";
    t.modeLabel = "";
    return t;
}

void knobs (TypeInfo& t, const char* a, const char* tipA, const char* b = "", const char* tipB = "",
            const char* c = "", const char* tipC = "")
{
    t.charLabel[0] = a; t.charTips[0] = tipA;
    t.charLabel[1] = b; t.charTips[1] = tipB;
    t.charLabel[2] = c; t.charTips[2] = tipC;
}

void choice (TypeInfo& t, const char* label, std::vector<const char*> names, const char* tip)
{
    t.modeLabel = label;
    t.modeNames = std::move (names);
    t.modeTip = tip;
}

Starting st (float timeMs, float repeats, float filter, float grit, float speed, float depth,
             float a = 0.0f, float b = 0.0f, float c = 0.0f, int mode = 0)
{
    Starting s;
    s.timeMs = timeMs; s.repeats = repeats; s.filter = filter; s.grit = grit;
    s.speed = speed; s.depth = depth; s.a = a; s.b = b; s.c = c; s.mode = mode;
    return s;
}

// Where a ratio list's entry sits on a 0..1 knob.
float ratioKnob (int index, int count) { return count > 1 ? (float) index / (float) (count - 1) : 0.0f; }

std::vector<TypeInfo> build()
{
    std::vector<TypeInfo> v;

    // --- CLEAN -------------------------------------------------------------------
    {
        auto t = make ("Digital", "CLEAN",
                       "A clear, voiced digital delay: pristine with FILTER and GRIT down, analog-voiced at noon, tape-voiced past it.",
                       "clean clear pristine modern 80s eighties studio simple standard slapback vocal guitar",
                       10.0f, 4000.0f, st (400.0f, 0.4f, 0.0f, 0.0f, 0.3f, 0.05f, 0.5f));
        knobs (t, "OFFSET", "Moves one side's echoes off the other's: left of centre delays the left more, right of centre the right. Centre is even. A little goes a long way for width.");
        t.bipolar[0] = true;
        v.push_back (t);
    }
    {
        auto t = make ("Dual", "CLEAN",
                       "Two delays at a musical ratio, one feeding the other or side by side, left and right.",
                       "two double twin multi rhythmic polyrhythm stereo wide ratio series parallel",
                       20.0f, 4000.0f, st (500.0f, 0.35f, 0.15f, 0.0f, 0.3f, 0.05f, ratioKnob (4, 9), 0.35f, 0.8f, 0));
        knobs (t, "TIME 2", "The second delay's time as a fraction or multiple of TIME. It follows TIME, SYNC and TAP.",
                  "REPEATS 2", "The second delay's repeats.",
                  "LEVEL 2", "The second delay's level against the first.");
        choice (t, "ROUTING", { "SERIES", "PARALLEL" },
                "SERIES: the first delay feeds the second, like two pedals. PARALLEL: both hear the input; the first plays left, the second right.");
        v.push_back (t);
    }
    {
        auto t = make ("Pattern", "CLEAN",
                       "Sixteen rhythms of taps across each echo, spread across the stereo field. The rhythm repeats with REPEATS.",
                       "rhythm rhythmic multitap multi tap taps groove dotted triplet gallop clave tresillo bounce scatter reflections",
                       60.0f, 4000.0f, st (500.0f, 0.3f, 0.1f, 0.0f, 0.3f, 0.05f, 0.7f, 0.5f, 0.0f, 0));
        knobs (t, "SPREAD", "How far the taps are spread left and right. 0% is mono.",
                  "LEAN", "Bends the rhythm: left of centre crowds the taps late, right of centre crowds them early.");
        t.bipolar[1] = true;
        std::vector<const char*> names;
        for (const auto& p : patterns()) names.push_back (p.name);
        choice (t, "PATTERN", names, "The rhythm of taps inside each echo.");
        v.push_back (t);
    }

    // --- VINTAGE -----------------------------------------------------------------
    {
        auto t = make ("Tape", "VINTAGE",
                       "A tape echo with up to three playback heads: AGE, BIAS, CRINKLE and WOW are the machine's wear and tear.",
                       "tape echo vintage warm wow flutter saturation heads dub reggae slapback rockabilly 60s 70s",
                       30.0f, 2500.0f, st (350.0f, 0.45f, 0.35f, 0.25f, 0.2f, 0.3f, 0.3f, 0.15f, 0.0f, 0));
        knobs (t, "LOW END", "Shapes the low end of the echoes, from full and boomy to thin. With lots of repeats it's most of the machine's sound.",
                  "HISS", "Tape hiss under the echoes.");
        choice (t, "HEADS", { "1 HEAD", "HEADS 1+2", "HEADS 2+3", "HEADS 1+3", "HEADS 1+2+3" },
                "Which playback heads are on. TIME is the furthest head; the others sit closer, for rhythmic echoes.");
        t.filterCaption = "AGE";
        t.gritCaption = "BIAS";
        t.speedCaption = "CRINKLE";
        t.depthCaption = "WOW";
        v.push_back (t);
    }
    {
        auto t = make ("Bucket", "VINTAGE",
                       "A bucket-brigade analog delay: dark, warm and smeared, getting darker and grittier as the time gets longer.",
                       "analog analogue bbd bucket brigade warm dark vintage chorus",
                       20.0f, 800.0f, st (300.0f, 0.45f, 0.5f, 0.3f, 0.35f, 0.15f, 0.4f, 0.2f, 0.0f, 0));
        knobs (t, "COMPANDER", "The compander that kept these chips quiet: it breathes and pumps on the echoes as you turn it up.",
                  "HISS", "The chips' hiss and clock noise.");
        choice (t, "CHIPS", { "SINGLE", "DOUBLE" },
                "SINGLE: one 4096-stage chip, up to 400 ms, warm and fuzzy. DOUBLE: two in a row, up to 800 ms, cleaner at the same time.");
        t.gritCaption = "LOSS";
        v.push_back (t);
    }
    {
        auto t = make ("Lo-Fi", "VINTAGE",
                       "Creative destruction: low sample rates, few bits, vinyl crackle and the sound of old speakers.",
                       "lofi lo fi bitcrush crush bits sample rate aliasing vinyl crackle radio phone telephone megaphone broken",
                       2.0f, 4000.0f, st (380.0f, 0.4f, 0.2f, 0.3f, 0.3f, 0.1f, 0.45f, 0.5f, 0.3f, 3));
        knobs (t, "SAMPLE RATE", "The echoes' sample rate, from 48 kHz down to 750 Hz. Low settings alias wildly.",
                  "BITS", "The echoes' bit depth, from 24 bits down to 4. Fewer bits, more fuzz.",
                  "VINYL", "Dust and crackle. The first half crackles only with the echoes; the second half crackles all the time.");
        choice (t, "SHAPE", { "OFF", "AMP", "VICTROLA", "RADIO", "MEGAPHONE", "PHONE", "INTERCOM", "WALKIE" },
                "The speaker the echoes come out of: an old amp, a gramophone, a clock radio, a megaphone, a telephone, an intercom or a walkie-talkie.");
        v.push_back (t);
    }

    // --- MOVING ------------------------------------------------------------------
    {
        auto t = make ("Trem", "MOVING",
                       "A tremolo on the echoes, locked to the delay time.",
                       "tremolo trem pulse chop stutter rhythmic mandolin volume amplitude",
                       60.0f, 4000.0f, st (450.0f, 0.45f, 0.15f, 0.0f, 0.3f, 0.05f, 0.7f, ratioKnob (5, 10), 0.0f, 0));
        knobs (t, "TREM", "How deep the tremolo goes. All the way up, the echoes go silent at the bottom of each wave.",
                  "RATE", "Tremolo cycles per echo. It follows TIME, SYNC and TAP.");
        choice (t, "SHAPE", { "SINE", "TRIANGLE", "SQUARE", "SAW", "RAMP" },
                "The tremolo's shape: SINE and TRIANGLE pulse, SQUARE chops, SAW plucks, RAMP swells backwards.");
        v.push_back (t);
    }
    {
        auto t = make ("Filter", "MOVING",
                       "A resonant filter sweeping the echoes, locked to the delay time or following your playing.",
                       "filter sweep wah auto envelope resonant resonance synth lfo phaser",
                       60.0f, 4000.0f, st (450.0f, 0.45f, 0.45f, 0.0f, 0.3f, 0.05f, 0.6f, 0.55f, ratioKnob (3, 10), 0));
        knobs (t, "SWEEP", "How far the filter sweeps either side of CENTRE.",
                  "RESONANCE", "How sharp the filter's peak is. High settings whistle and get louder: use MIX to balance.",
                  "RATE", "Sweeps per echo. It follows TIME, SYNC and TAP. (ENVELOPE ignores it.)");
        choice (t, "SHAPE", { "SINE", "TRIANGLE", "SQUARE", "RAMP", "SAW", "RANDOM", "ENVELOPE" },
                "How the filter moves. ENVELOPE follows how hard you play, like an auto-wah.");
        t.filterCaption = "CENTRE";
        v.push_back (t);
    }
    {
        auto t = make ("Swell", "MOVING",
                       "Each note's attack is faded out so the echoes swell in. Set RISE near the delay time.",
                       "swell volume pedal violin bow fade ambient pad attack",
                       60.0f, 4000.0f, st (500.0f, 0.5f, 0.1f, 0.0f, 0.3f, 0.1f, 0.45f, 0.0f, 0.0f, 0));
        knobs (t, "RISE", "How long each swell takes to rise. Try it about the same as the delay time.");
        choice (t, "SWELLS", { "REPEATS", "EVERYTHING" },
                "REPEATS: only the echoes swell. EVERYTHING: the dry sound swells too, like a volume pedal.");
        v.push_back (t);
    }

    // --- STRANGE -----------------------------------------------------------------
    {
        auto t = make ("Reverse", "STRANGE",
                       "Each slice of what you play comes back backwards. The slices start when you do, so it stays in time.",
                       "reverse backwards backward psychedelic beatles hendrix swell octave",
                       80.0f, 2000.0f, st (600.0f, 0.25f, 0.1f, 0.0f, 0.3f, 0.05f, 0.5f, 0.0f, 0.0f, 0));
        knobs (t, "SMOOTH", "How softly each backwards slice fades in and out. Low is choppy, high is a smooth swell.");
        choice (t, "PLAYBACK", { "REVERSE", "REVERSE -8VA", "REVERSE +8VA" },
                "REVERSE plays the slice backwards; -8VA an octave down (at half speed), +8VA an octave up.");
        v.push_back (t);
    }
    {
        auto t = make ("Ice", "STRANGE",
                       "The echoes are sliced and pitched: glassy octaves and fifths, shimmering as they repeat.",
                       "ice shimmer pitch octave fifth harmony glass crystal slice granular grain ambient",
                       60.0f, 2500.0f, st (500.0f, 0.5f, 0.1f, 0.0f, 0.3f, 0.05f, 0.35f, 0.6f, 0.5f, 6));
        knobs (t, "SLICE", "How long each pitched slice is: short for glitter, long for smooth shimmer.",
                  "BLEND", "The plain echo against the pitched one, on every repeat. Below half keeps it musical; with lots of repeats it climbs.",
                  "CHOP", "Gaps between the slices. 0% is a smooth shimmer; up is glassy and stuttering.");
        choice (t, "INTERVAL", { "-1 OCT", "-5TH", "-4TH", "+3RD", "+4TH", "+5TH", "+1 OCT", "+1 OCT +5TH", "+2 OCT" },
                "How far each slice is pitched, every time round.");
        v.push_back (t);
    }
    {
        auto t = make ("Cloud", "STRANGE",
                       "Echoes smeared into a drifting wash, somewhere between a delay and a reverb.",
                       "cloud ambient diffuse diffusion wash pad reverb space smear dreamy drone",
                       30.0f, 2500.0f, st (320.0f, 0.55f, 0.25f, 0.0f, 0.25f, 0.25f, 0.7f, 0.5f, 0.0f, 0));
        knobs (t, "DIFFUSE", "How much each echo is smeared. Low you hear the echoes; high they melt into a wash.",
                  "SIZE", "How big the smear is: a short blur to a long bloom.");
        v.push_back (t);
    }
    return v;
}
} // namespace

const std::vector<TypeInfo>& types()
{
    static const std::vector<TypeInfo> list = build();
    return list;
}

const TypeInfo& typeInfo (int index)
{
    const auto& t = types();
    return t[(size_t) std::max (0, std::min ((int) t.size() - 1, index))];
}

const std::vector<const char*>& categories()
{
    static const std::vector<const char*> c { "CLEAN", "VINTAGE", "MOVING", "STRANGE" };
    return c;
}

float timeFor (int type, int mode, float ms)
{
    const auto& t = typeInfo (type);
    float hi = t.maxMs;
    if (type == TBucket && mode == 0) hi = 400.0f;
    return clampf (ms, t.minMs, hi);
}

const std::vector<Ratio>& dualRatios()
{
    static const std::vector<Ratio> r {
        { "x 1/4", 0.25f }, { "x 1/3", 1.0f / 3.0f }, { "x 1/2", 0.5f }, { "x 2/3", 2.0f / 3.0f }, { "x 3/4", 0.75f },
        { "x 1", 1.0f }, { "x 4/3", 4.0f / 3.0f }, { "x 3/2", 1.5f }, { "x 2", 2.0f } };
    return r;
}

const std::vector<Ratio>& lfoRatios()
{
    static const std::vector<Ratio> r {
        { "1/4 PER ECHO", 0.25f }, { "1/3 PER ECHO", 1.0f / 3.0f }, { "1/2 PER ECHO", 0.5f }, { "1 PER ECHO", 1.0f },
        { "3/2 PER ECHO", 1.5f }, { "2 PER ECHO", 2.0f }, { "3 PER ECHO", 3.0f }, { "4 PER ECHO", 4.0f },
        { "6 PER ECHO", 6.0f }, { "8 PER ECHO", 8.0f } };
    return r;
}

int ratioIndex (float knob, int count)
{
    return std::max (0, std::min (count - 1, (int) std::lround (clampf (knob, 0.0f, 1.0f) * (float) (count - 1))));
}

int iceSemis (int mode)
{
    static const int s[] { -12, -7, -5, 4, 5, 7, 12, 19, 24 };
    return s[std::max (0, std::min (8, mode))];
}

const std::vector<Pattern>& patterns()
{
    static const std::vector<Pattern> p = []
    {
        std::vector<Pattern> v {
            { "EIGHTHS",     { { 0.5f, 0.8f, -0.5f }, { 1.0f, 1.0f, 0.5f } } },
            { "TRIPLETS",    { { 1.0f / 3.0f, 0.7f, -0.6f }, { 2.0f / 3.0f, 0.8f, 0.6f }, { 1.0f, 1.0f, 0.0f } } },
            { "SIXTEENTHS",  { { 0.25f, 0.5f, -0.7f }, { 0.5f, 0.65f, 0.7f }, { 0.75f, 0.8f, -0.3f }, { 1.0f, 1.0f, 0.3f } } },
            { "DOTTED",      { { 0.375f, 0.65f, -0.6f }, { 0.75f, 0.9f, 0.6f } } },
            { "GALLOP",      { { 0.5f, 0.85f, -0.5f }, { 0.75f, 0.7f, 0.5f }, { 1.0f, 1.0f, 0.0f } } },
            { "SHUFFLE",     { { 2.0f / 3.0f, 0.75f, -0.5f }, { 1.0f, 1.0f, 0.5f } } },
            { "TRESILLO",    { { 0.375f, 0.9f, -0.6f }, { 0.75f, 0.9f, 0.6f } } },
            { "CLAVE",       { { 0.1875f, 0.7f, -0.6f }, { 0.375f, 0.8f, 0.6f }, { 0.625f, 0.7f, -0.3f }, { 0.75f, 0.9f, 0.3f } } },
            { "BOUNCE",      { { 0.4f, 1.0f, -0.8f }, { 0.65f, 0.75f, 0.8f }, { 0.8f, 0.55f, -0.5f }, { 0.9f, 0.4f, 0.5f },
                               { 0.96f, 0.28f, -0.2f }, { 1.0f, 0.2f, 0.2f } } },
            { "RISE",        { { 0.25f, 0.2f, -0.8f }, { 0.5f, 0.4f, -0.3f }, { 0.75f, 0.65f, 0.3f }, { 1.0f, 1.0f, 0.8f } } },
            { "DOUBLE TAP",  { { 0.9f, 0.8f, -0.4f }, { 1.0f, 1.0f, 0.4f } } },
            { "SCATTER",     { { 0.13f, 0.5f, 0.7f }, { 0.29f, 0.4f, -0.9f }, { 0.41f, 0.65f, 0.2f }, { 0.62f, 0.5f, -0.5f },
                               { 0.71f, 0.35f, 0.9f }, { 0.88f, 0.75f, -0.2f } } },
            { "SWEEP", {} },
            { "THREE ON FOUR", { { 0.25f, 0.6f, -0.7f }, { 1.0f / 3.0f, 0.6f, 0.7f }, { 0.5f, 0.7f, -0.4f }, { 2.0f / 3.0f, 0.7f, 0.4f },
                                 { 0.75f, 0.8f, -0.1f }, { 1.0f, 1.0f, 0.1f } } },
            { "FIVES", {} },
            { "REFLECTIONS", {} },
        };
        for (int k = 1; k <= 8; ++k)
            v[12].taps.push_back ({ (float) k / 8.0f, 0.85f, -1.0f + 2.0f * (float) (k - 1) / 7.0f });
        for (int k = 1; k <= 5; ++k)
            v[14].taps.push_back ({ (float) k / 5.0f, 0.95f - 0.1f * (float) (k - 1), (k % 2 == 0 ? 0.6f : -0.6f) });
        // Early-reflection style: gaps opening out, levels falling.
        Rng rng (11);
        for (int k = 0; k < 14; ++k)
        {
            const float x = (float) (k + 1) / 14.0f;
            v[15].taps.push_back ({ 0.03f + 0.97f * x * x, 0.95f * std::pow (0.86f, (float) k), rng.bi() });
        }
        return v;
    }();
    return p;
}

// ---------------------------------------------------------------------------
// Knob text
// ---------------------------------------------------------------------------
std::string charText (int type, int slot, float v)
{
    char buf[48];
    auto pct = [&]
    {
        std::snprintf (buf, sizeof buf, "%d%%", (int) std::lround (v * 100.0f));
        return std::string (buf);
    };
    auto ms = [&] (float x)
    {
        if (x >= 1000.0f) std::snprintf (buf, sizeof buf, "%.2f s", x / 1000.0f);
        else std::snprintf (buf, sizeof buf, "%d ms", (int) std::lround (x));
        return std::string (buf);
    };
    auto bip = [&]
    {
        const int i = (int) std::lround ((v - 0.5f) * 200.0f);
        if (i == 0) return std::string ("CENTRE");
        std::snprintf (buf, sizeof buf, "%s%d%%", i > 0 ? "R +" : "L +", std::abs (i));
        return std::string (buf);
    };
    switch (type)
    {
        case TDigital:
            if (slot == 0) return bip();
            return pct();
        case TDual:
            if (slot == 0) return dualRatios()[(size_t) ratioIndex (v, (int) dualRatios().size())].name;
            return pct();
        case TPattern:
            if (slot == 1)
            {
                const int i = (int) std::lround ((v - 0.5f) * 200.0f);
                if (i == 0) return "STRAIGHT";
                std::snprintf (buf, sizeof buf, "%s %d%%", i > 0 ? "EARLY" : "LATE", std::abs (i));
                return buf;
            }
            return pct();
        case TLofi:
            if (slot == 0)
            {
                const float hz = expMap (v, 750.0f, 48000.0f);
                if (hz >= 1000.0f) std::snprintf (buf, sizeof buf, "%.1f kHz", hz / 1000.0f);
                else std::snprintf (buf, sizeof buf, "%d Hz", (int) std::lround (hz));
                return buf;
            }
            if (slot == 1)
            {
                std::snprintf (buf, sizeof buf, "%d BITS", (int) std::lround (4.0f + 20.0f * clampf (v, 0.0f, 1.0f)));
                return buf;
            }
            if (v < 0.005f) return "OFF";
            std::snprintf (buf, sizeof buf, "%s %d%%", v < 0.5f ? "WITH ECHOES" : "ALWAYS",
                           (int) std::lround ((v < 0.5f ? v * 2.0f : (v - 0.5f) * 2.0f) * 100.0f));
            return buf;
        case TTrem:
            if (slot == 1) return lfoRatios()[(size_t) ratioIndex (v, (int) lfoRatios().size())].name;
            return pct();
        case TFilter:
            if (slot == 2) return lfoRatios()[(size_t) ratioIndex (v, (int) lfoRatios().size())].name;
            return pct();
        case TSwell:
            if (slot == 0) return ms (expMap (v, 30.0f, 4000.0f));
            return pct();
        case TIce:
            if (slot == 0) return ms (expMap (v, 15.0f, 400.0f));
            return pct();
        case TTape:
        case TBucket:
            if (slot == 1 && v < 0.005f) return "OFF";
            return pct();
        default:
            return pct();
    }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------
namespace
{
std::string lower (const std::string& s)
{
    std::string o (s);
    for (auto& c : o) c = (char) std::tolower ((unsigned char) c);
    return o;
}

// Words, ignoring punctuation (so "lofi" finds "Lo-Fi").
std::vector<std::string> words (const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : lower (s))
    {
        if (std::isalnum ((unsigned char) c)) cur += c;
        else if (c == '-' || c == '\'') continue;
        else if (! cur.empty()) { out.push_back (cur); cur.clear(); }
    }
    if (! cur.empty()) out.push_back (cur);
    return out;
}

bool startsWith (const std::string& w, const std::string& p) { return w.compare (0, p.size(), p) == 0; }
} // namespace

int matchScore (const std::string& query, const std::string& name, const std::string& category,
                const std::string& blurb, const std::string& keywords)
{
    const auto q = words (query);
    if (q.empty())
        return 1;
    const auto n = words (name), c = words (category), k = words (keywords), b = words (blurb);
    std::string joined;
    for (auto& w : n) joined += w;
    int total = 0;
    for (const auto& term : q)
    {
        int best = 0;
        if (startsWith (joined, term)) best = 100;
        for (auto& w : n) best = std::max (best, w == term ? 90 : startsWith (w, term) ? 70 : 0);
        for (auto& w : c) best = std::max (best, startsWith (w, term) ? 50 : 0);
        for (auto& w : k) best = std::max (best, w == term ? 45 : startsWith (w, term) ? 35 : 0);
        for (auto& w : b) best = std::max (best, startsWith (w, term) && term.size() >= 3 ? 20 : 0);
        if (best == 0)
            return 0;
        total += best;
    }
    return total;
}
} // namespace lathe::dly
