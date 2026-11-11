// Lathe's built-in effects. Reverb and Delay each run a full engine (Reverb/, Delay/) with a
// small set of its types and controls on the panel; Mastering runs Master/; Compressor and
// Saturator are small and self-contained.
#include "Device.h"
#include "Instruments.h"
#include "Mutable.h"
#include "Reverb/Engine.h"
#include "Delay/Engine.h"
#include "Master/Master.h"

#include <juce_dsp/juce_dsp.h>

namespace lathe::devices
{
namespace
{
/** The playhead's tempo, or 120. */
double tempoOf (AudioProcessor& p)
{
    if (auto* ph = p.getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                return jlimit (20.0, 400.0, *bpm);
    return 120.0;
}

/** Runs a stereo engine over a buffer that may be mono. */
template <typename Fn>
void stereo (AudioBuffer<float>& buffer, int numIn, std::vector<float>& scratch, Fn&& process)
{
    const int n = buffer.getNumSamples();
    if (n == 0 || buffer.getNumChannels() == 0)
        return;
    float* l = buffer.getWritePointer (0);
    if (buffer.getNumChannels() >= 2)
    {
        if (numIn < 2)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);   // mono in: both sides start the same
        process (l, buffer.getWritePointer (1), n);
        return;
    }
    if (scratch.size() < (size_t) n)
        scratch.resize ((size_t) n);   // only if the host lied about the block size
    std::copy (l, l + n, scratch.data());
    process (l, scratch.data(), n);
    for (int i = 0; i < n; ++i)
        l[i] = 0.5f * (l[i] + scratch[(size_t) i]);
}

//==============================================================================
// Reverb: the reverb engine (Reverb/) with six of its types.
class Reverb final : public Device
{
public:
    static constexpr int kTypes[] { rvb::TStudio, rvb::TLiveRoom, rvb::TChamber, rvb::TConcertHall, rvb::TPlate, rvb::TSpring };

    Reverb() : Device ("reverb", "Reverb", Kind::effect, layout())
    {
        setCurrentProgram (0);   // the panel and the sound start on the first preset
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("type", "Type", { "Small Room", "Room", "Chamber", "Hall", "Plate", "Spring" }, 3));
        l.add (number ("decay", "Decay", { 0.1f, 20.0f, 0.0f, 0.4f }, 3.0f, [] (float v) { return secondsText (v); }));
        l.add (percent ("size", "Size", 0.55f));
        l.add (number ("predelay", "Pre-delay", { 0.0f, 250.0f, 0.0f, 0.5f }, 20.0f, [] (float v) { return msText (v); }));
        l.add (percent ("tone", "Tone", 0.5f));
        l.add (number ("lowEnd", "Low End", { -10.0f, 10.0f }, 0.0f, [] (float v)
        {
            return std::abs (v) < 0.05f ? String ("Even") : (v > 0 ? "+" : "") + String (v, 1);
        }));
        l.add (percent ("mix", "Mix", 0.3f));
        return l;
    }

    String blurb() const override   { return "Rooms, a chamber, a hall, a plate and a spring."; }
    String modeParam() const override   { return "type"; }

    void modeChosen (int index) override
    {
        // Each type starts where it sounds like itself.
        const auto& st = rvb::typeInfo (kTypes[jlimit (0, 5, index)]).start;
        set ("decay", st.decay);
        set ("size", st.size);
    }

    std::vector<Section> sections() const override
    {
        return {
            { "Space", { { "decay", "Decay", "How long the tail rings: seconds for it to die away." },
                         { "size", "Size", "How big the space is. Bigger spaces take longer to fill." },
                         { "predelay", "Pre-delay", "A gap before the reverb starts, which keeps the dry sound clear." } } },
            { "Colour", { { "tone", "Tone", "Dark to bright: how fast the highs die away in the tail." },
                          { "lowEnd", "Low End", "Thinner or fuller: how long the lows ring compared with the mids." } } },
            { "Output", { { "mix", "Mix", "Dry to wet. 50% is both at full level; use 100% on a return." } } } };
    }

    std::vector<Preset> presets() const override
    {
        return {
            { "Hall",           { { "type", 3 }, { "decay", 3.0f }, { "size", 0.55f },{ "predelay", 20 }, { "tone", 0.5f },  { "lowEnd", 0 },  { "mix", 0.3f } } },
            { "Room",           { { "type", 1 }, { "decay", 1.2f }, { "size", 0.5f }, { "predelay", 5 },  { "tone", 0.55f }, { "lowEnd", 0 },  { "mix", 0.25f } } },
            { "Small room",     { { "type", 0 }, { "decay", 0.7f }, { "size", 0.4f }, { "predelay", 0 },  { "tone", 0.5f },  { "lowEnd", -1 }, { "mix", 0.2f } } },
            { "Drum room",      { { "type", 1 }, { "decay", 0.9f }, { "size", 0.6f }, { "predelay", 0 },  { "tone", 0.65f }, { "lowEnd", -2 }, { "mix", 0.3f } } },
            { "Vocal chamber",  { { "type", 2 }, { "decay", 1.6f }, { "size", 0.5f }, { "predelay", 25 }, { "tone", 0.55f }, { "lowEnd", -3 }, { "mix", 0.22f } } },
            { "Long hall",      { { "type", 3 }, { "decay", 7.0f }, { "size", 0.8f }, { "predelay", 40 }, { "tone", 0.42f }, { "lowEnd", 2 },  { "mix", 0.35f } } },
            { "Bright plate",   { { "type", 4 }, { "decay", 1.8f }, { "size", 0.5f }, { "predelay", 15 }, { "tone", 0.7f },  { "lowEnd", -3 }, { "mix", 0.25f } } },
            { "Dark plate",     { { "type", 4 }, { "decay", 2.6f }, { "size", 0.55f },{ "predelay", 10 }, { "tone", 0.3f },  { "lowEnd", 1 },  { "mix", 0.25f } } },
            { "Spring",         { { "type", 5 }, { "decay", 2.4f }, { "size", 0.5f }, { "predelay", 0 },  { "tone", 0.5f },  { "lowEnd", 0 },  { "mix", 0.3f } } },
            { "Return: hall",   { { "type", 3 }, { "decay", 3.2f }, { "size", 0.6f }, { "predelay", 25 }, { "tone", 0.5f },  { "lowEnd", 0 },  { "mix", 1.0f } } },
            { "Return: plate",  { { "type", 4 }, { "decay", 2.0f }, { "size", 0.5f }, { "predelay", 15 }, { "tone", 0.6f },  { "lowEnd", -2 }, { "mix", 1.0f } } } };
    }

    void prepareToPlay (double sampleRate, int block) override
    {
        engine.prepare (sampleRate, block);
        scratch.assign ((size_t) jmax (block, 512), 0.0f);
    }

    void reset() override   { engine.reset(); }
    double getTailLengthSeconds() const override   { return jmin (30.0, (double) get ("decay") * 1.5 + 0.3); }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        midi.clear();
        const int type = kTypes[jlimit (0, 5, geti ("type"))];
        const auto& st = rvb::typeInfo (type).start;
        rvb::EngineSettings s;
        s.type = type;
        s.decay = get ("decay");
        s.size = get ("size");
        s.predelayMs = get ("predelay");
        s.diffusion = st.diffusion;
        s.early = st.early;
        s.a = st.a;
        s.b = st.b;
        s.c = st.c;
        s.mode = st.mode;
        s.tone = get ("tone");
        s.lowEnd = get ("lowEnd");
        s.mix = get ("mix");
        stereo (buffer, getTotalNumInputChannels(), scratch, [&] (float* l, float* r, int n)
        {
            engine.process (l, r, l, r, n, s);
        });
    }

private:
    rvb::Engine engine;
    std::vector<float> scratch;
};

//==============================================================================
// Delay: the delay engine (Delay/) with four of its types.
class Delay final : public Device
{
public:
    static constexpr int kTypes[] { dly::TDigital, dly::TTape, dly::TBucket, dly::TLofi };
    struct Division { const char* name; double beats; };
    static const std::vector<Division>& divisions()
    {
        static const std::vector<Division> d {
            { "1/32", 0.125 }, { "1/16T", 1.0 / 6 }, { "1/16", 0.25 }, { "1/8T", 1.0 / 3 }, { "1/16D", 0.375 },
            { "1/8", 0.5 }, { "1/4T", 2.0 / 3 }, { "1/8D", 0.75 }, { "1/4", 1.0 }, { "1/2T", 4.0 / 3 },
            { "1/4D", 1.5 }, { "1/2", 2.0 }, { "1/2D", 3.0 }, { "1 bar", 4.0 } };
        return d;
    }

    Delay() : Device ("delay", "Delay", Kind::effect, layout())
    {
        setCurrentProgram (0);   // the panel and the sound start on the first preset
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        StringArray notes;
        for (auto& d : divisions())
            notes.add (d.name);
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("type", "Type", { "Digital", "Tape", "Analog", "Lo-fi" }, 0));
        l.add (number ("time", "Time", { 10.0f, 2500.0f, 0.0f, 0.4f }, 400.0f, [] (float v) { return msText (v); }));
        l.add (toggle ("sync", "Sync", true));
        l.add (choice ("note", "Note", notes, 7));   // 1/8D
        l.add (number ("repeats", "Repeats", { 0.0f, 1.05f }, 0.4f, [] (float v)
        {
            return v > 1.0f ? String ("Runaway") : String (roundToInt (v * 100.0f)) + "%";
        }));
        l.add (percent ("tone", "Tone", 0.0f));
        l.add (percent ("grit", "Grit", 0.0f));
        l.add (percent ("wobble", "Wobble", 0.05f));
        l.add (choice ("stereo", "Stereo", { "Stereo", "Ping-pong", "Mono" }, 0));
        l.add (percent ("mix", "Mix", 0.35f));
        return l;
    }

    String blurb() const override   { return "Clean, tape, analog and lo-fi echoes, in time with the song."; }
    bool isControlActive (const String& param) const override
    {
        if (param == "time") return geti ("sync") == 0;
        if (param == "note") return geti ("sync") != 0;
        return true;
    }
    String modeParam() const override   { return "type"; }

    void modeChosen (int index) override
    {
        const auto& st = dly::typeInfo (kTypes[jlimit (0, 3, index)]).start;
        set ("tone", st.filter);
        set ("grit", st.grit);
        set ("wobble", st.depth);
        set ("repeats", st.repeats);
        if (geti ("sync") == 0)
            set ("time", dly::timeFor (kTypes[jlimit (0, 3, index)], st.mode, get ("time")));
    }

    std::vector<Section> sections() const override
    {
        return {
            { "Time", { { "sync", "Sync", "In time with the song (Note) or free (Time)." },
                        { "note", "Note", "The echo's note value when Sync is on." },
                        { "time", "Time", "The echo time in milliseconds when Sync is off." },
                        { "repeats", "Repeats", "How many echoes: past 100% they build up and run away." } } },
            { "Colour", { { "tone", "Tone", "How the echoes are voiced: clean at 0%, darker and warmer as it goes up." },
                          { "grit", "Grit", "Drive and wear on the echoes; each type has its own." },
                          { "wobble", "Wobble", "Pitch movement: chorus on Digital, wow on Tape, drift on Analog." } } },
            { "Output", { { "stereo", "Stereo", "Stereo, ping-pong between the sides, or mono." },
                          { "mix", "Mix", "Dry to wet. 50% is both at full level; use 100% on a return." } } } };
    }

    std::vector<Preset> presets() const override
    {
        return {
            { "Dotted eighth",   { { "type", 0 }, { "sync", 1 }, { "note", 7 },  { "repeats", 0.4f },  { "tone", 0.1f },  { "grit", 0.0f },  { "wobble", 0.05f }, { "stereo", 0 }, { "mix", 0.3f } } },
            { "Slapback",        { { "type", 1 }, { "sync", 0 }, { "time", 110 }, { "repeats", 0.1f },  { "tone", 0.35f }, { "grit", 0.25f }, { "wobble", 0.2f },  { "stereo", 2 }, { "mix", 0.3f } } },
            { "Quarter ping-pong", { { "type", 0 }, { "sync", 1 }, { "note", 8 }, { "repeats", 0.45f }, { "tone", 0.2f },  { "grit", 0.0f },  { "wobble", 0.05f }, { "stereo", 1 }, { "mix", 0.3f } } },
            { "Tape echo",       { { "type", 1 }, { "sync", 1 }, { "note", 8 },  { "repeats", 0.5f },  { "tone", 0.35f }, { "grit", 0.3f },  { "wobble", 0.3f },  { "stereo", 0 }, { "mix", 0.3f } } },
            { "Dub",             { { "type", 1 }, { "sync", 1 }, { "note", 7 },  { "repeats", 0.85f }, { "tone", 0.5f },  { "grit", 0.4f },  { "wobble", 0.35f }, { "stereo", 0 }, { "mix", 0.35f } } },
            { "Warm analog",     { { "type", 2 }, { "sync", 1 }, { "note", 5 },  { "repeats", 0.45f }, { "tone", 0.5f },  { "grit", 0.3f },  { "wobble", 0.15f }, { "stereo", 0 }, { "mix", 0.3f } } },
            { "Lo-fi",           { { "type", 3 }, { "sync", 1 }, { "note", 8 },  { "repeats", 0.4f },  { "tone", 0.5f },  { "grit", 0.5f },  { "wobble", 0.2f },  { "stereo", 0 }, { "mix", 0.3f } } },
            { "Return: quarter", { { "type", 0 }, { "sync", 1 }, { "note", 8 },  { "repeats", 0.4f },  { "tone", 0.25f }, { "grit", 0.0f },  { "wobble", 0.05f }, { "stereo", 1 }, { "mix", 1.0f } } } };
    }

    void prepareToPlay (double sampleRate, int block) override
    {
        engine.prepare (sampleRate, block);
        scratch.assign ((size_t) jmax (block, 512), 0.0f);
    }
    void reset() override   { engine.reset(); }
    double getTailLengthSeconds() const override   { return 8.0; }

    float echoMs (double bpm) const
    {
        if (geti ("sync") != 0)
        {
            const auto& d = divisions();
            return (float) (d[(size_t) jlimit (0, (int) d.size() - 1, geti ("note"))].beats * 60000.0 / bpm);
        }
        return get ("time");
    }

    String statusLine() const override
    {
        const int type = kTypes[jlimit (0, 3, geti ("type"))];
        dly::EngineSettings s;
        s.type = type;
        s.mode = dly::typeInfo (type).start.mode;
        s.timeMs = echoMs (shownBpm.load());
        const float ms = dly::Engine::effectiveTimeMs (s);
        return "Echo every " + msText (ms) + (std::abs (ms - s.timeMs) > 0.5f ? " (as far as this type goes)" : String());
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        midi.clear();
        const double bpm = tempoOf (*this);
        shownBpm = bpm;
        const int type = kTypes[jlimit (0, 3, geti ("type"))];
        const auto& st = dly::typeInfo (type).start;
        dly::EngineSettings s;
        s.type = type;
        s.timeMs = echoMs (bpm);
        s.repeats = get ("repeats");
        s.stereo = geti ("stereo");
        s.a = st.a;
        s.b = st.b;
        s.c = st.c;
        s.mode = st.mode;
        s.filter = get ("tone");
        s.grit = get ("grit");
        s.speed = st.speed;
        s.depth = get ("wobble");
        s.mix = get ("mix");
        stereo (buffer, getTotalNumInputChannels(), scratch, [&] (float* l, float* r, int n)
        {
            engine.process (l, r, l, r, n, s);
        });
    }

private:
    dly::Engine engine;
    std::vector<float> scratch;
    std::atomic<double> shownBpm { 120.0 };
};

//==============================================================================
// Compressor: a feed-forward, soft-knee compressor (the glue stage of the mastering
// engine, with its controls out on the panel), with makeup and parallel mix.
class Compressor final : public Device
{
public:
    Compressor() : Device ("compressor", "Compressor", Kind::effect, layout())
    {
        setCurrentProgram (0);   // the panel and the sound start on the first preset
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (decibels ("threshold", "Threshold", -50.0f, 0.0f, -18.0f));
        l.add (number ("ratio", "Ratio", { 1.0f, 20.0f, 0.0f, 0.4f }, 4.0f, [] (float v)
        {
            return v >= 19.95f ? String ("Limit") : String (v, v < 10.0f ? 1 : 0) + ":1";
        }));
        l.add (number ("attack", "Attack", { 0.1f, 100.0f, 0.0f, 0.4f }, 10.0f, [] (float v) { return msText (v); }));
        l.add (number ("release", "Release", { 10.0f, 1500.0f, 0.0f, 0.4f }, 150.0f, [] (float v) { return msText (v); }));
        l.add (decibels ("makeup", "Makeup", 0.0f, 24.0f, 0.0f));
        l.add (toggle ("auto", "Auto makeup", true));
        l.add (percent ("mix", "Mix", 1.0f));
        return l;
    }

    String blurb() const override   { return "Evens out levels: glue a bus, tame a vocal, squash drums."; }

    std::vector<Section> sections() const override
    {
        return {
            { "Compression", { { "threshold", "Threshold", "Level above which it starts turning things down." },
                               { "ratio", "Ratio", "How hard: 2:1 is gentle, 8:1 is firm, Limit stops peaks." },
                               { "attack", "Attack", "How fast it reacts. Slower lets the start of each hit through." },
                               { "release", "Release", "How fast it lets go after the level falls." } } },
            { "Output", { { "makeup", "Makeup", "Turns the result back up. Added to Auto when that's on." },
                          { "auto", "Auto makeup", "Turns it back up by about what it takes off, so on and off sound as loud." },
                          { "mix", "Mix", "Parallel compression: below 100% blends in the untouched sound." } } } };
    }

    std::optional<Meter> meter() const override   { return Meter { "Reduction", -24.0f, 0.0f }; }
    float meterValue() const override   { return -grShown.load(); }

    std::vector<Preset> presets() const override
    {
        return {
            { "Gentle glue",    { { "threshold", -18 }, { "ratio", 2.0f }, { "attack", 30 },  { "release", 250 }, { "makeup", 0 }, { "auto", 1 }, { "mix", 1.0f } } },
            { "Vocal",          { { "threshold", -22 }, { "ratio", 4.0f }, { "attack", 5 },   { "release", 120 }, { "makeup", 0 }, { "auto", 1 }, { "mix", 1.0f } } },
            { "Bass",           { { "threshold", -20 }, { "ratio", 5.0f }, { "attack", 15 },  { "release", 180 }, { "makeup", 0 }, { "auto", 1 }, { "mix", 1.0f } } },
            { "Drum bus",       { { "threshold", -16 }, { "ratio", 4.0f }, { "attack", 20 },  { "release", 100 }, { "makeup", 0 }, { "auto", 1 }, { "mix", 1.0f } } },
            { "Parallel smash", { { "threshold", -35 }, { "ratio", 12.0f },{ "attack", 1 },   { "release", 80 },  { "makeup", 0 }, { "auto", 1 }, { "mix", 0.4f } } },
            { "Peak catcher",   { { "threshold", -6 },  { "ratio", 20.0f },{ "attack", 0.5f },{ "release", 60 },  { "makeup", 0 }, { "auto", 0 }, { "mix", 1.0f } } } };
    }

    void prepareToPlay (double sampleRate, int) override
    {
        sr = sampleRate;
        reset();
    }
    void reset() override   { gr = 0.0f; grShown = 0.0f; }

    // The static curve: dB of reduction for a level, soft knee.
    static float reduction (float levelDb, float thresholdDb, float ratio)
    {
        constexpr float knee = 6.0f;
        const float over = levelDb - thresholdDb;
        const float slope = 1.0f - 1.0f / ratio;
        if (over <= -knee * 0.5f) return 0.0f;
        if (over >= knee * 0.5f)  return over * slope;
        const float x = over + knee * 0.5f;
        return slope * x * x / (2.0f * knee);
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        midi.clear();
        const int n = buffer.getNumSamples();
        const int chans = jmin (2, buffer.getNumChannels());
        if (n == 0 || chans == 0)
            return;
        if (chans == 2 && getTotalNumInputChannels() < 2)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);

        const float threshold = get ("threshold"), ratio = jmax (1.0f, get ("ratio"));
        const float att = (float) std::exp (-1.0 / (jmax (0.05f, get ("attack")) * 0.001 * sr));
        const float rel = (float) std::exp (-1.0 / (jmax (5.0f, get ("release")) * 0.001 * sr));
        // Auto makeup: half of what the curve takes off a full-scale peak, which keeps
        // on and off close in loudness for most material.
        const float autoDb = geti ("auto") != 0 ? 0.5f * reduction (0.0f, threshold, ratio) : 0.0f;
        const float makeup = Decibels::decibelsToGain (get ("makeup") + autoDb);
        const float mix = get ("mix");

        float* l = buffer.getWritePointer (0);
        float* r = chans > 1 ? buffer.getWritePointer (1) : nullptr;
        float peakGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float x = jmax (std::abs (l[i]), r != nullptr ? std::abs (r[i]) : 0.0f);
            const float levelDb = Decibels::gainToDecibels (x, -120.0f);
            const float target = reduction (levelDb, threshold, ratio);
            // Smooth the reduction: attack when it grows, release when it shrinks.
            gr = target > gr ? att * gr + (1.0f - att) * target : rel * gr + (1.0f - rel) * target;
            const float g = Decibels::decibelsToGain (-gr) * makeup;
            const float wet = mix, dry = 1.0f - mix;
            l[i] = l[i] * (dry + wet * g);
            if (r != nullptr)
                r[i] = r[i] * (dry + wet * g);
            peakGr = jmax (peakGr, gr);
        }
        grShown = peakGr;
    }

private:
    double sr = 48000.0;
    float gr = 0.0f;
    std::atomic<float> grShown { 0.0f };
};

//==============================================================================
// Saturator: drive into one of three curves, oversampled, with a tilt after.
class Saturator final : public Device
{
public:
    Saturator() : Device ("saturator", "Saturator", Kind::effect, layout())
    {
        setCurrentProgram (0);   // the panel and the sound start on the first preset
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("shape", "Shape", { "Tape", "Tube", "Clip" }, 0));
        l.add (decibels ("drive", "Drive", 0.0f, 30.0f, 6.0f));
        l.add (number ("tone", "Tone", { -1.0f, 1.0f }, 0.0f, [] (float v)
        {
            return std::abs (v) < 0.02f ? String ("Flat") : v < 0 ? "Darker " + String (roundToInt (-v * 100.0f)) : "Brighter " + String (roundToInt (v * 100.0f));
        }));
        l.add (percent ("mix", "Mix", 1.0f));
        l.add (decibels ("output", "Output", -18.0f, 6.0f, 0.0f));
        return l;
    }

    String blurb() const override   { return "Warmth and grit: tape, tube or clipped."; }
    String modeParam() const override   { return "shape"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Drive", { { "drive", "Drive", "How hard it's pushed. The level is kept about the same as you turn it." },
                         { "tone", "Tone", "Tilts the result darker or brighter." } } },
            { "Output", { { "mix", "Mix", "Below 100% blends in the clean sound." },
                          { "output", "Output", "Level after everything." } } } };
    }

    std::vector<Preset> presets() const override
    {
        return {
            { "Warm tape",    { { "shape", 0 }, { "drive", 6 },  { "tone", -0.2f }, { "mix", 1.0f }, { "output", 0 } } },
            { "Tape crunch",  { { "shape", 0 }, { "drive", 16 }, { "tone", -0.1f }, { "mix", 1.0f }, { "output", 0 } } },
            { "Tube warmth",  { { "shape", 1 }, { "drive", 8 },  { "tone", 0.0f },  { "mix", 1.0f }, { "output", 0 } } },
            { "Tube bite",    { { "shape", 1 }, { "drive", 18 }, { "tone", 0.2f },  { "mix", 0.8f }, { "output", 0 } } },
            { "Drum clip",    { { "shape", 2 }, { "drive", 8 },  { "tone", 0.0f },  { "mix", 1.0f }, { "output", 0 } } },
            { "Fuzz",         { { "shape", 2 }, { "drive", 28 }, { "tone", -0.3f }, { "mix", 1.0f }, { "output", -3 } } } };
    }

    void prepareToPlay (double sampleRate, int block) override
    {
        sr = sampleRate;
        oversampling = std::make_unique<dsp::Oversampling<float>> (2, 2, dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
        oversampling->initProcessing ((size_t) jmax (block, 16));
        latency = oversampling->getLatencyInSamples();
        setLatencySamples (roundToInt (latency));
        for (auto& f : tilt)
            f.reset();
        lastTone = 99.0f;
        driveS.reset (sampleRate, 0.05);
        mixS.reset (sampleRate, 0.05);
        outS.reset (sampleRate, 0.05);
        driveS.setCurrentAndTargetValue (Decibels::decibelsToGain (get ("drive")));
        mixS.setCurrentAndTargetValue (get ("mix"));
        outS.setCurrentAndTargetValue (Decibels::decibelsToGain (get ("output")));
        dryDelay.setMaximumDelayInSamples (64);
        dryDelay.prepare ({ sampleRate, (uint32) jmax (block, 16), 2 });
        dryDelay.setDelay (latency);
    }

    void reset() override
    {
        if (oversampling != nullptr)
            oversampling->reset();
        dryDelay.reset();
    }

    static float shapeSample (int shape, float x)
    {
        switch (shape)
        {
            case 0:   // tape: a soft, round curve, slightly lopsided
            {
                const float y = std::tanh (x + 0.08f * x * x);
                return y - std::tanh (0.0f);
            }
            case 1:   // tube: asymmetric, the positive side gives first (even harmonics)
                return x >= 0.0f ? x / (1.0f + 0.7f * x) : std::tanh (x);
            default:  // clip: hard, with a small rounded corner
            {
                const float a = std::abs (x);
                if (a < 0.8f) return x;
                const float y = a < 1.2f ? 0.8f + (a - 0.8f) - (a - 0.8f) * (a - 0.8f) / 0.8f : 1.0f;
                return std::copysign (jmin (1.0f, y), x);
            }
        }
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        midi.clear();
        const int n = buffer.getNumSamples();
        const int chans = jmin (2, buffer.getNumChannels());
        if (n == 0 || chans == 0 || oversampling == nullptr)
            return;
        if (chans == 2 && getTotalNumInputChannels() < 2)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
        if (dryCopy.getNumSamples() < n || dryCopy.getNumChannels() < 2)
            dryCopy.setSize (2, n, false, false, true);

        const int shape = geti ("shape");
        const float driveDb = get ("drive");
        driveS.setTargetValue (Decibels::decibelsToGain (driveDb));
        mixS.setTargetValue (get ("mix"));
        // Roughly level-matched: the curves top out near 1, so take back about what drive adds.
        const float compensate = Decibels::decibelsToGain (-0.6f * driveDb);
        outS.setTargetValue (Decibels::decibelsToGain (get ("output")) * compensate);

        const float tone = get ("tone");
        if (std::abs (tone - lastTone) > 0.005f)
        {
            const bool first = lastTone > 50.0f;
            lastTone = tone;
            // A tilt around 1 kHz: one shelf each way.
            const auto low = dsp::IIR::Coefficients<float>::makeLowShelf (sr, 1000.0f, 0.6f, Decibels::decibelsToGain (-6.0f * tone));
            const auto high = dsp::IIR::Coefficients<float>::makeHighShelf (sr, 1000.0f, 0.6f, Decibels::decibelsToGain (6.0f * tone));
            for (int c = 0; c < 2; ++c)
            {
                *tilt[(size_t) c * 2].coefficients = *low;
                *tilt[(size_t) c * 2 + 1].coefficients = *high;
            }
            if (first)
                for (auto& f : tilt)
                    f.reset();
        }

        for (int c = 0; c < chans; ++c)
            dryCopy.copyFrom (c, 0, buffer, c, 0, n);
        {
            dsp::AudioBlock<float> dryBlock (dryCopy.getArrayOfWritePointers(), (size_t) chans, (size_t) n);
            dsp::ProcessContextReplacing<float> ctx (dryBlock);
            dryDelay.process (ctx);
        }

        dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) chans, (size_t) n);
        auto up = oversampling->processSamplesUp (block);
        const int upN = (int) up.getNumSamples();
        // Drive moves smoothly over the block (at the oversampled rate).
        const float d0 = driveS.getCurrentValue();
        const float d1 = driveS.skip (n);
        for (int c = 0; c < (int) up.getNumChannels(); ++c)
        {
            float* x = up.getChannelPointer ((size_t) c);
            for (int i = 0; i < upN; ++i)
            {
                const float d = d0 + (d1 - d0) * ((float) i / (float) upN);
                x[i] = shapeSample (shape, x[i] * d);
            }
        }
        oversampling->processSamplesDown (block);

        for (int c = 0; c < chans; ++c)
        {
            float* x = buffer.getWritePointer (c);
            const float* dry = dryCopy.getReadPointer (c);
            auto& lo = tilt[(size_t) c * 2];
            auto& hi = tilt[(size_t) c * 2 + 1];
            auto m = mixS;
            auto o = outS;
            for (int i = 0; i < n; ++i)
            {
                const float wet = hi.processSample (lo.processSample (x[i])) * o.getNextValue();
                const float mx = m.getNextValue();
                x[i] = wet * mx + dry[i] * (1.0f - mx);
            }
            if (c == chans - 1)
            {
                mixS = m;
                outS = o;
            }
        }
    }

private:
    double sr = 48000.0;
    std::unique_ptr<dsp::Oversampling<float>> oversampling;
    std::array<dsp::IIR::Filter<float>, 4> tilt;
    float lastTone = 99.0f;
    SmoothedValue<float> driveS, mixS, outS;
    AudioBuffer<float> dryCopy;
    dsp::DelayLine<float, dsp::DelayLineInterpolationTypes::Linear> dryDelay;
    float latency = 0.0f;
};

//==============================================================================
// Mastering: the mastering engine (Master/). Style, Target, Amount and Push; Learn sets the gain and drive.
class Master final : public Device, private Timer
{
public:
    Master() : Device ("mastering", "Mastering", Kind::effect, layout())
    {
        setCurrentProgram (0);
        startTimerHz (10);
    }
    ~Master() override   { stopTimer(); }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("style", "Style", { "Gentle", "Warm", "Bright", "Punch", "Dense" }, 0));
        l.add (choice ("target", "Target", { "Streaming", "Download", "CD", "Vinyl" }, 0));
        l.add (percent ("amount", "Amount", 0.5f));
        l.add (decibels ("push", "Push", -6.0f, 6.0f, 0.0f));
        l.add (decibels ("learnedGain", "Learned gain", -24.0f, 24.0f, 0.0f));
        l.add (number ("learnedDrive", "Learned drive", { -6.0f, 40.0f }, 14.0f, [] (float v) { return String (v, 1); }));
        return l;
    }

    String blurb() const override   { return "Finishes a mix to a loudness target."; }
    String modeParam() const override   { return "style"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Master", { { "amount", "Amount", "How much of the style: from nothing to all of it." },
                          { "target", "Target", "Where it's going: sets the loudness and the peak ceiling." },
                          { "push", "Push", "Louder or quieter than the target." } } } };
    }

    std::optional<Meter> meter() const override   { return Meter { "Limiter", -12.0f, 0.0f }; }
    float meterValue() const override   { return limitGr.load(); }

    String actionName() const override   { return "Learn"; }
    bool actionActive() const override   { return learningNow.load(); }
    void action() override               { learnToggles.fetch_add (1); }

    String statusLine() const override
    {
        if (learningNow.load())
            return "Learning: play the song, ideally all of it, then press Learn again. " + String (learnSeconds.load(), 0) + " s so far.";
        if (learnMessage.isNotEmpty())
            return learnMessage;
        const float lufs = shortTerm.load();
        return lufs > -69.0f ? "Loudness " + String (lufs, 1) + " LUFS (short term). Press Learn and play the song to set the levels."
                             : "Press Learn and play the song: it sets the levels for this mix.";
    }

    std::vector<Preset> presets() const override
    {
        return {
            { "Gentle",        { { "style", 0 }, { "amount", 0.5f }, { "push", 0 } } },
            { "Warm",          { { "style", 1 }, { "amount", 0.5f }, { "push", 0 } } },
            { "Bright",        { { "style", 2 }, { "amount", 0.5f }, { "push", 0 } } },
            { "Punch",         { { "style", 3 }, { "amount", 0.6f }, { "push", 0 } } },
            { "Dense and loud",{ { "style", 4 }, { "amount", 0.7f }, { "push", 1 } } } };
    }

    void prepareToPlay (double sampleRate, int) override
    {
        engine.prepare (sampleRate);
        setLatencySamples (engine.latency());
    }
    void reset() override   { engine.reset(); }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        midi.clear();
        mst::MasterSettings s;
        s.style = geti ("style");
        s.target = geti ("target");
        s.amount = get ("amount");
        s.trimDb = get ("push");
        s.inputGainDb = get ("learnedGain");
        s.driveOffsetDb = get ("learnedDrive");
        s.learnToggles = learnToggles.load (std::memory_order_relaxed);
        s.meterResets = meterResets.load (std::memory_order_relaxed);

        // Starting playback starts a fresh reading.
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                const bool playing = pos->getIsPlaying();
                if (playing && ! wasPlaying && ! engine.learning())
                    meterResets.fetch_add (1);
                wasPlaying = playing;
            }

        stereo (buffer, getTotalNumInputChannels(), scratch, [&] (float* l, float* r, int n)
        {
            engine.process (l, r, n, s);
        });

        const auto res = engine.takeLearnResult();
        if (res.code != mst::LearnResult::None)
        {
            pendingGain = res.inputGainDb;
            pendingDrive = res.driveOffsetDb;
            pendingCode = res.code;
        }
        const auto& t = engine.telemetry();
        limitGr = t.limitGrDb;
        shortTerm = t.shortTerm;
        learningNow = t.learning;
        learnSeconds = t.learnSeconds;
    }

private:
    void timerCallback() override
    {
        const int code = pendingCode.exchange (0);
        if (code == mst::LearnResult::Done)
        {
            set ("learnedGain", pendingGain.load());
            set ("learnedDrive", pendingDrive.load());
            learnMessage = "Learned. Levels set for this mix.";
        }
        else if (code == mst::LearnResult::TooShort)
            learnMessage = "Too short to judge. Press Learn and play at least 5 seconds, ideally the whole song.";
    }

    mst::Master engine;
    std::vector<float> scratch;
    std::atomic<int> learnToggles { 0 }, meterResets { 0 }, pendingCode { 0 };
    std::atomic<float> pendingGain { 0.0f }, pendingDrive { 14.0f }, limitGr { 0.0f }, shortTerm { -70.0f }, learnSeconds { 0.0f };
    std::atomic<bool> learningNow { false };
    bool wasPlaying = false;
    String learnMessage;
};
} // namespace

//==============================================================================
const std::vector<Info>& all()
{
    static const std::vector<Info> list {
        { "bass", "Bass Synth", Kind::instrument, "A mono synth for basslines: saw, square or triangle, a sub, and glide.", [] { return makeBassSynth(); } },
        { "poly", "Poly Synth", Kind::instrument, "An eight-voice synth for pads, keys, brass and plucks.", [] { return makePolySynth(); } },
        { "multi", "Multi Synth", Kind::instrument, "Eight voices, 24 ways to make a sound: analog, FM, wavetables, chords, speech, strings and drums.", [] { return makeMultiSynth(); } },
        { "resonator", "Resonator", Kind::instrument, "Strings, bells and struck bars that ring when you play them.", [] { return makeResonator(); } },
        { "drums", "Drums", Kind::instrument, "Synthesized kits in the style of the classic drum machines: Simple 909, 808, 606 and 707.", [] { return makeDrums(); } },
        { "reverb", "Reverb", Kind::effect, "Rooms, a chamber, a hall, a plate and a spring.", [] { return std::unique_ptr<Device> (new Reverb()); } },
        { "delay", "Delay", Kind::effect, "Clean, tape, analog and lo-fi echoes, in time with the song.", [] { return std::unique_ptr<Device> (new Delay()); } },
        { "grains", "Grains", Kind::effect, "Chops what goes through it into grains, then smears, scatters, pitches and freezes them.", [] { return makeGrains(); } },
        { "compressor", "Compressor", Kind::effect, "Evens out levels: glue a bus, tame a vocal, squash drums.", [] { return std::unique_ptr<Device> (new Compressor()); } },
        { "saturator", "Saturator", Kind::effect, "Warmth and grit: tape, tube or clipped.", [] { return std::unique_ptr<Device> (new Saturator()); } },
        { "mastering", "Mastering", Kind::effect, "Finishes a mix to a loudness target.", [] { return std::unique_ptr<Device> (new Master()); } },
        { "randomnotes", "Random Notes", Kind::generator, "Plays random melodies and rhythms in time and in key, and can loop what it played.", [] { return makeRandomNotes(); } } };
    return list;
}
} // namespace lathe::devices
