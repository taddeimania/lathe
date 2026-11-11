#include "DemoSong.h"
#include "../Cloud/ProjectDiff.h"
#include "../Cloud/Revisions.h"

namespace lathe::demo
{
namespace
{
constexpr double kRate = 48000.0;
constexpr double kTempo = 96.0;

int64 samplesFor (double beats)   { return (int64) std::llround (beats * 60.0 / kTempo * kRate); }

Result writeWav (const File& f, std::vector<float>& x)
{
    float peak = 1.0e-9f;
    for (auto v : x)
        peak = jmax (peak, std::abs (v));
    for (auto& v : x)
        v = v / peak * 0.8f;

    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<OutputStream> stream (f.createOutputStream());
    if (stream == nullptr)
        return Result::fail ("Couldn't write " + f.getFullPathName());
    WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (kRate).withNumChannels (1).withBitsPerSample (24));
    if (writer == nullptr)
        return Result::fail ("Couldn't write " + f.getFullPathName());
    const float* chans[] = { x.data() };
    writer->writeFromFloatArrays (chans, 1, (int) x.size());
    return Result::ok();
}

/** A decaying sine with a pitch drop and some noise: kicks, snares and hats. */
void hit (std::vector<float>& out, double atBeat, double f0, double decay, double noise, float amp, Random& rnd)
{
    const auto start = samplesFor (atBeat);
    const auto n = samplesFor (0.9);
    for (int64 i = 0; i < n && start + i < (int64) out.size(); ++i)
    {
        const double t = (double) i / kRate;
        const double env = std::exp (-t * decay);
        const double s = std::sin (MathConstants<double>::twoPi * f0 * t * (1.0 + 2.0 * std::exp (-t * 40.0))) * env
                         + noise * (rnd.nextDouble() * 2.0 - 1.0) * std::exp (-t * decay * 1.5);
        out[(size_t) (start + i)] += amp * (float) s;
    }
}

double hz (int midi)   { return 440.0 * std::pow (2.0, (midi - 69) / 12.0); }

Result writeAudio (const File& dir)
{
    Random rnd (4);

    std::vector<float> drums ((size_t) samplesFor (64.0));
    for (int bar = 0; bar < 16; ++bar)
    {
        const double b = bar * 4.0;
        hit (drums, b, 55, 9, 0, 1.0f, rnd);
        hit (drums, b + 2.5, 55, 9, 0, 0.8f, rnd);
        hit (drums, b + 1, 180, 14, 0.9, 1.0f, rnd);
        hit (drums, b + 3, 180, 14, 0.9, 1.0f, rnd);
        for (int k = 0; k < 8; ++k)
            hit (drums, b + k * 0.5, 4000, 60, 0.6, 0.25f, rnd);
        if (bar % 4 == 3)
        {
            hit (drums, b + 3.5, 180, 18, 0.9, 0.7f, rnd);
            hit (drums, b + 3.75, 160, 18, 0.9, 0.7f, rnd);
        }
    }
    if (auto r = writeWav (dir.getChildFile ("Drums 1.wav"), drums); r.failed()) return r;

    const int roots[] = { 41, 41, 44, 39 };
    std::vector<float> bass ((size_t) samplesFor (64.0));
    for (int bar = 0; bar < 16; ++bar)
        for (int k = 0; k < 8; ++k)
        {
            const double f = hz (roots[bar % 4]);
            const auto start = samplesFor (bar * 4.0 + k * 0.5);
            const auto n = samplesFor (0.45);
            for (int64 i = 0; i < n && start + i < (int64) bass.size(); ++i)
            {
                const double t = (double) i / kRate;
                const double ph = std::sin (MathConstants<double>::twoPi * f * t);
                const double s = (ph >= 0 ? 0.4 : -0.4) + ph;
                bass[(size_t) (start + i)] += (float) (s * std::exp (-t * 3.0) * (1.0 - std::exp (-t * 300.0)));
            }
        }
    if (auto r = writeWav (dir.getChildFile ("Bass 1.wav"), bass); r.failed()) return r;

    std::vector<float> guitar ((size_t) samplesFor (32.0));
    for (int bar = 0; bar < 8; ++bar)
        for (double k : { 0.0, 2.5 })
        {
            const auto start = samplesFor (bar * 4.0 + k);
            const auto n = samplesFor (1.6);
            for (int64 i = 0; i < n && start + i < (int64) guitar.size(); ++i)
            {
                const double t = (double) i / kRate;
                double s = 0.0;
                for (int iv : { 0, 7, 12, 16, 19 })
                    s += std::sin (MathConstants<double>::twoPi * 110.0 * std::pow (2.0, iv / 12.0) * t);
                s = s * std::exp (-t * 2.2) + 0.15 * (rnd.nextDouble() * 2.0 - 1.0) * std::exp (-t * 20.0);
                guitar[(size_t) (start + i)] += (float) s;
            }
        }
    if (auto r = writeWav (dir.getChildFile ("Guitar 1.wav"), guitar); r.failed()) return r;

    std::vector<float> vocal ((size_t) samplesFor (48.0));
    const int steps[] = { 0, 2, 3, 5, 7, 10 };
    for (int phrase = 0; phrase < 5; ++phrase)
    {
        double at = phrase * 8 + 0.5 + rnd.nextDouble();
        const int syllables = 5 + rnd.nextInt (5);
        for (int s = 0; s < syllables; ++s)
        {
            const double lengths[] = { 0.4, 0.5, 0.75, 1.0 };
            const double len = lengths[rnd.nextInt (4)];
            const double f = 220.0 * std::pow (2.0, steps[rnd.nextInt (6)] / 12.0);
            const auto start = samplesFor (at);
            const auto n = samplesFor (len);
            for (int64 i = 0; i < n && start + i < (int64) vocal.size(); ++i)
            {
                const double t = (double) i / kRate;
                const double shape = std::pow (std::sin (MathConstants<double>::pi * (double) i / (double) n), 0.7);
                const double v = std::sin (MathConstants<double>::twoPi * f * t + 3.0 * std::sin (MathConstants<double>::twoPi * 5.0 * t))
                                 + 0.2 * (rnd.nextDouble() * 2.0 - 1.0);
                vocal[(size_t) (start + i)] += (float) (v * shape);
            }
            at += len + 0.1;
        }
    }
    return writeWav (dir.getChildFile ("Vocal take 3.wav"), vocal);
}

Clip audioClip (Edit& e, const File& dir, const String& name, double start, double length)
{
    Clip c;
    c.id = e.newId();
    c.kind = ClipKind::audio;
    c.name = name;
    c.file = dir.getChildFile ("Audio").getChildFile (name + ".wav");
    c.start = start;
    c.length = length;
    return c;
}

Clip midiClip (Edit& e, const String& name, double start, double length, std::vector<Note> notes)
{
    Clip c;
    c.id = e.newId();
    c.kind = ClipKind::midi;
    c.name = name;
    c.start = start;
    c.length = length;
    c.notes = std::move (notes);
    return c;
}

/** How far along the song is: 1 = drums only ... 5 = everything. Each stage is one push. */
struct Stage
{
    int step = 5;
    double tempo = kTempo;
};

void build (Edit& e, const File& dir, Stage stage)
{
    e.reset();
    e.workDir = dir;
    e.projectFile = dir.getChildFile ("Night Drive" + String (kProjectExtension));
    e.sampleRate = kRate;
    e.tempo = stage.tempo;
    e.loopEnabled = false;

    auto plate = e.addTrack (TrackKind::bus, "Plate");
    plate->volumeDb = -3.0f;
    auto echo = e.addTrack (TrackKind::bus, "Echo");
    echo->volumeDb = -6.0f;

    auto drums = e.addTrack (TrackKind::audio, "Drums");
    drums->input = { 0, false };
    drums->volumeDb = -2.0f;
    drums->clips.push_back (audioClip (e, dir, "Drums 1", 0.0, 64.0));
    drums->getSend (plate->id, true)->levelDb = -18.0f;

    auto bass = e.addTrack (TrackKind::audio, "Bass");
    bass->input = { 1, false };
    bass->volumeDb = stage.step >= 4 ? -4.0f : -6.0f;
    if (stage.step >= 2)
        bass->clips.push_back (audioClip (e, dir, "Bass 1", 0.0, 64.0));

    auto guitar = e.addTrack (TrackKind::audio, "Guitar");
    guitar->input = { 2, false };
    guitar->volumeDb = -6.0f;
    guitar->pan = stage.step >= 4 ? -0.35f : 0.0f;
    if (stage.step >= 2)
        guitar->clips.push_back (audioClip (e, dir, "Guitar 1", 16.0, 32.0));
    guitar->getSend (plate->id, true)->levelDb = -10.0f;
    guitar->getSend (echo->id, true)->levelDb = -14.0f;

    std::vector<Note> chords;
    const int chordRoots[4][4] = { { 53, 57, 60, 64 }, { 53, 57, 60, 64 }, { 56, 60, 63, 67 }, { 51, 55, 58, 62 } };
    for (int bar = 0; bar < 8; ++bar)
        for (int p : chordRoots[bar % 4])
            chords.push_back ({ p, bar * 4.0, 3.5, 0.7f });
    auto keys = e.addTrack (TrackKind::midi, "Keys");
    keys->volumeDb = stage.step >= 5 ? -4.5f : -3.0f;
    keys->pan = 0.2f;
    keys->clips.push_back (midiClip (e, "Keys", 0.0, 32.0, chords));
    keys->clips.push_back (midiClip (e, "Keys", 32.0, 32.0, chords));
    keys->getSend (plate->id, true)->levelDb = -8.0f;

    auto lead = e.addTrack (TrackKind::midi, "Lead");
    lead->volumeDb = -5.0f;
    lead->pan = 0.3f;
    if (stage.step >= 3)
    {
        Random rnd (11);
        const int scale[] = { 65, 67, 68, 70, 72, 75, 77 };
        const double lengths[] = { 0.5, 0.5, 1.0, 0.25, 1.5 };
        std::vector<Note> line;
        for (double pos = 0.0; pos < 32.0;)
        {
            const double len = lengths[rnd.nextInt (5)];
            if (rnd.nextFloat() > 0.2f)
                line.push_back ({ scale[rnd.nextInt (7)], pos, len * 0.9, 0.55f + 0.4f * rnd.nextFloat() });
            pos += len;
        }
        lead->clips.push_back (midiClip (e, "Lead", 32.0, 32.0, line));
    }
    lead->getSend (echo->id, true)->levelDb = -9.0f;

    auto vocal = e.addTrack (TrackKind::audio, "Vocal");
    vocal->input = { 3, false };
    vocal->arm = true;
    if (stage.step >= 5)
        vocal->clips.push_back (audioClip (e, dir, "Vocal take 3", 8.0, 48.0));
    vocal->getSend (plate->id, true)->levelDb = stage.step >= 5 ? -6.0f : -12.0f;
    vocal->getSend (echo->id, true)->levelDb = -12.0f;
}

Result save (Edit& e)
{
    e.projectFile.getParentDirectory().createDirectory();
    if (! e.projectFile.replaceWithText (JSON::toString (e.toVar (false))))
        return Result::fail ("Couldn't write " + e.projectFile.getFullPathName());
    return Result::ok();
}
} // namespace

Result write (const File& folder, Songs& out)
{
    const auto plainDir = folder.getChildFile ("Night Drive");
    const auto sharedDir = folder.getChildFile ("Night Drive (shared)");
    if (auto r = writeAudio (plainDir.getChildFile ("Audio")); r.failed())
        return r;
    sharedDir.getChildFile ("Audio").createDirectory();
    for (auto& f : plainDir.getChildFile ("Audio").findChildFiles (File::findFiles, false, "*.wav"))
        f.copyFileTo (sharedDir.getChildFile ("Audio").getChildFile (f.getFileName()));

    const auto now = Time::currentTimeMillis();
    constexpr int64 minute = 60 * 1000, day = 24 * 60 * minute;

    // --- the song, with a note thread on the vocal
    {
        Edit e;
        build (e, plainDir, {});
        int vocalId = -1;
        for (auto& t : e.tracks)
            if (t->name == "Vocal")
                vocalId = t->id;

        SongNote n;
        n.id = "demo-note-1";
        n.track = vocalId;
        n.trackName = "Vocal";
        n.beat = 32.0;   // bar 9
        n.author = "Dev@example.com";
        n.text = "Love this take. Can the harmony come in a bar later?";
        n.createdAt = n.updatedAt = now - day;
        SongNoteReply reply;
        reply.id = "demo-reply-1";
        reply.author = "Mara@example.com";
        reply.text = "Done, pushed. It comes in at bar 10 now.";
        reply.createdAt = now - 2 * minute;
        n.replies.push_back (reply);
        e.localNotes.push_back (n);

        if (auto r = save (e); r.failed())
            return r;
        out.plain = e.projectFile;
        out.noteId = n.id;
        out.noteTrackId = vocalId;
        out.noteBeat = n.beat;
    }

    // --- the shared copy: five pushes, newest last
    {
        struct Push { Stage stage; const char* author; const char* message; int64 ago; bool kept; };
        const Push pushes[] = {
            { { 1, 92.0 }, "Dev@example.com",  "Drums, first take",            9 * day,     false },
            { { 2, 96.0 }, "Dev@example.com",  "Tempo up, bass and guitar in", 6 * day,     false },
            { { 3, 96.0 }, "Mara@example.com", "Lead line for the chorus",     4 * day,     false },
            { { 4, 96.0 }, "Dev@example.com",  "Bass re-amped, mix for the label", 1 * day, true },
            { { 5, 96.0 }, "Mara@example.com", "New vocal take",               2 * minute,  false },
        };

        Edit e;
        cloud::LocalStore store (sharedDir.getChildFile ("Night Drive" + String (kProjectExtension)));
        store.link ("demo-night-drive", "Night Drive");
        var previous;
        String parent;
        int seq = 0;
        for (auto& p : pushes)
        {
            build (e, sharedDir, p.stage);
            auto snap = cloud::Snapshot::capture (e);
            if (auto r = snap.finish (store); r.failed())
                return r;
            store.putObject (snap.bytes);

            cloud::Revision rev;
            rev.id = "demo-r" + String (++seq);
            rev.seq = seq;
            rev.parent = parent;
            rev.author = p.author;
            rev.message = p.message;
            rev.summary = previous.isVoid() ? String ("First revision")
                                            : cloud::describeChanges (previous, snap.project).joinIntoString ("\n");
            rev.snapshot = snap.hash;
            rev.blobs.add (snap.hash);
            rev.createdAt = now - p.ago;
            rev.pinned = p.kept;
            store.putRevision (rev);

            previous = snap.project;
            parent = rev.id;
        }
        store.setBase (parent);
        if (auto r = save (e); r.failed())
            return r;
        out.shared = e.projectFile;
    }
    return Result::ok();
}

//==============================================================================
namespace
{
class DemoDevice : public AudioIODevice
{
public:
    DemoDevice() : AudioIODevice ("Audio Interface", "Demo") {}
    ~DemoDevice() override   { stop(); }

    StringArray getOutputChannelNames() override  { return names ("Output"); }
    StringArray getInputChannelNames() override   { return names ("Input"); }
    Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    Array<int> getAvailableBufferSizes() override  { return { 128 }; }
    int getDefaultBufferSize() override            { return 128; }
    String open (const BigInteger&, const BigInteger&, double, int) override { opened = true; return {}; }
    void close() override                          { opened = false; }
    bool isOpen() override                         { return opened; }
    void start (AudioIODeviceCallback* cb) override
    {
        if (cb != nullptr && callback == nullptr)
        {
            callback = cb;
            callback->audioDeviceAboutToStart (this);   // never calls back with audio: nothing plays
        }
    }
    void stop() override
    {
        if (auto* cb = std::exchange (callback, nullptr))
            cb->audioDeviceStopped();
    }
    bool isPlaying() override                      { return callback != nullptr; }
    String getLastError() override                 { return {}; }
    int getCurrentBufferSizeSamples() override     { return 128; }
    double getCurrentSampleRate() override         { return 48000.0; }
    int getCurrentBitDepth() override              { return 24; }
    BigInteger getActiveOutputChannels() const override { BigInteger b; b.setRange (0, 8, true); return b; }
    BigInteger getActiveInputChannels() const override  { BigInteger b; b.setRange (0, 8, true); return b; }
    int getOutputLatencyInSamples() override       { return 0; }
    int getInputLatencyInSamples() override        { return 0; }

private:
    static StringArray names (const String& base)
    {
        StringArray s;
        for (int i = 1; i <= 8; ++i)
            s.add (base + " " + String (i));
        return s;
    }
    bool opened = false;
    AudioIODeviceCallback* callback = nullptr;
};

class DemoDeviceType : public AudioIODeviceType
{
public:
    DemoDeviceType() : AudioIODeviceType ("Demo") {}
    void scanForDevices() override {}
    StringArray getDeviceNames (bool) const override       { return { "Audio Interface" }; }
    int getDefaultDeviceIndex (bool) const override        { return 0; }
    int getIndexOfDevice (AudioIODevice* d, bool) const override   { return d != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override      { return false; }
    AudioIODevice* createDevice (const String&, const String&) override   { return new DemoDevice(); }
};
} // namespace

Result writeGuitarTake (const File& wav, double bpm)
{
    Random rnd (11);
    const auto at = [bpm] (double beats) { return (int64) std::llround (beats * 60.0 / bpm * kRate); };
    std::vector<float> take ((size_t) at (16.0));
    // C (C E G C E) for two bars, then G (G B D G B): picked up and down in eighths, a little loose.
    const int chords[2][5] = { { 48, 52, 55, 60, 64 }, { 43, 47, 50, 55, 59 } };
    const int order[8] = { 0, 2, 3, 4, 3, 2, 1, 3 };
    for (int bar = 0; bar < 4; ++bar)
        for (int k = 0; k < 8; ++k)
        {
            const auto start = at (jmax (0.0, bar * 4.0 + k * 0.5 + (rnd.nextDouble() - 0.5) * 0.04));
            const double f = hz (chords[bar / 2][order[k]]);
            const auto n = at (1.2);
            const double amp = (k % 2 == 0 ? 1.0 : 0.7) * (0.85 + 0.3 * rnd.nextDouble());
            for (int64 i = 0; i < n && start + i < (int64) take.size(); ++i)
            {
                const double t = (double) i / kRate;
                double s = 0.0;
                for (int h = 1; h <= 5; ++h)
                    s += std::sin (MathConstants<double>::twoPi * f * h * t) * std::exp (-t * (1.5 + h * 1.2)) / h;
                s += 0.1 * (rnd.nextDouble() * 2.0 - 1.0) * std::exp (-t * 60.0);
                take[(size_t) (start + i)] += (float) (amp * s);
            }
        }
    return writeWav (wav, take);
}

std::unique_ptr<AudioIODeviceType> makeDeviceType()
{
    return std::make_unique<DemoDeviceType>();
}
} // namespace lathe::demo
