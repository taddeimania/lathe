#include "Types.h"
#include "Dsp.h"

#include <cctype>
#include <cstdio>

namespace lathe::rvb
{
namespace
{
Voicing space (float minMs, float maxMs, int er, float erMs, float erGain, int diff, float diffMs,
               float dark, float lowBias, float mod, float dMin, float dMax, unsigned features = 0)
{
    Voicing v;
    v.machine = Machine::Space;
    v.lineMinMs = minMs; v.lineMaxMs = maxMs;
    v.erPattern = er; v.erMs = erMs; v.erGain = erGain;
    v.diffusers = diff; v.diffuserMs = diffMs;
    v.darkness = dark; v.lowBias = lowBias; v.modScale = mod;
    v.decayMin = dMin; v.decayMax = dMax;
    v.features = features;
    return v;
}

Voicing machine (Machine m, float dMin, float dMax, float dark = 1.0f, float lowBias = 0.0f, unsigned features = 0)
{
    Voicing v;
    v.machine = m;
    v.decayMin = dMin; v.decayMax = dMax;
    v.darkness = dark; v.lowBias = lowBias;
    v.features = features;
    v.erPattern = 0;
    return v;
}

Starting start (float decay, float size, float diffusion, float early, float a = 0, float b = 0, float c = 0, int mode = 0)
{
    return { decay, size, diffusion, early, a, b, c, mode };
}

const char* kBuildTip = "BUILD: how slowly the tail thickens after the early reflections. Up for a softer, later bloom.";
const char* kDistanceTip = "DISTANCE: moves the sound further from you. The early reflections bunch up and the tail takes over.";

TypeInfo room (const char* name, const char* blurb, const char* kw, Voicing v, Starting s, const char* category = "ROOMS")
{
    TypeInfo t { name, category, blurb, kw, v, s, { "BUILD", "DISTANCE", "" }, "", {} };
    t.charTips[0] = kBuildTip;
    t.charTips[1] = kDistanceTip;
    return t;
}

std::vector<TypeInfo> makeTypes()
{
    std::vector<TypeInfo> t;
    t.reserve (NumTypes);

    // --- rooms ---------------------------------------------------------------------
    t.push_back (room ("Ambience", "Just the air around a sound: early reflections and a whisper of tail.",
                       "short small booth air tight early vocal dialogue dry natural subtle film podcast voiceover close intimate",
                       space (5.0f, 22.0f, 1, 24.0f, 1.0f, 3, 5.0f, 1.0f, -1.0f, 0.6f, 0.1f, 3.0f),
                       start (0.45f, 0.4f, 0.7f, 0.8f, 0.2f, 0.2f)));
    t.push_back (room ("Studio Room", "A well-tuned studio room: natural, controlled and flattering on anything.",
                       "room studio natural small medium tracking acoustic drums guitar piano vocal warm short",
                       space (11.0f, 46.0f, 2, 42.0f, 0.8f, 4, 8.0f, 1.0f, 0.0f, 0.7f, 0.2f, 20.0f),
                       start (0.9f, 0.5f, 0.65f, 0.6f, 0.25f, 0.25f)));
    t.push_back (room ("Live Room", "A bigger, livelier tracking room with hard walls and a bright splash.",
                       "room live big bright wood hard reflective lively drums guitar band short",
                       space (16.0f, 70.0f, 2, 62.0f, 0.9f, 4, 10.0f, 0.75f, 0.0f, 0.7f, 0.2f, 20.0f),
                       start (1.4f, 0.55f, 0.6f, 0.6f, 0.3f, 0.3f)));
    {
        auto d = room ("Drum Room", "Bright, punchy and short. SMASH crushes it like a room mic into a compressor.",
                       "drums room compressed crushed smash punchy kit snare kick bright short aggressive rock",
                       space (13.0f, 54.0f, 2, 50.0f, 1.0f, 3, 7.0f, 0.7f, 1.0f, 0.5f, 0.2f, 10.0f, FeatSmash),
                       start (0.8f, 0.45f, 0.55f, 0.75f, 0.15f, 0.2f, 0.5f));
        d.charLabel[2] = "SMASH";
        d.charTips[2] = "SMASH: squashes the room with a fast, hard compressor. Turn it up and the room explodes after every hit.";
        t.push_back (d);
    }
    t.push_back (room ("Club", "A big, lively nightclub. The bass hangs around.",
                       "club venue bar nightclub stage big lively live gig band bass warm",
                       space (24.0f, 96.0f, 3, 80.0f, 0.7f, 5, 12.0f, 0.95f, 2.0f, 0.8f, 0.3f, 20.0f),
                       start (1.8f, 0.55f, 0.7f, 0.5f, 0.35f, 0.35f)));
    t.push_back (room ("Chamber", "An echo chamber: a dense, smooth concrete room built for reverb. Classic on vocals and strings.",
                       "chamber echo chamber concrete vocal strings classic vintage smooth dense warm 60s 50s motown",
                       space (19.0f, 78.0f, 2, 34.0f, 0.5f, 6, 11.0f, 0.9f, 0.0f, 0.8f, 0.3f, 20.0f),
                       start (1.6f, 0.5f, 0.8f, 0.35f, 0.3f, 0.25f)));
    {
        TypeInfo r { "Reflections", "ROOMS",
                     "Move the source around a small room and hear hundreds of calculated reflections. Realism for dry tracks.",
                     "reflections early position move source room small realistic dry psychoacoustic x y place stereo",
                     machine (Machine::Reflections, 0.15f, 4.0f), start (0.6f, 0.5f, 0.6f, 0.9f, 0.5f, 0.35f, 0.0f, 1),
                     { "X", "Y", "WANDER" }, "SHAPE", { "SQUARE", "RECTANGLE", "OBLONG" } };
        r.sizeCaption = "ROOM SIZE";
        r.decayCaption = "TAIL";
        r.earlyCaption = "REFLECT";
        r.charTips[0] = "X: where the sound stands, left to right. The reflections are recalculated and the dry sound pans with it.";
        r.charTips[1] = "Y: front to back. At the front the dry sound dominates; at the back the reflections arrive with it.";
        r.charTips[2] = "WANDER: lets the source drift a little, for a gentle chorus against the dry sound.";
        r.modeTip = "SHAPE: SQUARE, RECTANGLE (wide and short) or OBLONG (long and narrow).";
        t.push_back (r);
    }

    // --- halls ---------------------------------------------------------------------
    t.push_back (room ("Small Hall", "A recital hall: warm, even and quick to bloom.",
                       "hall small recital warm medium classical piano strings acoustic natural",
                       space (28.0f, 110.0f, 3, 70.0f, 0.6f, 5, 13.0f, 1.0f, 0.0f, 1.0f, 0.4f, 20.0f),
                       start (1.8f, 0.5f, 0.75f, 0.5f, 0.35f, 0.3f), "HALLS"));
    t.push_back (room ("Concert Hall", "Spacious and warm with slow-building density. The all-rounder.",
                       "hall concert big orchestra warm classical lush allround vocals strings piano pad natural",
                       space (38.0f, 150.0f, 3, 92.0f, 0.55f, 6, 16.0f, 1.0f, 0.5f, 1.0f, 0.5f, 30.0f),
                       start (3.0f, 0.55f, 0.8f, 0.45f, 0.45f, 0.35f), "HALLS"));
    t.push_back (room ("Arena", "The largest enclosed venues: huge, enveloping and booming.",
                       "arena stadium huge big boom enormous rock long dark epic drums",
                       space (60.0f, 240.0f, 4, 160.0f, 0.45f, 6, 22.0f, 1.1f, 2.5f, 1.0f, 1.0f, 30.0f),
                       start (6.0f, 0.6f, 0.8f, 0.4f, 0.5f, 0.45f), "HALLS"));
    {
        auto c = room ("Cathedral", "Stone, height and a very long tail, with an ECHO from the far wall.",
                       "cathedral church stone huge long choir organ sacred gothic dark epic cinematic",
                       space (70.0f, 280.0f, 4, 190.0f, 0.5f, 7, 24.0f, 0.85f, 1.0f, 0.9f, 1.0f, 40.0f, FeatEcho),
                       start (8.0f, 0.6f, 0.85f, 0.45f, 0.55f, 0.4f, 0.3f), "HALLS");
        c.charLabel[2] = "ECHO";
        c.charTips[2] = "ECHO: a distinct slap off the far wall, a beat behind the sound.";
        t.push_back (c);
    }

    // --- plates --------------------------------------------------------------------
    auto plate = [] (const char* name, const char* blurb, const char* kw, float dark, float lowBias, Starting s)
    {
        TypeInfo p { name, "PLATES", blurb, kw, machine (Machine::Plate, 0.3f, 20.0f, dark, lowBias), s,
                     { "DRIVE", "", "" }, "PLATE", { "LARGE", "SMALL" } };
        p.charTips[0] = "DRIVE: pushes the plate's driver amplifier for some warmth and grit on loud notes.";
        p.modeTip = "LARGE: a studio plate, lush and smooth. SMALL: a project-sized plate, splashier and ringier.";
        p.diffusionCaption = "DENSITY";
        return p;
    };
    t.push_back (plate ("Plate", "A large studio plate: fast, dense and smooth, with no room cues.",
                        "plate studio vintage smooth dense vocals vocal snare drums classic bright metal", 1.0f, 0.0f,
                        start (2.2f, 0.5f, 0.75f, 0.0f, 0.0f)));
    t.push_back (plate ("Vocal Plate", "A brighter plate voiced for vocals: airy top and a light bottom.",
                        "plate vocal vocals bright airy singer voice pop shiny modern", 0.7f, -4.0f,
                        start (1.8f, 0.5f, 0.75f, 0.0f, 0.0f)));
    t.push_back (plate ("Vintage Plate", "A darker, older plate with some tube drive in front of it.",
                        "plate vintage old dark tube warm drive saturated 60s 70s retro", 1.4f, 1.0f,
                        start (2.6f, 0.55f, 0.7f, 0.0f, 0.45f)));

    // --- springs -------------------------------------------------------------------
    auto spring = [] (const char* name, const char* blurb, const char* kw, Starting s)
    {
        TypeInfo p { name, "SPRINGS", blurb, kw, machine (Machine::Spring, 0.6f, 10.0f), s,
                     { "DWELL", "DRIP", "" }, "SPRINGS", { "1 SPRING", "2 SPRINGS", "3 SPRINGS" } };
        p.charTips[0] = "DWELL: how hard the tank is driven. Low is clean; high is trashy and splashy.";
        p.charTips[1] = "DRIP: the boing. How much the springs chirp and drip on attacks.";
        p.modeTip = "SPRINGS: more springs means a denser, more complex tank.";
        p.sizeCaption = "LENGTH";
        p.diffusionCaption = "DENSITY";
        return p;
    };
    t.push_back (spring ("Spring", "A spring tank from a combo amp. Pick one to three springs and drive them.",
                         "spring amp guitar combo tank twang vintage retro dub reggae boing", start (2.4f, 0.5f, 0.5f, 0.0f, 0.3f, 0.45f, 0.0f, 1)));
    t.push_back (spring ("Surf Spring", "Three springs driven hard: the dripping, splashy sound of surf guitar.",
                         "spring surf drip splash wet guitar twang 60s retro dub boing trashy", start (3.2f, 0.55f, 0.45f, 0.0f, 0.75f, 0.85f, 0.0f, 2)));

    // --- ambient -------------------------------------------------------------------
    {
        TypeInfo c { "Cloud", "AMBIENT", "A vast, soft cloud of reverb that blurs everything into an ensemble.",
                     "cloud ambient huge pad wash blur soft lush infinite drone big long dreamy synth",
                     space (48.0f, 210.0f, 0, 0.0f, 0.0f, 8, 70.0f, 1.0f, 0.0f, 1.3f, 1.0f, 60.0f, FeatCloud),
                     start (9.0f, 0.6f, 0.8f, 0.0f, 0.45f, 0.5f), { "HAZE", "SMEAR", "" }, "", {} };
        c.charTips[0] = "HAZE: how much the cloud's diffusers drift. Higher is a moving, chorused haze.";
        c.charTips[1] = "SMEAR: how long the cloud takes to form. Higher stretches attacks into a long blur.";
        t.push_back (c);
    }
    {
        TypeInfo b { "Bloom", "AMBIENT", "The reverb swells after the note, from a feedback bloom stage feeding a big tank.",
                     "bloom swell ambient big lush pad build grow 90s long dreamy synth",
                     space (42.0f, 180.0f, 0, 0.0f, 0.0f, 4, 18.0f, 1.0f, 0.0f, 1.2f, 0.5f, 30.0f, FeatBloom),
                     start (6.0f, 0.55f, 0.75f, 0.0f, 0.5f, 0.35f), { "LENGTH", "FEEDBACK", "" }, "", {} };
        b.charTips[0] = "LENGTH: how long the bloom takes to open up before the tank takes over.";
        b.charTips[1] = "FEEDBACK: feeds the bloom back into itself for longer, resonant swells.";
        t.push_back (b);
    }
    auto shimmer = [] (const char* name, const char* blurb, const char* kw, Voicing v, Starting s)
    {
        TypeInfo p { name, "AMBIENT", blurb, kw, v, s, { "AMOUNT", "SHIFT 1", "SHIFT 2" }, "FEED", { "INPUT", "TANK", "BOTH" } };
        p.charTips[0] = "AMOUNT: how loud the shifted voices are in the reverb.";
        p.charTips[1] = "SHIFT 1: the first voice's interval, from an octave down to two octaves up.";
        p.charTips[2] = "SHIFT 2: a second voice, or OFF.";
        p.modeTip = "INPUT shifts what goes in, once. TANK shifts the tail over and over, so it climbs. BOTH does both.";
        return p;
    };
    t.push_back (shimmer ("Shimmer", "Pitch-shifted voices inside the reverb: octaves that rise and ring.",
                          "shimmer octave pitch ambient heavenly angelic rising bright pad dreamy long synth guitar",
                          space (40.0f, 170.0f, 0, 0.0f, 0.0f, 5, 16.0f, 0.9f, -1.0f, 1.1f, 0.5f, 30.0f, FeatShimmer),
                          start (7.0f, 0.55f, 0.8f, 0.0f, 0.5f, 0.78f, 0.0f, 1)));
    t.push_back (shimmer ("Halo", "A soft cloud with an octave and a twelfth shimmering above it.",
                          "halo shimmer cloud octave fifth heavenly pad ambient soft bright long dreamy",
                          space (48.0f, 200.0f, 0, 0.0f, 0.0f, 8, 55.0f, 0.9f, -1.0f, 1.3f, 1.0f, 40.0f, FeatShimmer | FeatCloud),
                          start (10.0f, 0.6f, 0.85f, 0.0f, 0.38f, 0.78f, 0.88f, 2)));
    t.push_back (shimmer ("Abyss", "Huge, dark and an octave down. A tail that rumbles on and on.",
                          "abyss dark low octave down sub deep drone cinematic huge long epic",
                          space (60.0f, 240.0f, 0, 0.0f, 0.0f, 6, 30.0f, 1.6f, 3.0f, 1.0f, 1.0f, 60.0f, FeatShimmer),
                          start (12.0f, 0.65f, 0.8f, 0.0f, 0.45f, 0.08f, 0.0f, 1)));
    {
        TypeInfo c { "Choir", "AMBIENT", "The reverb sings back in vowels, like a choir behind your sound.",
                     "choir vowel voices chorale formant singing vocal ah oh oo ethereal pad ambient spooky",
                     space (34.0f, 140.0f, 0, 0.0f, 0.0f, 5, 14.0f, 0.9f, 0.0f, 1.0f, 0.5f, 20.0f, FeatChoir),
                     start (4.5f, 0.5f, 0.8f, 0.0f, 0.5f, 0.5f, 0.0f, 0),
                     { "RESONANCE", "VOICES", "" }, "VOWEL", { "AH", "OH", "OO", "EE", "AH-OH", "OH-OO", "RANDOM" } };
        c.charTips[0] = "RESONANCE: how strongly the vowels ring. Low is a hint; high is a spooky choir.";
        c.charTips[1] = "VOICES: how many singers, and how different they sound from each other.";
        c.modeTip = "VOWEL: the vowel the choir sings. AH-OH and OH-OO move between two; RANDOM keeps changing.";
        t.push_back (c);
    }
    {
        TypeInfo s { "Swell", "AMBIENT", "The reverb (or the dry sound) fades in behind each note, like a volume pedal.",
                     "swell volume pedal fade in slow attack ambient pad violin bowed guitar soft",
                     space (36.0f, 150.0f, 0, 0.0f, 0.0f, 5, 14.0f, 1.0f, 0.0f, 1.0f, 0.5f, 30.0f, FeatSwell),
                     start (4.0f, 0.55f, 0.8f, 0.0f, 0.45f, 0.5f, 0.0f, 0),
                     { "RISE", "SENSE", "" }, "SWELL", { "WET", "DRY" } };
        s.charTips[0] = "RISE: how long each swell takes. Short for solos, long for chords.";
        s.charTips[1] = "SENSE: how easily a new note restarts the swell. Up for softer playing.";
        s.modeTip = "WET swells the reverb in behind the dry sound. DRY swells the dry sound itself into the reverb.";
        t.push_back (s);
    }

    // --- special -------------------------------------------------------------------
    auto nonlinear = [] (const char* name, const char* blurb, const char* kw, Starting s)
    {
        TypeInfo n { name, "SPECIAL", blurb, kw, machine (Machine::Nonlinear, 0.3f, 20.0f), s,
                     { "TIME", "FEEDBACK", "LATE" }, "SHAPE", { "SWOOSH", "REVERSE", "RAMP", "GATE", "GAUSS", "BOUNCE" } };
        n.decayCaption = "LATE DECAY";
        n.charTips[0] = "TIME: how long the shaped part lasts.";
        n.charTips[1] = "FEEDBACK: repeats the shape, for multi-tapped washes. Max with GATE is nearly endless.";
        n.charTips[2] = "LATE: a normal reverb tail behind the shape. All the way down, it's off.";
        n.modeTip = "SHAPE: SWOOSH, REVERSE and RAMP rise backwards; GATE is flat then cut; GAUSS is a bell; BOUNCE is an upside-down bell.";
        return n;
    };
    t.push_back (nonlinear ("Nonlinear", "Physics-defying decay shapes: swooshes, ramps, bells and gates, with a tail behind.",
                            "nonlinear shape weird special effect swoosh ramp gauss bounce drums experimental",
                            start (2.0f, 0.5f, 0.6f, 0.0f, 0.45f, 0.0f, 0.2f, 0)));
    t.push_back (nonlinear ("Gated", "The big gated drum sound: dense and even, then cut dead.",
                            "gated gate 80s drums snare cut dense nonlinear short punchy retro",
                            start (1.0f, 0.5f, 0.75f, 0.0f, 0.34f, 0.0f, 0.0f, 3)));
    t.push_back (nonlinear ("Reverse", "Reverb that rises into the note and stops. Backwards, without the tape.",
                            "reverse backwards rise inverse swell nonlinear weird experimental",
                            start (2.0f, 0.5f, 0.55f, 0.0f, 0.5f, 0.0f, 0.1f, 1)));
    {
        TypeInfo e { "Tape Echo", "SPECIAL", "A multi-head tape echo smeared into a room, with wow and flutter.",
                     "tape echo delay multi head vintage wow flutter 50s rockabilly slapback dub retro warm",
                     machine (Machine::Tape, 0.2f, 10.0f), start (1.2f, 0.5f, 0.35f, 0.35f, 0.42f, 0.35f, 0.3f, 1),
                     { "TIME", "FEEDBACK", "WOW" }, "HEADS", { "1 HEAD", "3 EVEN", "4 EVEN", "6 EVEN", "3 SCATTER", "4 SCATTER", "6 SCATTER" } };
        e.decayCaption = "ROOM DECAY";
        e.earlyCaption = "ROOM";
        e.diffusionCaption = "SMEAR";
        e.sizeCaption = "ROOM SIZE";
        e.charTips[0] = "TIME: the delay of the last head. The others fall evenly (or scattered) before it.";
        e.charTips[1] = "FEEDBACK: from the last head back to the tape. Past 90% it starts to run away, gently.";
        e.charTips[2] = "WOW: tape speed wobble, slow and fast.";
        e.modeTip = "HEADS: one head for a slapback, or three, four or six. EVEN is rhythmic; SCATTER is less regular.";
        t.push_back (e);
    }
    {
        TypeInfo l { "Lo-Fi", "SPECIAL", "A worn, crushed, wobbly reverb, like a cheap old box or a cassette.",
                     "lofi lo-fi crush bitcrush cassette tape worn cheap broken vintage digital 8bit wobble dark dirty retro",
                     space (22.0f, 100.0f, 2, 40.0f, 0.4f, 4, 12.0f, 1.2f, -2.0f, 1.0f, 0.3f, 20.0f, FeatLofi),
                     start (2.4f, 0.5f, 0.7f, 0.3f, 0.3f, 0.4f, 0.3f), { "CRUSH", "WOBBLE", "GRIT" }, "", {} };
        l.charTips[0] = "CRUSH: fewer bits and a lower sample rate.";
        l.charTips[1] = "WOBBLE: warped, wavering pitch, like a stretched tape.";
        l.charTips[2] = "GRIT: drive and hiss in the tank.";
        t.push_back (l);
    }
    {
        TypeInfo r { "Resonant", "SPECIAL", "A reverb tuned to a note or chord: the tail rings in key.",
                     "resonant tuned key chord note pitched harmonic ring metallic sympathetic strings drone pad ambient",
                     space (30.0f, 120.0f, 0, 0.0f, 0.0f, 5, 12.0f, 1.0f, 0.0f, 0.6f, 0.5f, 30.0f, FeatResonant),
                     start (5.0f, 0.5f, 0.75f, 0.0f, 0.6f, 0.4f, 0.0f, 9), { "RING", "CHORD", "" }, "NOTE",
                     { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } };
        r.charTips[0] = "RING: how much the tuned strings ring against the normal tail.";
        r.charTips[1] = "CHORD: which notes ring above the NOTE: just it, octaves, fifths, or a chord.";
        r.modeTip = "NOTE: the key the tail rings in.";
        t.push_back (r);
    }
    return t;
}
} // namespace

const std::vector<TypeInfo>& types()
{
    static const std::vector<TypeInfo> list = makeTypes();
    return list;
}

const TypeInfo& typeInfo (int index)
{
    const auto& t = types();
    return t[(size_t) std::max (0, std::min ((int) t.size() - 1, index))];
}

const std::vector<const char*>& categories()
{
    static const std::vector<const char*> c { "ROOMS", "HALLS", "PLATES", "SPRINGS", "AMBIENT", "SPECIAL" };
    return c;
}

float decayFor (int type, float s)
{
    const auto& v = typeInfo (type).voicing;
    return clampf (s, v.decayMin, v.decayMax);
}

// ---------------------------------------------------------------------------
// Shimmer intervals: the knob steps through these.
// ---------------------------------------------------------------------------
namespace
{
struct Interval { int cents; const char* name; };
const Interval kIntervals[] {
    { -1200, "-1 OCT" }, { -700, "-5TH" }, { -500, "-4TH" }, { -10, "-10 CENTS" }, { 10, "+10 CENTS" },
    { 500, "+4TH" }, { 700, "+5TH" }, { 1200, "+1 OCT" }, { 1900, "+OCT & 5TH" }, { 2400, "+2 OCT" } };
constexpr int kNumIntervals = (int) (sizeof (kIntervals) / sizeof (kIntervals[0]));

int intervalIndex (float knob, bool allowOff)
{
    const int n = kNumIntervals + (allowOff ? 1 : 0);
    const int i = std::min (n - 1, (int) (clampf (knob, 0.0f, 1.0f) * (float) n));
    return allowOff ? i - 1 : i;   // -1 = off
}
} // namespace

int shiftSemis100 (float knob, bool allowOff)
{
    const int i = intervalIndex (knob, allowOff);
    return i < 0 ? -100000 : kIntervals[i].cents;
}

const char* shiftName (float knob, bool allowOff)
{
    const int i = intervalIndex (knob, allowOff);
    return i < 0 ? "OFF" : kIntervals[i].name;
}

// ---------------------------------------------------------------------------
// Resonant chords
// ---------------------------------------------------------------------------
namespace
{
const char* kChordNames[] { "NOTE", "OCTAVES", "FIFTHS", "MAJOR", "MINOR", "SUS4", "MAJ 7", "MIN 7" };
const std::vector<int> kChords[] {
    { 0 }, { 0, 12 }, { 0, 7, 12 }, { 0, 4, 7, 12 }, { 0, 3, 7, 12 }, { 0, 5, 7, 12 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 } };
constexpr int kNumChords = 8;
} // namespace

int chordIndex (float knob) { return std::min (kNumChords - 1, (int) (clampf (knob, 0.0f, 1.0f) * kNumChords)); }
const char* chordName (int i) { return kChordNames[std::max (0, std::min (kNumChords - 1, i))]; }
const std::vector<int>& chordSemis (int i) { return kChords[std::max (0, std::min (kNumChords - 1, i))]; }
const char* noteName (int pc)
{
    static const char* n[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n[((pc % 12) + 12) % 12];
}

// ---------------------------------------------------------------------------
// Knob text
// ---------------------------------------------------------------------------
std::string charText (int type, int slot, float v)
{
    char buf[48];
    auto pct = [&] { std::snprintf (buf, sizeof buf, "%d%%", (int) std::lround (v * 100.0f)); return std::string (buf); };
    auto ms = [&] (float x)
    {
        if (x >= 1000.0f) std::snprintf (buf, sizeof buf, "%.2f s", x / 1000.0f);
        else std::snprintf (buf, sizeof buf, "%d ms", (int) std::lround (x));
        return std::string (buf);
    };
    switch (type)
    {
        case TReflections:
            if (slot == 0)
            {
                const int p = (int) std::lround ((v - 0.5f) * 200.0f);
                if (p == 0) return "CENTRE";
                std::snprintf (buf, sizeof buf, "%d%% %s", std::abs (p), p < 0 ? "L" : "R");
                return buf;
            }
            if (slot == 1)
            {
                std::snprintf (buf, sizeof buf, "%d%% BACK", (int) std::lround (v * 100.0f));
                return v < 0.005f ? std::string ("FRONT") : std::string (buf);
            }
            return pct();
        case TShimmer: case THalo: case TAbyss:
            if (slot == 1) return shiftName (v, false);
            if (slot == 2) return shiftName (v, true);
            return pct();
        case TSwell:
            if (slot == 0) return ms (expMap (v, 20.0f, 4000.0f));
            return pct();
        case TNonlinear: case TGated: case TReverse:
            if (slot == 0) return ms (expMap (v, 40.0f, 2000.0f));
            if (slot == 2 && v < 0.005f) return "OFF";
            return pct();
        case TTape:
            if (slot == 0) return ms (expMap (v, 60.0f, 1500.0f));
            if (slot == 1) { std::snprintf (buf, sizeof buf, "%d%%", (int) std::lround (v * 110.0f)); return buf; }
            return pct();
        case TResonant:
            if (slot == 1) return chordName (chordIndex (v));
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
} // namespace lathe::rvb
