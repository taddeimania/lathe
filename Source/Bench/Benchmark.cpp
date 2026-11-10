#include "Benchmark.h"
#include "../Engine/Engine.h"
#include "../Engine/PluginHost.h"
#include "../Engine/AudioPool.h"
#include "../Model/Edit.h"
#include <numeric>

namespace lathe::bench
{
namespace
{
/** A device that never runs: the benchmark calls the engine's audio callback itself. */
class BenchDevice : public AudioIODevice
{
public:
    explicit BenchDevice (int block) : AudioIODevice ("Benchmark", "Benchmark"), blockSize (block) {}

    StringArray getOutputChannelNames() override  { return { "Out 1", "Out 2" }; }
    StringArray getInputChannelNames() override   { return { "In 1", "In 2" }; }
    Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    Array<int> getAvailableBufferSizes() override  { return { blockSize }; }
    int getDefaultBufferSize() override            { return blockSize; }
    String open (const BigInteger&, const BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override                         { return true; }
    void start (AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override                      { return true; }
    String getLastError() override                 { return {}; }
    int getCurrentBufferSizeSamples() override     { return blockSize; }
    double getCurrentSampleRate() override         { return 48000.0; }
    int getCurrentBitDepth() override              { return 24; }
    BigInteger getActiveOutputChannels() const override { BigInteger b; b.setRange (0, 2, true); return b; }
    BigInteger getActiveInputChannels() const override  { BigInteger b; b.setRange (0, 2, true); return b; }
    int getOutputLatencyInSamples() override       { return 0; }
    int getInputLatencyInSamples() override        { return 0; }

private:
    int blockSize;
};

/** A quiet stereo wav with a little noise, for the audio clips. */
File writeTestWav (const File& f, int length)
{
    AudioBuffer<float> b (1, length);
    Random r (42);
    for (int i = 0; i < length; ++i)
        b.setSample (0, i, 0.05f * (r.nextFloat() - 0.5f));
    f.deleteFile();
    std::unique_ptr<OutputStream> stream (f.createOutputStream());
    WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
    w->writeFromAudioSampleBuffer (b, 0, length);
    return f;
}


/** A stand-in for a heavy plug-in: a bank of sine oscillators, `voices` of them per sample.
    Costs the same every block, and its output depends on the input, so nothing is optimised
    away and the result can be compared bit for bit. */
class Burner : public AudioPluginInstance
{
public:
    Burner (int numVoices, bool instrument)
        : AudioPluginInstance (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())),
          voices (numVoices), isSynth (instrument) {}

    void fillInPluginDescription (PluginDescription& d) const override
    {
        d.name = "Burner"; d.pluginFormatName = "Test"; d.isInstrument = isSynth;
    }
    const String getName() const override         { return "Burner"; }
    void prepareToPlay (double sr, int) override
    {
        phase.assign ((size_t) voices, 0.0f);
        inc.resize ((size_t) voices);
        for (int v = 0; v < voices; ++v)
            inc[(size_t) v] = (float) (MathConstants<double>::twoPi * (110.0 + 7.0 * v) / sr);
    }
    void releaseResources() override {}
    void processBlock (AudioBuffer<float>& b, MidiBuffer&) override
    {
        auto* l = b.getWritePointer (0);
        auto* r = b.getNumChannels() > 1 ? b.getWritePointer (1) : l;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            float sum = 0.0f;
            for (size_t v = 0; v < phase.size(); ++v)
            {
                phase[v] += inc[v];
                if (phase[v] > MathConstants<float>::twoPi)
                    phase[v] -= MathConstants<float>::twoPi;
                sum += std::sin (phase[v]);
            }
            const float out = sum * (0.5f / (float) jmax (1, voices));
            l[i] = 0.5f * l[i] + out;
            r[i] = 0.5f * r[i] - out;
        }
    }
    double getTailLengthSeconds() const override  { return 0.0; }
    bool acceptsMidi() const override             { return true; }
    bool producesMidi() const override            { return false; }
    AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override               { return false; }
    int getNumPrograms() override                 { return 1; }
    int getCurrentProgram() override              { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const String&) override {}
    void getStateInformation (MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    int voices;
    bool isSynth;
    std::vector<float> phase, inc;
};

std::shared_ptr<PluginSlot> burnerSlot (int voices, bool instrument)
{
    auto slot = std::make_shared<PluginSlot>();
    slot->description.name = "Burner";
    slot->description.isInstrument = instrument;
    slot->instance = std::make_unique<Burner> (voices, instrument);
    return slot;
}

/** What a benchmark scenario puts in the project. */
using BuildScenario = std::function<void (Edit&, const File& dir)>;

/** Synths: n MIDI tracks, each with a heavy instrument (a big synth patch). */
BuildScenario synthsScenario (int n)
{
    return [n] (Edit& e, const File&)
    {
        for (int i = 0; i < n; ++i)
        {
            auto t = e.addTrack (TrackKind::midi, "Synth " + String (i + 1));
            t->plugins.push_back (burnerSlot (256, true));
        }
    };
}

/** A band: 8 audio tracks playing clips through a light plug-in each (EQ/compressor-like),
    4 synths, two returns with a heavier plug-in (reverb/delay-like), every track sent to A. */
BuildScenario bandScenario()
{
    return [] (Edit& e, const File& dir)
    {
        auto wav = writeTestWav (dir.getChildFile ("bench.wav"), 48000 * 4);
        auto ret = e.addTrack (TrackKind::bus, "A Reverb");
        ret->plugins.push_back (burnerSlot (192, false));
        auto ret2 = e.addTrack (TrackKind::bus, "B Delay");
        ret2->plugins.push_back (burnerSlot (96, false));
        for (int i = 0; i < 8; ++i)
        {
            auto t = e.addTrack (TrackKind::audio, "Audio " + String (i + 1));
            t->input.channel = -1;
            Clip c;
            c.id = e.newId();
            c.kind = ClipKind::audio;
            c.file = wav;
            c.start = 0.0;
            c.length = 8.0;
            t->clips.push_back (c);
            t->plugins.push_back (burnerSlot (32, false));
            t->getSend (ret->id, true)->levelDb = -12.0f;
        }
        for (int i = 0; i < 4; ++i)
        {
            auto t = e.addTrack (TrackKind::midi, "Synth " + String (i + 1));
            t->plugins.push_back (burnerSlot (256, true));
            t->getSend (ret->id, true)->levelDb = -12.0f;
            t->getSend (ret2->id, true)->levelDb = -18.0f;
        }
    };
}

/** Chains: n synths, each recorded into an audio track that takes it as its input (armed and
    monitoring, as when you print a synth) and runs its own effect. Tests the dependency levels. */
BuildScenario chainScenario (int n)
{
    return [n] (Edit& e, const File&)
    {
        for (int i = 0; i < n; ++i)
        {
            auto synth = e.addTrack (TrackKind::midi, "Synth " + String (i + 1));
            synth->plugins.push_back (burnerSlot (192, true));
            auto print = e.addTrack (TrackKind::audio, "Print " + String (i + 1));
            print->input.channel = -1;
            print->input.track = synth->id;
            print->monitor = (int) Monitor::in;
            print->plugins.push_back (burnerSlot (128, false));
        }
    };
}

/** Every mixing path at once, for the bit-identical checks: pre- and post-fader sends, a
    group bus, a track straight to a hardware output, a cue mix, a synth printed to audio,
    live input, mute, pan and volume. */
BuildScenario routingScenario()
{
    return [] (Edit& e, const File& dir)
    {
        auto wav = writeTestWav (dir.getChildFile ("routing.wav"), 48000 * 4);
        auto clipOn = [&] (std::shared_ptr<Track> t, double start)
        {
            Clip c;
            c.id = e.newId();
            c.kind = ClipKind::audio;
            c.file = wav;
            c.start = start;
            c.length = 6.0;
            t->clips.push_back (c);
        };
        auto ret = e.addTrack (TrackKind::bus, "A Reverb");
        ret->plugins.push_back (burnerSlot (8, false));

        auto kick = e.addTrack (TrackKind::audio, "Kick");
        auto snare = e.addTrack (TrackKind::audio, "Snare");
        for (auto& t : { kick, snare })
        {
            t->input.channel = -1;
            clipOn (t, t == kick ? 0.0 : 0.25);
            t->plugins.push_back (burnerSlot (4, false));
        }
        auto drums = e.createGroup ({ kick->id, snare->id }, "Drums");
        auto drumBus = e.makeGroupBus (drums->id);
        drumBus->plugins.push_back (burnerSlot (6, false));
        snare->getSend (ret->id, true)->levelDb = -6.0f;
        snare->getSend (ret->id, true)->preFader = true;
        snare->volumeDb = -9.0f;

        auto vox = e.addTrack (TrackKind::audio, "Vox");
        vox->input = { 0, false };
        vox->monitor = (int) Monitor::in;     // live input
        vox->pan = -0.4f;
        vox->getSend (ret->id, true)->levelDb = -3.0f;
        vox->plugins.push_back (burnerSlot (4, false));

        auto click = e.addTrack (TrackKind::audio, "To hardware");
        click->input.channel = -1;
        clipOn (click, 0.5);
        click->output.kind = OutputRoute::Kind::hardware;
        click->output.channel = 0;

        auto muted = e.addTrack (TrackKind::audio, "Muted");
        muted->input.channel = -1;
        clipOn (muted, 0.0);
        muted->mute = true;

        auto synth = e.addTrack (TrackKind::midi, "Synth");
        synth->plugins.push_back (burnerSlot (12, true));
        synth->getSend (ret->id, true)->levelDb = -10.0f;
        auto print = e.addTrack (TrackKind::audio, "Synth print");
        print->input.channel = -1;
        print->input.track = synth->id;
        print->monitor = (int) Monitor::in;
        print->volumeDb = -4.5f;
        print->plugins.push_back (burnerSlot (5, false));

        auto cue = e.addCue ("In-ears", 0);
        e.cueLevel (*cue, *vox)->levelDb = 3.0f;
        e.cueLevel (*cue, *kick)->pan = 0.7f;
        e.cueLevel (*cue, *ret)->levelDb = -6.0f;
    };
}

/** A random session, for checking that any routing renders the same on any number of threads:
    audio and MIDI tracks (light plug-ins, clips, live input), tracks taking other tracks as
    their input (chains up to a few deep), returns and group buses, pre- and post-fader sends,
    hardware outputs, a cue mix, mutes and solos. The same seed always makes the same session. */
BuildScenario randomScenario (int seed)
{
    return [seed] (Edit& e, const File& dir)
    {
        Random r (seed);
        auto wav = writeTestWav (dir.getChildFile ("random.wav"), 48000 * 2);
        auto chance = [&r] (float p) { return r.nextFloat() < p; };

        std::vector<std::shared_ptr<Track>> returns;
        for (int b = r.nextInt (3); --b >= 0;)
        {
            auto ret = e.addTrack (TrackKind::bus, "Return " + String (b));
            if (chance (0.7f))
                ret->plugins.push_back (burnerSlot (1 + r.nextInt (6), false));
            if (chance (0.15f))
                ret->mute = true;
            returns.push_back (ret);
        }

        std::vector<std::shared_ptr<Track>> made;
        const int n = 2 + r.nextInt (11);
        for (int i = 0; i < n; ++i)
        {
            const bool midi = chance (0.4f);
            auto t = e.addTrack (midi ? TrackKind::midi : TrackKind::audio, "T" + String (i));
            if (midi)
                t->plugins.push_back (burnerSlot (1 + r.nextInt (8), true));
            else
            {
                t->input.channel = -1;
                if (chance (0.6f))
                {
                    Clip c;
                    c.id = e.newId();
                    c.kind = ClipKind::audio;
                    c.file = wav;
                    c.start = r.nextFloat() * 2.0;
                    c.length = 0.5 + r.nextFloat() * 3.0;
                    c.gainDb = -6.0f * r.nextFloat();
                    t->clips.push_back (c);
                }
                if (chance (0.25f))
                {
                    t->input = { r.nextInt (2), chance (0.5f) };
                    t->monitor = (int) Monitor::in;
                }
                for (int k = r.nextInt (3); --k >= 0;)
                    t->plugins.push_back (burnerSlot (1 + r.nextInt (5), false));
            }
            // Take another (earlier) track as the input: audio from anything, MIDI from MIDI.
            if (! made.empty() && chance (0.35f))
            {
                auto src = made[(size_t) r.nextInt ((int) made.size())];
                if (! midi || src->isMidi())
                {
                    t->input.channel = -1;
                    t->input.track = src->id;
                    t->monitor = chance (0.7f) ? (int) Monitor::in : (int) Monitor::automatic;
                    if (chance (0.3f))
                        t->arm = true;
                }
            }
            t->volumeDb = -12.0f * r.nextFloat();
            t->pan = 2.0f * r.nextFloat() - 1.0f;
            if (chance (0.1f)) t->mute = true;
            if (chance (0.08f)) t->solo = true;
            for (auto& ret : returns)
                if (chance (0.5f))
                {
                    auto send = t->getSend (ret->id, true);
                    send->levelDb = -20.0f * r.nextFloat();
                    send->preFader = chance (0.3f);
                }
            if (chance (0.12f))
            {
                t->output.kind = OutputRoute::Kind::hardware;
                t->output.channel = 0;
                t->output.stereo = chance (0.5f);
            }
            made.push_back (t);
        }

        if (made.size() >= 3 && chance (0.5f))
        {
            auto group = e.createGroup ({ made[0]->id, made[1]->id }, "G");
            if (chance (0.7f))
                if (auto bus = e.makeGroupBus (group->id))
                    bus->plugins.push_back (burnerSlot (1 + r.nextInt (4), false));
        }

        if (chance (0.5f))
        {
            auto cue = e.addCue ("Cue", 0);
            for (auto& t : made)
                if (chance (0.5f))
                    e.cueLevel (*cue, *t)->levelDb = -6.0f * r.nextFloat();
        }
    };
}

struct BenchRow
{
    String scenario;
    int block = 0, threads = 1;
    double meanUs = 0.0, p99Us = 0.0, deadlineUs = 0.0;
    float checksum = 0.0f;
};

/** Runs `blocks` callbacks of one scenario and times each one. threads: see Engine::setProcessingThreads
    (when the engine has it). */
BenchRow benchScenario (const String& name, const BuildScenario& build, int block, int threads, const File& dir, int blocks = 1500)
{
    PropertiesFile::Options opts;
    opts.applicationName = "LatheBench";
    opts.filenameSuffix = ".settings";
    opts.folderName = dir.getFullPathName();
    PropertiesFile props (dir.getChildFile ("bench.settings"), opts);
    PluginHost host (props);
    AudioPool pool;
    Edit edit;
    Engine engine (host, pool, props);
    edit.workDir = dir;
    edit.sampleRate = 48000.0;
    edit.tempo = 120.0;
    engine.setEdit (&edit);
    engine.setProcessingThreads (threads);
    BenchDevice device (block);
    engine.audioDeviceAboutToStart (&device);

    build (edit, dir);
    engine.rebuild();
    edit.insertMarker = 0.0;
    engine.play();

    AudioBuffer<float> outs (2, block), ins (2, block);
    ins.clear();
    std::vector<double> times;
    times.reserve ((size_t) blocks);
    float checksum = 0.0f;
    const double tickUs = 1.0e6 / (double) Time::getHighResolutionTicksPerSecond();
    for (int b = 0; b < blocks + 200; ++b)
    {
        const auto t0 = Time::getHighResolutionTicks();
        engine.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, block, {});
        const auto t1 = Time::getHighResolutionTicks();
        if (b >= 200)   // warm up first
        {
            times.push_back ((double) (t1 - t0) * tickUs);
            checksum += outs.getSample (0, block / 2) + outs.getSample (1, block - 1);
        }
    }
    engine.stop();
    engine.audioDeviceStopped();
    engine.setEdit (nullptr);

    BenchRow r;
    r.scenario = name;
    r.block = block;
    r.threads = threads;
    r.deadlineUs = 1.0e6 * block / 48000.0;
    r.meanUs = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
    std::sort (times.begin(), times.end());
    r.p99Us = times[(size_t) (0.99 * (double) (times.size() - 1))];
    r.checksum = checksum;
    return r;
}

/** Offline export of a scenario: wall time to render `bars` bars, as a % of the music's length. */
BenchRow benchExport (const String& name, const BuildScenario& build, int bars, int threads, const File& dir)
{
    PropertiesFile::Options opts;
    opts.applicationName = "LatheBench";
    opts.filenameSuffix = ".settings";
    opts.folderName = dir.getFullPathName();
    PropertiesFile props (dir.getChildFile ("bench.settings"), opts);
    PluginHost host (props);
    AudioPool pool;
    Edit edit;
    Engine engine (host, pool, props);
    edit.workDir = dir;
    edit.sampleRate = 48000.0;
    edit.tempo = 120.0;
    engine.setEdit (&edit);
    engine.setProcessingThreads (threads);
    BenchDevice device (512);
    engine.audioDeviceAboutToStart (&device);
    build (edit, dir);
    engine.rebuild();

    Engine::ExportSettings es;
    es.mix = true;
    es.stems = false;
    es.startBeat = 0.0;
    es.endBeat = 4.0 * bars;
    es.tailSeconds = 0.0;
    es.bitDepth = 32;
    es.folder = dir.getChildFile ("export");
    es.folder.deleteRecursively();
    es.folder.createDirectory();
    es.baseName = "bench";
    StringArray written;
    const auto t0 = Time::getMillisecondCounterHiRes();
    const auto result = engine.exportAudio (es, [] (double) { return true; }, written);
    const auto ms = Time::getMillisecondCounterHiRes() - t0;
    engine.audioDeviceStopped();
    engine.setEdit (nullptr);

    BenchRow r;
    r.scenario = name;
    r.block = 0;
    r.threads = threads;
    r.deadlineUs = 1.0e6 * es.endBeat * 0.5;   // 120 bpm: half a second a beat
    r.meanUs = r.p99Us = result.wasOk() ? ms * 1000.0 : -1.0;
    if (result.wasOk() && ! written.isEmpty())
    {
        // checksum: the sum of a stretch of the mix file
        AudioFormatManager fm;
        fm.registerBasicFormats();
        if (auto reader = std::unique_ptr<AudioFormatReader> (fm.createReaderFor (File (written[0]))))
        {
            AudioBuffer<float> b (2, 48000);
            reader->read (&b, 0, 48000, 24000, true, true);
            double sum = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    sum += b.getSample (ch, i);
            r.checksum = (float) sum;
        }
    }
    return r;
}


/** Renders `blocks` blocks with a deterministic live input and returns every output sample. */
std::vector<float> renderOutput (const BuildScenario& build, int block, int threads, const File& dir, int blocks)
{
    PropertiesFile::Options opts;
    opts.applicationName = "LatheBench";
    opts.filenameSuffix = ".settings";
    opts.folderName = dir.getFullPathName();
    PropertiesFile props (dir.getChildFile ("bench.settings"), opts);
    PluginHost host (props);
    AudioPool pool;
    Edit edit;
    Engine engine (host, pool, props);
    edit.workDir = dir;
    edit.sampleRate = 48000.0;
    edit.tempo = 120.0;
    engine.setEdit (&edit);
    engine.setProcessingThreads (threads);
    engine.setAlwaysShareWork (threads != 1);   // every block through the parallel path
    BenchDevice device (block);
    engine.audioDeviceAboutToStart (&device);
    build (edit, dir);
    engine.rebuild();
    edit.insertMarker = 0.0;
    engine.play();

    std::vector<float> all;
    all.reserve ((size_t) (blocks * block * 2));
    AudioBuffer<float> outs (2, block), ins (2, block);
    Random r (7);
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
                ins.setSample (ch, i, 0.2f * (r.nextFloat() - 0.5f));
        engine.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, block, {});
        for (int ch = 0; ch < 2; ++ch)
            all.insert (all.end(), outs.getReadPointer (ch), outs.getReadPointer (ch) + block);
    }
    engine.stop();
    engine.audioDeviceStopped();
    engine.setEdit (nullptr);
    return all;
}

String modeName (const String& mode)
{
    return mode == "1" ? String ("1 core") : String ("all cores (") + String (Engine::autoProcessingThreads()) + ")";
}
} // namespace

int dump (const File& folder, int threads, std::ostream& out)
{
    auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-benchmark-dump");
    dir.deleteRecursively();
    dir.createDirectory();
    folder.createDirectory();

    struct Scenario { String name; BuildScenario build; };
    const std::vector<Scenario> scenarios {
        { "routing", routingScenario() },
        { "band",    bandScenario() },
        { "chain",   chainScenario (3) },
        { "synths",  synthsScenario (5) },
    };
    for (auto& sc : scenarios)
        for (int block : { 64, 128, 256, 333, 512 })   // 333: not a power of two
        {
            const auto samples = renderOutput (sc.build, block, threads, dir, 40000 / block + 20);
            const auto f = folder.getChildFile (sc.name + "-" + String (block) + ".f32");
            f.replaceWithData (samples.data(), samples.size() * sizeof (float));
            double energy = 0.0;
            for (auto v : samples)
                energy += (double) v * v;
            out << f.getFileName() << "  " << samples.size() << " samples, energy " << String (energy, 6) << "\n";
        }
    // Offline export (mix and stems) of the routing session, which goes through the same render path.
    for (int stems = 0; stems < 2; ++stems)
    {
        PropertiesFile::Options opts;
        opts.applicationName = "LatheBench";
        opts.filenameSuffix = ".settings";
        opts.folderName = dir.getFullPathName();
        PropertiesFile props (dir.getChildFile ("bench.settings"), opts);
        PluginHost host (props);
        AudioPool pool;
        Edit edit;
        Engine engine (host, pool, props);
        edit.workDir = dir;
        edit.sampleRate = 48000.0;
        edit.tempo = 120.0;
        engine.setEdit (&edit);
        engine.setProcessingThreads (threads);
        engine.setAlwaysShareWork (threads != 1);
        BenchDevice device (512);
        engine.audioDeviceAboutToStart (&device);
        routingScenario() (edit, dir);
        engine.rebuild();
        Engine::ExportSettings es;
        es.mix = true;
        es.stems = stems == 1;
        es.endBeat = 8.0;
        es.tailSeconds = 0.5;
        es.bitDepth = 32;
        es.folder = folder.getChildFile (stems ? "export-stems" : "export-mix");
        es.folder.deleteRecursively();
        es.folder.createDirectory();
        es.baseName = "routing";
        StringArray written;
        const auto r = engine.exportAudio (es, [] (double) { return true; }, written);
        engine.audioDeviceStopped();
        engine.setEdit (nullptr);
        out << es.folder.getFileName() << "  " << (r.wasOk() ? String (written.size()) + " files" : r.getErrorMessage()) << "\n";
    }

    dir.deleteRecursively();
    return 0;
}

int checkRandom (int seeds, int threads, std::ostream& out)
{
    auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-benchmark-random");
    dir.deleteRecursively();
    dir.createDirectory();
    int failures = 0;
    for (int seed = 1; seed <= seeds; ++seed)
    {
        const int block = seed % 3 == 0 ? 333 : (seed % 3 == 1 ? 128 : 64);
        const auto one = renderOutput (randomScenario (seed), block, 1, dir, 24);
        const auto many = renderOutput (randomScenario (seed), block, threads, dir, 24);
        if (one.size() != many.size() || std::memcmp (one.data(), many.data(), one.size() * sizeof (float)) != 0)
        {
            ++failures;
            out << "  random session " << seed << " (" << block << " samples): differs on " << threads << " threads\n";
        }
    }
    dir.deleteRecursively();
    return failures;
}

int run (const File& csv, const StringArray& modes, std::ostream& out)
{
    auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-benchmark");
    dir.deleteRecursively();
    dir.createDirectory();

    struct Scenario { String name; BuildScenario build; };
    const std::vector<Scenario> scenarios {
        { "1 synth",                                synthsScenario (1) },
        { "2 synths",                               synthsScenario (2) },
        { "4 synths",                               synthsScenario (4) },
        { "8 synths",                               synthsScenario (8) },
        { "12 synths",                              synthsScenario (12) },
        { "Band (8 audio + 4 synths + 2 returns)",  bandScenario() },
        { "4 synths printed to audio (chained)",    chainScenario (4) },
    };

    out << "Lathe benchmark: " << SystemStats::getCpuModel() << ", " << SystemStats::getNumPhysicalCpus()
        << " cores. Each line is the share of the audio deadline used (mean and 99th percentile);"
           " over 100% means dropouts.\n\n";
    out << String ("scenario").paddedRight (' ', 40) << " buffer  " << String ("cores").paddedRight (' ', 16) << "  mean%    p99%\n";

    String text = "scenario,buffer_samples,mode,threads,deadline_us,mean_us,p99_us,mean_load_pct,p99_load_pct,checksum\n";
    auto row = [&] (const BenchRow& r, const String& mode)
    {
        const double meanPct = 100.0 * r.meanUs / r.deadlineUs, p99Pct = 100.0 * r.p99Us / r.deadlineUs;
        text << "\"" << r.scenario << "\"," << r.block << "," << mode << "," << (mode == "1" ? 1 : Engine::autoProcessingThreads()) << ","
             << String (r.deadlineUs, 1) << "," << String (r.meanUs, 2) << "," << String (r.p99Us, 2) << ","
             << String (meanPct, 2) << "," << String (p99Pct, 2) << "," << String (r.checksum, 6) << "\n";
        return std::pair<double, double> { meanPct, p99Pct };
    };

    for (auto& sc : scenarios)
        for (int block : { 128, 256, 512 })
            for (auto& mode : modes)
            {
                const auto r = benchScenario (sc.name, sc.build, block, mode == "1" ? 1 : 0, dir);
                const auto [mean, p99] = row (r, mode);
                out << sc.name.paddedRight (' ', 40) << String (block).paddedLeft (' ', 7) << "  " << modeName (mode).paddedRight (' ', 16)
                    << String (mean, 1).paddedLeft (' ', 6) << "  " << String (p99, 1).paddedLeft (' ', 6) << "\n" << std::flush;
            }

    for (auto& mode : modes)
    {
        const auto r = benchExport ("Export: band, 32 bars", bandScenario(), 32, mode == "1" ? 1 : 0, dir);
        const auto [pct, unused] = row (r, mode);
        ignoreUnused (unused);
        out << r.scenario.paddedRight (' ', 40) << "      -  " << modeName (mode).paddedRight (' ', 16)
            << String (pct, 1).paddedLeft (' ', 6) << "  (" << String (r.meanUs / 1000.0, 0) << " ms for "
            << String (r.deadlineUs / 1.0e6, 0) << " s of music)\n" << std::flush;
    }

    csv.replaceWithText (text);
    out << "\nWrote " << csv.getFullPathName() << "\n";
    dir.deleteRecursively();
    return 0;
}
} // namespace lathe::bench
