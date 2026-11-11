// Multi Synth, Resonator, Grains and Random Notes: Lathe devices built on Émilie Gillet's
// open-source DSP (MIT licence, ThirdParty/mutable). That code was written for fixed-rate
// hardware, so it runs at its own rate here (CoreRate resamples when the project's rate
// differs) and is driven from MIDI and the transport instead of knobs and patch cables.
#include "Mutable.h"

// The DSP's own headers. TEST selects their desktop code (no ARM-only instructions); it's
// defined the same way when ThirdParty/mutable is compiled (CMakeLists.txt).
#define TEST 1
#include "plaits/dsp/voice.h"
#include "rings/dsp/part.h"
#include "beads/dsp/processor.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"
#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/preset_scales.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/gate_flags.h"
#undef TEST

namespace lathe::devices
{
//==============================================================================
void CoreRate::prepare (double hostRate, double coreRate, int chunkSize)
{
    host = hostRate > 0.0 ? hostRate : 48000.0;
    core = coreRate;
    chunk = jlimit (1, kMaxChunk, chunkSize);
    direct = std::abs (host - core) < 0.5;
    for (auto* i : { &up[0], &up[1], &down[0], &down[1] })
        i->reset();
    for (auto* v : { &out[0], &out[1], &in[0], &in[1] })
    {
        v->clear();
        v->reserve (65536);
    }
    outRead = inRead = 0;
    latency = 0;
    if (! direct)
    {
        // A little in hand, so the output side never waits for the next chunk.
        float silence[kMaxChunk] = {};
        for (int i = 0; i < 2; ++i)
            push (silence, silence, chunk);
        latency = (int) std::round ((2.0 * chunk + 4.0) * host / core + 2.0 * (double) up[0].getBaseLatency());
    }
}

void CoreRate::push (const float* l, const float* r, int n)
{
    out[0].insert (out[0].end(), l, l + n);
    out[1].insert (out[1].end(), r, r + n);
}

void CoreRate::pull (float* left, float* right, int numSamples)
{
    const double ratio = core / host;   // core samples per host sample
    const int need = (int) std::ceil (numSamples * ratio) + 4;
    if (available() < need)
    {
        // Only if the input ran short (a hiccup): fill with silence rather than read past the end.
        float silence[kMaxChunk] = {};
        while (available() < need)
            push (silence, silence, chunk);
    }
    const int used = up[0].process (ratio, out[0].data() + outRead, left, numSamples);
    up[1].process (ratio, out[1].data() + outRead, right, numSamples);
    outRead += used;
    compact();
}

void CoreRate::compact()
{
    // Drop what's been read, now and then, so the buffers never grow (no allocation:
    // erasing keeps the capacity reserved in prepare()).
    if (outRead > 8192)
    {
        for (auto& v : out)
            v.erase (v.begin(), v.begin() + outRead);
        outRead = 0;
    }
    if (inRead > 8192)
    {
        for (auto& v : in)
            v.erase (v.begin(), v.begin() + inRead);
        inRead = 0;
    }
}

namespace
{
String noteName (float note)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = jlimit (0, 127, roundToInt (note));
    return String (names[n % 12]) + String (n / 12 - 2);   // middle C (60) is C3, as everywhere in Lathe
}

std::unique_ptr<AudioParameterFloat> semitones (const char* id, const char* name, float range, float def)
{
    return number (id, name, { -range, range, 0.0f, 1.0f }, def, [] (float v)
    {
        const float r = std::round (v * 10.0f) / 10.0f;
        return (r > 0.0f ? "+" : "") + String (r, 1) + " st";
    });
}

std::unique_ptr<AudioParameterFloat> bipolar (const char* id, const char* name, float def)
{
    return number (id, name, { -1.0f, 1.0f, 0.0f, 1.0f }, def, [] (float v)
    {
        const int p = roundToInt (v * 100.0f);
        return (p > 0 ? "+" : "") + String (p) + "%";
    });
}

/** Splits a block at its MIDI events: render (from, to), then the event, in order. */
template <typename Render, typename Event>
void splitAtMidi (int numSamples, const MidiBuffer& midi, Render&& render, Event&& event)
{
    int pos = 0;
    for (const auto m : midi)
    {
        const int at = jlimit (0, numSamples, m.samplePosition);
        if (at > pos)
        {
            render (pos, at);
            pos = at;
        }
        event (m.getMessage());
    }
    if (pos < numSamples)
        render (pos, numSamples);
}

//==============================================================================
// Multi Synth: eight voices of a 24-model macro oscillator (each with its own low-pass
// gate), played like any synth: notes start and stop them, velocity is how hard.
class MultiSynth final : public Device
{
public:
    static constexpr int kVoices = 8;
    static constexpr int kChunk = 12;          // the block size its envelopes are timed for
    static constexpr float kCoreRate = 48000.0f;

    // The models, in the order the panel lists them, and which of the DSP's engines each one is.
    struct Model { const char* name; int engine; };
    static const std::vector<Model>& models()
    {
        static const std::vector<Model> list {
            { "Analog pair", 8 },        { "Filtered analog", 0 },   { "Waveshaper", 9 },      { "Two-op FM", 10 },
            { "Six-op FM: keys", 2 },    { "Six-op FM: bells", 3 },  { "Six-op FM: basses", 4 }, { "Phase distortion", 1 },
            { "Wavetable", 13 },         { "Wave terrain", 5 },      { "Harmonics", 12 },      { "Formants", 11 },
            { "Chords", 14 },            { "String ensemble", 6 },   { "Chiptune", 7 },        { "Speech", 15 },
            { "Swarm", 16 },             { "Filtered noise", 17 },   { "Particles", 18 },      { "Plucked string", 19 },
            { "Struck bell", 20 },       { "Kick", 21 },             { "Snare", 22 },          { "Hi-hat", 23 } };
        return list;
    }

    MultiSynth() : Device ("multi", "Multi Synth", Kind::instrument, layout())
    {
        for (auto& v : voices)
            v = std::make_unique<Voice>();
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        StringArray names;
        for (auto& m : models())
            names.add (m.name);
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("model", "Model", names, 0));
        l.add (percent ("harmonics", "Harmonics", 0.5f));
        l.add (percent ("timbre", "Timbre", 0.5f));
        l.add (percent ("morph", "Morph", 0.5f));
        l.add (bipolar ("envTimbre", "Env to timbre", 0.0f));
        l.add (bipolar ("envMorph", "Env to morph", 0.0f));
        l.add (bipolar ("envPitch", "Env to pitch", 0.0f));
        l.add (percent ("decay", "Decay", 0.5f));
        l.add (percent ("colour", "Colour", 0.5f));
        l.add (number ("attack", "Attack", { 0.5f, 3000.0f, 0.0f, 0.35f }, 2.0f, [] (float v) { return msText (v); }));
        l.add (number ("release", "Release", { 5.0f, 6000.0f, 0.0f, 0.4f }, 300.0f, [] (float v) { return msText (v); }));
        l.add (percent ("alt", "Alt output", 0.0f));
        l.add (choice ("voices", "Voices", { "Eight", "One" }, 0));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -6.0f));
        return l;
    }

    String blurb() const override   { return "Eight voices, 24 ways to make a sound: analog, FM, wavetables, chords, speech, strings and drums."; }

    std::vector<Section> sections() const override
    {
        return {
            { "Sound", { { "model", "Model", "How the sound is made. Each model uses the three knobs beside it its own way." },
                         { "harmonics", "Harmonics", "The model's first control: often the mix of partials or the pitch of a second oscillator." },
                         { "timbre", "Timbre", "The model's main tone control: brightness, wave shape or filter." },
                         { "morph", "Morph", "The model's third control: a second shape, the noise, or the variation." } } },
            { "Envelope", { { "envTimbre", "Env to timbre", "How much each note's decay sweeps Timbre (left: downwards)." },
                            { "envMorph", "Env to morph", "How much each note's decay sweeps Morph." },
                            { "envPitch", "Env to pitch", "A pitch drop or rise at the start of each note, as on a drum." },
                            { "decay", "Decay", "How fast that sweep (and the strings, bells and drums) dies away." } } },
            { "Gate", { { "attack", "Attack", "How fast a note fades in." },
                        { "release", "Release", "How long it rings after you let go. Drums, strings, bells and six-op FM ring on their own: Decay sets how long." },
                        { "colour", "Colour", "Low: notes close darker as they fade, like a plucked filter. High: they just get quieter." } } },
            { "Output", { { "alt", "Alt output", "Blends in the model's second output: a sub, a variation or another part of the sound." },
                          { "voices", "Voices", "Eight to play chords, or one for a single line (each note cuts the last)." },
                          { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int model, float h, float t, float m, float eT, float eM, float eP, float dec,
                     float col, float att, float rel, float alt, float level) -> Preset
        {
            return { name, { { "model", (float) model }, { "harmonics", h }, { "timbre", t }, { "morph", m }, { "envTimbre", eT },
                             { "envMorph", eM }, { "envPitch", eP }, { "decay", dec }, { "colour", col }, { "attack", att },
                             { "release", rel }, { "alt", alt }, { "voices", 0.0f }, { "level", level } } };
        };
        return {
            p ("Analog brass",     0,  0.25f, 0.45f, 0.5f,  0.35f, 0.0f,  0.0f,  0.55f, 0.3f,  15.0f,  250.0f, 0.0f, -8.0f),
            p ("Glassy keys",      4,  0.3f,  0.45f, 0.4f,  0.0f,  0.0f,  0.0f,  0.6f,  0.7f,  1.0f,   600.0f, 0.0f, -7.0f),
            p ("Bell tree",        5,  0.5f,  0.6f,  0.35f, 0.0f,  0.0f,  0.0f,  0.7f,  0.8f,  1.0f,   1500.0f, 0.3f, -8.0f),
            p ("Wavetable pad",    8,  0.4f,  0.35f, 0.6f,  0.0f,  0.25f, 0.0f,  0.8f,  0.9f,  600.0f, 1800.0f, 0.2f, -10.0f),
            p ("Choir",            11, 0.55f, 0.5f,  0.45f, 0.0f,  0.0f,  0.0f,  0.6f,  0.9f,  300.0f, 1200.0f, 0.0f, -9.0f),
            p ("Chord stabs",      12, 0.3f,  0.55f, 0.5f,  0.3f,  0.0f,  0.0f,  0.35f, 0.2f,  1.0f,   200.0f, 0.0f, -11.0f),
            p ("Talking lead",     15, 0.75f, 0.5f,  0.5f,  0.0f,  0.0f,  0.0f,  0.5f,  0.8f,  5.0f,   300.0f, 0.0f, -6.0f),
            p ("Nylon pluck",      19, 0.4f,  0.55f, 0.45f, 0.0f,  0.0f,  0.0f,  0.55f, 0.5f,  0.5f,   400.0f, 0.0f, -4.0f),
            p ("Marimba",          20, 0.35f, 0.6f,  0.4f,  0.0f,  0.0f,  0.0f,  0.45f, 0.5f,  0.5f,   500.0f, 0.0f, -4.0f),
            p ("Chip arp",         14, 0.5f,  0.5f,  0.3f,  0.0f,  0.0f,  0.0f,  0.4f,  0.5f,  0.5f,   120.0f, 0.0f, -12.0f),
            p ("Deep kick",        21, 0.3f,  0.45f, 0.6f,  0.0f,  0.0f,  0.0f,  0.55f, 0.5f,  0.5f,   300.0f, 0.0f, -2.0f) };
    }

    double getTailLengthSeconds() const override   { return get ("release") * 0.001 + 2.0; }

    void prepareToPlay (double rate, int) override
    {
        bridge.prepare (rate, kCoreRate, kChunk);
        setLatencySamples (bridge.latencySamples());
        reset();
    }

    void reset() override
    {
        for (auto& v : voices)
        {
            stmlib::BufferAllocator allocator (v->ram, sizeof (v->ram));
            v->core.Init (&allocator);
            v->active = v->held = false;
            v->level = 0.0f;
        }
        bend = mod = 0.0f;
        age = 0;
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0)
            return;

        readControls();
        float* outL = buffer.getWritePointer (0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : scratch.data();
        if ((int) scratch.size() < n)
            scratch.resize ((size_t) n);   // only if the block is bigger than prepared for

        splitAtMidi (n, midi, [&] (int from, int to)
        {
            bridge.generate (outL + from, outR + from, to - from, [this] (float* l, float* r, int count) { render (l, r, count); });
        },
        [&] (const MidiMessage& m)
        {
            if (m.isNoteOn())
                noteOn (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
                noteOff (m.getNoteNumber());
            else if (m.isPitchWheel())
                bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isController() && m.getControllerNumber() == 1)
                mod = (float) m.getControllerValue() / 127.0f;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                for (auto& v : voices)
                {
                    v->held = false;
                    if (m.isAllSoundOff())
                        v->active = false;
                }
        });

        const float gain = Decibels::decibelsToGain (get ("level"));
        buffer.applyGain (gain);
    }

private:
    struct Voice
    {
        plaits::Voice core;
        char ram[16384];        // the models' working memory (they share it, one at a time)
        int note = 60;
        float velocity = 0.0f, level = 0.0f, pan = 0.0f;
        bool held = false, active = false, retrigger = false;
        uint64 started = 0;
        int quietSamples = 0;
    };

    struct Controls
    {
        int engine = 8;
        float harmonics, timbre, morph, envTimbre, envMorph, envPitch, decay, colour, attackStep, releaseStep, alt;
        bool mono = false;
    } c;

    void readControls()
    {
        const auto& list = models();
        c.engine = list[(size_t) jlimit (0, (int) list.size() - 1, geti ("model"))].engine;
        c.harmonics = get ("harmonics");
        c.timbre = get ("timbre");
        c.morph = get ("morph");
        c.envTimbre = get ("envTimbre");
        c.envMorph = get ("envMorph");
        c.envPitch = get ("envPitch");
        c.decay = get ("decay");
        c.colour = get ("colour");
        c.attackStep = (float) kChunk / (get ("attack") * 0.001f * kCoreRate);
        c.releaseStep = (float) kChunk / (get ("release") * 0.001f * kCoreRate);
        c.alt = get ("alt");
        c.mono = geti ("voices") == 1;
    }

    void noteOn (int note, float velocity)
    {
        Voice* v = nullptr;
        if (c.mono)
        {
            v = voices[0].get();
            for (int i = 1; i < kVoices; ++i)
                voices[(size_t) i]->held = voices[(size_t) i]->active = false;
        }
        else
        {
            // The same note again, else a free voice, else the oldest one.
            for (auto& x : voices)
                if (x->active && x->note == note)
                    v = x.get();
            for (auto& x : voices)
                if (v == nullptr && ! x->active)
                    v = x.get();
            if (v == nullptr)
            {
                v = voices[0].get();
                for (auto& x : voices)
                    if (x->started < v->started)
                        v = x.get();
            }
        }
        v->retrigger = v->active;   // a voice already sounding needs its gate to fall first
        v->note = note;
        v->velocity = jlimit (0.05f, 1.0f, velocity);
        v->held = v->active = true;
        v->started = ++age;
        v->quietSamples = 0;
        v->pan = ((age & 1) != 0 ? 0.15f : -0.15f) * (c.mono ? 0.0f : 1.0f);
    }

    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v->held && v->note == note)
                v->held = false;
    }

    void render (float* l, float* r, int count)
    {
        std::fill (l, l + count, 0.0f);
        std::fill (r, r + count, 0.0f);
        plaits::Patch patch {};
        patch.harmonics = c.harmonics;
        patch.timbre = jlimit (0.0f, 1.0f, c.timbre + mod * 0.5f);
        patch.morph = c.morph;
        patch.frequency_modulation_amount = c.envPitch;
        patch.timbre_modulation_amount = c.envTimbre;
        patch.morph_modulation_amount = c.envMorph;
        patch.engine = c.engine;
        patch.decay = c.decay;
        patch.lpg_colour = c.colour;

        for (auto& vp : voices)
        {
            auto& v = *vp;
            if (! v.active)
                continue;
            // The gate envelope, once per chunk.
            const float before = v.level;
            if (v.held && ! v.retrigger)
                v.level = jmin (1.0f, v.level + c.attackStep);
            else
                v.level = jmax (0.0f, v.level - c.releaseStep);

            // The hardware's clock runs at 47872 Hz, 4.6 cents under 48 kHz: its pitch maths assumes that.
            patch.note = (float) v.note + bend - 0.0461f;
            plaits::Modulations m {};
            m.trigger_patched = true;
            m.level_patched = true;
            m.trigger = v.held && ! v.retrigger ? 1.0f : 0.0f;
            m.level = v.level * v.velocity;
            plaits::Voice::Frame frames[kChunk];
            v.core.Render (patch, m, frames, (size_t) count);
            v.retrigger = false;

            // Models that shape their own decay (six-op FM, plucked string, bell, drums) ring on after the key
            // is up. The rest follow the Attack and Release envelope at the output too, since the core's own
            // low-pass gate tail doesn't stop when it's told to.
            const bool vca = ! selfEnveloped (c.engine);
            float peak = 0.0f;
            const float gl = 0.5f * (1.0f - v.pan), gr = 0.5f * (1.0f + v.pan);
            for (int i = 0; i < count; ++i)
            {
                float s = ((float) frames[i].out * (1.0f - c.alt) + (float) frames[i].aux * c.alt) / 32768.0f;
                if (vca)
                    s *= before + (v.level - before) * (float) (i + 1) / (float) count;
                l[i] += s * gl;
                r[i] += s * gr;
                peak = jmax (peak, std::abs (s));
            }

            // Strings, bells and drums ring on after the key is up: stop once it's quiet for a while.
            if (! v.held && v.level <= 0.0f)
            {
                v.quietSamples = peak < 1.0e-4f ? v.quietSamples + count : 0;
                if (v.quietSamples > (int) (0.25f * kCoreRate))
                    v.active = false;
            }
        }
    }

    static bool selfEnveloped (int engine)
    {
        switch (engine) { case 2: case 3: case 4: case 19: case 20: case 21: case 22: case 23: return true; default: return false; }
    }

    std::array<std::unique_ptr<Voice>, kVoices> voices;
    CoreRate bridge;
    std::vector<float> scratch = std::vector<float> (8192);
    float bend = 0.0f, mod = 0.0f;
    uint64 age = 0;
};

//==============================================================================
// Resonator: modelled strings, bells and bars, struck by each note. It rings until it dies
// away (there's no note-off: let go and it keeps ringing, like a real one).
class Resonator final : public Device
{
public:
    static constexpr int kChunk = 24;

    Resonator() : Device ("resonator", "Resonator", Kind::instrument, layout())
    {
        core = std::make_unique<Core>();
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("model", "Model", { "Bells & bars", "Sympathetic strings", "String", "FM tones", "Chord strings", "String & reverb" }, 0));
        l.add (percent ("structure", "Structure", 0.4f));
        l.add (percent ("brightness", "Brightness", 0.6f));
        l.add (percent ("damping", "Damping", 0.55f));
        l.add (percent ("position", "Position", 0.35f));
        l.add (percent ("velocity", "Velocity", 0.6f));
        l.add (choice ("polyphony", "Voices", { "One", "Two", "Four" }, 2));
        l.add (percent ("width", "Width", 0.35f));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -3.0f));
        return l;
    }

    String blurb() const override   { return "Strings, bells and struck bars that ring when you play them."; }
    String modeParam() const override   { return "model"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Resonator", { { "structure", "Structure", "The shape of the thing that rings: bar to bell to plate; for strings, which chord sounds along." },
                             { "brightness", "Brightness", "How bright it's struck and how bright it rings." },
                             { "damping", "Damping", "How long it rings: left is short and muted, right rings for ages." },
                             { "position", "Position", "Where it's struck or plucked: the middle sounds hollow, near the end thin." } } },
            { "Playing", { { "velocity", "Velocity", "How much harder notes sound louder and brighter." },
                           { "polyphony", "Voices", "How many notes can ring at once; each new note takes the next one." } } },
            { "Output", { { "width", "Width", "Notes take turns left and right (one note: its two sides). This is how far apart." },
                          { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int model, float s, float b, float d, float pos, float poly) -> Preset
        {
            return { name, { { "model", (float) model }, { "structure", s }, { "brightness", b }, { "damping", d },
                             { "position", pos }, { "polyphony", poly } } };
        };
        return {
            p ("Marimba",          0, 0.25f, 0.45f, 0.45f, 0.3f,  2.0f),
            p ("Glass bells",      0, 0.7f,  0.75f, 0.75f, 0.25f, 2.0f),
            p ("Gong",             0, 0.85f, 0.4f,  0.85f, 0.5f,  1.0f),
            p ("Harp",             2, 0.5f,  0.55f, 0.6f,  0.2f,  2.0f),
            p ("Muted guitar",     2, 0.4f,  0.35f, 0.25f, 0.15f, 2.0f),
            p ("Sitar drone",      1, 0.6f,  0.7f,  0.8f,  0.3f,  2.0f),
            p ("Chord strings",    4, 0.3f,  0.6f,  0.7f,  0.35f, 2.0f),
            p ("Electric piano",   3, 0.35f, 0.5f,  0.55f, 0.4f,  2.0f),
            p ("Spacey strings",   5, 0.5f,  0.6f,  0.7f,  0.3f,  2.0f) };
    }

    double getTailLengthSeconds() const override   { return 8.0; }

    void prepareToPlay (double rate, int) override
    {
        bridge.prepare (rate, 48000.0, kChunk);
        setLatencySamples (bridge.latencySamples());
        reset();
    }

    void reset() override
    {
        core->part.Init (core->reverb);
        settling = 4;   // a strike in the first blocks after Init is lost while the resonators set up
        queued = queueRead = 0;
        lastNote = 60.0f;
        bend = 0.0f;
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0)
            return;

        static constexpr int kPoly[] { 1, 2, 4 };
        auto& part = core->part;
        part.set_model ((rings::ResonatorModel) jlimit (0, 5, geti ("model")));   // does nothing unless it changed
        // Setting the polyphony restarts the resonators, so only when it changes.
        if (const int poly = kPoly[jlimit (0, 2, geti ("polyphony"))]; poly != part.polyphony())
            part.set_polyphony (poly);
        patch.structure = get ("structure");
        patch.brightness = get ("brightness");
        patch.damping = get ("damping");
        patch.position = get ("position");
        velocityAmount = get ("velocity");

        float* outL = buffer.getWritePointer (0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : scratch.data();
        splitAtMidi (n, midi, [&] (int from, int to)
        {
            bridge.generate (outL + from, outR + from, to - from, [this] (float* l, float* r, int count) { render (l, r, count); });
        },
        [&] (const MidiMessage& m)
        {
            // Notes in a chord are struck one chunk apart (half a millisecond): a strum.
            if (m.isNoteOn() && queued - queueRead < (int) pending.size())
                pending[(size_t) (queued++ % (int) pending.size())] = { (float) m.getNoteNumber(), m.getFloatVelocity() };
            else if (m.isPitchWheel())
                bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isAllSoundOff())
                reset();
        });

        // Width: the two outputs ring differently; narrow pulls them together.
        const float width = get ("width");
        const float gain = Decibels::decibelsToGain (get ("level"));
        if (buffer.getNumChannels() > 1)
            for (int i = 0; i < n; ++i)
            {
                const float mid = 0.5f * (outL[i] + outR[i]), side = 0.5f * (outL[i] - outR[i]) * width;
                outL[i] = (mid + side) * gain;
                outR[i] = (mid - side) * gain;
            }
        else
            buffer.applyGain (gain);
    }

private:
    struct Core
    {
        rings::Part part;
        uint16_t reverb[32768];
    };
    struct Strike { float note, velocity; };

    void render (float* l, float* r, int count)
    {
        rings::PerformanceState ps {};
        ps.internal_exciter = true;   // it strikes and plucks itself
        ps.internal_strum = false;
        ps.internal_note = false;
        ps.tonic = 0.0f;
        ps.fm = bend;
        ps.chord = jlimit (0, rings::kNumChords - 1, roundToInt (patch.structure * (float) (rings::kNumChords - 1)));
        ps.strum_level = 1.0f;
        rings::Patch p = patch;
        if (settling > 0)
            --settling;
        else if (queueRead < queued)
        {
            const auto s = pending[(size_t) (queueRead++ % (int) pending.size())];
            ps.strum = true;
            lastNote = s.note;
            // Velocity: softer notes are struck softer, and a little darker.
            const float soft = velocityAmount * (1.0f - s.velocity);
            ps.strum_level = 1.0f - soft * 0.9f;
            p.brightness = patch.brightness * (1.0f - soft * 0.3f);
        }
        ps.note = lastNote;
        float input[kChunk] = {};
        core->part.Process (ps, p, input, l, r, (size_t) count);
    }

    std::unique_ptr<Core> core;
    rings::Patch patch {};
    CoreRate bridge;
    std::array<Strike, 32> pending {};
    int queued = 0, queueRead = 0, settling = 4;
    float lastNote = 60.0f, bend = 0.0f, velocityAmount = 0.6f;
    std::vector<float> scratch = std::vector<float> (8192);
};

//==============================================================================
// Grains: records what goes through it and plays it back as grains, which it can smear,
// pitch, scatter and freeze. Size all the way up turns it into a delay.
class Grains final : public Device
{
public:
    static constexpr int kChunk = 24;

    Grains() : Device ("grains", "Grains", Kind::effect, layout())
    {
        core = std::make_unique<Core>();
        uint8_t* buffers[3] { core->delay1, core->delay1 + 256 * 1024, core->delay2 };
        core->processor.Init (buffers, core->reverb);
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("quality", "Quality", { "Clean", "Grainy", "Tape", "Worn tape" }, 0));
        l.add (percent ("density", "Density", 0.6f));
        l.add (toggle ("scatter", "Random timing", false));
        l.add (percent ("time", "Time", 0.15f));
        l.add (percent ("size", "Size", 0.5f));
        l.add (percent ("shape", "Shape", 0.5f));
        l.add (semitones ("pitch", "Pitch", 24.0f, 0.0f));
        l.add (percent ("timeRandom", "Time spread", 0.2f));
        l.add (percent ("sizeRandom", "Size spread", 0.0f));
        l.add (percent ("shapeRandom", "Shape spread", 0.0f));
        l.add (percent ("pitchRandom", "Pitch spread", 0.0f));
        l.add (toggle ("freeze", "Freeze", false));
        l.add (percent ("feedback", "Feedback", 0.2f));
        l.add (percent ("reverb", "Reverb", 0.25f));
        l.add (percent ("mix", "Mix", 0.5f));
        return l;
    }

    String blurb() const override   { return "Chops what goes through it into grains, then smears, scatters, pitches and freezes them."; }
    String modeParam() const override   { return "quality"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Grains", { { "density", "Density", "How many grains: from a few to a dense cloud." },
                          { "scatter", "Random timing", "Grains come at random moments instead of in a steady stream." },
                          { "time", "Time", "How far back in what it's heard each grain starts." },
                          { "size", "Size", "How long each grain is. All the way up, it becomes a delay (Time sets the echo)." },
                          { "shape", "Shape", "Each grain's envelope: clicky, smooth, or played backwards." },
                          { "pitch", "Pitch", "Transposes the grains, up to two octaves either way." } } },
            { "Spread", { { "timeRandom", "Time spread", "Each grain starts somewhere a little different." },
                          { "sizeRandom", "Size spread", "Each grain is a different length." },
                          { "shapeRandom", "Shape spread", "Each grain has a different envelope." },
                          { "pitchRandom", "Pitch spread", "Each grain is a different pitch." } } },
            { "Output", { { "freeze", "Freeze", "Stops recording: the grains keep playing what's already there." },
                          { "feedback", "Feedback", "Sends the grains back in: they build up into washes." },
                          { "reverb", "Reverb", "A reverb on the grains." },
                          { "mix", "Mix", "Dry to wet." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int quality, float density, bool scatter, float time, float size, float shape, float pitch,
                     float tr, float sr, float pr, float fb, float rev, float mix) -> Preset
        {
            return { name, { { "quality", (float) quality }, { "density", density }, { "scatter", scatter ? 1.0f : 0.0f },
                             { "time", time }, { "size", size }, { "shape", shape }, { "pitch", pitch }, { "timeRandom", tr },
                             { "sizeRandom", sr }, { "shapeRandom", 0.0f }, { "pitchRandom", pr }, { "freeze", 0.0f },
                             { "feedback", fb }, { "reverb", rev }, { "mix", mix } } };
        };
        return {
            p ("Soft cloud",       0, 0.7f,  true,  0.35f, 0.55f, 0.55f, 0.0f,   0.35f, 0.2f, 0.0f,  0.3f, 0.4f, 0.5f),
            p ("Shimmer up",       0, 0.65f, true,  0.3f,  0.6f,  0.6f,  12.0f,  0.25f, 0.2f, 0.0f,  0.45f, 0.5f, 0.45f),
            p ("Stutter",          0, 0.45f, false, 0.1f,  0.2f,  0.2f,  0.0f,   0.0f,  0.0f, 0.0f,  0.1f, 0.1f, 0.6f),
            p ("Octave scatter",   1, 0.75f, true,  0.4f,  0.35f, 0.4f,  0.0f,   0.5f,  0.3f, 0.6f,  0.2f, 0.3f, 0.5f),
            p ("Tape echo",        2, 0.6f,  false, 0.45f, 1.0f,  0.5f,  0.0f,   0.0f,  0.0f, 0.0f,  0.45f, 0.2f, 0.35f),
            p ("Worn memories",    3, 0.65f, true,  0.6f,  0.7f,  0.6f,  -12.0f, 0.4f,  0.3f, 0.1f,  0.35f, 0.55f, 0.55f),
            p ("Reverse swell",    0, 0.7f,  true,  0.5f,  0.65f, 0.95f, 0.0f,   0.3f,  0.2f, 0.0f,  0.25f, 0.45f, 0.5f) };
    }

    double getTailLengthSeconds() const override   { return 12.0; }

    void prepareToPlay (double rate, int) override
    {
        hostRate = rate;
        media = -1;   // set up on the first block
        configure (jlimit (0, 3, geti ("quality")));
    }

    void reset() override
    {
        media = -1;
        configure (jlimit (0, 3, geti ("quality")));
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer&) override
    {
        ScopedNoDenormals noDenormals;
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0)
            return;
        configure (jlimit (0, 3, geti ("quality")));

        // Beads' density knob: steady grains left of centre (faster further left), random
        // ones right of it (faster further right), none in the middle.
        const float rate = get ("density");
        density.base = (bool) geti ("scatter") ? 0.53f + 0.47f * rate : 0.47f * (1.0f - rate);
        density.cv = 0.0f;
        auto randomized = [] (beads::RandomizedParameter& p, float base, float spread)
        {
            p.base = base;
            p.attenuverter = -spread;   // random per grain
            p.cv = 0.0f;
            p.has_cv = false;
        };
        randomized (params.time, get ("time"), get ("timeRandom"));
        randomized (params.size, get ("size"), get ("sizeRandom"));
        randomized (params.shape, get ("shape"), get ("shapeRandom"));
        randomized (params.pitch, get ("pitch"), get ("pitchRandom"));
        params.monophonic_mode = false;
        mix.feedback = get ("feedback");
        mix.dry_wet = get ("mix");
        mix.reverb_amount = get ("reverb");
        mix.output_gate = false;
        freeze = (bool) geti ("freeze");

        const float* inL = buffer.getReadPointer (0);
        const float* inR = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : inL;
        // The output overwrites the input, so take a copy of it first.
        dry.setSize (2, n, false, false, true);
        dry.copyFrom (0, 0, inL, n);
        dry.copyFrom (1, 0, inR, n);
        float* outL = buffer.getWritePointer (0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : scratch.data();
        bridge.process (dry.getReadPointer (0), dry.getReadPointer (1), outL, outR, n,
                        [this] (const float* a, const float* b, float* l, float* r, int count)
        {
            beads::FloatFrame in[kChunk], out[kChunk];
            bool frozen[kChunk];
            for (int i = 0; i < count; ++i)
            {
                in[i].l = a[i];
                in[i].r = b[i];
                frozen[i] = freeze;
            }
            core->processor.Process (beads::SEED_BUTTON_STATE_LOCKED, nullptr, frozen, density, params, mix, in, out, (size_t) count);
            for (int i = 0; i < count; ++i)
            {
                l[i] = out[i].l;
                r[i] = out[i].r;
            }
        });
    }

private:
    struct Core
    {
        beads::Processor processor;
        uint8_t delay1[512 * 1024];
        uint8_t delay2[256 * 1024];
        uint16_t reverb[32768];
    };

    /** The four qualities run at different rates on the module (24, 32 and 48 kHz), and their
        timing is built on it, so the core rate follows the quality. */
    void configure (int quality)
    {
        static constexpr beads::Processor::Media kMedia[] { beads::Processor::MEDIA_DIGITAL, beads::Processor::MEDIA_VINTAGE_DIGITAL,
                                                            beads::Processor::MEDIA_CLEAN_TAPE, beads::Processor::MEDIA_DIRTY_TAPE };
        if (quality == media)
            return;
        media = quality;
        core->processor.Configure (kMedia[quality], false);
        bridge.prepare (hostRate, (double) core->processor.sample_rate(), kChunk);
        setLatencySamples (bridge.latencySamples());
    }

    std::unique_ptr<Core> core;
    CoreRate bridge;
    double hostRate = 48000.0;
    int media = -1;
    beads::Parameters params {};
    beads::ModulatedParameter density {};
    beads::Processor::MixParameters mix {};
    bool freeze = false;
    AudioBuffer<float> dry { 2, 8192 };
    std::vector<float> scratch = std::vector<float> (8192);
};

//==============================================================================
// Random Notes: a note generator. In time with the song (it plays while Lathe plays), it
// rolls dice for when notes happen and which notes they are, in a scale, and can loop what
// it rolled. Put it before an instrument on a MIDI track.
class RandomNotes final : public Device
{
public:
    static constexpr int kChunk = 8;

    RandomNotes() : Device ("randomnotes", "Random Notes", Kind::generator, layout())
    {
        core = std::make_unique<Core>();
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("rhythm", "Rhythm", { "Coin toss", "Clusters", "Drum patterns", "Independent", "Divider", "Three ways", "Markov" }, 0));
        l.add (choice ("clock", "Steps", { "1/4", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32" }, 3));
        l.add (choice ("play", "Play", { "Every step", "Random steps", "Both" }, 1));
        l.add (percent ("balance", "Balance", 0.5f));
        l.add (percent ("jitter", "Jitter", 0.0f));
        l.add (percent ("length", "Note length", 0.4f));
        l.add (percent ("lengthRandom", "Length spread", 0.0f));
        l.add (percent ("spread", "Spread", 0.5f));
        l.add (percent ("register", "Register", 0.5f));
        l.add (percent ("steps", "Steps", 0.7f));
        l.add (choice ("scale", "Scale", { "Major", "Minor", "Pentatonic", "Pelog", "Bhairav", "Shri" }, 0));
        l.add (choice ("range", "Range", { "Two octaves", "Five octaves" }, 0));
        l.add (number ("lowest", "Lowest note", { 24.0f, 96.0f, 1.0f }, 48.0f, [] (float v) { return noteName (v); }));
        l.add (toggle ("followKeys", "Follow keys", false));
        l.add (percent ("dejaVu", "Deja vu", 0.0f));
        l.add (choice ("loop", "Loop", { "1", "2", "3", "4", "5", "6", "8", "10", "12", "16" }, 7));
        l.add (percent ("dynamics", "Dynamics", 0.3f));
        l.add (number ("variation", "Variation", { 1.0f, 99.0f, 1.0f }, 1.0f, [] (float v) { return String (roundToInt (v)); }));
        return l;
    }

    String blurb() const override   { return "Plays random melodies and rhythms in time and in key, and can loop what it played."; }
    String modeParam() const override   { return "rhythm"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Rhythm", { { "clock", "Steps", "The grid it plays on, in time with the song." },
                          { "play", "Play", "A note on every step, notes on random steps (two lines, swapping), or both." },
                          { "balance", "Balance", "Random steps: which of the two lines gets more of them (or, for some rhythms, how busy it is)." },
                          { "jitter", "Jitter", "Pushes notes off the grid, from a little human to drunk." },
                          { "length", "Note length", "How long each note is held, as a share of a step." },
                          { "lengthRandom", "Length spread", "Each note a different length." } } },
            { "Notes", { { "spread", "Spread", "How far the notes jump: left stays near one pitch, right uses the whole range." },
                         { "register", "Register", "Low notes or high ones more often." },
                         { "steps", "Steps", "Left: any note of the scale, evenly. Right: mostly its strongest notes (root, fifth...)." },
                         { "scale", "Scale", "The notes it picks from." },
                         { "range", "Range", "How wide it can go, from the lowest note up." },
                         { "lowest", "Lowest note", "Where the range starts (and the key: the scale is built on this note)." },
                         { "followKeys", "Follow keys", "Play a key on this track and the scale moves to start on it." } } },
            { "Memory", { { "dejaVu", "Deja vu", "Left: always new. Middle: loops what it played. Right: shuffles the loop." },
                          { "loop", "Loop", "How many steps it loops." },
                          { "dynamics", "Dynamics", "How much loudness varies between notes." },
                          { "variation", "Variation", "Each number is a different sequence; the same one plays the same way each time." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int rhythm, int clock, int play, float balance, float length, float spread, float reg,
                     float steps, int scale, float lowest, float deja, int loop) -> Preset
        {
            return { name, { { "rhythm", (float) rhythm }, { "clock", (float) clock }, { "play", (float) play }, { "balance", balance },
                             { "jitter", 0.0f }, { "length", length }, { "lengthRandom", 0.0f }, { "spread", spread },
                             { "register", reg }, { "steps", steps }, { "scale", (float) scale }, { "range", 0.0f },
                             { "lowest", lowest }, { "dejaVu", deja }, { "loop", (float) loop } } };
        };
        return {
            p ("Wandering melody",   0, 3, 1, 0.5f,  0.45f, 0.5f,  0.5f,  0.7f,  0, 60.0f, 0.0f, 7),
            p ("Looping riff",       0, 3, 1, 0.55f, 0.35f, 0.45f, 0.45f, 0.75f, 1, 48.0f, 0.5f, 6),
            p ("Bassline",           3, 3, 0, 0.4f,  0.3f,  0.3f,  0.35f, 0.85f, 1, 36.0f, 0.5f, 9),
            p ("Sparse bells",       1, 2, 1, 0.35f, 0.6f,  0.7f,  0.6f,  0.6f,  2, 60.0f, 0.0f, 7),
            p ("Arpeggio",           4, 3, 0, 0.5f,  0.4f,  0.6f,  0.5f,  0.9f,  0, 60.0f, 0.5f, 3),
            p ("Hats and kicks",     2, 3, 2, 0.5f,  0.2f,  0.2f,  0.5f,  0.5f,  0, 36.0f, 0.5f, 9) };
    }

    void prepareToPlay (double rate, int) override
    {
        sampleRate = rate > 0.0 ? rate : 48000.0;
        restart();
        wasPlaying = false;
        for (auto& s : sounding)
            s = -1;
        std::fill (std::begin (count), std::end (count), 0);
    }

    void reset() override   { prepareToPlay (sampleRate, 0); }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        buffer.clear();
        const int n = buffer.getNumSamples();

        // The transport: where the song is, and whether it's playing.
        double ppq = 0.0, bpm = 120.0;
        bool playing = false;
        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
            {
                playing = pos->getIsPlaying() || pos->getIsRecording();
                ppq = pos->getPpqPosition().orFallback (0.0);
                bpm = pos->getBpm().orFallback (120.0);
            }

        // Keys played on the track: with Follow keys, the last one held sets where the scale starts.
        // Otherwise they pass through, so you can play along.
        MidiBuffer outMidi;
        const bool follow = (bool) geti ("followKeys");
        for (const auto m : midi)
        {
            const auto msg = m.getMessage();
            if (follow && msg.isNoteOn())
                heldKey = msg.getNoteNumber();
            else if (follow && msg.isNoteOff() && msg.getNoteNumber() == heldKey)
                heldKey = -1;
            else if (! follow || ! (msg.isNoteOnOrOff()))
                outMidi.addEvent (msg, m.samplePosition);
        }

        if (! playing)
        {
            if (wasPlaying)
                allOff (outMidi, 0);
            wasPlaying = false;
            midi.swapWith (outMidi);
            return;
        }
        if (! wasPlaying)
            restart();   // same Variation, same start: what you hear is what exports
        wasPlaying = true;

        configure();
        static constexpr double kStepsPerBeat[] { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0 };
        const double steps = kStepsPerBeat[jlimit (0, 5, geti ("clock"))];
        const double beatsPerSample = bpm / 60.0 / sampleRate;
        const int play = geti ("play");
        const bool everyStep = play != 1, randomSteps = play != 0;
        const int lowest = follow && heldKey >= 0 ? heldKey : roundToInt (get ("lowest"));
        const float dynamics = get ("dynamics");

        auto& c = *core;
        for (int done = 0; done < n; done += kChunk)
        {
            const int size = jmin (kChunk, n - done);
            stmlib::GateFlags clock[kChunk];
            for (int i = 0; i < size; ++i)
            {
                const double step = (ppq + (double) (done + i) * beatsPerSample) * steps;
                const bool high = step - std::floor (step) < 0.5;
                clock[i] = previousClock = stmlib::ExtractGateFlags (previousClock, high);
            }
            float ramp[kChunk * 4];
            marbles::Ramps ramps;
            ramps.master = &ramp[0];
            ramps.external = &ramp[kChunk];
            ramps.slave[0] = &ramp[kChunk * 2];
            ramps.slave[1] = &ramp[kChunk * 3];
            bool gates[kChunk * 2];
            float volts[kChunk * 4];
            bool resetT = resetPending, resetX = resetPending;
            resetPending = false;
            c.t.Process (true, &resetT, clock, ramps, gates, (size_t) size);
            c.xy.Process (marbles::CLOCK_SOURCE_INTERNAL_T1_T2_T3, x, y, &resetX, clock, ramps, volts, (size_t) size);

            for (int i = 0; i < size; ++i)
            {
                // Three lines: t1 and t3 are the random steps, t2 the steady clock; each has its own note.
                const bool gate[3] { gates[2 * i] && randomSteps, ramps.master[i] < 0.5f && everyStep, gates[2 * i + 1] && randomSteps };
                const float y01 = jlimit (0.0f, 1.0f, volts[4 * i + 3] / 2.0f);
                const int velocity = jlimit (1, 127, roundToInt (100.0f + (y01 - 0.5f) * 2.0f * 60.0f * dynamics));
                for (int k = 0; k < 3; ++k)
                {
                    if (gate[k] && ! last[k])
                        noteOn (outMidi, done + i, k, lowest + roundToInt (volts[4 * i + k] * 12.0f), velocity);
                    else if (! gate[k] && last[k])
                        noteOff (outMidi, done + i, k);
                    last[k] = gate[k];
                }
            }
        }
        midi.swapWith (outMidi);
    }

private:
    struct Core
    {
        marbles::RandomGenerator generator;
        marbles::RandomStream stream;
        marbles::TGenerator t;
        marbles::XYGenerator xy;
    };

    void restart()
    {
        auto& c = *core;
        c.generator.Init ((uint32_t) (roundToInt (get ("variation")) * 7919));
        c.stream.Init (&c.generator);
        c.t.Init (&c.stream, (float) sampleRate);
        c.xy.Init (&c.stream, (float) sampleRate);
        for (int i = 0; i < marbles::kNumPresetScales; ++i)
            c.xy.LoadScale (i, marbles::preset_scales[i]);
        previousClock = stmlib::GATE_FLAG_LOW;
        resetPending = true;
        for (auto& l : last)
            l = false;
    }

    void configure()
    {
        static constexpr int kLoops[] { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16 };
        auto& c = *core;
        const int loop = kLoops[jlimit (0, 9, geti ("loop"))];
        // Deja vu: Marbles' knob has a flat spot in the middle where the loop is locked.
        float deja = get ("dejaVu");
        deja = deja < 0.47f ? deja * 1.0638f : deja > 0.53f ? 0.5f + (deja - 0.53f) * 1.0638f : 0.5f;

        c.t.set_model ((marbles::TGeneratorModel) jlimit (0, 6, geti ("rhythm")));
        c.t.set_range (marbles::T_GENERATOR_RANGE_1X);
        c.t.set_rate (0.0f);   // one step per clock
        c.t.set_bias (get ("balance"));
        c.t.set_jitter (get ("jitter"));
        c.t.set_deja_vu (deja);
        c.t.set_length (loop);
        c.t.set_pulse_width_mean (get ("length"));
        c.t.set_pulse_width_std (get ("lengthRandom"));

        x.control_mode = marbles::CONTROL_MODE_IDENTICAL;
        x.voltage_range = geti ("range") == 1 ? marbles::VOLTAGE_RANGE_POSITIVE : marbles::VOLTAGE_RANGE_NARROW;
        x.register_mode = false;
        x.register_value = 0.0f;
        x.spread = get ("spread");
        x.bias = get ("register");
        x.steps = get ("steps");
        x.deja_vu = deja;
        x.scale_index = jlimit (0, marbles::kNumPresetScales - 1, geti ("scale"));
        x.length = loop;
        x.ratio.p = 1;
        x.ratio.q = 1;

        y = x;   // the fourth output, used for loudness: slow and smooth
        y.voltage_range = marbles::VOLTAGE_RANGE_NARROW;
        y.spread = 0.5f;
        y.bias = 0.5f;
        y.steps = 0.0f;
        y.deja_vu = 0.0f;
        y.length = 1;
    }

    void noteOn (MidiBuffer& out, int at, int line, int note, int velocity)
    {
        note = jlimit (0, 127, note);
        if (sounding[line] >= 0)
            noteOff (out, at, line);
        if (count[note] > 0)   // another line is holding it: restrike it
            out.addEvent (MidiMessage::noteOff (1, note), at);
        out.addEvent (MidiMessage::noteOn (1, note, (uint8) velocity), at);
        ++count[note];
        sounding[line] = note;
    }

    void noteOff (MidiBuffer& out, int at, int line)
    {
        const int note = sounding[line];
        if (note < 0)
            return;
        sounding[line] = -1;
        if (--count[note] <= 0)
        {
            count[note] = 0;
            out.addEvent (MidiMessage::noteOff (1, note), at);
        }
    }

    void allOff (MidiBuffer& out, int at)
    {
        for (int line = 0; line < 3; ++line)
            noteOff (out, at, line);
        for (auto& l : last)
            l = false;
    }

    std::unique_ptr<Core> core;
    marbles::GroupSettings x {}, y {};
    double sampleRate = 48000.0;
    stmlib::GateFlags previousClock = stmlib::GATE_FLAG_LOW;
    bool resetPending = true, wasPlaying = false;
    bool last[3] {};
    int sounding[3] { -1, -1, -1 };
    int count[128] {};
    int heldKey = -1;
};
} // namespace

std::unique_ptr<Device> makeMultiSynth()   { return std::make_unique<MultiSynth>(); }
std::unique_ptr<Device> makeResonator()    { return std::make_unique<Resonator>(); }
std::unique_ptr<Device> makeGrains()       { return std::make_unique<Grains>(); }
std::unique_ptr<Device> makeRandomNotes()  { return std::make_unique<RandomNotes>(); }
} // namespace lathe::devices
