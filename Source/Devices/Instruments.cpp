// Lathe's built-in instruments: a mono bass synth, a poly synth and a drum machine.
// Small and self-contained: band-limited oscillators, a 24 dB low-pass, ADSRs, and
// synthesized drum voices.
#include "Instruments.h"

namespace lathe::devices
{
namespace synth
{
//==============================================================================
float polyBlep (float t, float dt)
{
    if (t < dt)        { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

float Osc::next (Wave wave, float inc)
{
    const float t = phase;
    phase += inc;
    if (phase >= 1.0f)
        phase -= 1.0f;
    switch (wave)
    {
        case Wave::saw:      return 2.0f * t - 1.0f - polyBlep (t, inc);
        case Wave::square:
        {
            float y = t < 0.5f ? 1.0f : -1.0f;
            y += polyBlep (t, inc);
            float t2 = t + 0.5f;
            if (t2 >= 1.0f) t2 -= 1.0f;
            return y - polyBlep (t2, inc);
        }
        case Wave::triangle: return 4.0f * std::abs (t - 0.5f) - 1.0f;
        case Wave::sine:     return std::sin (MathConstants<float>::twoPi * t);
    }
    return 0.0f;
}

void Svf::set (float hz, float k, float sr)
{
    const float g = std::tan (MathConstants<float>::pi * jlimit (10.0f, sr * 0.45f, hz) / sr);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    damping = k;
}

float Svf::low (float x)
{
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return v2;
}

float Svf::band (float x)
{
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return v1;
}

float Svf::high (float x)
{
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return x - damping * v1 - v2;
}

void Ladder::set (float hz, float resonance, float sr)
{
    // Two state-variable stages: the first Butterworth, the second carries the resonance.
    a.set (hz, 1.41f, sr);
    b.set (hz, 2.0f - 1.94f * jlimit (0.0f, 1.0f, resonance), sr);
}

float Ladder::tick (float x)
{
    return b.low (a.low (x));
}

float noteHz (float note)   { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }

/** Splits a block at its MIDI events: render (from, to), then the event, in order. */
template <typename Render, typename Event>
void withMidi (int numSamples, const MidiBuffer& midi, Render&& render, Event&& event)
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
} // namespace synth

namespace
{
using namespace synth;

String cutoffText (float hz)  { return hzText (hz); }

//==============================================================================
// Bass Synth: one voice, last note wins, glide between held notes.
class BassSynth final : public Device
{
public:
    BassSynth() : Device ("bass", "Bass Synth", Kind::instrument, layout())
    {
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("wave", "Wave", { "Saw", "Square", "Triangle" }, 0));
        l.add (percent ("sub", "Sub", 0.4f));
        l.add (percent ("detune", "Detune", 0.0f));
        l.add (number ("glide", "Glide", { 0.0f, 500.0f, 0.0f, 0.5f }, 0.0f, [] (float v) { return v < 0.5f ? String ("Off") : msText (v); }));
        l.add (number ("cutoff", "Cutoff", { 40.0f, 12000.0f, 0.0f, 0.3f }, 600.0f, cutoffText));
        l.add (percent ("resonance", "Resonance", 0.2f));
        l.add (percent ("envAmount", "Env amount", 0.5f));
        l.add (number ("envDecay", "Env decay", { 10.0f, 2000.0f, 0.0f, 0.4f }, 250.0f, [] (float v) { return msText (v); }));
        l.add (percent ("drive", "Drive", 0.2f));
        l.add (number ("attack", "Attack", { 0.5f, 2000.0f, 0.0f, 0.35f }, 2.0f, [] (float v) { return msText (v); }));
        l.add (number ("decay", "Decay", { 5.0f, 3000.0f, 0.0f, 0.4f }, 300.0f, [] (float v) { return msText (v); }));
        l.add (percent ("sustain", "Sustain", 0.8f));
        l.add (number ("release", "Release", { 5.0f, 3000.0f, 0.0f, 0.4f }, 80.0f, [] (float v) { return msText (v); }));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -6.0f));
        return l;
    }

    String blurb() const override   { return "A mono synth for basslines: saw, square or triangle, a sub, and glide."; }
    String modeParam() const override   { return "wave"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Oscillator", { { "sub", "Sub", "An octave below, for weight." },
                              { "detune", "Detune", "A second oscillator, slightly out of tune, for a thicker sound." },
                              { "glide", "Glide", "Slides between notes you play joined up." } } },
            { "Filter", { { "cutoff", "Cutoff", "How bright: the low-pass filter's frequency." },
                          { "resonance", "Resonance", "A peak at the cutoff; high values squelch." },
                          { "envAmount", "Env amount", "How far each note opens the filter." },
                          { "envDecay", "Env decay", "How fast the filter closes again." },
                          { "drive", "Drive", "Pushes the filter for grit." } } },
            { "Amp", { { "attack", "Attack", "How fast a note starts." },
                       { "decay", "Decay", "How fast it falls to the sustain level." },
                       { "sustain", "Sustain", "The level while the key is held." },
                       { "release", "Release", "How long it rings after you let go." } } },
            { "Output", { { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int wave, float subLevel, float detune, float glide, float cutoff, float res, float env,
                     float envDecay, float drive, float a, float d, float s, float r, float level) -> Preset
        {
            return { name, { { "wave", (float) wave }, { "sub", subLevel }, { "detune", detune }, { "glide", glide }, { "cutoff", cutoff },
                             { "resonance", res }, { "envAmount", env }, { "envDecay", envDecay }, { "drive", drive },
                             { "attack", a }, { "decay", d }, { "sustain", s }, { "release", r }, { "level", level } } };
        };
        return {
            p ("Round bass",   0, 0.4f, 0.0f,  0.0f,  600.0f,  0.2f,  0.5f,  250.0f, 0.2f, 2.0f, 300.0f,  0.8f, 80.0f,  -6.0f),
            p ("Acid",         0, 0.0f, 0.0f,  60.0f, 300.0f,  0.85f, 0.8f,  180.0f, 0.5f, 1.0f, 200.0f,  0.6f, 40.0f,  -8.0f),
            p ("Square sub",   1, 0.7f, 0.0f,  0.0f,  900.0f,  0.1f,  0.2f,  300.0f, 0.1f, 2.0f, 500.0f,  0.9f, 60.0f,  -7.0f),
            p ("Pluck",        0, 0.3f, 0.0f,  0.0f,  250.0f,  0.35f, 0.75f, 120.0f, 0.2f, 1.0f, 250.0f,  0.0f, 120.0f, -5.0f),
            p ("Reese",        0, 0.3f, 0.8f,  0.0f,  1400.0f, 0.15f, 0.1f,  400.0f, 0.3f, 5.0f, 800.0f,  1.0f, 150.0f, -9.0f),
            p ("Soft sub",     2, 0.6f, 0.0f,  0.0f,  2000.0f, 0.0f,  0.0f,  300.0f, 0.0f, 5.0f, 400.0f,  1.0f, 120.0f, -4.0f),
            p ("Funk",         1, 0.2f, 0.0f,  0.0f,  400.0f,  0.4f,  0.7f,  140.0f, 0.3f, 1.0f, 180.0f,  0.5f, 50.0f,  -6.0f),
            p ("Slide",        0, 0.4f, 0.2f,  150.0f, 700.0f, 0.3f,  0.4f,  300.0f, 0.25f, 3.0f, 400.0f, 0.9f, 100.0f, -7.0f),
            p ("Growl",        1, 0.5f, 0.4f,  0.0f,  350.0f,  0.55f, 0.6f,  500.0f, 0.8f, 2.0f, 600.0f,  0.8f, 80.0f,  -10.0f),
            p ("Synth bass",   0, 0.2f, 0.3f,  0.0f,  1100.0f, 0.25f, 0.45f, 220.0f, 0.15f, 1.0f, 350.0f, 0.7f, 70.0f,  -7.0f) };
    }

    void prepareToPlay (double rate, int) override
    {
        sr = (float) rate;
        reset();
    }

    void reset() override
    {
        held.clear();
        amp.reset();
        filt.reset();
        amp.setSampleRate (sr);
        filt.setSampleRate (sr);
        o1 = o2 = sub = {};
        ladder = {};
        currentNote = targetNote = 45.0f;
        bend = 0.0f;
    }

    double getTailLengthSeconds() const override   { return get ("release") * 0.001; }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0)
            return;

        const auto wave = (Wave) jlimit (0, 2, geti ("wave"));
        const float subLevel = get ("sub"), detune = get ("detune"), drive = get ("drive");
        const float cutoff = get ("cutoff"), res = get ("resonance"), envAmt = get ("envAmount");
        const float glideMs = get ("glide");
        const float glideCoef = glideMs < 0.5f ? 0.0f : std::exp (-1.0f / (glideMs * 0.001f * sr));
        const float level = Decibels::decibelsToGain (get ("level"));
        amp.setParameters ({ get ("attack") * 0.001f, get ("decay") * 0.001f, get ("sustain"), get ("release") * 0.001f });
        filt.setParameters ({ 0.001f, get ("envDecay") * 0.001f, 0.0f, get ("envDecay") * 0.001f });

        float* l = buffer.getWritePointer (0);
        withMidi (n, midi, [&] (int from, int to)
        {
            for (int i = from; i < to; ++i)
            {
                currentNote = glideCoef > 0.0f ? targetNote + (currentNote - targetNote) * glideCoef : targetNote;
                if (((i - from) & 15) == 0)
                {
                    // Filter: cutoff, plus the envelope (up to 5 octaves), plus a little key tracking.
                    const float e = filtLevel;
                    const float hz = cutoff * std::pow (2.0f, envAmt * 5.0f * e + (currentNote - 45.0f) / 24.0f);
                    ladder.set (hz, res, sr);
                }
                const float hz = noteHz (currentNote + bend);
                const float inc = hz / sr;
                float x = o1.next (wave, inc);
                if (detune > 0.001f)
                    x = 0.6f * (x + o2.next (wave, inc * std::pow (2.0f, detune * 0.3f / 12.0f)));
                x += subLevel * sub.next (Wave::square, inc * 0.5f) * 0.8f;
                x = std::tanh (x * (1.0f + 3.0f * drive)) / (1.0f + drive);
                filtLevel = filt.getNextSample();
                const float y = ladder.tick (x) * amp.getNextSample();
                l[i] = y * level;
            }
        },
        [&] (const MidiMessage& m)
        {
            if (m.isNoteOn())
            {
                const bool legato = ! held.isEmpty();
                held.removeAllInstancesOf (m.getNoteNumber());
                held.add (m.getNoteNumber());
                targetNote = (float) m.getNoteNumber();
                if (! legato || glideCoef <= 0.0f)
                {
                    if (! legato)
                        currentNote = targetNote;
                    amp.noteOn();
                    filt.noteOn();
                }
            }
            else if (m.isNoteOff())
            {
                held.removeAllInstancesOf (m.getNoteNumber());
                if (held.isEmpty())
                {
                    amp.noteOff();
                    filt.noteOff();
                }
                else
                    targetNote = (float) held.getLast();
            }
            else if (m.isPitchWheel())
                bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                held.clear();
                amp.reset();
                filt.reset();
            }
        });

        for (int c = 1; c < buffer.getNumChannels(); ++c)
            buffer.copyFrom (c, 0, buffer, 0, 0, n);
    }

private:
    float sr = 48000.0f, currentNote = 45.0f, targetNote = 45.0f, bend = 0.0f, filtLevel = 0.0f;
    Array<int> held;
    ADSR amp, filt;
    Osc o1, o2, sub;
    Ladder ladder;
};

//==============================================================================
// Poly Synth: eight voices, two oscillators each, a filter, vibrato and chorus.
class PolySynth final : public Device
{
public:
    static constexpr int kVoices = 8;

    PolySynth() : Device ("poly", "Poly Synth", Kind::instrument, layout())
    {
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("wave", "Wave", { "Saw", "Square", "Triangle", "Sine" }, 0));
        l.add (choice ("second", "Second osc", { "Unison", "Octave up", "Fifth up", "Octave down" }, 0));
        l.add (percent ("detune", "Detune", 0.3f));
        l.add (number ("cutoff", "Cutoff", { 60.0f, 18000.0f, 0.0f, 0.3f }, 2500.0f, cutoffText));
        l.add (percent ("resonance", "Resonance", 0.15f));
        l.add (percent ("envAmount", "Env amount", 0.3f));
        l.add (number ("attack", "Attack", { 0.5f, 5000.0f, 0.0f, 0.35f }, 10.0f, [] (float v) { return msText (v); }));
        l.add (number ("decay", "Decay", { 5.0f, 5000.0f, 0.0f, 0.4f }, 600.0f, [] (float v) { return msText (v); }));
        l.add (percent ("sustain", "Sustain", 0.7f));
        l.add (number ("release", "Release", { 5.0f, 8000.0f, 0.0f, 0.4f }, 400.0f, [] (float v) { return msText (v); }));
        l.add (percent ("vibrato", "Vibrato", 0.0f));
        l.add (percent ("chorus", "Chorus", 0.3f));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -8.0f));
        return l;
    }

    String blurb() const override   { return "An eight-voice synth for pads, keys, brass and plucks."; }
    String modeParam() const override   { return "wave"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Oscillators", { { "second", "Second osc", "What the second oscillator plays against the first." },
                               { "detune", "Detune", "How far apart the two are tuned: wider is lusher." } } },
            { "Filter", { { "cutoff", "Cutoff", "How bright." },
                          { "resonance", "Resonance", "A peak at the cutoff." },
                          { "envAmount", "Env amount", "How much each note opens the filter, following the envelope." } } },
            { "Envelope", { { "attack", "Attack", "How fast a note fades in." },
                            { "decay", "Decay", "How fast it falls to the sustain level." },
                            { "sustain", "Sustain", "The level while the key is held." },
                            { "release", "Release", "How long it rings after you let go." } } },
            { "Output", { { "vibrato", "Vibrato", "Pitch wobble. The mod wheel adds more." },
                          { "chorus", "Chorus", "Widens and thickens the sound." },
                          { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int wave, int second, float detune, float cutoff, float res, float env,
                     float a, float d, float s, float r, float vib, float chorus, float level) -> Preset
        {
            return { name, { { "wave", (float) wave }, { "second", (float) second }, { "detune", detune }, { "cutoff", cutoff },
                             { "resonance", res }, { "envAmount", env }, { "attack", a }, { "decay", d }, { "sustain", s },
                             { "release", r }, { "vibrato", vib }, { "chorus", chorus }, { "level", level } } };
        };
        return {
            p ("Warm pad",     0, 0, 0.35f, 1800.0f,  0.1f,  0.2f,  600.0f, 1500.0f, 0.8f,  1800.0f, 0.0f,  0.5f, -10.0f),
            p ("Strings",      0, 0, 0.5f,  4500.0f,  0.05f, 0.1f,  250.0f, 800.0f,  0.85f, 900.0f,  0.15f, 0.6f, -10.0f),
            p ("Brass",        0, 0, 0.2f,  900.0f,  0.2f,  0.6f,  40.0f,  500.0f,  0.7f,  250.0f,  0.1f,  0.2f, -9.0f),
            p ("Pluck",        0, 1, 0.15f, 700.0f,  0.25f, 0.7f,  1.0f,   350.0f,  0.0f,  400.0f,  0.0f,  0.3f, -8.0f),
            p ("Keys",         2, 1, 0.1f,  3000.0f, 0.1f,  0.3f,  2.0f,   1200.0f, 0.3f,  500.0f,  0.0f,  0.3f, -7.0f),
            p ("Glass",        3, 1, 0.05f, 9000.0f, 0.0f,  0.0f,  5.0f,   2000.0f, 0.2f,  1500.0f, 0.0f,  0.4f, -6.0f),
            p ("Square lead",  1, 2, 0.1f,  2500.0f, 0.3f,  0.3f,  5.0f,   400.0f,  0.8f,  200.0f,  0.25f, 0.1f, -10.0f),
            p ("Soft organ",   3, 1, 0.0f,  6000.0f, 0.0f,  0.0f,  8.0f,   100.0f,  1.0f,  120.0f,  0.1f,  0.5f, -8.0f),
            p ("Dark pad",     1, 3, 0.4f,  600.0f,  0.2f,  0.15f, 1200.0f, 2000.0f, 0.9f, 2500.0f, 0.0f,  0.6f, -9.0f),
            p ("Stabs",        0, 2, 0.25f, 1200.0f, 0.3f,  0.65f, 1.0f,   180.0f,  0.0f,  150.0f,  0.0f,  0.25f, -8.0f) };
    }

    void prepareToPlay (double rate, int block) override
    {
        sr = (float) rate;
        chorusL.assign ((size_t) (sr * 0.05f), 0.0f);
        chorusR.assign ((size_t) (sr * 0.05f), 0.0f);
        ignoreUnused (block);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices)
        {
            v = {};
            v.amp.setSampleRate (sr);
            v.filt.setSampleRate (sr);
        }
        std::fill (chorusL.begin(), chorusL.end(), 0.0f);
        std::fill (chorusR.begin(), chorusR.end(), 0.0f);
        chorusPos = 0;
        lfoPhase = chorusPhase = 0.0f;
        bend = mod = 0.0f;
        age = 0;
    }

    double getTailLengthSeconds() const override   { return get ("release") * 0.001 + 0.05; }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0)
            return;

        const auto wave = (Wave) jlimit (0, 3, geti ("wave"));
        static constexpr float kSecond[] { 0.0f, 12.0f, 7.0f, -12.0f };
        const float second = kSecond[jlimit (0, 3, geti ("second"))];
        const float detuneSemis = get ("detune") * 0.25f;
        const float cutoff = get ("cutoff"), res = get ("resonance"), envAmt = get ("envAmount");
        const ADSR::Parameters env { get ("attack") * 0.001f, get ("decay") * 0.001f, get ("sustain"), get ("release") * 0.001f };
        const float vibrato = get ("vibrato"), chorusMix = get ("chorus");
        const float level = Decibels::decibelsToGain (get ("level")) * 0.35f;
        for (auto& v : voices)
        {
            v.amp.setParameters (env);
            v.filt.setParameters (env);
        }

        float* outL = buffer.getWritePointer (0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        withMidi (n, midi, [&] (int from, int to)
        {
            for (int i = from; i < to; ++i)
            {
                lfoPhase += 5.2f / sr;
                if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
                const float vib = std::sin (MathConstants<float>::twoPi * lfoPhase) * (vibrato + mod) * 0.3f;
                float l = 0.0f, r = 0.0f;
                for (auto& v : voices)
                {
                    if (! v.amp.isActive())
                        continue;
                    if (((i - from) & 15) == 0)
                    {
                        const float hz = cutoff * std::pow (2.0f, envAmt * 4.0f * v.filtLevel + (v.note - 60.0f) / 24.0f);
                        v.ladder.set (hz, res, sr);
                    }
                    const float base = v.note + bend + vib;
                    const float a = v.o1.next (wave, noteHz (base - detuneSemis) / sr);
                    const float b = v.o2.next (wave, noteHz (base + second + detuneSemis) / sr);
                    v.filtLevel = v.filt.getNextSample();
                    const float g = v.amp.getNextSample() * v.velocity;
                    const float xl = v.ladder.tick (0.6f * a + 0.4f * b) * g;
                    // Each voice sits a little to one side, alternately.
                    l += xl * (1.0f - 0.2f * v.pan);
                    r += xl * (1.0f + 0.2f * v.pan);
                }
                scratchL = l * level;
                scratchR = r * level;

                // Chorus: two delay lines swept in opposite directions.
                const int size = (int) chorusL.size();
                chorusL[(size_t) chorusPos] = scratchL;
                chorusR[(size_t) chorusPos] = scratchR;
                chorusPhase += 0.45f / sr;
                if (chorusPhase >= 1.0f) chorusPhase -= 1.0f;
                const float sweep = std::sin (MathConstants<float>::twoPi * chorusPhase);
                auto tap = [&] (const std::vector<float>& line, float ms)
                {
                    float d = ms * 0.001f * sr;
                    float pos = (float) chorusPos - d;
                    while (pos < 0.0f) pos += (float) size;
                    const int i0 = (int) pos;
                    const float f = pos - (float) i0;
                    return line[(size_t) i0] * (1.0f - f) + line[(size_t) ((i0 + 1) % size)] * f;
                };
                const float wl = tap (chorusR, 12.0f + 4.0f * sweep);
                const float wr = tap (chorusL, 12.0f - 4.0f * sweep);
                chorusPos = (chorusPos + 1) % size;
                outL[i] = scratchL * (1.0f - 0.5f * chorusMix) + wl * chorusMix;
                if (outR != nullptr)
                    outR[i] = scratchR * (1.0f - 0.5f * chorusMix) + wr * chorusMix;
            }
        },
        [&] (const MidiMessage& m)
        {
            if (m.isNoteOn())
                start (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
            {
                for (auto& v : voices)
                    if (v.held && v.noteNumber == m.getNoteNumber())
                    {
                        v.held = false;
                        v.amp.noteOff();
                        v.filt.noteOff();
                    }
            }
            else if (m.isPitchWheel())
                bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isController() && m.getControllerNumber() == 1)
                mod = (float) m.getControllerValue() / 127.0f;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                for (auto& v : voices)
                {
                    v.held = false;
                    v.amp.reset();
                    v.filt.reset();
                }
        });
    }

private:
    struct Voice
    {
        Osc o1, o2;
        Ladder ladder;
        ADSR amp, filt;
        float note = 60.0f, velocity = 1.0f, filtLevel = 0.0f, pan = 0.0f;
        int noteNumber = -1;
        bool held = false;
        uint32 started = 0;
    };

    void start (int note, float velocity)
    {
        // A free voice, else the one released longest ago, else the oldest.
        Voice* pick = nullptr;
        for (auto& v : voices)
            if (! v.amp.isActive()) { pick = &v; break; }
        if (pick == nullptr)
            for (auto& v : voices)
                if (! v.held && (pick == nullptr || v.started < pick->started))
                    pick = &v;
        if (pick == nullptr)
            for (auto& v : voices)
                if (pick == nullptr || v.started < pick->started)
                    pick = &v;

        auto& v = *pick;
        v.note = (float) note;
        v.noteNumber = note;
        v.velocity = 0.35f + 0.65f * velocity;
        v.held = true;
        v.started = ++age;
        v.pan = (age & 1) ? 1.0f : -1.0f;
        if (! v.amp.isActive())
        {
            v.o1.phase = 0.0f;
            v.o2.phase = 0.37f;
            v.ladder = {};
        }
        v.amp.noteOn();
        v.filt.noteOn();
    }

    float sr = 48000.0f, bend = 0.0f, mod = 0.0f, lfoPhase = 0.0f, chorusPhase = 0.0f, scratchL = 0.0f, scratchR = 0.0f;
    std::array<Voice, kVoices> voices;
    std::vector<float> chorusL, chorusR;
    int chorusPos = 0;
    uint32 age = 0;
};

//==============================================================================
// Drums: synthesized kits in the style of the classic drum machines, on the
// General MIDI drum notes.
struct KitSpec
{
    // Kick: pitch falls from start to end; click is the beater.
    float kickStart, kickEnd, kickPitchMs, kickDecayMs, kickClick, kickDrive;
    // Snare: two tuned bodies and a burst of noise.
    float snareLow, snareHigh, snareToneMs, snareNoiseMs, snareNoiseHz, snareNoise;
    float clapTailMs;
    // Hats and cymbals: metallic partials and noise.
    float hatMetal, hatClosedMs, hatOpenMs, hatHz;
    float tomLow, tomMid, tomHigh, tomDecayMs, tomBend;
    float cymbalMs;
};

const KitSpec& kitSpec (int kit)
{
    static const KitSpec kits[] {
        // Simple 909: punchy kick with a click, bright noisy hats.
        { 190.0f, 52.0f, 45.0f, 420.0f, 0.65f, 0.35f,   190.0f, 330.0f, 90.0f, 190.0f, 1500.0f, 0.6f,   260.0f,
          0.35f, 55.0f, 380.0f, 8500.0f,   95.0f, 135.0f, 185.0f, 320.0f, 0.6f,   1400.0f },
        // Simple 808: long, round kick; metallic hats; soft toms.
        { 115.0f, 47.0f, 70.0f, 1100.0f, 0.12f, 0.08f,   238.0f, 476.0f, 110.0f, 130.0f, 1800.0f, 0.38f,   420.0f,
          1.0f, 42.0f, 420.0f, 7500.0f,   80.0f, 120.0f, 165.0f, 650.0f, 0.25f,   1800.0f },
        // Simple 606: small and tight, a thin kick and sizzling hats.
        { 165.0f, 62.0f, 25.0f, 220.0f, 0.3f, 0.12f,   220.0f, 400.0f, 60.0f, 140.0f, 2600.0f, 0.75f,   200.0f,
          0.8f, 32.0f, 260.0f, 9500.0f,   115.0f, 155.0f, 205.0f, 240.0f, 0.4f,   1000.0f },
        // Simple 707: crisp and bright, a snappy snare and short hats.
        { 205.0f, 56.0f, 35.0f, 320.0f, 0.85f, 0.25f,   200.0f, 340.0f, 70.0f, 210.0f, 1200.0f, 0.78f,   230.0f,
          0.2f, 38.0f, 300.0f, 10000.0f,   100.0f, 142.0f, 192.0f, 360.0f, 0.5f,   1200.0f } };
    return kits[jlimit (0, 3, kit)];
}

enum class Drum { kick, rim, snare, clap, closedHat, openHat, tomLow, tomMid, tomHigh, crash, ride, cowbell, count };

std::optional<Drum> drumForNote (int note)
{
    switch (note)
    {
        case 35: case 36:           return Drum::kick;
        case 37:                    return Drum::rim;
        case 38: case 40:           return Drum::snare;
        case 39:                    return Drum::clap;
        case 42: case 44:           return Drum::closedHat;
        case 46:                    return Drum::openHat;
        case 41: case 43:           return Drum::tomLow;
        case 45: case 47:           return Drum::tomMid;
        case 48: case 50:           return Drum::tomHigh;
        case 49: case 52: case 55: case 57: return Drum::crash;
        case 51: case 53: case 59:  return Drum::ride;
        case 56:                    return Drum::cowbell;
        default:                    return std::nullopt;
    }
}

struct Tweaks
{
    float kickTune = 0.0f, kickDecay = 1.0f, snareTune = 0.0f, snap = 0.5f, hatDecay = 1.0f, hatTone = 0.5f, tomTune = 0.0f;
};

class DrumVoice
{
public:
    void prepare (float rate, Drum d, uint32 seed)
    {
        sr = rate;
        drum = d;
        rng = seed;
        active = false;
    }

    void trigger (float velocity, const KitSpec& k, const Tweaks& t)
    {
        active = true;
        chokeTime = -1.0f;
        vel = velocity;
        time = 0.0f;
        phases.fill (0.0f);
        bp1 = bp2 = hp = Svf();
        const float kick = std::pow (2.0f, t.kickTune / 12.0f), snareT = std::pow (2.0f, t.snareTune / 12.0f);
        const float tom = std::pow (2.0f, t.tomTune / 12.0f);
        switch (drum)
        {
            case Drum::kick:
                f0 = k.kickStart * kick; f1 = k.kickEnd * kick; pitchTau = k.kickPitchMs * 0.001f;
                decay = k.kickDecayMs * 0.001f * t.kickDecay; click = k.kickClick; drive = k.kickDrive;
                break;
            case Drum::snare:
                f0 = k.snareLow * snareT; f1 = k.snareHigh * snareT; pitchTau = k.snareToneMs * 0.001f;
                decay = k.snareNoiseMs * 0.001f * (0.7f + 0.6f * t.snap); click = k.snareNoise * (0.4f + 1.2f * t.snap);
                hp.set (k.snareNoiseHz, 1.0f, sr);
                bp1.set (k.snareNoiseHz * 2.6f, 0.9f, sr);
                break;
            case Drum::clap:
                decay = k.clapTailMs * 0.001f;
                bp1.set (1150.0f, 0.55f, sr);
                break;
            case Drum::closedHat: case Drum::openHat: case Drum::crash: case Drum::ride:
            {
                metal = drum == Drum::crash ? 0.5f : drum == Drum::ride ? 0.8f : k.hatMetal;
                const float base = drum == Drum::closedHat ? k.hatClosedMs : drum == Drum::openHat ? k.hatOpenMs
                                 : drum == Drum::ride ? k.cymbalMs * 1.6f : k.cymbalMs;
                decay = base * 0.001f * (drum == Drum::crash || drum == Drum::ride ? 1.0f : t.hatDecay);
                const float bright = std::pow (2.0f, (t.hatTone - 0.5f) * 1.5f);
                const float hz = (drum == Drum::crash ? 5200.0f : drum == Drum::ride ? 6500.0f : k.hatHz) * bright;
                hp.set (hz * 0.7f, 1.2f, sr);
                bp1.set (hz, 0.8f, sr);
                break;
            }
            case Drum::tomLow: case Drum::tomMid: case Drum::tomHigh:
                f1 = (drum == Drum::tomLow ? k.tomLow : drum == Drum::tomMid ? k.tomMid : k.tomHigh) * tom;
                f0 = f1 * (1.0f + k.tomBend);
                pitchTau = 0.06f;
                decay = k.tomDecayMs * 0.001f;
                break;
            case Drum::rim:
                decay = 0.022f;
                bp1.set (1700.0f, 0.15f, sr);
                bp2.set (500.0f, 0.2f, sr);
                break;
            case Drum::cowbell:
                decay = 0.28f;
                bp1.set (2600.0f, 0.7f, sr);
                break;
            case Drum::count: break;
        }
    }

    void choke()   { if (active) chokeTime = time; }

    // Adds into l and r.
    void render (float* l, float* r, int n, float panPos)
    {
        if (! active)
            return;
        const float dt = 1.0f / sr;
        const float pl = std::sqrt (0.5f * (1.0f - panPos)), pr = std::sqrt (0.5f * (1.0f + panPos));
        for (int i = 0; i < n; ++i)
        {
            float y = 0.0f;
            const float t = time;
            switch (drum)
            {
                case Drum::kick:
                {
                    const float hz = f1 + (f0 - f1) * std::exp (-t / pitchTau);
                    phases[0] += hz * dt;
                    const float body = std::sin (MathConstants<float>::twoPi * phases[0]) * std::exp (-t / (decay * 0.45f));
                    const float beater = click * noise() * std::exp (-t / 0.0025f);
                    y = std::tanh ((body + beater) * (1.0f + 3.0f * drive)) / (1.0f + 1.2f * drive);
                    break;
                }
                case Drum::snare:
                {
                    phases[0] += f0 * dt;
                    phases[1] += f1 * dt;
                    const float tone = (std::sin (MathConstants<float>::twoPi * phases[0]) + 0.6f * std::sin (MathConstants<float>::twoPi * phases[1]))
                                     * std::exp (-t / (pitchTau * 0.5f)) * 0.6f;
                    const float nz = noise();
                    const float rattle = (hp.high (nz) * 0.7f + bp1.band (nz) * 0.6f) * std::exp (-t / (decay * 0.45f)) * click;
                    y = tone + rattle;
                    break;
                }
                case Drum::clap:
                {
                    // Four quick bursts, then the tail.
                    float env = 0.0f;
                    for (int b = 0; b < 4; ++b)
                    {
                        const float tb = t - (float) b * 0.0095f;
                        if (tb >= 0.0f)
                            env = jmax (env, (b == 3 ? 1.0f : 0.8f) * std::exp (-tb / (b == 3 ? decay * 0.4f : 0.0035f)));
                    }
                    y = bp1.band (noise()) * env * 2.2f;
                    break;
                }
                case Drum::closedHat: case Drum::openHat: case Drum::crash: case Drum::ride:
                {
                    // Six square partials at inharmonic ratios, like the analog machines' metal.
                    static constexpr float kPartials[] { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
                    float m = 0.0f;
                    for (int p = 0; p < 6; ++p)
                    {
                        phases[(size_t) p] += kPartials[p] * (drum == Drum::ride ? 1.5f : 1.0f) * dt;
                        if (phases[(size_t) p] >= 1.0f) phases[(size_t) p] -= 1.0f;
                        m += phases[(size_t) p] < 0.5f ? 1.0f : -1.0f;
                    }
                    const float src = metal * m * 0.17f + (1.0f - metal) * noise();
                    float env = std::exp (-t / (decay * 0.45f));
                    if (chokeTime >= 0.0f)
                        env *= std::exp (-(t - chokeTime) / 0.008f);
                    y = bp1.band (hp.high (src)) * env * 1.6f;
                    break;
                }
                case Drum::tomLow: case Drum::tomMid: case Drum::tomHigh:
                {
                    const float hz = f1 + (f0 - f1) * std::exp (-t / pitchTau);
                    phases[0] += hz * dt;
                    y = std::sin (MathConstants<float>::twoPi * phases[0]) * std::exp (-t / (decay * 0.45f)) * 0.9f
                      + noise() * 0.15f * std::exp (-t / 0.006f);
                    break;
                }
                case Drum::rim:
                {
                    const float kick = t < 0.0005f ? 1.0f : 0.0f;
                    y = (bp1.band (kick * 8.0f) * 1.2f + bp2.band (kick * 6.0f)) * std::exp (-t / decay);
                    break;
                }
                case Drum::cowbell:
                {
                    phases[0] += 540.0f * dt;
                    phases[1] += 800.0f * dt;
                    for (int p = 0; p < 2; ++p)
                        if (phases[(size_t) p] >= 1.0f) phases[(size_t) p] -= 1.0f;
                    const float sq = (phases[0] < 0.5f ? 1.0f : -1.0f) + (phases[1] < 0.5f ? 1.0f : -1.0f);
                    const float env = 0.7f * std::exp (-t / 0.012f) + 0.3f * std::exp (-t / decay);
                    y = bp1.band (sq * 0.5f) * env * 1.8f;
                    break;
                }
                case Drum::count: break;
            }
            y *= vel;
            l[i] += y * pl;
            r[i] += y * pr;
            time += dt;
        }
        const float limit = decay * 8.0f + 0.05f;
        if (time > limit || (chokeTime >= 0.0f && time - chokeTime > 0.08f))
        {
            active = false;
            chokeTime = -1.0f;
        }
    }

    bool active = false;

private:
    float noise()
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) (rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Drum drum = Drum::kick;
    float sr = 48000.0f, time = 0.0f, vel = 1.0f, chokeTime = -1.0f;
    float f0 = 100.0f, f1 = 50.0f, pitchTau = 0.05f, decay = 0.3f, click = 0.0f, drive = 0.0f, metal = 0.5f;
    std::array<float, 6> phases {};
    Svf bp1, bp2, hp;
    uint32 rng = 1;
};

class Drums final : public Device
{
public:
    Drums() : Device ("drums", "Drums", Kind::instrument, layout())
    {
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        auto semis = [] (const char* id, const char* name)
        {
            return number (id, name, { -12.0f, 12.0f }, 0.0f, [] (float v)
            {
                return std::abs (v) < 0.05f ? String ("0") : (v > 0 ? "+" : "") + String (v, 1);
            });
        };
        auto times = [] (const char* id, const char* name)
        {
            return number (id, name, { 0.25f, 2.0f, 0.0f, 0.5f }, 1.0f, [] (float v) { return String (roundToInt (v * 100.0f)) + "%"; });
        };
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("kit", "Kit", { "Simple 909", "Simple 808", "Simple 606", "Simple 707" }, 0));
        l.add (semis ("kickTune", "Kick tune"));
        l.add (times ("kickDecay", "Kick decay"));
        l.add (semis ("snareTune", "Snare tune"));
        l.add (percent ("snap", "Snap", 0.5f));
        l.add (times ("hatDecay", "Hat decay"));
        l.add (percent ("hatTone", "Hat tone", 0.5f));
        l.add (semis ("tomTune", "Tom tune"));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -4.0f));
        return l;
    }

    String blurb() const override   { return "Synthesized drum kits in the style of the classic machines, on the usual drum notes."; }
    String modeParam() const override   { return "kit"; }

    void modeChosen (int) override
    {
        // A new kit starts as it was designed.
        for (auto* id : { "kickTune", "snareTune", "tomTune" }) set (id, 0.0f);
        for (auto* id : { "kickDecay", "hatDecay" }) set (id, 1.0f);
        set ("snap", 0.5f);
        set ("hatTone", 0.5f);
    }

    std::vector<Section> sections() const override
    {
        return {
            { "Kick", { { "kickTune", "Tune", "Kick pitch, in semitones." },
                        { "kickDecay", "Decay", "How long the kick booms." } } },
            { "Snare", { { "snareTune", "Tune", "Snare pitch, in semitones." },
                         { "snap", "Snap", "How much of the rattle, and how long." } } },
            { "Hats", { { "hatDecay", "Decay", "How long the hats ring." },
                        { "hatTone", "Tone", "Darker or brighter hats." } } },
            { "Toms", { { "tomTune", "Tune", "Tom pitch, in semitones." } } },
            { "Output", { { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int kit, float kt, float kd, float st, float snap, float hd, float ht, float tt) -> Preset
        {
            return { name, { { "kit", (float) kit }, { "kickTune", kt }, { "kickDecay", kd }, { "snareTune", st }, { "snap", snap },
                             { "hatDecay", hd }, { "hatTone", ht }, { "tomTune", tt }, { "level", -4.0f } } };
        };
        return {
            p ("Simple 909",       0,  0.0f, 1.0f,  0.0f, 0.5f, 1.0f, 0.5f, 0.0f),
            p ("Simple 808",       1,  0.0f, 1.0f,  0.0f, 0.5f, 1.0f, 0.5f, 0.0f),
            p ("Simple 606",       2,  0.0f, 1.0f,  0.0f, 0.5f, 1.0f, 0.5f, 0.0f),
            p ("Simple 707",       3,  0.0f, 1.0f,  0.0f, 0.5f, 1.0f, 0.5f, 0.0f),
            p ("909 hard",         0,  2.0f, 0.7f,  1.5f, 0.8f, 0.8f, 0.65f, 2.0f),
            p ("808 long boom",    1, -2.0f, 1.8f,  0.0f, 0.4f, 1.2f, 0.4f, -2.0f),
            p ("606 tight",        2,  1.0f, 0.7f,  2.0f, 0.6f, 0.6f, 0.7f, 0.0f),
            p ("707 bright",       3,  0.0f, 0.9f,  1.0f, 0.7f, 0.9f, 0.75f, 1.0f) };
    }

    String statusLine() const override
    {
        return "Kick C1 (36), snare D1 (38), clap 39, closed hat 42, open hat 46, toms 41 45 48, crash 49, ride 51, rim 37, cowbell 56.";
    }

    void prepareToPlay (double rate, int) override
    {
        sr = (float) rate;
        for (int d = 0; d < (int) Drum::count; ++d)
            voices[(size_t) d].prepare (sr, (Drum) d, 12345u + 7919u * (uint32) d);
    }

    void reset() override
    {
        for (auto& v : voices)
            v.active = false;
    }

    double getTailLengthSeconds() const override   { return 2.5; }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0)
            return;
        if (right.size() < (size_t) n)
            right.resize ((size_t) n);   // only if the host lied about the block size
        std::fill (right.begin(), right.begin() + n, 0.0f);

        const auto& kit = kitSpec (geti ("kit"));
        Tweaks t;
        t.kickTune = get ("kickTune");
        t.kickDecay = get ("kickDecay");
        t.snareTune = get ("snareTune");
        t.snap = get ("snap");
        t.hatDecay = get ("hatDecay");
        t.hatTone = get ("hatTone");
        t.tomTune = get ("tomTune");
        const float level = Decibels::decibelsToGain (get ("level"));
        static constexpr float kPan[(int) Drum::count] { 0.0f, 0.1f, 0.05f, 0.0f, 0.3f, 0.3f, -0.35f, 0.0f, 0.35f, -0.25f, 0.25f, -0.15f };

        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : right.data();
        withMidi (n, midi, [&] (int from, int to)
        {
            for (int d = 0; d < (int) Drum::count; ++d)
                voices[(size_t) d].render (l + from, r + from, to - from, kPan[d]);
        },
        [&] (const MidiMessage& m)
        {
            if (! m.isNoteOn())
                return;
            if (auto d = drumForNote (m.getNoteNumber()))
            {
                if (*d == Drum::closedHat)
                    voices[(size_t) Drum::openHat].choke();
                voices[(size_t) *d].trigger (0.25f + 0.75f * m.getFloatVelocity(), kit, t);
            }
        });

        for (int i = 0; i < n; ++i)
        {
            l[i] *= level;
            r[i] *= level;
        }
        if (buffer.getNumChannels() == 1)
            for (int i = 0; i < n; ++i)
                l[i] = 0.5f * (l[i] + r[i]);
    }

private:
    float sr = 48000.0f;
    std::array<DrumVoice, (size_t) Drum::count> voices;
    std::vector<float> right = std::vector<float> (8192, 0.0f);
};
} // namespace

std::unique_ptr<Device> makeBassSynth()   { return std::make_unique<BassSynth>(); }
std::unique_ptr<Device> makePolySynth()   { return std::make_unique<PolySynth>(); }
std::unique_ptr<Device> makeDrums()       { return std::make_unique<Drums>(); }
} // namespace lathe::devices
