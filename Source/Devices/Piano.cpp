// Lathe's Piano: a modelled piano, no samples. Each key is one to three strings; each string is a
// waveguide (a delay line one period long) with a loss filter for how it dies away, a few
// allpasses for the stiffness that makes a real string's upper partials a little sharp, and a
// fractional-delay allpass that puts it exactly in tune. A felt hammer strikes it about an eighth
// of the way along.
// The acoustic types are "commuted": the hammer doesn't push the string with a clean pulse but
// with the soundboard's own response (a dense cloud of woody resonances and a knock, made once in
// prepareToPlay), smoothed by the felt. What the strings play then goes through a small, dense
// body (a feedback-delay network) that rings longer with the sustain pedal down, the way a real
// piano's open strings ring along. The Electric type is the plain model: a clean pulse and a few
// clear resonances, which is what an electric piano sounds like.
// Dampers stop a string when its key comes up (unless the sustain pedal is down); the top keys
// have none, as on a real piano. All of it is allocated in prepareToPlay; the audio thread only
// reads and writes.
#include "Instruments.h"
#include <complex>

namespace lathe::devices
{
namespace
{
using namespace synth;

constexpr int kVoices = 24;        // keys sounding at once
constexpr int kStringsPerKey = 3;
constexpr int kStiffness = 4;      // allpasses per string for inharmonicity
constexpr int kBodyModes = 8;
constexpr int kTopDamper = 88;     // E6: keys above this have no damper
constexpr int kElectric = 3;       // the type that keeps the plain model
constexpr float kBoardSeconds = 0.09f;
constexpr int kExcitationRing = 4096;   // longer than any strike-point delay, at up to 192 kHz

/** Phase delay (samples) of a filter with response h at angular frequency w. */
double phaseDelay (std::complex<double> h, double w)
{
    return -std::arg (h) / w;
}
/** First-order allpass (c + z^-1) / (1 + c z^-1). */
std::complex<double> allpassAt (double c, double w)
{
    const auto z1 = std::polar (1.0, -w);
    return (c + z1) / (1.0 + c * z1);
}
/** The loss filter's shape, (1 - a) / (1 - a z^-1). */
std::complex<double> lossAt (double a, double w)
{
    return (1.0 - a) / (1.0 - a * std::polar (1.0, -w));
}

struct PianoString
{
    std::vector<float> line;
    int mask = 0, pos = 0, length = 2;
    float eta = 0.0f, tx = 0.0f, ty = 0.0f;           // fractional-delay allpass
    float stiff = 0.0f;                               // stiffness allpass coefficient
    int stiffCount = 0;
    float sx[kStiffness] {}, sy[kStiffness] {};
    float lossA = 0.2f, lossY = 0.0f;                 // loss filter
    float gain = 0.999f, gainHeld = 0.999f, gainDamped = 0.99f;
    float strike = 1.0f;                              // how hard the hammer meets this one
    float panL = 1.0f, panR = 1.0f;                   // where this string sits

    void clear()
    {
        std::fill (line.begin(), line.end(), 0.0f);
        pos = 0;
        tx = ty = lossY = 0.0f;
        std::fill (std::begin (sx), std::end (sx), 0.0f);
        std::fill (std::begin (sy), std::end (sy), 0.0f);
    }

    /** One sample: what reaches the bridge. `in` is the hammer's push this sample. */
    float tick (float in)
    {
        float y = line[(size_t) ((pos - length) & mask)];
        const float t = eta * y + tx - eta * ty;      // fractional delay
        tx = y;
        ty = t;
        y = t;
        for (int i = 0; i < stiffCount; ++i)
        {
            const float v = stiff * y + sx[i] - stiff * sy[i];
            sx[i] = y;
            sy[i] = v;
            y = v;
        }
        lossY = gain * ((1.0f - lossA) * y + lossA * lossY);
        line[(size_t) pos] = lossY + in * strike;
        pos = (pos + 1) & mask;
        return lossY;
    }
};

struct PianoVoice
{
    PianoString strings[kStringsPerKey];
    int numStrings = 0, note = -1;
    bool held = false, sustained = false, active = false;
    uint32 started = 0;
    // The hammer: a raised-cosine push `width` samples long, less the same push `combDelay`
    // later (the strike point: it leaves out every eighth or so partial).
    int hammerAt = 0, hammerWidth = 1, combDelay = 1;
    float hammerAmp = 0.0f;
    float level = 1.0f, envelope = 0.0f;
    int quiet = 0;
    // Acoustic types: the soundboard response, through the felt (two one-pole low-passes), less
    // itself `combDelay` later. The ring keeps what's needed for that.
    bool commuted = false;
    int excAt = 0, excEnd = 0;
    float excCoef = 0.5f, excAmp = 0.0f, excLp1 = 0.0f, excLp2 = 0.0f;
    std::vector<float> excRing;

    float hammer (int k) const
    {
        auto pulse = [this] (int i)
        {
            if (i < 0 || i >= hammerWidth)
                return 0.0f;
            const float s = std::sin (MathConstants<float>::pi * (float) i / (float) hammerWidth);
            return s * s;
        };
        return hammerAmp * (pulse (k) - pulse (k - combDelay));
    }
};

/** A soundboard: resonances the strings drive, mixed with what they play directly. */
struct Body
{
    Svf modes[kBodyModes];
    float gains[kBodyModes] {};
    Svf tone;      // a gentle low-pass for the felt piano
    bool useTone = false;

    float tick (float x, float amount)
    {
        float wet = 0.0f;
        for (int i = 0; i < kBodyModes; ++i)
            wet += gains[i] * modes[i].band (x);
        float y = x + amount * wet;
        return useTone ? tone.low (y) : y;
    }
};

/** A small, dense body: four delay lines through a Hadamard matrix, darker on every pass. */
struct Board
{
    static constexpr int kLines = 4;
    std::vector<float> line[kLines];
    int length[kLines] {}, pos[kLines] {};
    float damp[kLines] {}, gain[kLines] {};
    float hp = 0.0f;

    void prepare (float sr)
    {
        for (auto& l : line)
            l.assign ((size_t) (sr * 0.04f) + 4, 0.0f);
        clear();
    }
    void clear()
    {
        for (auto& l : line)
            std::fill (l.begin(), l.end(), 0.0f);
        for (int i = 0; i < kLines; ++i) { pos[i] = 0; damp[i] = 0.0f; }
        hp = 0.0f;
    }
    void set (float sr, float scale, float t60)
    {
        static const float ms[kLines] { 7.3f, 11.1f, 16.9f, 23.3f };
        for (int i = 0; i < kLines; ++i)
        {
            length[i] = jlimit (8, (int) line[i].size() - 1, roundToInt (ms[i] * scale * 0.001f * sr));
            gain[i] = std::pow (0.001f, (float) length[i] / (t60 * sr));
        }
    }
    void tick (float in, float& outL, float& outR)
    {
        // Keep the lowest rumble out of it.
        hp += 0.012f * (in - hp);
        in -= hp;
        float y[kLines];
        for (int i = 0; i < kLines; ++i)
        {
            const int n = (int) line[i].size();
            int r = pos[i] - length[i];
            if (r < 0) r += n;
            damp[i] += 0.55f * (line[i][(size_t) r] - damp[i]);   // darker every pass
            y[i] = damp[i] * gain[i];
        }
        const float a = y[0] + y[1], b = y[0] - y[1], c = y[2] + y[3], d = y[2] - y[3];
        const float mixed[kLines] { 0.5f * (a + c), 0.5f * (b + d), 0.5f * (a - c), 0.5f * (b - d) };
        for (int i = 0; i < kLines; ++i)
        {
            line[i][(size_t) pos[i]] = mixed[i] + in * 0.5f;
            if (++pos[i] >= (int) line[i].size()) pos[i] = 0;
        }
        outL = 0.5f * (y[0] - y[1] + y[2] - y[3]);
        outR = 0.5f * (y[0] + y[1] - y[2] - y[3]);
    }
};

class Piano final : public Device
{
public:
    Piano() : Device ("piano", "Piano", Kind::instrument, layout())
    {
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (choice ("type", "Piano", { "Grand", "Upright", "Felt", "Electric" }, 0));
        l.add (percent ("hammer", "Hammer", 0.55f));
        l.add (percent ("stiffness", "Stiffness", 0.5f));
        l.add (percent ("unison", "Unison", 0.06f));
        l.add (percent ("decay", "Decay", 0.55f));
        l.add (number ("dampers", "Dampers", { 40.0f, 2000.0f, 0.0f, 0.4f }, 250.0f, [] (float v) { return msText (v); }));
        l.add (percent ("body", "Body", 0.5f));
        l.add (percent ("width", "Width", 0.6f));
        l.add (decibels ("level", "Level", -24.0f, 6.0f, -6.0f));
        return l;
    }

    String blurb() const override   { return "A modelled piano: grand, upright, felt or electric. Responds to how hard you play and to the sustain pedal."; }
    String modeParam() const override   { return "type"; }

    std::vector<Section> sections() const override
    {
        return {
            { "Hammer and strings", { { "hammer", "Hammer", "Soft felt to hard: how bright a note starts, on top of how hard you play." },
                                      { "stiffness", "Stiffness", "How much the upper partials stretch sharp, as in a real string. Most in the bass." },
                                      { "unison", "Unison", "How far the two or three strings of a key drift apart: in tune to honky-tonk." } } },
            { "Sustain", { { "decay", "Decay", "How long a held note rings." },
                           { "dampers", "Dampers", "How fast a note stops when you let go (and lift the pedal)." } } },
            { "Output", { { "body", "Body", "How much of the piano's body you hear ring around the strings." },
                          { "width", "Width", "Low notes to the left, high to the right, as the player hears it." },
                          { "level", "Level", "Volume." } } } };
    }

    std::vector<Preset> presets() const override
    {
        auto p = [] (const char* name, int type, float hammer, float stiffness, float unison, float decay,
                     float dampers, float body, float width, float level) -> Preset
        {
            return { name, { { "type", (float) type }, { "hammer", hammer }, { "stiffness", stiffness }, { "unison", unison },
                             { "decay", decay }, { "dampers", dampers }, { "body", body }, { "width", width }, { "level", level } } };
        };
        return {
            p ("Grand",          0, 0.55f, 0.5f,  0.06f, 0.55f, 250.0f, 0.5f,  0.6f,  -6.0f),
            p ("Bright grand",   0, 0.8f,  0.55f, 0.07f, 0.5f,  220.0f, 0.45f, 0.7f,  -7.0f),
            p ("Soft and close", 0, 0.3f,  0.45f, 0.05f, 0.6f,  350.0f, 0.4f,  0.3f,  -5.0f),
            p ("Upright",        1, 0.5f,  0.45f, 0.12f, 0.4f,  200.0f, 0.6f,  0.45f, -9.0f),
            p ("Honky-tonk",     1, 0.7f,  0.5f,  0.75f, 0.45f, 180.0f, 0.5f,  0.6f,  -10.0f),
            p ("Felt piano",     2, 0.15f, 0.35f, 0.1f,  0.35f, 300.0f, 0.55f, 0.5f,  -6.0f),
            p ("Electric",       3, 0.55f, 0.5f,  0.06f, 0.55f, 250.0f, 0.5f,  0.6f,  -6.0f),
            p ("Electric, bright", 3, 0.85f, 0.4f, 0.04f, 0.45f, 200.0f, 0.35f, 0.75f, -8.0f) };
    }

    void prepareToPlay (double rate, int block) override
    {
        ignoreUnused (block);
        sr = (float) rate;
        // A0 (27.5 Hz) is the longest string; round its period up to a power of two.
        int size = 1;
        while (size < (int) (sr / 27.0f) + 16)
            size <<= 1;
        for (auto& v : voices)
        {
            for (auto& s : v.strings)
            {
                s.line.assign ((size_t) size, 0.0f);
                s.mask = size - 1;
            }
            v.excRing.assign ((size_t) kExcitationRing, 0.0f);
        }
        for (int type = 0; type < kElectric; ++type)
            makeSoundboard (type);
        for (auto& b : board)
            b.prepare (sr);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices)
        {
            for (auto& s : v.strings)
                s.clear();
            v.active = v.held = v.sustained = false;
            v.note = -1;
            v.envelope = 0.0f;
            v.excAt = v.excEnd = 0;
            v.excLp1 = v.excLp2 = 0.0f;
            std::fill (v.excRing.begin(), v.excRing.end(), 0.0f);
        }
        for (auto& b : board)
            b.clear();
        boardT60 = 0.0f;
        pedal = false;
        softPedal = false;
        age = 0;
        bodyType = -1;
        for (auto& b : body)
            for (auto& m : b.modes) m.ic1 = m.ic2 = 0.0f;
    }

    double getTailLengthSeconds() const override   { return get ("dampers") * 0.001 + 0.5; }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0 || buffer.getNumChannels() == 0 || sr <= 0.0f)
            return;

        const int type = jlimit (0, kElectric, geti ("type"));
        if (type != bodyType)
            setBody (type);
        const float bodyAmount = get ("body") * 1.6f;
        // The body rings longer with the sustain pedal down: the open strings ring along.
        if (type != kElectric)
        {
            const float base = type == 0 ? 0.55f : 0.38f;
            const float want = base * (pedal ? 2.5f : 1.0f);
            const float t60 = boardT60 <= 0.0f ? want : boardT60 + 0.15f * (want - boardT60);
            if (std::abs (t60 - boardT60) > 1.0e-4f)
            {
                boardT60 = t60;
                for (auto& b : board)
                    b.set (sr, type == 0 ? 1.0f : 0.8f, boardT60);
                board[1].set (sr, (type == 0 ? 1.0f : 0.8f) * 1.13f, boardT60);   // the right side a little different
            }
        }
        const float level = Decibels::decibelsToGain (get ("level")) * 2.5f;

        float* outL = buffer.getWritePointer (0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        withMidi (n, midi, [&] (int from, int to)
        {
            for (int i = from; i < to; ++i)
            {
                float l = 0.0f, r = 0.0f;
                for (auto& v : voices)
                {
                    if (! v.active)
                        continue;
                    float push = 0.0f;
                    if (v.commuted)
                    {
                        if (v.excAt < v.excEnd)
                        {
                            const auto& ir = soundboard[jlimit (0, kElectric - 1, type)];
                            const float in = v.excAt < (int) ir.size() ? ir[(size_t) v.excAt] * v.excAmp : 0.0f;
                            v.excLp1 += v.excCoef * (in - v.excLp1);
                            v.excLp2 += v.excCoef * (v.excLp1 - v.excLp2);
                            v.excRing[(size_t) (v.excAt & (kExcitationRing - 1))] = v.excLp2;
                            const float earlier = v.excAt >= v.combDelay ? v.excRing[(size_t) ((v.excAt - v.combDelay) & (kExcitationRing - 1))] : 0.0f;
                            push = v.excLp2 - earlier;
                            ++v.excAt;
                        }
                    }
                    else if (v.hammerAt < v.hammerWidth + v.combDelay)
                    {
                        push = v.hammer (v.hammerAt++);
                    }
                    float x = 0.0f;
                    for (int k = 0; k < v.numStrings; ++k)
                    {
                        auto& s = v.strings[k];
                        s.gain += 0.002f * ((v.held || v.sustained ? s.gainHeld : s.gainDamped) - s.gain);
                        const float y = s.tick (push) * v.level;
                        x += y;
                        l += y * s.panL;
                        r += y * s.panR;
                    }
                    // Free the voice once it has died away.
                    v.envelope = jmax (std::abs (x), v.envelope * 0.9995f);
                    const bool struck = v.commuted ? v.excAt >= v.excEnd : v.hammerAt >= v.hammerWidth + v.combDelay;
                    if (v.envelope < 1.0e-5f && struck)
                    {
                        if (++v.quiet > 1024)
                            v.active = false;
                    }
                    else
                    {
                        v.quiet = 0;
                    }
                }
                float left, right;
                if (type == kElectric)
                {
                    left = body[0].tick (l, bodyAmount);
                    right = body[1].tick (r, bodyAmount);
                }
                else
                {
                    float wl, wr;
                    board[0].tick (0.5f * (l + r), wl, wr);
                    left = l + bodyAmount * 0.9f * wl;
                    right = r + bodyAmount * 0.9f * wr;
                }
                outL[i] = softLimit (left * level);
                if (outR != nullptr)
                    outR[i] = softLimit (right * level);
            }
        },
        [&] (const MidiMessage& m)
        {
            if (m.isNoteOn())
                strike (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
            {
                for (auto& v : voices)
                    if (v.active && v.held && v.note == m.getNoteNumber())
                    {
                        v.held = false;
                        v.sustained = pedal;
                    }
            }
            else if (m.isSustainPedalOn())
            {
                pedal = true;
                for (auto& v : voices)
                    if (v.active)
                        v.sustained = true;
            }
            else if (m.isSustainPedalOff())
            {
                pedal = false;
                for (auto& v : voices)
                    v.sustained = false;
            }
            else if (m.isSoftPedalOn())
                softPedal = true;
            else if (m.isSoftPedalOff())
                softPedal = false;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                pedal = false;
                for (auto& v : voices)
                    v.held = v.sustained = false;
                if (m.isAllSoundOff())
                    for (auto& v : voices)
                        v.active = false;
            }
        });
    }

private:
    PianoVoice voices[kVoices];
    Body body[2];
    Board board[2];
    std::vector<float> soundboard[kElectric];
    float boardT60 = 0.0f;
    float sr = 48000.0f;
    bool pedal = false, softPedal = false;
    uint32 age = 0;
    int bodyType = -1;

    /** The soundboard's response for an acoustic type: a knock, then a dense cloud of decaying
        resonances, more and lower for the grand, boxier for the upright, darker for the felt. Made
        once, here, so a strike only reads it. */
    void makeSoundboard (int type)
    {
        auto& ir = soundboard[type];
        ir.assign ((size_t) (kBoardSeconds * sr), 0.0f);
        Random rng (1234 + type);
        const int modes = type == 0 ? 96 : 72;
        const float lo = type == 0 ? 55.0f : 85.0f, hi = 7000.0f;
        double modeEnergy = 0.0;
        std::vector<double> sum (ir.size(), 0.0);
        for (int m = 0; m < modes; ++m)
        {
            const float f = lo * std::pow (hi / lo, rng.nextFloat());
            float amp = std::pow (f / 150.0f, -0.35f) * (0.5f + 0.5f * rng.nextFloat()) * (rng.nextBool() ? 1.0f : -1.0f);
            if (type == 1 && f > 170.0f && f < 520.0f) amp *= 1.6f;   // the upright's boxy middle
            if (type == 2 && f > 2200.0f) amp *= 0.35f;               // felt: the top taken off
            const float tau = jlimit (0.004f, 0.08f, 0.06f * std::pow (200.0f / f, 0.6f)) * (type == 0 ? 1.0f : 0.75f);
            const double w = MathConstants<double>::twoPi * f / sr, phase = MathConstants<double>::twoPi * rng.nextFloat();
            const double fall = std::exp (-1.0 / (tau * sr));
            double env = 1.0;
            for (size_t n = 0; n < sum.size(); ++n)
            {
                sum[n] += amp * env * std::sin (w * (double) n + phase);
                env *= fall;
                if (env < 1.0e-5) break;
            }
        }
        for (auto x : sum)
            modeEnergy += x * x;
        // The knock (the direct push) carries as much energy as the whole cloud of resonances.
        const double scale = modeEnergy > 0.0 ? std::sqrt (1.0 / modeEnergy) : 0.0;
        for (size_t n = 0; n < ir.size(); ++n)
            ir[n] = (float) (sum[n] * scale);
        ir[0] += 1.0f;
        if (ir.size() > 1) ir[1] += 0.5f;
    }

    /** Untouched up to -2 dBFS, then a smooth bend that never passes 0 dBFS: only a whole keyboard
        at full velocity with the pedal down gets there. */
    static float softLimit (float x)
    {
        constexpr float knee = 0.8f;
        const float a = std::abs (x);
        if (a <= knee)
            return x;
        const float over = (a - knee) / (1.0f - knee);
        return std::copysign (knee + (1.0f - knee) * over / (1.0f + over), x);
    }

    void setBody (int type)
    {
        // Grand: a big soundboard, lots of low resonance. Upright: smaller, the low modes higher
        // and boxier. Felt: the upright with felt between hammer and string, so darker still.
        static const float grand[kBodyModes]   { 95.0f, 180.0f, 290.0f, 430.0f, 640.0f, 980.0f, 1550.0f, 2500.0f };
        static const float upright[kBodyModes] { 140.0f, 240.0f, 370.0f, 520.0f, 780.0f, 1150.0f, 1750.0f, 2700.0f };
        static const float level[kBodyModes]   { 0.9f, 0.8f, 0.7f, 0.55f, 0.45f, 0.35f, 0.25f, 0.18f };
        const float* f = type == 0 || type == kElectric ? grand : upright;
        for (auto& b : body)
        {
            for (int i = 0; i < kBodyModes; ++i)
            {
                // Left and right a few percent apart, and each side favouring different modes, as
                // two microphones over a soundboard would hear it.
                const bool right = &b != body;
                const float shift = right ? 1.06f : 0.97f;
                b.modes[i].set (f[i] * shift, 1.0f / 3.5f, sr);
                b.gains[i] = level[i] * (type == 0 || type == kElectric ? 1.0f : 0.85f) * ((i % 2 == 0) == right ? 0.7f : 1.15f);
            }
            b.useTone = type == 2;
            b.tone.set (2600.0f, 1.2f, sr);
        }
        bodyType = type;
    }

    void strike (int note, float velocity)
    {
        note = jlimit (21, 108, note);
        // The key that's already sounding (struck again), else a free voice, else the one
        // released longest ago, else the oldest.
        PianoVoice* pick = nullptr;
        for (auto& v : voices)
            if (v.active && v.note == note) { pick = &v; break; }
        if (pick == nullptr)
            for (auto& v : voices)
                if (! v.active) { pick = &v; break; }
        if (pick == nullptr)
            for (auto& v : voices)
                if (! v.held && ! v.sustained && (pick == nullptr || v.started < pick->started))
                    pick = &v;
        if (pick == nullptr)
            for (auto& v : voices)
                if (pick == nullptr || v.started < pick->started)
                    pick = &v;

        auto& v = *pick;
        const bool again = v.active && v.note == note;
        if (! again)
            for (auto& s : v.strings)
                s.clear();
        tune (v, note);

        const int type = jlimit (0, kElectric, geti ("type"));
        const float t = (float) (note - 21) / 87.0f;   // 0 at A0, 1 at C8
        float hardness = jlimit (0.0f, 1.0f, 0.55f * get ("hammer") + 0.45f * velocity);
        if (type == 2) hardness *= 0.45f;              // felt
        if (softPedal) hardness *= 0.6f;
        // Contact time: long and soft for a gentle bass note, short and bright for a hard treble one.
        const float contactMs = jmap (hardness, 4.0f, 0.5f) * (1.6f - 0.9f * t);
        v.hammerWidth = jmax (2, roundToInt (contactMs * 0.001f * sr));
        const float period = sr / noteHz ((float) note);
        v.combDelay = jmax (1, roundToInt (period / (type == 1 ? 9.0f : type == 2 ? 7.0f : 8.0f)));
        v.hammerAt = 0;
        const float loud = std::pow (jlimit (0.0f, 1.0f, velocity), 1.2f) * (softPedal ? 0.7f : 1.0f);
        // Short pushes carry less energy: make up for it so loudness follows velocity, not hardness.
        v.hammerAmp = 0.5f * loud * std::sqrt (24.0f / (float) v.hammerWidth);
        // Acoustic types: the soundboard through the felt. The felt's corner follows the contact time.
        v.commuted = type != kElectric;
        if (v.commuted)
        {
            const float corner = jlimit (80.0f, sr * 0.4f, 0.7f / (contactMs * 0.001f));
            v.excCoef = 1.0f - std::exp (-MathConstants<float>::twoPi * corner / sr);
            v.excAmp = 0.35f * loud * std::sqrt (24.0f / (float) v.hammerWidth) * (float) v.hammerWidth * 0.5f;
            v.excAt = 0;
            v.excLp1 = v.excLp2 = 0.0f;
            v.excEnd = (int) soundboard[type].size() + v.combDelay + roundToInt (8.0f / v.excCoef);
        }
        v.level = 0.55f / (float) v.numStrings;

        // Low notes left, high right, as the player hears it; each of a key's strings a little apart.
        const float width = get ("width");
        for (int k = 0; k < v.numStrings; ++k)
        {
            const float spread = v.numStrings == 1 ? 0.0f : ((float) k / (float) (v.numStrings - 1) - 0.5f) * 0.5f;
            const float pan = jlimit (-1.0f, 1.0f, ((t * 2.0f - 1.0f) * 1.6f + spread) * width);
            v.strings[k].panL = std::sqrt (0.5f * (1.0f - pan)) * 1.41421f;
            v.strings[k].panR = std::sqrt (0.5f * (1.0f + pan)) * 1.41421f;
        }

        v.note = note;
        v.held = true;
        v.sustained = pedal;
        v.active = true;
        v.quiet = 0;
        v.started = ++age;
    }

    /** Lengths, filters and losses for a key's strings, so each sounds at its pitch. */
    void tune (PianoVoice& v, int note)
    {
        const int type = jlimit (0, kElectric, geti ("type"));
        const float t = (float) (note - 21) / 87.0f;
        v.numStrings = note < 33 ? 1 : note < 46 ? 2 : 3;

        // How long it rings: long in the bass, short at the top. Decay scales it 0.35x to 2.2x.
        const float decay = 0.35f * std::pow (2.2f / 0.35f, get ("decay"));
        const float typeDecay = type == 0 || type == kElectric ? 1.0f : type == 1 ? 0.7f : 0.55f;
        const float t60 = 20.0f * std::pow (2.0f, -(float) (note - 21) / 22.0f) * decay * typeDecay;
        const float damperSeconds = note >= kTopDamper ? t60 : jmax (0.02f, get ("dampers") * 0.001f);

        // Stiffness: the bass strings are the stiffest; nothing above about A6.
        const float stiffAmount = get ("stiffness");
        const int stiffCount = note < 72 ? kStiffness : note < 93 ? 2 : 0;
        const float stiff = -jlimit (0.0f, 0.85f, stiffAmount * (0.2f + 0.55f * (1.0f - t)));

        // Unison: the strings of a key drift apart by a fraction of a cent up to ~15 cents.
        const float spread = 0.25f + 15.0f * std::pow (get ("unison"), 1.5f);
        static const float offsets3[] { -1.0f, 0.0f, 1.0f };
        static const float offsets2[] { -0.5f, 0.5f };

        // The loss filter takes the upper partials down faster than the fundamental. Gentler at the
        // top, where the fundamental itself is close to where it starts to cut.
        const float lossA = jlimit (0.005f, 0.6f, (0.015f + 0.35f * (1.0f - t) * (1.0f - t)) * (type == 2 ? 1.5f : 1.0f));

        for (int k = 0; k < v.numStrings; ++k)
        {
            auto& s = v.strings[k];
            const float cents = v.numStrings == 1 ? 0.0f : spread * (v.numStrings == 3 ? offsets3[k] : offsets2[k]);
            const double f0 = noteHz ((float) note + cents / 100.0f);
            const double w = MathConstants<double>::twoPi * f0 / sr;
            const double period = sr / f0;

            s.stiff = stiff;
            s.stiffCount = stiffCount;
            s.lossA = lossA;
            // Everything in the loop but the delay line and the tuning allpass, at the pitch.
            double other = phaseDelay (lossAt (lossA, w), w);
            if (stiffCount > 0)
                other += stiffCount * phaseDelay (allpassAt (stiff, w), w);
            double rest = period - other;
            int whole = (int) std::floor (rest - 0.5);
            whole = jlimit (1, s.mask - 1, whole);
            double frac = rest - whole;                       // 0.5 .. 1.5: the allpass is flat and stable there
            for (int pass = 0; pass < 3; ++pass)              // the allpass's delay at the pitch, not at DC
            {
                const double eta = (1.0 - frac) / (1.0 + frac);
                const double err = phaseDelay (allpassAt (eta, w), w) - frac;
                frac = jlimit (0.3, 1.7, (rest - whole) - err);
            }
            s.length = whole;
            s.eta = (float) ((1.0 - frac) / (1.0 + frac));

            // Loss per trip round the string, for the fundamental to fall 60 dB in t60, allowing for
            // what the loss filter already takes from it. Never above unity, so nothing can build up.
            const double filterAtPitch = std::abs (lossAt (lossA, w));
            s.gainHeld = (float) jmin (0.99995, std::pow (0.001, 1.0 / (t60 * f0)) / filterAtPitch);
            s.gainDamped = (float) jmin (0.99995, std::pow (0.001, 1.0 / (damperSeconds * f0)) / filterAtPitch);
            if (s.gain <= 0.0f || ! v.active)
                s.gain = s.gainHeld;
            s.strike = 1.0f - 0.08f * (float) k;   // the hammer meets the strings a little unevenly
        }
    }
};
} // namespace

std::unique_ptr<Device> makePiano()   { return std::unique_ptr<Device> (new Piano()); }
} // namespace lathe::devices
