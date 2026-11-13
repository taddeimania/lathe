// Headless engine tests: a fake audio device drives the real audio callback.
// Build with -DLATHE_BUILD_TESTS=ON and run LatheTests.

#include "Engine/Engine.h"
#include "Devices/Device.h"
#include "Devices/Instruments.h"
#include "Devices/Reverb/Types.h"
#include "Engine/ClipJoin.h"
#include "Patcher/Updater.h"
#include "Cloud/ProjectDiff.h"
#include "Cloud/Revisions.h"
#include "Cloud/Packing.h"
#include "Cloud/Milestones.h"
#include "Assistant/AssistantServer.h"
#include "Assistant/AssistantTools.h"
#include "Assistant/MixAnalysis.h"
#include "Assistant/SessionWatch.h"
#include "Assistant/AssistantService.h"
#include "Bench/Benchmark.h"
#include "UI/CloudUI.h"
#include "Demo/Welcome.h"
#include "UI/CloudLinks.h"
#include "UI/UndoHistory.h"
#include "Compare/Compare.h"
#include "Host/RemotePlugin.h"
#if ! JUCE_WINDOWS
 #include <sys/stat.h>
#endif
#if ! JUCE_WINDOWS
 #include <unistd.h>
#endif

using namespace lathe;

namespace
{
int failures = 0;

void check (bool ok, const String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

/** Minimal device: fixed rate/block, reports latencies, never actually runs. */
class FakeDevice : public AudioIODevice
{
public:
    FakeDevice (int ins, int outs, int inLat, int outLat, int block = 256)
        : AudioIODevice ("Fake", "Fake"), numIns (ins), numOuts (outs), inLatency (inLat), outLatency (outLat), blockSize (block) {}

    StringArray getOutputChannelNames() override  { StringArray s; for (int i = 0; i < numOuts; ++i) s.add ("Out " + String (i + 1)); return s; }
    StringArray getInputChannelNames() override   { StringArray s; for (int i = 0; i < numIns; ++i) s.add ("In " + String (i + 1)); return s; }
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
    BigInteger getActiveOutputChannels() const override { BigInteger b; b.setRange (0, numOuts, true); return b; }
    BigInteger getActiveInputChannels() const override  { BigInteger b; b.setRange (0, numIns, true); return b; }
    int getOutputLatencyInSamples() override       { return outLatency; }
    int getInputLatencyInSamples() override        { return inLatency; }

private:
    int numIns, numOuts, inLatency, outLatency, blockSize;
};

/** Pure delay "plug-in" that reports its latency, for testing compensation. */
/** A MIDI generator (like an arpeggiator): plays one note on its own, from
    sample `on` to sample `off` of the song, whatever comes in. */
class NoteGenerator : public AudioPluginInstance
{
public:
    NoteGenerator (int64 onAt, int64 offAt)
        : AudioPluginInstance (BusesProperties().withOutput ("Out", AudioChannelSet::stereo())), on (onAt), off (offAt) {}

    void fillInPluginDescription (PluginDescription& d) const override { d.name = "Gen"; d.pluginFormatName = "Test"; }
    const String getName() const override         { return "Gen"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (AudioBuffer<float>& b, MidiBuffer& midi) override
    {
        midi.clear();
        auto* head = getPlayHead();
        const auto pos = head != nullptr ? head->getPosition() : Optional<AudioPlayHead::PositionInfo>();
        if (! pos.hasValue() || ! pos->getIsPlaying() || ! pos->getTimeInSamples().hasValue())
            return;
        const int64 start = *pos->getTimeInSamples();
        if (on >= start && on < start + b.getNumSamples())   midi.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), (int) (on - start));
        if (off >= start && off < start + b.getNumSamples()) midi.addEvent (MidiMessage::noteOff (1, 64), (int) (off - start));
    }
    double getTailLengthSeconds() const override  { return 0; }
    bool acceptsMidi() const override             { return true; }
    bool producesMidi() const override            { return true; }
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
    int64 on, off;
};

/** A "synth": swallows the notes it gets (as instruments do). */
class NoteEater : public NoteGenerator
{
public:
    NoteEater() : NoteGenerator (-1, -1) {}
    bool producesMidi() const override { return false; }
    void processBlock (AudioBuffer<float>&, MidiBuffer& midi) override   { midi.clear(); }
};

/** A "synth" that counts the notes it gets. */
class NoteCounter : public NoteEater
{
public:
    void processBlock (AudioBuffer<float>&, MidiBuffer& midi) override
    {
        for (const auto m : midi)
        {
            const auto msg = m.getMessage();
            if (msg.isNoteOn())       { ++ons; lastNote = msg.getNoteNumber(); lastVelocity = msg.getVelocity(); }
            else if (msg.isNoteOff()) ++offs;
        }
        midi.clear();
    }
    std::atomic<int> ons { 0 }, offs { 0 }, lastNote { -1 }, lastVelocity { 0 };
};

/** Takes a steady few hundred microseconds per block, like a real plug-in would. */
class Spinner : public NoteEater
{
public:
    explicit Spinner (double microseconds) : us (microseconds) {}
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override
    {
        const auto until = Time::getHighResolutionTicks() + (int64) (us * 1.0e-6 * (double) Time::getHighResolutionTicksPerSecond());
        while (Time::getHighResolutionTicks() < until) {}
    }
    double us;
};

class LatencyPlugin : public AudioPluginInstance
{
public:
    explicit LatencyPlugin (int samples)
        : AudioPluginInstance (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())),
          delay (samples)
    {
        setLatencySamples (samples);
    }

    void fillInPluginDescription (PluginDescription& d) const override { d.name = "Latency"; d.pluginFormatName = "Test"; }
    const String getName() const override         { return "Latency"; }
    void prepareToPlay (double, int) override     { ring.setSize (2, delay + 1); ring.clear(); pos = 0; }
    void releaseResources() override {}
    void processBlock (AudioBuffer<float>& b, MidiBuffer&) override
    {
        const int size = delay + 1;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                ring.setSample (ch, pos, b.getSample (ch, i));
                b.setSample (ch, i, ring.getSample (ch, (pos + 1) % size));
            }
            pos = (pos + 1) % size;
        }
    }
    double getTailLengthSeconds() const override  { return 0; }
    bool acceptsMidi() const override             { return false; }
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
    int delay, pos = 0;
    AudioBuffer<float> ring;
};

File writeImpulseWav (const File& f, int length, int impulseAt)
{
    AudioBuffer<float> b (1, length);
    b.clear();
    b.setSample (0, impulseAt, 0.5f);
    f.deleteFile();
    std::unique_ptr<OutputStream> stream (f.createOutputStream());
    WavAudioFormat wav;
    auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
    w->writeFromAudioSampleBuffer (b, 0, length);
    return f;
}

/** Runs n callbacks, returns the left output channel concatenated. */
std::vector<float> runCallbacks (Engine& engine, int blocks, const AudioBuffer<float>* input = nullptr)
{
    std::vector<float> out;
    AudioBuffer<float> outs (2, 256), ins (2, 256);
    for (int b = 0; b < blocks; ++b)
    {
        ins.clear();
        if (input != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                ins.copyFrom (ch, 0, *input, ch, b * 256, 256);
        engine.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, 256, {});
        out.insert (out.end(), outs.getReadPointer (0), outs.getReadPointer (0) + 256);
    }
    return out;
}

int peakIndex (const std::vector<float>& v)
{
    int best = -1;
    float bestValue = 0.0f;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::abs (v[i]) > bestValue + 1.0e-6f) { bestValue = std::abs (v[i]); best = (int) i; }
    return best;
}

} // namespace

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;
    if (argc >= 2 && String (argv[1]) == "--random")   // LatheTests --random [seeds] [threads]
    {
        const int seeds = argc >= 3 ? String (argv[2]).getIntValue() : 300, threads = argc >= 4 ? String (argv[3]).getIntValue() : 4;
        const int failures = lathe::bench::checkRandom (seeds, threads, std::cout);
        std::cout << seeds << " random sessions on " << threads << " threads: " << failures << " differ from one thread" << std::endl;
        return failures == 0 ? 0 : 1;
    }
    if (argc >= 3 && String (argv[1]) == "--dump")    // LatheTests --dump <folder> [threads]
        return lathe::bench::dump (File::getCurrentWorkingDirectory().getChildFile (argv[2]), argc >= 4 ? String (argv[3]).getIntValue() : 1, std::cout);
    if (argc >= 2 && String (argv[1]) == "--bench")   // LatheTests --bench [out.csv] [1,full]
    {
        const File csv = File::getCurrentWorkingDirectory().getChildFile (argc >= 3 ? argv[2] : "lathe-bench.csv");
        const auto modes = StringArray::fromTokens (argc >= 4 ? argv[3] : "1,full", ",", "");
        return lathe::bench::run (csv, modes, std::cout);
    }
    auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-tests");
    dir.deleteRecursively();
    dir.createDirectory();

    PropertiesFile::Options opts;
    opts.applicationName = "LatheTests";
    opts.filenameSuffix = ".settings";
    opts.folderName = dir.getFullPathName();
    PropertiesFile props (dir.getChildFile ("test.settings"), opts);

    PluginHost host (props);
    AudioPool pool;
    Edit edit;
    Engine engine (host, pool, props);

    edit.workDir = dir;
    edit.sampleRate = 48000.0;
    edit.tempo = 120.0;   // 24000 samples per beat at 48 kHz
    engine.setEdit (&edit);

    FakeDevice device (2, 2, 100, 200);
    engine.audioDeviceAboutToStart (&device);

    std::cout << "Playback" << std::endl;
    {
        auto wav = writeImpulseWav (dir.getChildFile ("impulse.wav"), 48000, 0);
        auto t = edit.addTrack (TrackKind::audio, "A");
        t->input.channel = -1;
        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::audio;
        c.file = wav;
        c.start = 0.5;     // 12000 samples
        c.length = 1.0;
        t->clips.push_back (c);

        // Impulse 1000 samples into the file (clip edges have a short fade), clip at
        // beat 0.5 -> it should come out at 12000 + 1000.
        t->clips[0].file = writeImpulseWav (dir.getChildFile ("impulse2.wav"), 48000, 1000);
        ignoreUnused (wav);
        engine.rebuild();
        edit.insertMarker = 0.0;
        engine.play();
        auto out = runCallbacks (engine, 100);
        engine.stop();
        check (peakIndex (out) == 13000, "audio clip plays at the right sample (got " + String (peakIndex (out)) + ", want 13000)");
        check (std::abs (out[13000] - 0.5f) < 0.01f, "unity gain through track and master (got " + String (out[13000]) + ")");
    }

    std::cout << "Plug-in delay compensation" << std::endl;
    {
        // Second track plays the same clip; first track gets a 300-sample latency plug-in.
        auto t2 = edit.addTrack (TrackKind::audio, "B");
        t2->input.channel = -1;
        t2->clips.push_back (edit.tracks[0]->clips[0]);
        t2->clips.back().id = edit.newId();

        auto slot = std::make_shared<PluginSlot>();
        slot->instance = std::make_unique<LatencyPlugin> (300);
        slot->description.name = "Latency";
        engine.prepareSlot (*slot);
        edit.tracks[0]->plugins.push_back (slot);
        engine.rebuild();
        check (engine.getGraphLatency() == 300, "graph latency is 300 (got " + String (engine.getGraphLatency()) + ")");

        engine.play();
        auto out = runCallbacks (engine, 100);
        engine.stop();
        const int p = peakIndex (out);
        check (p == 13300, "both tracks line up after compensation (peak at " + String (p) + ", want 13300)");
        check (std::abs (out[(size_t) jmax (0, p)] - 1.0f) < 0.01f, "the two impulses sum to 1.0 (got " + String (out[(size_t) jmax (0, p)]) + ")");
    }

    std::cout << "Mute, solo, fader" << std::endl;
    {
        edit.tracks[1]->mute = true;
        engine.play();
        auto out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 0.5f) < 0.01f, "muting one track halves the sum");
        edit.tracks[1]->mute = false;

        edit.tracks[0]->solo = true;
        engine.play();
        out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 0.5f) < 0.01f, "solo silences the other track");
        edit.tracks[0]->solo = false;

        edit.master->volumeDb = -6.0206f;
        engine.play();
        runCallbacks (engine, 2);   // let the fader ramp settle
        engine.stop();
        engine.play();
        out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 0.5f) < 0.01f, "master at -6 dB halves the level (got " + String (out[13300]) + ")");
        edit.master->volumeDb = 0.0f;
    }

    std::cout << "Sends and returns" << std::endl;
    {
        auto bus = edit.addTrack (TrackKind::bus, "A Return");
        edit.tracks[1]->getSend (bus->id, true)->levelDb = 0.0f;
        edit.tracks[1]->mute = false;
        edit.tracks[0]->mute = true;
        engine.rebuild();
        engine.play();
        auto out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 1.0f) < 0.01f, "post-fader send adds the track again via the return (got " + String (out[13300]) + ")");
        edit.tracks[0]->mute = false;
        edit.removeTrack (bus->id);
        engine.rebuild();
    }

    std::cout << "Track groups" << std::endl;
    {
        // tracks[0] (with the latency plug-in) and tracks[1] both play the impulse at 13300.
        auto g = edit.createGroup ({ edit.tracks[0]->id });
        check (g != nullptr && edit.tracks[0]->groupId == g->id, "group created");
        engine.rebuild();

        g->mute = true;
        engine.play();
        auto out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 0.5f) < 0.01f, "group mute silences its member (got " + String (out[13300]) + ")");
        check (! edit.tracks[0]->mute.load() && edit.isEffectivelyMuted (*edit.tracks[0]), "member's own mute is untouched");
        g->mute = false;

        g->solo = true;
        engine.play();
        out = runCallbacks (engine, 100);
        engine.stop();
        check (std::abs (out[13300] - 0.5f) < 0.01f, "group solo plays only its members (got " + String (out[13300]) + ")");
        g->solo = false;

        edit.toggleGroupArm (g->id);
        check (edit.tracks[0]->arm.load() && edit.isGroupArmed (g->id), "arm all arms the members");
        edit.toggleGroupArm (g->id);
        check (! edit.tracks[0]->arm.load(), "and disarms them again");

        edit.ungroup (g->id);
        check (edit.groups.empty() && edit.tracks[0]->groupId == -1, "ungroup keeps the tracks, removes the group");
        engine.rebuild();
    }

    {
        Edit e;
        auto a = e.addTrack (TrackKind::audio, "A");
        auto b = e.addTrack (TrackKind::audio, "B");
        auto c = e.addTrack (TrackKind::midi, "C");
        auto d = e.addTrack (TrackKind::audio, "D");
        auto bus = e.addTrack (TrackKind::bus);
        auto g = e.createGroup ({ a->id, d->id, bus->id }, "Drums");
        check (g != nullptr && e.groupMembers (g->id).size() == 2, "returns can't join a group");
        check (e.tracks[0] == a && e.tracks[1] == d && e.tracks[2] == b, "members are moved next to each other");

        e.setTrackGroup (c->id, g->id);
        check (e.tracks[2] == c && c->groupId == g->id, "adding a track puts it after the other members");

        g->mute = true;
        g->collapsed = true;
        auto json = JSON::toString (e.toVar (false));
        Edit loaded;
        loaded.loadFrom (JSON::parse (json), File(), [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        auto lg = loaded.groups.empty() ? nullptr : loaded.groups[0];
        check (lg != nullptr && lg->name == "Drums" && lg->mute.load() && lg->collapsed && loaded.groupMembers (lg->id).size() == 3,
               "groups survive save and load");

        e.removeTrack (a->id);
        e.removeTrack (d->id);
        e.removeTrack (c->id);
        check (e.groups.empty(), "a group disappears with its last track");
    }

    std::cout << "Loop" << std::endl;
    {
        edit.loopEnabled = true;
        edit.loopStart = 0.0;
        edit.loopEnd = 1.0;   // 24000 samples
        engine.rebuild();
        edit.insertMarker = 0.0;
        engine.play();
        auto out = runCallbacks (engine, 200);   // 51200 samples
        engine.stop();
        int hits = 0;
        for (size_t i = 1; i < out.size(); ++i)
            if (std::abs (out[i]) > 0.9f)
                ++hits;
        check (hits == 2, "loop of one beat plays the impulse twice in 51200 samples (got " + String (hits) + ")");
        edit.loopEnabled = false;
        engine.rebuild();
    }

    std::cout << "Auto-stop (play to a bar, play the loop N times)" << std::endl;
    {
        edit.insertMarker = 0.0;
        engine.play();
        engine.setAutoStop (0.5, 0);   // 12000 samples: not a block boundary
        check (std::abs (engine.getAutoStopBeats() - 0.5) < 1.0e-9, "the stop point reads back in beats");
        runCallbacks (engine, 100);
        check (! engine.isPlaying() && std::abs (engine.getPlayheadBeats() - 0.5) < 1.0e-9 && engine.getAutoStopBeats() < 0.0,
               "stops by itself exactly on the stop sample, mid-block (at " + String (engine.getPlayheadBeats(), 6) + " beats)");

        engine.play();
        engine.setAutoStop (0.5, 0);
        engine.stop();
        check (engine.getAutoStopBeats() < 0.0 && engine.getLoopPassesLeft() == 0, "Stop cancels a pending auto-stop");
        engine.play();
        engine.setAutoStop (0.5, 0);
        engine.seekTo (0.25);
        check (engine.getAutoStopBeats() < 0.0, "a seek cancels it too");
        engine.stop();

        edit.loopEnabled = true;
        edit.loopStart = 0.0;
        edit.loopEnd = 1.0;   // 24000 samples
        engine.rebuild();
        edit.insertMarker = 0.0;
        engine.play();
        engine.setAutoStop (-1.0, 2);
        auto out = runCallbacks (engine, 300);   // 76800 samples: room for three passes
        int hits = 0;
        for (size_t i = 1; i < out.size(); ++i)
            if (std::abs (out[i]) > 0.9f)
                ++hits;
        check (hits == 2 && ! engine.isPlaying() && std::abs (engine.getPlayheadBeats() - 1.0) < 1.0e-9 && engine.getLoopPassesLeft() == 0,
               "loop_times 2: two passes, then stops at the loop end (hits " + String (hits) + ", at " + String (engine.getPlayheadBeats(), 6) + ")");
        edit.loopEnabled = false;
        engine.rebuild();
        engine.stop();
    }

    std::cout << "Recording" << std::endl;
    {
        auto rec = edit.addTrack (TrackKind::audio, "Rec");
        rec->input = { 0, false };
        rec->arm = true;
        rec->monitor = (int) Monitor::off;
        engine.rebuild();

        // Input impulse at sample 5000 of the take.
        AudioBuffer<float> input (2, 256 * 60);
        input.clear();
        input.setSample (0, 5000, 0.7f);

        edit.insertMarker = 0.0;
        engine.toggleRecord();
        runCallbacks (engine, 60, &input);
        engine.stop();

        check (rec->clips.size() == 1, "a clip was created on the armed track");
        if (! rec->clips.empty())
        {
            const auto& c = rec->clips[0];
            const int expectedOffset = 100 + 200 + 300;   // in + out + graph latency
            check (c.sourceOffset == expectedOffset, "take skips the round-trip latency (" + String (c.sourceOffset) + ", want " + String (expectedOffset) + ")");
            auto src = pool.get (c.file);
            check (src != nullptr && src->lengthInSamples == 256 * 60, "take has every recorded sample");
            if (src != nullptr)
            {
                float buf[256 * 60] {};
                float* chans[1] = { buf };
                src->reader->read (chans, 1, 0, 256 * 60);
                int p = -1;
                for (int i = 0; i < 256 * 60; ++i)
                    if (std::abs (buf[i]) > 0.5f) p = i;
                check (p == 5000, "recorded impulse at sample 5000 of the file (got " + String (p) + ")");
            }
        }
        rec->arm = false;
        edit.removeTrack (rec->id);
        engine.rebuild();
    }

    std::cout << "Recording the notes a plug-in generates" << std::endl;
    {
        std::vector<std::pair<Track*, bool>> wasMuted;
        for (auto& t : edit.tracks)
        {
            wasMuted.push_back ({ t.get(), t->mute.load() });
            t->mute = true;
        }
        auto addPlugin = [&] (Track& t, std::unique_ptr<AudioPluginInstance> p, const String& name, bool instrument)
        {
            auto slot = std::make_shared<PluginSlot>();
            slot->instance = std::move (p);
            slot->description.name = name;
            slot->description.isInstrument = instrument;
            engine.prepareSlot (*slot);
            t.plugins.push_back (slot);
        };

        // Gen plays a note from sample 10000 to 20000; the synth after it swallows the notes.
        auto gen = edit.addTrack (TrackKind::midi, "Arp");
        addPlugin (*gen, std::make_unique<NoteGenerator> (10000, 20000), "Gen", false);
        addPlugin (*gen, std::make_unique<NoteEater>(), "Synth", true);
        gen->arm = true;
        engine.rebuild();

        edit.insertMarker = 0.0;
        engine.toggleRecord();
        runCallbacks (engine, 100);
        engine.stop();
        const double spb = edit.samplesPerBeat (48000.0);
        check (gen->clips.size() == 1 && gen->clips[0].notes.size() == 1, "armed on its own track: the generated note is recorded");
        if (! gen->clips.empty() && ! gen->clips[0].notes.empty())
        {
            const auto& n = gen->clips[0].notes[0];
            check (n.pitch == 64 && std::abs ((gen->clips[0].start + n.start) * spb - 10000.0) < 1.0
                       && std::abs (n.length * spb - 10000.0) < 1.0,
                   "exactly where it played, no latency taken off (start " + String ((gen->clips[0].start + n.start) * spb) + ")");
        }
        gen->arm = false;
        gen->clips.clear();

        // From a track: another MIDI track records (and plays) what Gen generates.
        auto rec = edit.addTrack (TrackKind::midi, "Synth track");
        rec->input.track = gen->id;
        rec->arm = true;
        engine.rebuild();
        engine.toggleRecord();
        runCallbacks (engine, 100);
        engine.stop();
        check (gen->clips.empty(), "the generator's own track records nothing when it isn't armed");
        check (rec->clips.size() == 1 && rec->clips[0].notes.size() == 1 && rec->clips[0].notes[0].pitch == 64,
               "a MIDI track taking its input from it records the note");
        if (! rec->clips.empty() && ! rec->clips[0].notes.empty())
            check (std::abs ((rec->clips[0].start + rec->clips[0].notes[0].start) * spb - 10000.0) < 1.0, "on time there too");

        // Hearing notes from the piano roll: straight to that track's instrument,
        // stopped or recording, and never into a clip.
        rec->arm = false;
        {
            auto keys = edit.addTrack (TrackKind::midi, "Keys");
            auto counter = std::make_unique<NoteCounter>();
            auto* counted = counter.get();
            addPlugin (*keys, std::move (counter), "Counter", true);
            auto other = edit.addTrack (TrackKind::midi, "Other");
            auto otherCounter = std::make_unique<NoteCounter>();
            auto* otherCounted = otherCounter.get();
            addPlugin (*other, std::move (otherCounter), "Counter", true);
            engine.rebuild();

            engine.previewNote (keys->id, 60, 0.8f);
            runCallbacks (engine, 2);
            check (counted->ons == 1 && counted->lastNote == 60 && counted->lastVelocity == 102,
                   "a heard note reaches the track's instrument while stopped (" + String (counted->ons.load()) + " on, velocity "
                       + String (counted->lastVelocity.load()) + ")");
            check (otherCounted->ons == 0, "and only that track's");
            engine.previewNote (keys->id, 60, 0.0f);
            runCallbacks (engine, 2);
            check (counted->offs >= 1, "and stops when let go");

            keys->arm = true;
            edit.insertMarker = 0.0;
            engine.toggleRecord();
            runCallbacks (engine, 10);
            engine.previewNote (keys->id, 64, 0.8f);
            runCallbacks (engine, 10);
            engine.previewNote (keys->id, 64, 0.0f);
            runCallbacks (engine, 10);
            engine.stop();
            check (counted->ons == 2 && counted->lastNote == 64, "it plays while recording too");
            check (keys->clips.empty() || keys->clips[0].notes.empty(), "but is never recorded");
            keys->arm = false;
            edit.removeTrack (other->id);
            edit.removeTrack (keys->id);
        }

        // Keyboard input still records as before on a track without a generator.
        edit.removeTrack (rec->id);
        edit.removeTrack (gen->id);
        for (auto& [t, m] : wasMuted)
            t->mute = m;
        engine.rebuild();
    }

    std::cout << "Recording a real generator plug-in (LATHE_TEST_GENERATOR=<a .vst3>)" << std::endl;
    if (auto genFile = File (SystemStats::getEnvironmentVariable ("LATHE_TEST_GENERATOR", {})); genFile.exists())
    {
        VST3PluginFormat vst3;
        OwnedArray<PluginDescription> found;
        vst3.findAllTypesForFile (found, genFile.getFullPathName());
        String error;
        auto instance = found.isEmpty() ? nullptr : vst3.createInstanceFromDescription (*found[0], 48000.0, 256, error);
        check (instance != nullptr, "loads " + genFile.getFileName() + " " + error);
        if (instance != nullptr)
        {
            std::cout << "    " << found[0]->name << ": producesMidi " << (int) instance->producesMidi()
                      << ", isInstrument " << (int) found[0]->isInstrument << std::endl;
            std::vector<std::pair<Track*, bool>> wasMuted;
            for (auto& t : edit.tracks)
            {
                wasMuted.push_back ({ t.get(), t->mute.load() });
                t->mute = true;
            }
            auto gen = edit.addTrack (TrackKind::midi, "Generator");
            instance.reset();
            String slotError;
            auto slot = engine.makeSlot (*found[0], {}, slotError);   // the way the app loads it
            check (slot->instance != nullptr, "loads it the way the app does " + slotError);
            gen->plugins.push_back (slot);
            gen->arm = true;
            engine.rebuild();

            const int bars4 = (int) (edit.samplesPerBeat (48000.0) * 16 / 256);
            auto attempt = [&] (const String& how, int countIn, bool playFirst, double from)
            {
                gen->clips.clear();
                engine.countInBars = countIn;
                edit.insertMarker = from;
                engine.seekTo (from);
                if (playFirst)
                {
                    engine.play();
                    runCallbacks (engine, 50);
                }
                engine.toggleRecord();
                runCallbacks (engine, bars4 + (countIn > 0 ? bars4 / 4 : 0));
                engine.stop();
                runCallbacks (engine, 4);
                const size_t notes = gen->clips.empty() ? 0 : gen->clips[0].notes.size();
                check (notes > 0, "records the notes it generates, " + how + " (" + String ((int) notes) + " in 4 bars)");
                engine.countInBars = 0;
            };
            attempt ("from a stop", 0, false, 0.0);
            attempt ("with a count-in", 1, false, 0.0);
            attempt ("while playing", 0, true, 0.0);
            attempt ("from bar 5", 0, false, 16.0);
            attempt ("from bar 5 with a count-in", 1, false, 16.0);
            gen->arm = false;
            edit.removeTrack (gen->id);
            for (auto& [t, m] : wasMuted)
                t->mute = m;
            engine.rebuild();
        }
    }
    else
    {
        std::cout << "  (skipped)" << std::endl;
    }

    std::cout << "Recording another track (an instrument into an audio track)" << std::endl;
    {
        std::vector<std::pair<Track*, bool>> wasMuted;
        for (auto& t : edit.tracks)
        {
            wasMuted.push_back ({ t.get(), t->mute.load() });
            t->mute = true;
        }

        // The "instrument": a track playing an impulse at sample 13000 of the song.
        auto synth = edit.addTrack (TrackKind::audio, "Synth");
        synth->input.channel = -1;
        Clip c;
        c.id = edit.newId();
        c.kind = ClipKind::audio;
        c.file = writeImpulseWav (dir.getChildFile ("synth.wav"), 48000, 1000);
        c.start = 0.5;
        c.length = 1.0;
        synth->clips.push_back (c);

        auto rec = edit.addTrack (TrackKind::audio, "Resample");
        rec->input.track = synth->id;
        rec->arm = true;
        rec->monitor = (int) Monitor::automatic;
        engine.rebuild();

        edit.insertMarker = 0.0;
        engine.toggleRecord();
        const auto out = runCallbacks (engine, 60);
        engine.stop();

        // Heard once (through the recording track), not twice.
        float peak = 0.0f;
        for (auto v : out) peak = jmax (peak, std::abs (v));
        check (std::abs (peak - 0.5f) < 0.01f, "the source is heard once while the recording track monitors it (peak " + String (peak) + ")");

        check (rec->clips.size() == 1, "the receiving track got a take");
        if (! rec->clips.empty())
        {
            const auto& take = rec->clips[0];
            const int want = 200 + engine.getGraphLatency();   // like a live MIDI performance: output + graph latency
            check (take.sourceOffset == want, "take from a track skips output + graph latency (" + String (take.sourceOffset) + ", want " + String (want) + ")");
            auto src = pool.get (take.file);
            check (src != nullptr && src->numChannels == 2, "take from a track is stereo");
            if (src != nullptr)
            {
                std::vector<float> buf ((size_t) src->lengthInSamples);
                float* chans[1] = { buf.data() };
                src->reader->read (chans, 1, 0, (int) buf.size());
                int p = -1;
                for (size_t k = 0; k < buf.size(); ++k)
                    if (std::abs (buf[k]) > 0.4f) p = (int) k;
                check (p == 13000, "the source's sound is in the take where it played (got " + String (p) + ", want 13000)");
            }
        }

        // Saved and loaded; deleting the source clears the input.
        Edit copy;
        copy.loadFrom (edit.toVar (false), dir, [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        const auto loaded = copy.findTrack (rec->id);
        check (loaded != nullptr && loaded->input.track == synth->id, "the input track is saved with the project");
        rec->arm = false;
        edit.removeTrack (synth->id);
        check (rec->input.track == -1, "deleting the source track clears the input");
        edit.removeTrack (rec->id);
        for (auto& [t, m] : wasMuted)
            t->mute = m;
        engine.rebuild();
    }

    std::cout << "Count-in" << std::endl;
    {
        auto rec = edit.addTrack (TrackKind::audio, "Rec");
        rec->input = { 0, false };
        rec->arm = true;
        rec->monitor = (int) Monitor::off;
        engine.rebuild();
        engine.metronomeOn = false;   // the count-in clicks even with the metronome off

        const int barSamples = 96000;          // 4/4 at 120 BPM, 48 kHz
        const int blocksPerBar = barSamples / 256;
        AudioBuffer<float> input (2, 256 * (blocksPerBar + 60));
        input.clear();
        input.setSample (0, 1000, 0.9f);                // played during the count-in: not recorded
        input.setSample (0, barSamples + 5000, 0.7f);   // 5000 samples after the downbeat

        edit.insertMarker = 0.0;
        engine.countInBars = 1;
        engine.toggleRecord();
        check (engine.isCountingIn() && ! engine.isRecording() && ! engine.isPlaying(), "record starts a count-in, not a recording");
        check (engine.getCountInBeatsLeft() == 4, "four beats to go (" + String (engine.getCountInBeatsLeft()) + ")");

        const auto during = runCallbacks (engine, blocksPerBar - 1, &input);
        check (engine.isCountingIn() && ! engine.isRecording() && engine.getCountInBeatsLeft() == 1, "still counting in the last beat");
        int clicks = 0;
        for (int beat = 0; beat < 3; ++beat)
        {
            float peak = 0.0f;
            for (int i = beat * 24000; i < beat * 24000 + 1000; ++i)
                peak = jmax (peak, std::abs (during[(size_t) i]));
            clicks += peak > 0.1f ? 1 : 0;
        }
        check (clicks == 3, "a click on every beat of the count-in (" + String (clicks) + " of 3 heard)");
        check (std::abs (engine.getPlayheadBeats()) < 1.0e-9, "the playhead waits at the start marker");

        // The rest: the last count-in block, then recording.
        AudioBuffer<float> rest (2, 256 * 61);
        rest.copyFrom (0, 0, input, 0, 256 * (blocksPerBar - 1), 256 * 61);
        rest.copyFrom (1, 0, input, 1, 256 * (blocksPerBar - 1), 256 * 61);
        runCallbacks (engine, 1, &rest);
        check (engine.isRecording() && engine.isPlaying() && ! engine.isCountingIn(), "recording starts on the downbeat");
        AudioBuffer<float> after (2, 256 * 60);
        after.copyFrom (0, 0, rest, 0, 256, 256 * 60);
        after.copyFrom (1, 0, rest, 1, 256, 256 * 60);
        runCallbacks (engine, 60, &after);
        engine.stop();

        check (rec->clips.size() == 1 && rec->clips[0].start == 0.0, "one take, at the start marker");
        if (! rec->clips.empty())
        {
            auto src = pool.get (rec->clips[0].file);
            check (src != nullptr && src->lengthInSamples == 256 * 60, "the take holds only what came after the count-in ("
                       + String (src != nullptr ? src->lengthInSamples : -1) + " samples)");
            if (src != nullptr)
            {
                std::vector<float> buf ((size_t) src->lengthInSamples);
                float* chans[1] = { buf.data() };
                src->reader->read (chans, 1, 0, (int) src->lengthInSamples);
                int p = -1;
                for (int i = 0; i < (int) buf.size(); ++i)
                    if (std::abs (buf[(size_t) i]) > 0.5f) p = i;
                check (p == 5000, "the impulse 5000 samples after the downbeat is at 5000 in the take (got " + String (p) + ")");
            }
        }

        // Stop during the count-in: nothing recorded, nothing playing.
        rec->clips.clear();
        engine.toggleRecord();
        runCallbacks (engine, 50);
        engine.stop();
        runCallbacks (engine, 2);
        check (! engine.isCountingIn() && ! engine.isRecording() && ! engine.isPlaying() && rec->clips.empty(),
               "Stop during the count-in cancels it, with no take");

        // Off: record starts at once.
        engine.countInBars = 0;
        engine.toggleRecord();
        check (engine.isRecording() && ! engine.isCountingIn(), "with count-in off, recording starts straight away");
        engine.stop();
        rec->clips.clear();

        rec->arm = false;
        edit.removeTrack (rec->id);
        engine.rebuild();
    }

    std::cout << "Template -> new project -> record every armed track" << std::endl;
    {
        // A template like "Dev Jam Start": Kick on input 1, Snare on input 2, two returns.
        Edit source;
        auto kick = source.addTrack (TrackKind::audio, "Kick");
        kick->input = { 0, false };
        auto snare = source.addTrack (TrackKind::audio, "Snare");
        snare->input = { 1, false };
        snare->arm = true;
        source.addTrack (TrackKind::bus, "A Reverb");
        source.addTrack (TrackKind::bus, "B Delay");
        const auto templateJson = JSON::toString (source.toVar (true));

        // Load it the way File > New from Template does.
        Edit fresh;
        fresh.loadFrom (JSON::parse (templateJson), File(), [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        fresh.workDir = dir.getChildFile ("template-session");
        fresh.sampleRate = 48000.0;
        check (fresh.tracks.size() == 2 && fresh.tracks[0]->input.channel == 0 && fresh.tracks[1]->input.channel == 1,
               "template keeps both tracks and their inputs");

        Engine eng (host, pool, props);
        eng.setEdit (&fresh);
        FakeDevice dev (2, 2, 0, 0);
        eng.audioDeviceAboutToStart (&dev);
        eng.rebuild();

        for (auto& t : fresh.tracks)
            t->arm = true;

        // Different signal on each input: a 0.25 impulse on input 1, a 0.75 one on input 2.
        AudioBuffer<float> input (2, 256 * 40);
        input.clear();
        input.setSample (0, 3000, 0.25f);
        input.setSample (1, 3000, 0.75f);

        eng.toggleRecord();
        std::vector<float> meterPeaks;
        AudioBuffer<float> outs (2, 256), ins (2, 256);
        float kickMeter = 0.0f, snareMeter = 0.0f;
        for (int b = 0; b < 40; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                ins.copyFrom (ch, 0, input, ch, b * 256, 256);
            eng.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, 256, {});
            kickMeter = jmax (kickMeter, fresh.tracks[0]->meterL.exchange (0.0f));
            snareMeter = jmax (snareMeter, fresh.tracks[1]->meterL.exchange (0.0f));
        }
        eng.stop();

        check (kickMeter > 0.1f && snareMeter > 0.1f, "both armed tracks show input on their meters while recording ("
                                                       + String (kickMeter, 2) + ", " + String (snareMeter, 2) + ")");

        auto peakOf = [&] (const Track& t)
        {
            if (t.clips.size() != 1) return -1.0f;
            auto src = pool.get (t.clips[0].file);
            if (src == nullptr) return -2.0f;
            AudioBuffer<float> b (1, (int) src->lengthInSamples);
            float* chans[1] = { b.getWritePointer (0) };
            src->reader->read (chans, 1, 0, (int) src->lengthInSamples);
            return b.getMagnitude (0, 0, b.getNumSamples());
        };
        const float kickPeak = peakOf (*fresh.tracks[0]);
        const float snarePeak = peakOf (*fresh.tracks[1]);
        check (std::abs (kickPeak - 0.25f) < 0.01f, "Kick got one clip with input 1's signal (peak " + String (kickPeak, 3) + ")");
        check (std::abs (snarePeak - 0.75f) < 0.01f, "Snare got one clip with input 2's signal (peak " + String (snarePeak, 3) + ")");
        eng.setEdit (nullptr);
    }

    std::cout << "Export" << std::endl;
    {
        Engine::ExportSettings s;
        s.mix = true;
        s.stems = true;
        s.startBeat = 0.0;
        s.endBeat = 2.0;
        s.tailSeconds = 0.5;
        s.bitDepth = 24;
        s.folder = dir.getChildFile ("Exports");
        s.baseName = "Song";
        StringArray files;
        auto result = engine.exportAudio (s, [] (double) { return true; }, files);
        check (result.wasOk(), "export succeeded " + result.getErrorMessage());
        check (files.size() == 3, "mix + 2 stems written (got " + String (files.size()) + ")");

        if (files.size() > 0)
        {
            AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (File (files[0])));
            check (r != nullptr && r->lengthInSamples == 48000 * 2 / 2 + 24000, "mix length = 2 beats + 0.5 s tail");
            if (r != nullptr)
            {
                AudioBuffer<float> b (2, (int) r->lengthInSamples);
                r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
                int p = -1;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    if (std::abs (b.getSample (0, i)) > 0.9f) p = i;
                check (p == 13000, "exported mix is latency-compensated: impulse at 13000 (got " + String (p) + ")");
            }
        }
    }

    std::cout << "Milestones (anonymous, opt-in)" << std::endl;
    {
        PropertiesFile::Options po;
        po.applicationName = "milestones-test";
        po.filenameSuffix = ".settings";
        po.folderName = dir.getChildFile ("milestones").getFullPathName();
        dir.getChildFile ("milestones").createDirectory();
        PropertiesFile p (dir.getChildFile ("milestones/m.settings"), po);
        Array<var> sent;
        milestones::setSenderForTests ([&] (const var& v) { sent.add (v); });

        milestones::startup (p, true);
        milestones::reach (p, "recorded");
        check (milestones::choice (p) == milestones::Choice::notAsked && sent.isEmpty(), "nothing is sent before they're asked");

        milestones::setChoice (p, true, true);
        check (sent.size() == 1 && sent[0]["milestone"] == "launched" && sent[0]["cohort"] == "new", "yes in the welcome window sends launched, as a new install");
        auto* o = sent[0].getDynamicObject();
        check (o != nullptr && o->getProperties().size() == 5 && sent[0]["kind"] == "app" && sent[0]["version"].toString().isNotEmpty()
               && StringArray { "macos", "windows", "linux" }.contains (sent[0]["system"].toString()),
               "the message is kind, milestone, cohort, system and version: nothing else");
        milestones::reach (p, "recorded");
        milestones::reach (p, "recorded");
        milestones::reach (p, "not_a_milestone");
        check (sent.size() == 2 && sent[1]["milestone"] == "recorded", "each milestone is sent once, and only the listed ones");
        milestones::reach (p, "came_back");
        check (sent.size() == 2, "came_back isn't sent by asking; startup decides");
        milestones::startup (p, false);
        check (sent.size() == 2, "not on the first day");
        p.setValue ("firstLaunchDay", (Time::getCurrentTime() - RelativeTime::days (8)).formatted ("%Y-%m-%d"));
        milestones::startup (p, false);
        check (sent.size() == 3 && sent[2]["milestone"] == "came_back", "came_back a week or more later");

        milestones::setChoice (p, false, false);
        milestones::reach (p, "exported");
        check (sent.size() == 3, "turned off: nothing more is sent");

        PropertiesFile q (dir.getChildFile ("milestones/q.settings"), po);
        sent.clear();
        milestones::startup (q, false);
        milestones::setChoice (q, true, true);
        check (sent.size() == 1 && sent[0]["cohort"] == "existing", "an install from before milestones counts as existing");
        PropertiesFile r (dir.getChildFile ("milestones/r.settings"), po);
        sent.clear();
        milestones::startup (r, true);
        milestones::setChoice (r, false, true);
        milestones::reach (r, "launched");
        check (sent.isEmpty() && milestones::choice (r) == milestones::Choice::no, "no means nothing at all");
        milestones::setChoice (r, true, false);
        check (sent.size() == 1 && sent[0]["cohort"] == "existing", "turned on later in Settings: existing, since its early steps went unseen");
        // Only a Lathe that has never run here asks. An updated one (settings from before) never does.
        PropertiesFile upd (dir.getChildFile ("milestones/updated.settings"), po);
        upd.setValue ("lastProject", "/somewhere/song.lathe");
        upd.setValue ("welcomeSongShown", true);
        milestones::startup (upd, false);
        check (! milestones::shouldAsk (upd), "after an update, the question never shows");
        PropertiesFile fresh (dir.getChildFile ("milestones/fresh.settings"), po);
        milestones::markNeverRunBefore (fresh);
        milestones::startup (fresh, true);
        check (milestones::shouldAsk (fresh), "a brand-new install is asked");
        milestones::setChoice (fresh, false, true);
        check (! milestones::shouldAsk (fresh), "and only until it answers");

        check (milestones::names().size() == 8, "eight milestones (the privacy page lists the same eight)");
        for (auto& n : milestones::names())
            check (milestones::describe (n).isNotEmpty(), "described: " + n);
        milestones::setSenderForTests ({});
    }

    std::cout << "Export formats" << std::endl;
    {
        using F = Engine::ExportSettings::Format;
        struct Case { F format; int bits, kbps; const char* ext; bool lossy; };
        for (auto c : { Case { F::wav, 32, 0, ".wav", false }, Case { F::aiff, 24, 0, ".aiff", false },
                        Case { F::flac, 24, 0, ".flac", false }, Case { F::flac, 32, 0, ".flac", false },
                        Case { F::mp3, 24, 320, ".mp3", true }, Case { F::mp3, 24, 128, ".mp3", true },
                        Case { F::ogg, 24, 192, ".ogg", true } })
        {
            const auto what = Engine::formatName (c.format) + " " + String (c.lossy ? c.kbps : c.bits);
            Engine::ExportSettings s;
            s.mix = true;
            s.stems = false;
            s.startBeat = 0.0;
            s.endBeat = 2.0;
            s.tailSeconds = 0.5;
            s.format = c.format;
            s.bitDepth = c.bits;
            s.kbps = c.kbps;
            s.folder = dir.getChildFile ("Exports " + what);
            s.baseName = "Song";
            StringArray files;
            auto result = engine.exportAudio (s, [] (double) { return true; }, files);
            check (result.wasOk() && files.size() == 1, what + ": export succeeded " + result.getErrorMessage());
            if (files.size() != 1)
                continue;
            const File f (files[0]);
            check (f.getFileName() == String ("Song") + c.ext, what + ": named " + f.getFileName());

            AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
            check (r != nullptr && r->sampleRate == 48000.0 && r->numChannels == 2, what + ": reads back as 48 kHz stereo");
            if (r == nullptr)
                continue;
            if (! c.lossy)
                check ((int) r->bitsPerSample == (c.format == F::wav ? c.bits : jmin (24, c.bits)), what + ": bit depth "
                       + String (r->bitsPerSample) + (c.format == F::flac && c.bits == 32 ? " (FLAC tops out at 24)" : ""));

            AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            int p = -1;
            float loudest = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (std::abs (b.getSample (0, i)) > loudest) { loudest = std::abs (b.getSample (0, i)); p = i; }
            if (c.lossy)
            {
                // Lossy codecs smear an impulse and MP3 decoders add a short delay, so: near, and about as loud.
                check (r->lengthInSamples >= 72000, what + ": at least as long as the render (" + String (r->lengthInSamples) + ")");
                check (p >= 12900 && p <= 13000 + 2400 && loudest > 0.5f, what + ": the impulse is there (at " + String (p) + ", " + String (loudest, 2) + ")");
            }
            else
            {
                check (r->lengthInSamples == 72000, what + ": exact length");
                check (p == 13000, what + ": impulse at 13000, sample for sample (got " + String (p) + ")");
            }

            if (c.format == F::mp3)
            {
                MemoryBlock data;
                f.loadFileAsData (data);
                const auto* bytes = static_cast<const char*> (data.getData());
                check (data.getSize() > 10 && String (bytes, 3) == "ID3", what + ": starts with an ID3 tag");
                // The Info frame (LAME's header for CBR) is filled in at the end with the number of frames.
                int at = -1;
                for (size_t i = 0; i + 16 < jmin ((size_t) 4096, data.getSize()); ++i)
                    if (std::memcmp (bytes + i, "Info", 4) == 0) { at = (int) i; break; }
                const auto frames = at < 0 ? 0 : ((uint8) bytes[at + 8] << 24 | (uint8) bytes[at + 9] << 16 | (uint8) bytes[at + 10] << 8 | (uint8) bytes[at + 11]);
                check (at > 0 && frames > 50, what + ": its Info frame counts the frames (" + String ((int) frames) + ")");
                auto has = [&] (const char* needle, size_t n)
                {
                    const auto* end = bytes + jmin ((size_t) 4096, data.getSize());
                    return std::search (bytes, end, needle, needle + n) != end;
                };
                check (has ("TIT2", 4) && (has ("S\0o\0n\0g", 7) || has ("\0S\0o\0n\0g", 8)), what + ": the title is tagged (UTF-16)");
                if (c.kbps == 128)
                    check (data.getSize() < 30000, what + ": 1.5 s at 128 kbps is about 24 kB (" + String ((int) data.getSize()) + " bytes)");
            }
        }

        check (Engine::formatName (F::flac) == "flac", "formatName");
        auto parsed = F::wav;
        check (Engine::parseFormat ("AIF", parsed) && parsed == F::aiff, "parseFormat takes aif");
        check (! Engine::parseFormat ("aac", parsed), "parseFormat refuses aac");
    }

    std::cout << "Export some tracks" << std::endl;
    {
        // Mute the first track and solo the second: an export of just the first still plays it.
        auto first = edit.tracks[0], second = edit.tracks[1];
        first->mute = true;
        second->solo = true;

        Engine::ExportSettings s;
        s.mix = true;
        s.stems = true;
        s.startBeat = 0.0;
        s.endBeat = 2.0;
        s.tailSeconds = 0.5;
        s.folder = dir.getChildFile ("Exports one");
        s.baseName = "Song";
        s.trackIds = { first->id };
        StringArray files;
        auto result = engine.exportAudio (s, [] (double) { return true; }, files);
        check (result.wasOk() && files.size() == 2, "mix + the one stem (got " + files.joinIntoString (", ") + ")");
        check (files.size() == 2 && File (files[0]).getFileName() == "Song - " + first->name + ".wav", "mix is named after the track (" + (files.isEmpty() ? String() : File (files[0]).getFileName()) + ")");
        check (files.size() == 2 && File (files[1]).getFileName().startsWith ("Song - 01 "), "the stem keeps its song number");

        if (files.size() > 0)
        {
            AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (File (files[0])));
            AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            const float peak = b.getMagnitude (0, 0, b.getNumSamples());
            check (std::abs (peak - 0.5f) < 0.01f, "only that track is in the mix, though it's muted and another is soloed (peak " + String (peak) + ")");
        }

        // Live playback is untouched afterwards.
        check (first->mute.load() && second->solo.load(), "mute and solo are left as they were");
        first->mute = false;
        second->solo = false;

        s.folder = dir.getChildFile ("Exports both");
        s.trackIds = { first->id, second->id };
        files.clear();
        engine.exportAudio (s, [] (double) { return true; }, files);
        check (files.size() == 3 && File (files[0]).getFileName() == "Song - " + first->name + " + " + second->name + ".wav", "two tracks: both names in the mix's file name");
    }

    std::cout << "Project file round trip" << std::endl;
    {
        auto text = JSON::toString (edit.toVar (false));
        Edit loaded;
        loaded.workDir = dir;
        auto result = loaded.loadFrom (JSON::parse (text), dir, [] (const PluginDescription& d, const MemoryBlock& state, bool)
        {
            auto slot = std::make_shared<PluginSlot>();
            slot->description = d;
            slot->savedState = state;
            return slot;
        });
        check (result.wasOk(), "project parses");
        check (loaded.tracks.size() == edit.tracks.size(), "same number of tracks");
        check (! loaded.tracks.empty() && loaded.tracks[0]->clips.size() == 1
               && loaded.tracks[0]->clips[0].file == edit.tracks[0]->clips[0].file, "clip file path survives (relative to project)");
        check (! loaded.tracks.empty() && loaded.tracks[0]->plugins.size() == 1, "plug-in slot kept (as missing) when not installed");
        check (JSON::toString (loaded.toVar (false)) == text, "saving the loaded project gives identical JSON");
    }

    std::cout << "Song notes (not in the cloud)" << std::endl;
    {
        Edit e;
        e.sampleRate = 48000.0;
        auto bass = e.addTrack (TrackKind::audio, "Bass");
        SongNote n;
        n.id = "1-a";
        n.track = bass->id;
        n.trackName = "Bass";
        n.beat = 240.0;
        n.text = "The bass part doesn't go well here";
        n.replies.push_back ({ "r1", "dev@example.com", {}, "On it", 5 });
        e.localNotes.push_back (n);
        SongNote song;
        song.id = "2-b";
        song.text = "Where we are";
        e.localNotes.push_back (song);

        const auto saved = e.toVar (false);
        Edit back;
        back.loadFrom (saved, File(), [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        check (back.localNotes.size() == 2 && back.localNotes[0].track == bass->id && back.localNotes[0].beat == 240.0
                   && back.localNotes[0].replies.size() == 1 && back.localNotes[1].isSongNote(),
               "notes and their replies survive save and load");
        check (! e.toVar (true).hasProperty ("notes"), "templates don't carry notes");
        const auto snap = cloud::Snapshot::capture (e);
        check (! snap.project.hasProperty ("notes"), "cloud revisions never carry notes (they live on the server)");
    }

    std::cout << "Clip editing" << std::endl;
    {
        Edit e;
        e.sampleRate = 48000.0;
        auto t = e.addTrack (TrackKind::midi);
        Clip c;
        c.id = e.newId();
        c.kind = ClipKind::midi;
        c.start = 0.0;
        c.length = 8.0;
        c.notes = { { 60, 0.0, 1.0, 0.8f }, { 62, 3.5, 1.0, 0.8f }, { 64, 6.0, 1.0, 0.8f } };
        t->clips.push_back (c);
        e.clearRange (*t, 2.0, 4.0);
        check (t->clips.size() == 2, "clearing the middle of a clip splits it in two");
        check (t->clips.size() == 2 && std::abs (t->clips[0].length - 2.0) < 1e-9 && std::abs (t->clips[1].start - 4.0) < 1e-9, "split points are right");
        check (t->clips.size() == 2 && t->clips[1].notes.size() == 2 && std::abs (t->clips[1].notes[0].start) < 1e-9
               && std::abs (t->clips[1].notes[0].length - 0.5) < 1e-9, "a note crossing the cut is trimmed, later notes shift");
        e.beginClipEdit();
        t->clips.clear();
        e.undo();
        check (t->clips.size() == 2, "undo restores clips");
    }

    std::cout << "Undo: tracks, routing and plug-ins" << std::endl;
    {
        Edit e;
        auto slotNamed = [] (const String& name)
        {
            auto s = std::make_shared<PluginSlot>();
            s->description.name = name;
            return s;
        };

        check (! e.canUndo(), "a fresh project has nothing to undo");

        auto keys = e.addTrack (TrackKind::midi, "Keys");
        auto verb = e.addTrack (TrackKind::bus, "Verb");
        e.commitUndoStep();
        check (e.getUndoDepth() == 1, "adding two tracks in one go is one step (got " + String (e.getUndoDepth()) + ")");

        // Plug-ins and a send, as the mixer and the device chain do it.
        auto arp = slotNamed ("Arpeggiator"), drumSynth = slotNamed ("DrumSynth"), bigVerb = slotNamed ("BigVerb");
        keys->plugins = { arp, drumSynth };
        verb->plugins = { bigVerb };
        e.changed();
        e.commitUndoStep();
        keys->getSend (verb->id, true)->levelDb = -12.0f;
        e.changed();
        e.commitUndoStep();
        // The mix controls are steps too: a fader drag and a pan move (one step once they settle).
        keys->volumeDb = -3.0f;
        e.adjusted();
        keys->volumeDb = -6.0f;
        keys->pan = 0.4f;
        e.adjusted();
        e.commitUndoStep();
        // And the send level and the channel EQ.
        keys->sends[0]->levelDb = -20.0f;
        keys->eq.bands[2].gainDb = -3.0f;
        keys->eq.bands[2].freq = 300.0f;
        e.adjusted();

        check (e.undo() && keys->sends[0]->levelDb.load() == -12.0f && keys->eq.bands[2].gainDb.load() == 0.0f
                 && keys->eq.bands[2].freq.load() != 300.0f, "undo puts the send level and the EQ back");
        check (e.undo() && keys->volumeDb.load() == 0.0f && keys->pan.load() == 0.0f, "the next undo takes the whole fader move and the pan back");
        check (e.redo() && keys->volumeDb.load() == -6.0f && keys->pan.load() == 0.4f, "redo puts them where they were left");
        e.undo();
        check (e.undo() && keys->sends.empty(), "undo takes the new send away");
        check (keys->plugins.size() == 2, "and leaves the plug-ins where they were");
        check (e.redo() && keys->sends.size() == 1 && keys->sends[0]->levelDb.load() == -12.0f, "redo brings the same send back, level and all");
        {
            // The history: what each step did, in words, and who did it.
            e.redo();   // the fader and pan step again; the send level and EQ step stays undone
            const auto& done = e.getDoneSteps();
            check ((int) done.size() == e.getUndoDepth(), "history: one entry per step");
            check (done.back().label.contains ("\"Keys\" fader -6.0 dB") && done.back().label.contains ("pan 20R") && ! done.back().byAssistant,
                   "history: a fader and pan move is described (" + done.back().label + ")");
            check (e.getUndoneSteps().size() == 1 && e.getUndoneSteps()[0].label.contains ("send level"),
                   "history: the undone step waits to be redone (" + (e.getUndoneSteps().empty() ? String() : e.getUndoneSteps()[0].label) + ")");
            auto rows = UndoHistoryWindow::rowsFor (e);
            check (rows.size() == done.size() + 2 && rows.front().undone && rows.back().start && ! rows[1].undone
                     && rows[1].depthWhenClicked == e.getUndoDepth() - 1,
                   "history window: newest at the top, undone steps above, the opened state at the bottom");
            keys->name = "Keys 2";
            e.labelNextStep ("Renamed it for you", true);
            e.commitUndoStep();
            check (e.getDoneSteps().back().byAssistant && e.getDoneSteps().back().label == "Renamed it for you" && e.getUndoneSteps().empty(),
                   "history: the assistant's step carries its own words, and a new step drops the redo");
            keys->name = "Keys";
            e.commitUndoStep();
            check (e.getDoneSteps().back().label == "Renamed \"Keys 2\" to \"Keys\"" && ! e.getDoneSteps().back().byAssistant,
                   "history: the label is only for that one step (" + e.getDoneSteps().back().label + ")");
            const int depth = e.getUndoDepth();
            check (e.goToDepth (depth - 2) && e.getUndoDepth() == depth - 2 && keys->name == "Keys" && e.getUndoneSteps().size() == 2,
                   "history: going back two steps undoes both");
            check (e.goToDepth (depth) && e.getUndoDepth() == depth && keys->name == "Keys", "history: and forward again redoes them");
            e.goToDepth (depth - 2);
        }
        {
            // Cue (monitor) mix levels aren't part of undo.
            const auto depthBefore = e.getUndoDepth();
            e.markDirty();
            e.commitUndoStep();
            check (e.getUndoDepth() == depthBefore, "a change undo doesn't cover makes no step");
        }

        // Deleting a track brings back the very same track, plug-ins still loaded.
        e.removeTrack (verb->id);
        e.commitUndoStep();
        check (e.buses.empty() && keys->sends.empty(), "deleting a return removes the sends to it");
        check (e.undo() && e.buses.size() == 1 && e.buses[0] == verb && verb->plugins.size() == 1 && verb->plugins[0] == bigVerb,
               "undo brings the deleted return back with its plug-in instance");
        check (keys->sends.size() == 1 && keys->sends[0]->busId == verb->id, "and the send to it");

        // Bypass, reorder, routing and renames are steps too.
        drumSynth->bypassed = true;
        e.changed();
        e.commitUndoStep();
        std::swap (keys->plugins[0], keys->plugins[1]);
        keys->output.kind = OutputRoute::Kind::bus;
        keys->output.busId = verb->id;
        keys->name = "Drums";
        e.changed();
        e.commitUndoStep();
        check (e.undo() && keys->plugins[0] == arp && keys->output.kind == OutputRoute::Kind::master && keys->name == "Keys",
               "one undo puts back the order, the output and the name changed together");
        check (e.undo() && ! drumSynth->bypassed.load(), "the next undo turns the plug-in back on");

        // Something that calls changed() several times (a group bus) is still one step.
        auto a = e.addTrack (TrackKind::audio, "Kick");
        auto b = e.addTrack (TrackKind::audio, "Snare");
        e.commitUndoStep();
        auto g = e.createGroup ({ a->id, b->id }, "Drums");
        e.makeGroupBus (g->id);
        e.commitUndoStep();
        check (e.buses.size() == 2 && a->output.kind == OutputRoute::Kind::bus, "group bus made");
        check (e.undo() && e.groups.empty() && e.buses.size() == 1 && a->groupId == -1 && a->output.kind == OutputRoute::Kind::master,
               "one undo takes the group and its bus away together");

        // A change nobody announced still becomes its own step once the next edit begins.
        a->name = "Kick In";
        e.beginClipEdit();
        Clip hit;
        hit.id = e.newId();
        hit.kind = ClipKind::midi;
        b->clips.push_back (hit);
        e.changed();
        e.commitUndoStep();
        check (e.undo() && b->clips.empty() && a->name == "Kick In", "undo takes the new clip, not the rename before it");
        check (e.undo() && a->name == "Kick", "then the rename");

        // A failed edit can be rolled back without leaving a step behind.
        const int depth = e.getUndoDepth();
        b->clips.push_back ({});
        e.discardUncommitted();
        check (b->clips.empty() && e.getUndoDepth() == depth, "discardUncommitted puts things back and leaves no step");

        // Loading or starting a project starts the history over.
        e.clearUndo();
        check (! e.canUndo() && ! e.canRedo(), "clearUndo forgets every step");

        // A new edit after an undo drops the redo history.
        e.addTrack (TrackKind::audio, "One");
        e.commitUndoStep();
        e.undo();
        e.addTrack (TrackKind::audio, "Two");
        e.commitUndoStep();
        check (! e.canRedo(), "a new step after undo clears redo");
    }


    std::cout << "Built-in devices" << std::endl;
    {
        using namespace lathe::devices;
        StringArray ids;
        for (auto& info : all())
            ids.add (info.id);
        check (ids.joinIntoString (",") == "bass,poly,multi,resonator,drums,reverb,delay,grains,compressor,saturator,mastering,randomnotes", "the built-in devices (" + ids.joinIntoString (",") + ")");

        // Noise for half a second, then silence; instruments get a chord (or a drum hit
        // every eighth note) for that half second instead.
        auto runDevice = [] (AudioPluginInstance& p, int seconds, std::vector<float>& left, float level = 0.3f)
        {
            constexpr int block = 256;
            p.prepareToPlay (48000.0, block);
            AudioBuffer<float> buffer (2, block);
            MidiBuffer midi;
            Random rng (7);
            left.clear();
            const bool instrument = p.acceptsMidi();
            for (int pos = 0; pos < 48000 * seconds; pos += block)
            {
                midi.clear();
                if (instrument)
                {
                    static const int drumNotes[] { 36, 42, 38, 46, 39, 41, 49, 37, 45, 51, 48, 56 };
                    const bool drums = p.getName() == "Drums";
                    if ((drums && pos % 6144 == 0 && pos < 24000) || pos == 0)
                        for (int k = 0; k < 3; ++k)
                            midi.addEvent (MidiMessage::noteOn (1, drums ? drumNotes[(pos / 6144 * 3 + k) % 12] : 45 + 7 * k, jmin (1.0f, level)), 0);
                    if (! drums && pos == 23808)
                        for (int k = 0; k < 3; ++k)
                            midi.addEvent (MidiMessage::noteOff (1, 45 + 7 * k), 10);
                }
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample (c, i, instrument ? 0.0f : (pos + i < 24000 ? level * (rng.nextFloat() * 2.0f - 1.0f) : 0.0f));
                p.processBlock (buffer, midi);
                for (int i = 0; i < block; ++i)
                    left.push_back (buffer.getSample (0, i));
            }
        };
        auto rms = [] (const std::vector<float>& v, int from, int to)
        {
            double s = 0.0;
            for (int i = from; i < to && i < (int) v.size(); ++i)
                s += (double) v[(size_t) i] * v[(size_t) i];
            return std::sqrt (s / jmax (1, to - from));
        };
        auto finite = [] (const std::vector<float>& v)
        {
            for (auto x : v)
                if (! std::isfinite (x))
                    return false;
            return true;
        };

        for (auto& info : all())
        {
            const auto desc = describe (info);
            check (desc.pluginFormatName == "Lathe" && desc.fileOrIdentifier == "lathe:" + info.id && find (desc) == &info,
                   info.name + ": described as lathe:" + info.id);
            auto resolved = host.resolve (desc);
            check (resolved.has_value() && resolved->fileOrIdentifier == desc.fileOrIdentifier, info.name + ": the host resolves it");

            String error;
            auto slot = engine.makeSlot (desc, {}, error);
            check (slot->instance != nullptr && slot->instance->getName() == info.name, info.name + ": loads through the engine " + error);
            if (slot->instance == nullptr)
                continue;
            auto& device = dynamic_cast<Device&> (*slot->instance);
            check (device.getNumPrograms() >= 5, info.name + ": has presets (" + String (device.getNumPrograms()) + ")");

            std::vector<float> out;
            runDevice (device, 2, out);
            if (info.kind == Kind::generator)   // it makes notes, not sound (tested below)
                check (finite (out) && rms (out, 0, 24000) < 1.0e-6, info.name + ": makes no sound of its own");
            else
                check (finite (out) && rms (out, 0, 24000) > 0.01, info.name + ": processes sound (rms " + String (rms (out, 0, 24000), 3) + ")");

            // Every preset plays without blowing up.
            bool allFinite = true;
            for (int i = 0; i < device.getNumPrograms(); ++i)
            {
                device.setCurrentProgram (i);
                std::vector<float> o;
                runDevice (device, 1, o, 0.8f);
                allFinite = allFinite && finite (o);
            }
            check (allFinite, info.name + ": every preset gives finite output at a hot level");

            // State round-trip through a fresh instance.
            device.setCurrentProgram (1);
            MemoryBlock saved;
            device.getStateInformation (saved);
            auto again = engine.makeSlot (desc, saved, error);
            auto& copy = dynamic_cast<Device&> (*again->instance);
            bool same = copy.getCurrentProgram() == 1;
            for (auto* p : device.getParameters())
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
                    same = same && std::abs (rp->getValue() - copy.state.getParameter (rp->getParameterID())->getValue()) < 1.0e-5f;
            check (same, info.name + ": settings and preset survive a save and load");

            // The panel builds, at a sensible size.
            std::unique_ptr<AudioProcessorEditor> editor (device.createEditor());
            check (editor != nullptr && editor->getWidth() >= 400 && editor->getWidth() <= 1400 && editor->getHeight() >= 150 && editor->getHeight() <= 700,
                   info.name + ": panel " + String (editor != nullptr ? editor->getWidth() : 0) + " x " + String (editor != nullptr ? editor->getHeight() : 0));
            editor.reset();
        }

        auto load = [&] (const char* id) -> std::shared_ptr<PluginSlot>
        {
            for (auto& info : all())
                if (info.id == id)
                {
                    String error;
                    return engine.makeSlot (describe (info), {}, error);
                }
            return {};
        };

        // Reverb: a tail after the sound stops; nothing of it at Mix 0.
        {
            auto slot = load ("reverb");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.setCurrentProgram (0);   // Hall
            std::vector<float> out;
            runDevice (d, 3, out);
            const double tail = rms (out, 30000, 60000);
            check (tail > 0.002, "reverb: the hall rings on after the noise stops (" + String (tail, 4) + ")");
            d.set ("mix", 0.0f);
            runDevice (d, 2, out);
            check (rms (out, 30000, 60000) < 1.0e-5, "reverb: Mix 0 is only the dry sound");
            d.modeChosen (5);
            check (std::abs (d.get ("decay") - rvb::typeInfo (rvb::TSpring).start.decay) < 0.01f, "reverb: picking Spring moves Decay to where a spring starts");
        }

        // Delay: the first echo lands on the beat (1/4 at 120 BPM = 500 ms, digital, clean).
        {
            auto slot = load ("delay");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.set ("type", 0); d.set ("sync", 1); d.set ("note", 8); d.set ("repeats", 0.0f);
            d.set ("tone", 0.0f); d.set ("grit", 0.0f); d.set ("wobble", 0.0f); d.set ("mix", 1.0f); d.set ("stereo", 0);
            d.prepareToPlay (48000.0, 256);
            AudioBuffer<float> buffer (2, 256);
            MidiBuffer midi;
            std::vector<float> out;
            for (int pos = 0; pos < 48000; pos += 256)
            {
                buffer.clear();
                if (pos == 0)
                    buffer.setSample (0, 0, 1.0f), buffer.setSample (1, 0, 1.0f);
                d.processBlock (buffer, midi);
                for (int i = 0; i < 256; ++i)
                    out.push_back (buffer.getSample (0, i));
            }
            int peak = 0;
            for (int i = 1; i < (int) out.size(); ++i)
                if (std::abs (out[(size_t) i]) > std::abs (out[(size_t) peak]))
                    peak = i;
            check (std::abs (peak - 24000) < 120, "delay: a quarter note at 120 BPM echoes at 500 ms (" + String (peak / 48.0, 1) + " ms)");
            check (d.statusLine().contains ("500"), "delay: the panel says when the echo comes (" + d.statusLine() + ")");
        }

        // Compressor: a steady tone 12 dB over a -18 dB threshold at 4:1 comes out about 9 dB down.
        {
            auto slot = load ("compressor");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.set ("threshold", -18.0f); d.set ("ratio", 4.0f); d.set ("attack", 1.0f); d.set ("release", 50.0f);
            d.set ("makeup", 0.0f); d.set ("auto", 0); d.set ("mix", 1.0f);
            d.prepareToPlay (48000.0, 256);
            AudioBuffer<float> buffer (2, 256);
            MidiBuffer midi;
            float lastPeak = 0.0f;
            for (int pos = 0; pos < 48000; pos += 256)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const float s = 0.5f * std::sin (2.0f * MathConstants<float>::pi * 220.0f * (float) (pos + i) / 48000.0f);   // -6 dBFS peaks
                    buffer.setSample (0, i, s);
                    buffer.setSample (1, i, s);
                }
                d.processBlock (buffer, midi);
                lastPeak = buffer.getMagnitude (0, 0, 256);
            }
            const float outDb = Decibels::gainToDecibels (lastPeak);
            check (std::abs (outDb - (-15.0f)) < 1.5f, "compressor: -6 dB peaks come out near -15 dB (" + String (outDb, 1) + " dB)");
            check (std::abs (d.meterValue() + 9.0f) < 1.5f, "compressor: the meter shows about 9 dB of reduction (" + String (d.meterValue(), 1) + ")");
        }

        // Saturator: reports its latency, and Mix 0 is the dry sound delayed by exactly that.
        {
            auto slot = load ("saturator");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.set ("mix", 0.0f); d.set ("output", 0.0f);
            std::vector<float> out;
            runDevice (d, 1, out, 0.3f);
            check (d.getLatencySamples() > 0, "saturator: reports its oversampling latency (" + String (d.getLatencySamples()) + " samples)");
        }

        // --- The mix analysis (analyze_mix), on signals with known answers
        {
            const double sr = 48000.0, perBar = sr * 2.0;   // a bar is 2 seconds
            const int64 length = (int64) (perBar * 4);       // four bars
            using assistant::MixAnalysis;
            MixAnalysis a ({ { "Mix", true, false }, { "Bass", false, false }, { "Synth", false, false },
                             { "Hats", false, false }, { "Pad", false, false }, { "Verb", false, true } },
                           sr, perBar, 1.0, length, 2);
            // Bass and Synth both sit at 100 Hz for the whole thing, a dB apart: they mask each other.
            // Hats: 8 kHz, hard left, bars 3 and 4 only. Pad: 1 kHz, its right side flipped (wide), quiet.
            const int block = 512;
            AudioBuffer<float> mix (2, block), bass (2, block), synth (2, block), hats (2, block), pad (2, block), verb (2, block);
            verb.clear();
            for (int64 pos = 0; pos < length; pos += block)
            {
                for (int i = 0; i < block; ++i)
                {
                    const double t = (double) (pos + i) / sr;
                    const float b = 0.30f * (float) std::sin (MathConstants<double>::twoPi * 100.0 * t);
                    const float s = 0.27f * (float) std::sin (MathConstants<double>::twoPi * 100.0 * t + 0.4);
                    const float h = pos + i >= perBar * 2 ? 0.2f * (float) std::sin (MathConstants<double>::twoPi * 8000.0 * t) : 0.0f;
                    const float p = 0.05f * (float) std::sin (MathConstants<double>::twoPi * 1000.0 * t);
                    bass.setSample (0, i, b);  bass.setSample (1, i, b);
                    synth.setSample (0, i, s); synth.setSample (1, i, s);
                    hats.setSample (0, i, h);  hats.setSample (1, i, 0.0f);
                    pad.setSample (0, i, p);   pad.setSample (1, i, -p);
                    mix.setSample (0, i, b + s + h + p);
                    mix.setSample (1, i, b + s - p);
                }
                a.add (0, mix, 0, block, pos);
                a.add (1, bass, 0, block, pos);
                a.add (2, synth, 0, block, pos);
                a.add (3, hats, 0, block, pos);
                a.add (4, pad, 0, block, pos);
                a.add (5, verb, 0, block, pos);
            }
            const auto r = a.report();
            const auto* tracksOut = r["tracks"].getArray();
            const auto* masking = r["masking"].getArray();
            auto track = [&] (const String& name) { for (auto& t : *tracksOut) if (t["name"].toString() == name) return t; return var(); };
            check (tracksOut != nullptr && tracksOut->size() == 5 && masking != nullptr && masking->size() >= 1
                       && (*masking)[0]["band"].toString().startsWith ("bass") && (*masking)[0]["tracks"][0].toString() == "Bass"
                       && (*masking)[0]["tracks"][1].toString() == "Synth" && (int) (*masking)[0]["percent_of_the_time"] > 90
                       && (*masking)[0]["louder"].toString().startsWith ("Bass by"),
                   "analysis: two tracks on the same note mask each other in the bass band (" + JSON::toString (r["masking"], true).substring (0, 300) + ")");
            bool hatsMask = false;
            for (auto& m : *masking)
                if (m["tracks"][0].toString() == "Hats" || m["tracks"][1].toString() == "Hats")
                    hatsMask = true;
            check (! hatsMask, "analysis: a track alone in its band masks nothing");
            check (track ("Hats")["plays_in_bars"].toString() == "3-4" && track ("Bass")["plays_in_bars"].toString() == "1-4"
                       && track ("Hats")["most_energy_in"].toString().startsWith ("air") && (bool) track ("Verb")["silent"]
                       && track ("Hats")["stereo"]["leans"].toString().startsWith ("left") && (double) track ("Pad")["stereo"]["correlation"] < -0.9
                       && track ("Bass")["stereo"]["width"].toString() == "mono",
                   "analysis: where each track plays, its band, and its stereo (" + JSON::toString (track ("Hats"), true).substring (0, 200) + ")");
            const auto* sections = r["sections"].getArray();
            check (sections != nullptr && sections->size() == 2 && (*sections)[0]["bars"].toString() == "1-2"
                       && ! (*sections)[0]["loudest_tracks"].toString().contains ("Hats") && (*sections)[1]["loudest_tracks"].toString().isNotEmpty()
                       && (double) r["mix"]["integrated_lufs"] < 0.0 && r["mix"]["band_balance_db"]["bass (60-250 Hz)"].isDouble(),
                   "analysis: two-bar sections, the mix's loudness and band balance (" + JSON::toString (r["sections"], true).substring (0, 300) + ")");
        }

        // --- Pushing only the tracks the owner shared (a guitarist pushes the guitar)
        {
            auto project = [] (double tempo, double bassVolume, const String& guitarComment, bool withKeys)
            {
                Array<var> tracks;
                auto track = [] (int id, const String& name, double volume, const String& comment)
                {
                    DynamicObject::Ptr o = new DynamicObject();
                    o->setProperty ("id", id);
                    o->setProperty ("name", name);
                    o->setProperty ("volume", volume);
                    o->setProperty ("comment", comment);
                    o->setProperty ("clips", Array<var>());
                    return var (o.get());
                };
                tracks.add (track (1, "Drums", -3.0, {}));
                tracks.add (track (2, "Bass", bassVolume, {}));
                tracks.add (track (3, "Guitar", 0.0, guitarComment));
                if (withKeys)
                    tracks.add (track (9, "Keys", -4.25, {}));
                DynamicObject::Ptr p = new DynamicObject();
                p->setProperty ("format", "lathe-project");
                p->setProperty ("tempo", tempo);
                p->setProperty ("tracks", tracks);
                return var (p.get());
            };
            // The owner's newest revision, and the guitarist's copy: new guitar, and (just for
            // them) a faster tempo, a louder bass and a keys track.
            const auto base = project (100.0, -6.0, "DI", false);
            const auto local = project (128.0, 0.0, "Crunch, doubled", true);
            var merged;
            const auto r = cloud::mergeTracks (base, local, { 3 }, merged);
            auto* m = merged["tracks"].getArray();
            check (r.wasOk() && m != nullptr && m->size() == 3 && (double) merged["tempo"] == 100.0
                       && (double) (*m)[1]["volume"] == -6.0 && (*m)[2]["comment"].toString() == "Crunch, doubled"
                       && (int) (*m)[2]["id"] == 3,
                   "pushing a track: only it comes from your copy, the rest is the newest revision as it is");
            // Everything but the guitar is exactly the newest revision (what Lathe Cloud checks).
            auto blank = [] (const var& v)
            {
                auto copy = JSON::parse (JSON::toString (v));
                if (auto* list = copy["tracks"].getArray())
                    for (int i = 0; i < list->size(); ++i)
                        if ((int) (*list)[i]["id"] == 3)
                            list->set (i, var());
                return JSON::toString (copy);
            };
            check (blank (merged) == blank (base), "pushing a track: nothing outside it changes, byte for byte");

            var none;
            check (cloud::mergeTracks (base, local, { 9 }, none).getErrorMessage().contains ("removed"),
                   "pushing a track the owner no longer has: refused, and says so");
            check (cloud::mergeTracks (base, project (100.0, -6.0, "", false), { 3 }, none).wasOk()
                       && cloud::mergeTracks (base, [&] { auto v = JSON::parse (JSON::toString (local));
                                                          v["tracks"].getArray()->remove (2); return v; }(), { 3 }, none)
                              .getErrorMessage().contains ("isn't in your copy"),
                   "pushing a track you've deleted here: refused, and says how to get it back");

            // A real project survives the copy unchanged (numbers and all), so the rest matches.
            Edit e;
            e.tempo = 97.3;
            auto t1 = e.addTrack (TrackKind::audio, "Take");
            t1->volumeDb = -7.123456789;
            t1->pan = 0.3333333;
            const auto snap = cloud::Snapshot::capture (e);
            const auto once = JSON::toString (snap.project);
            check (JSON::toString (JSON::parse (once)) == once, "a project's JSON reads back exactly as it was written");
        }

        // --- The assistant is told what the person changed since it last looked
        {
            const String base = R"({"format":"lathe-project","version":1,"tempo":100.0,"timeSignature":[4,4],
                "loop":{"enabled":false,"start":0.0,"end":16.0},"insertMarker":0.0,
                "tracks":[{"id":1,"kind":"audio","name":"Bass","volume":-6.0,"pan":0.0,"mute":false,
                  "plugins":[{"description":"<PLUGIN name=\"Comp\"/>","state":"AAA","bypassed":false,"params":{"Ratio":"4:1"}}],
                  "clips":[]}],
                "returns":[],"master":{"id":4,"kind":"audio","name":"Master","volume":0.0,"pan":0.0,"plugins":[]}})";
            const File song ("/tmp/song.lathe");
            assistant::SessionWatch w;
            check (! w.compare (JSON::parse (base), song).any, "assistant watch: nothing to compare before it has looked");
            w.seen (JSON::parse (base), song);

            auto changed = [&] (std::function<void (var&)> f) { auto p = JSON::parse (base); f (p); return p; };
            auto bass = [] (var& p) { return (*p["tracks"].getArray())[0].getDynamicObject(); };

            auto c = w.compare (changed ([&] (var& p) { bass (p)->setProperty ("volume", -3.0); p.getDynamicObject()->setProperty ("insertMarker", 8.0); }), song);
            check (c.any && ! c.structural && c.list().contains ("fader"), "assistant watch: a fader move is reported but doesn't stop it");

            c = w.compare (changed ([&] (var& p)
            {
                (*bass (p)->getProperty ("plugins").getArray())[0].getDynamicObject()->setProperty ("state", "BBB");
            }), song);
            check (! c.any, "assistant watch: a plug-in's saved state changing by itself, same settings, isn't a change");

            c = w.compare (changed ([&] (var& p)
            {
                auto* plug = (*bass (p)->getProperty ("plugins").getArray())[0].getDynamicObject();
                plug->setProperty ("state", "CCC");
                plug->setProperty ("params", JSON::parse (R"({"Ratio":"8:1"})"));
            }), song);
            check (c.structural && c.list().contains ("Ratio"), "assistant watch: a plug-in setting the person turned stops a change, in words");

            c = w.compare (changed ([&] (var& p) { p["tracks"].getArray()->clear(); }), song);
            check (c.structural, "assistant watch: a removed track stops a change");

            c = w.compare (JSON::parse (base), File ("/tmp/other.lathe"));
            check (c.structural && c.otherProject == "other", "assistant watch: another project open now is said by name");

            check (assistant::SessionWatch::changesSong ("edit_session") && ! assistant::SessionWatch::changesSong ("describe_session")
                       && ! assistant::SessionWatch::changesSong ("analyze_mix"),
                   "assistant watch: only tools that change the song are held back");
        }

        // --- Putting set-aside changes back on top of a newer revision (three-way merge)
        {
            // r1: Drums (id 1, one clip), Guitar (id 2), a Plate return (id 3), the master (id 4).
            const String r1 = R"({"format":"lathe-project","version":1,"tempo":100.0,"timeSignature":[4,4],
                "loop":{"enabled":false,"start":0.0,"end":16.0},
                "tracks":[
                  {"id":1,"kind":"audio","name":"Drums","volume":-6.0,"pan":0.0,"mute":false,
                   "plugins":[{"description":"<PLUGIN name=\"Comp\"/>","state":"AAA","bypassed":false,"params":{"Ratio":"4:1"}}],
                   "clips":[{"id":10,"kind":"audio","name":"Drums 1","start":0.0,"length":16.0,"file":"Audio/d.wav","hash":"h10"}]},
                  {"id":2,"kind":"audio","name":"Guitar","volume":-6.0,"pan":0.0,"mute":false,"plugins":[],"clips":[]}],
                "returns":[{"id":3,"kind":"audio","name":"Plate","volume":-6.0,"pan":0.0,"plugins":[]}],
                "master":{"id":4,"kind":"audio","name":"Master","volume":0.0,"pan":0.0,"plugins":[]}})";
            const auto base = JSON::parse (r1);
            auto edit = [&] (std::function<void (var&)> change) { auto p = JSON::parse (r1); change (p); return p; };
            auto track = [] (var& p, int id) -> DynamicObject*
            {
                for (auto& t : *p["tracks"].getArray())
                    if ((int) t["id"] == id)
                        return t.getDynamicObject();
                return nullptr;
            };
            auto newClip = [] (int id, const String& name, const String& hash)
            {
                return JSON::parse ("{\"id\":" + String (id) + ",\"kind\":\"audio\",\"name\":\"" + name
                                    + "\",\"start\":4.0,\"length\":4.0,\"file\":\"Audio/x.wav\",\"hash\":\"" + hash + "\"}");
            };
            auto newTrack = [] (int id, const String& name)
            {
                return JSON::parse ("{\"id\":" + String (id) + ",\"kind\":\"audio\",\"name\":\"" + name
                                    + "\",\"volume\":0.0,\"pan\":0.0,\"plugins\":[],\"clips\":[]}");
            };

            // You: Guitar louder, a guitar take (clip 11), a new Vocal track (id 5).
            const auto yours = edit ([&] (var& p)
            {
                track (p, 2)->setProperty ("volume", -3.0);
                track (p, 2)->getProperty ("clips").getArray()->add (newClip (11, "Guitar 1", "hg"));
                p["tracks"].getArray()->add (newTrack (5, "Vocal"));
            });
            // r2: Drums quieter, faster, a drum fill (also clip 11), a new Keys track (also id 5).
            const auto theirs = edit ([&] (var& p)
            {
                p.getDynamicObject()->setProperty ("tempo", 104.0);
                track (p, 1)->setProperty ("volume", -9.0);
                track (p, 1)->getProperty ("clips").getArray()->add (newClip (11, "Fill", "hf"));
                p["tracks"].getArray()->add (newTrack (5, "Keys"));
            });
            auto m = cloud::mergeProjects (base, yours, theirs);
            auto* tracks = m.project["tracks"].getArray();
            std::set<int> ids;
            StringArray names;
            for (auto& t : *tracks)
            {
                ids.insert ((int) t["id"]);
                names.add (t["name"].toString());
                for (auto& c : *t["clips"].getArray())
                    ids.insert ((int) c["id"]);
            }
            check (m.conflicts.empty() && m.yoursChangedAnything && (double) m.project["tempo"] == 104.0
                       && (double) track (m.project, 1)->getProperty ("volume") == -9.0
                       && (double) track (m.project, 2)->getProperty ("volume") == -3.0
                       && names.joinIntoString (",") == "Drums,Guitar,Keys,Vocal",
                   "putting changes back: theirs and yours on different tracks both stay");
            check (ids.size() == 7 && track (m.project, 2)->getProperty ("clips").getArray()->size() == 1
                       && (int) (*track (m.project, 2)->getProperty ("clips").getArray())[0]["id"] > 11
                       && track (m.project, 1)->getProperty ("clips").getArray()->size() == 2,
                   "putting changes back: your new clip and track get new ids where theirs used the same ones");

            // Both moved the Guitar fader: a conflict, theirs until you choose yours.
            const auto louder = edit ([&] (var& p) { track (p, 2)->setProperty ("volume", -1.0); });
            m = cloud::mergeProjects (base, yours, louder);
            check (m.conflicts.size() == 1 && m.conflicts[0].what == "\"Guitar\": fader"
                       && m.conflicts[0].yours.contains ("-3.0 dB") && m.conflicts[0].theirs.contains ("-1.0 dB")
                       && (double) track (m.project, 2)->getProperty ("volume") == -1.0,
                   "putting changes back: the same fader moved both ways is one conflict, in words");
            m.conflicts[0].takeYours (m.project);
            check ((double) track (m.project, 2)->getProperty ("volume") == -3.0
                       && track (m.project, 2)->getProperty ("clips").getArray()->size() == 1,
                   "putting changes back: keeping yours changes just that");

            // The same change on both sides isn't a conflict; neither are plug-in readouts that
            // only exist where the plug-in loaded.
            const auto same = edit ([&] (var& p)
            {
                track (p, 2)->setProperty ("volume", -3.0);
                (*track (p, 1)->getProperty ("plugins").getArray())[0].getDynamicObject()->removeProperty ("params");
            });
            m = cloud::mergeProjects (base, edit ([&] (var& p) { track (p, 2)->setProperty ("volume", -3.0); }), same);
            check (m.conflicts.empty(), "putting changes back: the same change on both sides, or missing plug-in readouts, isn't a conflict");

            // You removed Drums; they changed it: they keep it unless you choose.
            m = cloud::mergeProjects (base, edit ([&] (var& p) { p["tracks"].getArray()->remove (0); }), theirs);
            check (m.conflicts.size() == 1 && m.conflicts[0].yours == "removed it" && track (m.project, 1) != nullptr,
                   "putting changes back: a track you removed and they changed is asked about");
            m.conflicts[0].takeYours (m.project);
            check (track (m.project, 1) == nullptr, "putting changes back: choosing yours removes it");

            // You removed Guitar, which they didn't touch: gone, no question.
            m = cloud::mergeProjects (base, edit ([&] (var& p) { p["tracks"].getArray()->remove (1); }), theirs);
            check (m.conflicts.empty() && track (m.project, 2) == nullptr && track (m.project, 1) != nullptr,
                   "putting changes back: a track only you removed goes");

            // Both edited the drum clip.
            auto moveClip = [&] (double start) { return edit ([&] (var& p)
            {
                (*track (p, 1)->getProperty ("clips").getArray())[0].getDynamicObject()->setProperty ("start", start);
            }); };
            m = cloud::mergeProjects (base, moveClip (1.0), moveClip (2.0));
            check (m.conflicts.size() == 1 && m.conflicts[0].what.contains ("clip \"Drums 1\""),
                   "putting changes back: the same clip edited both ways is asked about");

            // You reordered; they didn't: your order, with their new track after its neighbour.
            const auto swapped = edit ([&] (var& p) { auto* a = p["tracks"].getArray(); a->swap (0, 1); });
            m = cloud::mergeProjects (base, swapped, theirs);
            StringArray order;
            for (auto& t : *m.project["tracks"].getArray())
                order.add (t["name"].toString());
            check (order.joinIntoString (",") == "Guitar,Drums,Keys", "putting changes back: your track order, their new track kept");

            check (! cloud::mergeProjects (base, base, theirs).yoursChangedAnything,
                   "putting changes back: nothing of yours to put back is said so");

            // Their tempo, your tempo: one plain conflict.
            m = cloud::mergeProjects (base, edit ([] (var& p) { p.getDynamicObject()->setProperty ("tempo", 90.0); }), theirs);
            check (m.conflicts.size() == 1 && m.conflicts[0].what == "Tempo" && m.conflicts[0].yours.contains ("90.0"),
                   "putting changes back: tempo changed both ways is asked about");
        }

        // --- Invite links pasted after a trip through a social site
        {
            const String c ("Ab3_d-9xYz12");
            StringArray wrong;
            for (auto link : { "https://getlathe.app/join.html#" + c, "https://getlathe.app/join.html?trk=public_post#" + c,
                               "https://getlathe.app/join.html#" + c + "?trk=public_post", "https://getlathe.app/join.html#" + c + "&fbclid=IwAR0x",
                               "https://getlathe.app/join.html#" + c + "/", "https://getlathe.app/join.html%23" + c + "?utm_source=linkedin",
                               "  " + c + "\n" })
                if (cloudlinks::inviteCode (link) != c)
                    wrong.add (link + " -> " + cloudlinks::inviteCode (link));
            for (auto notLink : { String ("https://getlathe.app/download.html"), String ("https://getlathe.app/join.html#short?x=1"),
                                  String ("https://getlathe.app/?fbclid=abcdefghijk") })
                if (cloudlinks::inviteCode (notLink).isNotEmpty())
                    wrong.add (notLink + " -> " + cloudlinks::inviteCode (notLink));
            check (wrong.isEmpty(), "invite links with tracking added still give their code" + (wrong.isEmpty() ? String() : " (" + wrong.joinIntoString ("; ") + ")"));
        }

        // --- Multi Synth, Resonator, Grains and Random Notes (ThirdParty/mutable)
        struct TestHead : AudioPlayHead
        {
            bool playing = true;
            double ppq = 0.0, bpm = 120.0;
            Optional<PositionInfo> getPosition() const override
            {
                PositionInfo p;
                p.setIsPlaying (playing);
                p.setPpqPosition (ppq);
                p.setBpm (bpm);
                return p;
            }
        };
        // Plays one note (note-on at 0, note-off at `offAt` samples) and returns the left channel.
        auto playNote = [] (AudioPluginInstance& p, double rate, int note, float velocity, int total, int offAt)
        {
            constexpr int block = 256;
            p.prepareToPlay (rate, block);
            AudioBuffer<float> buffer (2, block);
            std::vector<float> out;
            for (int pos = 0; pos < total; pos += block)
            {
                MidiBuffer midi;
                if (pos == 0)
                    midi.addEvent (MidiMessage::noteOn (1, note, velocity), 0);
                if (offAt >= pos && offAt < pos + block)
                    midi.addEvent (MidiMessage::noteOff (1, note), offAt - pos);
                buffer.clear();
                p.processBlock (buffer, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back (0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i)));
            }
            return out;
        };
        // The pitch of a steady tone, from its zero crossings (rising), in Hz.
        auto pitchOf = [] (const std::vector<float>& v, int from, int to, double rate)
        {
            int first = -1, last = -1, crossings = 0;
            for (int i = from + 1; i < to && i < (int) v.size(); ++i)
                if (v[(size_t) i - 1] < 0.0f && v[(size_t) i] >= 0.0f)
                {
                    if (first < 0) first = i;
                    last = i;
                    ++crossings;
                }
            return crossings > 1 ? (crossings - 1) * rate / (double) (last - first) : 0.0;
        };

        // Multi Synth: every model sounds, A4 is 440 Hz at 48 and 44.1 kHz, and a note stops after its release.
        {
            auto slot = load ("multi");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            const int models = d.state.getParameter ("model")->getNumSteps();
            StringArray silent;
            bool allFinite = true;
            for (int m = 0; m < models; ++m)
            {
                d.set ("model", (float) m);
                const auto out = playNote (d, 48000.0, 60, 0.9f, 24000, 12000);
                if (rms (out, 0, 12000) < 0.003)
                    silent.add (d.state.getParameter ("model")->getCurrentValueAsText());
                allFinite = allFinite && finite (out);
            }
            check (silent.isEmpty() && allFinite, "Multi Synth: all " + String (models) + " models make sound"
                                                       + (silent.isEmpty() ? String() : " (silent: " + silent.joinIntoString (", ") + ")"));
            d.setCurrentProgram (0);
            d.set ("model", 3.0f);   // Two-op FM: with no modulation (Timbre 0), a sine
            d.set ("harmonics", 0.5f); d.set ("timbre", 0.0f); d.set ("morph", 0.5f); d.set ("envTimbre", 0.0f); d.set ("colour", 1.0f);
            for (const double rate : { 48000.0, 44100.0 })
            {
                const auto out = playNote (d, rate, 69, 1.0f, (int) rate, (int) (rate * 0.6));
                const double hz = pitchOf (out, (int) (rate * 0.1), (int) (rate * 0.5), rate);
                check (std::abs (hz - 440.0) < 1.5, "Multi Synth: A4 plays at 440 Hz at " + String (rate / 1000.0, 1) + " kHz ("
                                                       + String (hz, 2) + " Hz)");
                double after[4];
                for (int k = 0; k < 4; ++k)
                    after[k] = rms (out, (int) (rate * (0.6 + 0.1 * k)), (int) (rate * (0.7 + 0.1 * k)));
                check (after[3] < 1.0e-3, "...and it's quiet once released at " + String (rate / 1000.0, 1) + " kHz (rms per 100 ms after: "
                           + String (after[0], 4) + " " + String (after[1], 4) + " " + String (after[2], 4) + " " + String (after[3], 4) + ")");
            }
        }

        // Resonator: each model rings, keeps ringing after the key is up, and soft notes are softer.
        {
            auto slot = load ("resonator");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            StringArray dead;
            for (int m = 0; m < 6; ++m)
            {
                d.setCurrentProgram (0);
                d.set ("model", (float) m);
                d.set ("damping", 0.7f);   // the first preset is a short marimba
                const auto out = playNote (d, 44100.0, 57, 1.0f, 44100, 2000);
                // Still ringing a third of a second after the key is up, at a fair share of how it started.
                const auto start = rms (out, 0, 8000), tail = rms (out, 8000, 22000);
                if (tail < 0.002 || tail < start * 0.2 || ! finite (out))
                    dead.add (String (m) + "=" + String (rms (out, 8000, 22000), 4) + "/" + String (rms (out, 0, 8000), 4));
            }
            check (dead.isEmpty(), "Resonator: every model rings on after a short note" + (dead.isEmpty() ? String() : " (not: " + dead.joinIntoString (",") + ")"));
            d.setCurrentProgram (0);
            const auto loud = playNote (d, 48000.0, 57, 1.0f, 24000, 1000);
            const auto soft = playNote (d, 48000.0, 57, 0.15f, 24000, 1000);
            check (rms (soft, 0, 24000) < rms (loud, 0, 24000) * 0.9, "Resonator: a soft note is softer ("
                       + String (rms (soft, 0, 24000), 4) + " vs " + String (rms (loud, 0, 24000), 4) + ")");
        }

        // Grains: wet isn't the dry sound, Freeze keeps playing after the input stops, at any rate and quality.
        {
            auto slot = load ("grains");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            bool ok = true;
            for (const double rate : { 48000.0, 44100.0, 96000.0 })
                for (int q = 0; q < 4; ++q)
                {
                    d.setCurrentProgram (0);
                    d.set ("quality", (float) q);
                    d.set ("mix", 1.0f);
                    d.set ("time", 0.02f);   // grains from just behind what's coming in
                    d.set ("scatter", 0.0f);
                    d.set ("density", 0.8f);
                    d.prepareToPlay (rate, 256);
                    AudioBuffer<float> buffer (2, 256);
                    MidiBuffer midi;
                    std::vector<float> out;
                    const int total = (int) rate * 3;
                    for (int pos = 0; pos < total; pos += 256)
                    {
                        if (pos >= (int) rate * 2)
                            d.set ("freeze", 1.0f);
                        for (int c = 0; c < 2; ++c)
                            for (int i = 0; i < 256; ++i)
                                buffer.setSample (c, i, pos + i < (int) rate * 2 ? 0.3f * std::sin ((float) (pos + i) * 0.03f) : 0.0f);
                        d.processBlock (buffer, midi);
                        for (int i = 0; i < 256; ++i)
                            out.push_back (buffer.getSample (0, i));
                    }
                    d.set ("freeze", 0.0f);
                    const double wet = rms (out, (int) rate, (int) rate * 2), frozen = rms (out, (int) (rate * 2.5), total);
                    if (! finite (out) || wet < 0.01 || frozen < 0.005)
                    {
                        ok = false;
                        std::cout << "    grains at " << rate << " Hz, quality " << q << ": wet " << wet << ", frozen " << frozen << std::endl;
                    }
                }
            check (ok, "Grains: sounds, and Freeze holds the sound after the input stops (every quality, at 44.1, 48 and 96 kHz)");
            check (d.getTailLengthSeconds() >= 5.0, "Grains: says it has a long tail");
        }

        // Random Notes: notes only while playing, in the scale and range, the same each time for a Variation.
        {
            auto slot = load ("randomnotes");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            TestHead head;
            d.setPlayHead (&head);
            struct Played { int note, velocity, at; };
            auto run = [&] (int seconds, std::function<void (int pos, MidiBuffer&)> feed = {})
            {
                std::vector<Played> ons;
                int offs = 0;
                d.prepareToPlay (48000.0, 256);
                AudioBuffer<float> buffer (2, 256);
                head.ppq = 0.0;
                for (int pos = 0; pos < 48000 * seconds; pos += 256)
                {
                    MidiBuffer midi;
                    if (feed)
                        feed (pos, midi);
                    d.processBlock (buffer, midi);
                    for (const auto m : midi)
                    {
                        if (m.getMessage().isNoteOn())
                            ons.push_back ({ m.getMessage().getNoteNumber(), m.getMessage().getVelocity(), pos + m.samplePosition });
                        else if (m.getMessage().isNoteOff())
                            ++offs;
                    }
                    if (head.playing)
                        head.ppq += 256.0 * head.bpm / 60.0 / 48000.0;
                }
                return std::make_pair (ons, offs);
            };
            d.setCurrentProgram (0);
            d.set ("scale", 0.0f);   // major
            d.set ("lowest", 48.0f); // C2
            d.set ("range", 0.0f);   // two octaves
            d.set ("play", 2.0f);    // both
            head.playing = true;
            auto [ons, offs] = run (8);
            bool inKey = true, inRange = true;
            static const std::set<int> major { 0, 2, 4, 5, 7, 9, 11 };
            for (auto& p : ons)
            {
                inKey = inKey && major.count (p.note % 12) > 0;
                inRange = inRange && p.note >= 48 && p.note <= 72;
            }
            check (ons.size() > 20, "Random Notes: plays notes while the song plays (" + String ((int) ons.size()) + " in 8 s)");
            check (inKey && inRange, "Random Notes: every note is in C major, between C2 and C4");
            auto again = run (8);
            bool same = again.first.size() == ons.size();
            for (size_t i = 0; same && i < ons.size(); ++i)
                same = again.first[i].note == ons[i].note && again.first[i].at == ons[i].at;
            check (same, "Random Notes: the same Variation plays the same notes, at the same times");
            d.set ("variation", 2.0f);
            auto other = run (8);
            bool differs = other.first.size() != ons.size();
            for (size_t i = 0; ! differs && i < ons.size(); ++i)
                differs = other.first[i].note != ons[i].note;
            check (differs, "Random Notes: another Variation plays something else");
            d.set ("variation", 1.0f);

            // Stopping: the notes sounding stop, and nothing else is played.
            auto stopped = run (2, [&] (int pos, MidiBuffer&) { head.playing = pos < 48000; });
            bool afterStop = false;
            for (auto& p : stopped.first)
                afterStop = afterStop || p.at >= 48000 + 256;
            check (! afterStop && (int) stopped.first.size() <= stopped.second, "Random Notes: stopping ends every note and plays no more ("
                       + String ((int) stopped.first.size()) + " on, " + String (stopped.second) + " off)");
            head.playing = true;

            // Follow keys: hold D3 and the notes move up to start there.
            d.set ("followKeys", 1.0f);
            auto followed = run (4, [&] (int pos, MidiBuffer& m) { if (pos == 0) m.addEvent (MidiMessage::noteOn (1, 62, 1.0f), 0); });
            bool fromD = ! followed.first.empty();
            for (auto& p : followed.first)
                fromD = fromD && p.note >= 62 && major.count ((p.note - 62 + 12) % 12) > 0;
            check (fromD, "Random Notes: with Follow keys, a held D3 moves the scale to start on D3 (and the key isn't played)");
            d.set ("followKeys", 0.0f);
            d.setPlayHead (nullptr);
        }

        // Mastering: the limiter keeps peaks under the target's ceiling, and it reports its latency.
        {
            auto slot = load ("mastering");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.set ("amount", 0.8f);
            std::vector<float> out;
            runDevice (d, 2, out, 0.9f);
            float peak = 0.0f;
            for (auto x : out)
                peak = jmax (peak, std::abs (x));
            check (d.getLatencySamples() > 0, "mastering: reports the limiter's look-ahead (" + String (d.getLatencySamples()) + " samples)");
            check (peak <= Decibels::decibelsToGain (-0.9f), "mastering: peaks stay under the -1 dBTP ceiling (" + String (Decibels::gainToDecibels (peak), 2) + " dB)");
        }

        // Instruments: play notes, read what comes out.
        auto play = [] (Device& d, std::vector<std::tuple<int, int, int, float>> events, int samples)   // at, note, on (1) / off (0), velocity
        {
            constexpr int block = 256;
            d.prepareToPlay (48000.0, block);
            AudioBuffer<float> buffer (2, block);
            std::vector<float> out;
            for (int pos = 0; pos < samples; pos += block)
            {
                MidiBuffer midi;
                for (auto& [at, note, on, vel] : events)
                    if (at >= pos && at < pos + block)
                        midi.addEvent (on ? MidiMessage::noteOn (1, note, vel) : MidiMessage::noteOff (1, note), at - pos);
                d.processBlock (buffer, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back (buffer.getSample (0, i));
            }
            return out;
        };
        auto crossingsHz = [] (const std::vector<float>& v, int from, int to)
        {
            int crossings = 0;
            for (int i = from + 1; i < to; ++i)
                if ((v[(size_t) i - 1] < 0.0f) != (v[(size_t) i] < 0.0f))
                    ++crossings;
            return crossings * 0.5 * 48000.0 / (to - from);
        };

        {
            auto slot = load ("bass");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            check (d.acceptsMidi() && slot->description.isInstrument, "bass: an instrument that takes notes");
            d.set ("wave", 2); d.set ("sub", 0.0f); d.set ("detune", 0.0f); d.set ("drive", 0.0f);
            d.set ("cutoff", 12000.0f); d.set ("envAmount", 0.0f); d.set ("glide", 0.0f); d.set ("sustain", 1.0f);
            auto out = play (d, { { 0, 45, 1, 0.8f } }, 48000);
            const double hz = crossingsHz (out, 4800, 48000);
            check (std::abs (hz - 110.0) < 2.0, "bass: A2 plays at 110 Hz (" + String (hz, 1) + " Hz)");

            d.set ("glide", 200.0f);
            out = play (d, { { 0, 45, 1, 0.8f }, { 24000, 57, 1, 0.8f } }, 72000);
            const double during = crossingsHz (out, 24000 + 2400, 24000 + 7200), after = crossingsHz (out, 60000, 72000);
            check (during > 120.0 && during < 210.0 && std::abs (after - 220.0) < 4.0,
                   "bass: glide slides up an octave (" + String (during, 0) + " Hz on the way, " + String (after, 1) + " Hz after)");

            d.set ("glide", 0.0f);
            d.set ("release", 50.0f);
            out = play (d, { { 0, 45, 1, 0.8f }, { 12000, 45, 0, 0.0f } }, 48000);
            check (rms (out, 2000, 12000) > 0.05 && rms (out, 24000, 48000) < 1.0e-4, "bass: a note stops after its release");
        }

        {
            auto slot = load ("poly");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            d.setCurrentProgram (0);
            auto out = play (d, { { 0, 60, 1, 0.8f }, { 0, 64, 1, 0.8f }, { 0, 67, 1, 0.8f }, { 0, 72, 1, 0.8f } }, 48000);
            check (finite (out) && rms (out, 24000, 48000) > 0.02, "poly: a chord rings (rms " + String (rms (out, 24000, 48000), 3) + ")");

            d.setCurrentProgram (3);   // Pluck: no sustain, 400 ms release
            std::vector<std::tuple<int, int, int, float>> many;
            for (int k = 0; k < 12; ++k)
                many.push_back ({ k * 100, 48 + k * 2, 1, 0.7f });   // more notes than voices
            for (int k = 0; k < 12; ++k)
                many.push_back ({ 24000, 48 + k * 2, 0, 0.0f });
            out = play (d, many, 4 * 48000);
            check (finite (out) && rms (out, 0, 12000) > 0.01 && rms (out, 3 * 48000, 4 * 48000) < 1.0e-4,
                   "poly: twelve notes on eight voices play, then die away");
        }

        {
            // The chorus delay line: every read stays inside the line. The old float wrap turned a
            // position a hair below 0 into exactly `size` (one past the end), which read stray
            // memory: random full-scale bursts on one channel.
            constexpr int size = 2400;
            std::vector<float> line ((size_t) size + 1);
            for (int i = 0; i < size; ++i)
                line[(size_t) i] = (float) i;
            line[(size_t) size] = 1.0e9f;   // one past the end: must never be read
            int bad = 0;
            for (int w = 0; w < size; ++w)
                for (float base : { 0.0f, 1.0f, 384.0f, 575.0f, 768.0f })
                    for (float eps : { 0.0f, 1.0e-7f, 1.0e-5f, 0.25f, 0.9999f })
                    {
                        const float d = (float) w + base + eps;   // lands just past a wrap, among others
                        const float got = synth::readDelay (line.data(), size, w, std::fmod (d, (float) size));
                        if (! std::isfinite (got) || got < 0.0f || got > (float) size)
                            ++bad;
                    }
            check (bad == 0, "poly chorus: reading the delay line never goes past its end (" + String (bad) + " bad reads)");
            line.resize ((size_t) size);
            const float half = synth::readDelay (line.data(), size, 10, 2.5f);
            check (std::abs (half - 7.5f) < 1.0e-4f && std::abs (synth::readDelay (line.data(), size, 1, 3.0f) - 2398.0f) < 1.0e-3f,
                   "poly chorus: reads between samples and wraps round the start (" + String (half, 3) + ")");
        }

        {
            auto slot = load ("drums");
            auto& d = dynamic_cast<Device&> (*slot->instance);
            const int notes[] { 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 56, 57, 59 };
            StringArray silent;
            for (int note : notes)
            {
                auto out = play (d, { { 0, note, 1, 1.0f } }, 12000);
                if (! finite (out) || rms (out, 0, 12000) < 0.005)
                    silent.add (String (note));
            }
            check (silent.isEmpty(), "drums: every General MIDI drum note it knows makes a sound" + (silent.isEmpty() ? String() : " (silent: " + silent.joinIntoString (", ") + ")"));
            check (rms (play (d, { { 0, 60, 1, 1.0f } }, 12000), 0, 12000) < 1.0e-6, "drums: other notes are silent");

            // No drum may come out far hotter than the rest (the rim once peaked at +14 dBFS).
            auto peakOf = [] (const std::vector<float>& v) { float p = 0.0f; for (auto x : v) p = jmax (p, std::abs (x)); return p; };
            StringArray hot;
            for (int kit = 0; kit < 4; ++kit)
            {
                d.set ("kit", (float) kit);
                for (int note : notes)
                    if (const float pk = peakOf (play (d, { { 0, note, 1, 1.0f } }, 12000)); pk > 1.0f)
                        hot.add ("kit " + String (kit + 1) + " note " + String (note) + " " + String (pk, 2));
            }
            d.set ("kit", 0.0f);
            check (hot.isEmpty(), "drums: every voice of every kit peaks under 0 dBFS at full velocity" + (hot.isEmpty() ? String() : " (" + hot.joinIntoString (", ") + ")"));
            const float loud = peakOf (play (d, { { 0, 37, 1, 1.0f } }, 12000));
            const float soft = peakOf (play (d, { { 0, 37, 1, 32.0f / 127.0f } }, 12000));
            check (soft < loud * 0.25f, "drums: velocity 32 is at least 12 dB under 127 (" + String (Decibels::gainToDecibels (soft / loud), 1) + " dB)");

            const auto open = play (d, { { 0, 46, 1, 1.0f } }, 24000);
            const auto choked = play (d, { { 0, 46, 1, 1.0f }, { 2400, 42, 1, 0.01f } }, 24000);
            check (rms (choked, 9600, 24000) < 0.25 * rms (open, 9600, 24000), "drums: a closed hat chokes the open hat");

            auto kickTail = [&] (int kit)
            {
                d.set ("kit", (float) kit);
                d.modeChosen (kit);
                return rms (play (d, { { 0, 36, 1, 1.0f } }, 48000), 14400, 38400);
            };
            const double k909 = kickTail (0), k808 = kickTail (1), k606 = kickTail (2);
            check (k808 > 2.0 * k909 && k909 > k606, "drums: the 808's kick booms longest, the 606's is shortest ("
                   + String (k808, 3) + ", " + String (k909, 3) + ", " + String (k606, 3) + ")");
            check (d.getProgramName (0) == "Simple 909" && d.getProgramName (1) == "Simple 808", "drums: the kits are named Simple 909, Simple 808...");
        }

        // On a return, Lathe's effects start fully wet.
        {
            auto slot = load ("reverb");
            startOnReturn (*slot->instance);
            check (dynamic_cast<Device&> (*slot->instance).get ("mix") == 1.0f, "an effect put on a return starts at Mix 100%");
        }

        // A project keeps them, settings and all.
        {
            Edit ep;
            auto tr = ep.addTrack (TrackKind::audio, "Vox");
            auto slot = load ("delay");
            dynamic_cast<Device&> (*slot->instance).set ("repeats", 0.77f);
            tr->plugins.push_back (slot);
            const auto saved = ep.toVar (false);
            Edit back;
            back.loadFrom (saved, dir, [&] (const PluginDescription& d, const MemoryBlock& s, bool)
            {
                String error;
                return engine.makeSlot (d, s, error);
            });
            std::shared_ptr<Track> t2;
            for (auto& x : back.tracks)
                if (x->name == "Vox")
                    t2 = x;
            check (t2 != nullptr && t2->plugins.size() == 1 && t2->plugins[0]->instance != nullptr
                   && std::abs (dynamic_cast<Device&> (*t2->plugins[0]->instance).get ("repeats") - 0.77f) < 0.001f,
                   "a saved project loads its built-in delay back with its settings");
        }

        // The plug-in menu lists them first, and the host lists them with everything else.
        std::vector<PluginDescription> menuItems;
        host.buildMenu (false, menuItems);
        check (menuItems.size() >= 5 && devices::isBuiltIn (menuItems.front()), "the add-plug-in menu has Lathe's own first");
        check (host.allTypes().size() == (int) all().size() + host.known.getNumTypes(), "allTypes is the built-ins plus the scanned plug-ins");
    }

    std::cout << "The welcome song" << std::endl;
    {
        // A new install opens it: every device in it must be one of Lathe's own (nothing to
        // install), and it must play loud enough to hear and never clip.
        const auto projects = dir.getChildFile ("Welcome projects");
        File song;
        check (welcome::write (projects, song).wasOk() && song.existsAsFile(), "the welcome song is written to the projects folder");
        check (song == projects.getChildFile ("Welcome to Lathe").getChildFile ("Welcome to Lathe.lathe"), "as Welcome to Lathe/Welcome to Lathe.lathe");
        song.appendText (" ");   // changed by its owner
        File again;
        welcome::write (projects, again);
        check (again == song && song.loadFileAsString().endsWithChar (' '), "writing it again opens the copy already there, untouched");

        Edit w;
        w.projectFile = song;
        w.workDir = song.getParentDirectory();
        StringArray foreign;
        const auto loaded = w.loadFrom (JSON::parse (song), w.workDir, [&] (const PluginDescription& d, const MemoryBlock& s, bool)
        {
            if (! devices::isBuiltIn (d))
                foreign.add (d.name);
            String error;
            return engine.makeSlot (d, s, error);
        });
        int devicesUp = 0, devicesAll = 0;
        auto count = [&] (const Track& tr)
        {
            for (auto& slot : tr.plugins)
            {
                ++devicesAll;
                devicesUp += slot->instance != nullptr ? 1 : 0;
            }
        };
        for (auto& tr : w.tracks)  count (*tr);
        for (auto& tr : w.buses)   count (*tr);
        count (*w.master);
        check (loaded.wasOk() && foreign.isEmpty() && devicesAll >= 5 && devicesUp == devicesAll,
               "the welcome song uses only built-in devices, and they all load (" + String (devicesUp) + " of " + String (devicesAll)
                 + (foreign.isEmpty() ? String() : "; not built in: " + foreign.joinIntoString (", ")) + ")");

        Engine eng (host, pool, props);
        eng.setEdit (&w);
        FakeDevice dev (2, 2, 0, 0);
        eng.audioDeviceAboutToStart (&dev);
        eng.rebuild();
        Engine::ExportSettings s;
        s.mix = true;
        s.stems = false;
        s.startBeat = 0.0;
        s.endBeat = 32.0;
        s.tailSeconds = 1.0;
        s.bitDepth = 32;
        s.folder = dir.getChildFile ("Welcome mix");
        s.baseName = "Welcome";
        StringArray files;
        eng.exportAudio (s, [] (double) { return true; }, files);
        float peak = 0.0f;
        if (files.size() == 1)
        {
            AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (File (files[0])));
            if (r != nullptr)
            {
                AudioBuffer<float> b (2, (int) r->lengthInSamples);
                r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
                peak = b.getMagnitude (0, 0, b.getNumSamples());
            }
        }
        check (peak > 0.1f && peak <= 1.0f, "the welcome song's first 8 bars are audible and don't clip (peak " + String (peak, 3) + ")");
        eng.audioDeviceStopped();
        eng.setEdit (nullptr);
    }

    std::cout << "Out-of-process plug-in scanning" << std::endl;
    if (auto fixtures = File (SystemStats::getEnvironmentVariable ("LATHE_TEST_VST3_DIR", {})); fixtures.isDirectory())
    {
        // Needs a working "GainFixture", a VST3 whose name starts with "Crash" (crashes
        // while loading or unloading, like VCV Rack) and one starting "Hang" (never returns).
        // The build makes these when LATHE_BUILD_TESTS is on (build/test-vst3).
        auto scanDir = dir.getChildFile ("scan");
        scanDir.createDirectory();
        PropertiesFile scanProps (scanDir.getChildFile ("scan.settings"), opts);
        PluginHost scanHost (scanProps);
        scanHost.scanTimeoutMs = 3000;
        scanHost.setExtraFolders ({ fixtures.getFullPathName() });
        check (PluginHost::getScannerExecutable().existsAsFile(), "LatheScanner sits next to the executable");

        bool finished = false;
        scanHost.onScanFinished = [&] { finished = true; };
        scanHost.startScan (true);

        // The message thread must stay responsive the whole time.
        double longestStall = 0.0;
        const auto start = Time::getMillisecondCounterHiRes();
        while (! finished && Time::getMillisecondCounterHiRes() - start < 60000)
        {
            const auto before = Time::getMillisecondCounterHiRes();
            MessageManager::getInstance()->runDispatchLoopUntil (10);
            longestStall = jmax (longestStall, Time::getMillisecondCounterHiRes() - before);
        }

        check (finished, "scan finished (" + String ((Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) + " s)");
        // A real freeze (a plug-in scanned inside Lathe) lasts seconds: the hang fixture never
        // returns. Shared CI machines can pause for a few hundred ms on their own, so allow 1 s.
        check (longestStall < 1000.0, "UI never froze during the scan (longest stall " + String (roundToInt (longestStall)) + " ms)");

        bool gainFound = false;
        for (auto& d : scanHost.known.getTypes())
            gainFound = gainFound || d.name == "GainFixture";
        check (gainFound, "the working plug-in was found");

        const auto skipped = scanHost.getSkippedPlugins().joinIntoString (", ");
        bool crashSkipped = false, hangSkipped = false;
        for (auto& s2 : scanHost.getSkippedPlugins())
        {
            crashSkipped = crashSkipped || (s2.startsWith ("Crash") && s2.contains ("(crashed while being scanned)"));
            hangSkipped = hangSkipped || (s2.startsWith ("Hang") && s2.contains ("(stopped responding"));
        }
        check (crashSkipped, "the crashing plug-in was skipped: " + skipped);
        check (hangSkipped, "the hanging plug-in was skipped");

        // Skipped plug-ins stay skipped next time, and the next scan is quick.
        finished = false;
        const auto again = Time::getMillisecondCounterHiRes();
        scanHost.startScan (false);
        while (! finished && Time::getMillisecondCounterHiRes() - again < 30000)
            MessageManager::getInstance()->runDispatchLoopUntil (10);
        check (finished && Time::getMillisecondCounterHiRes() - again < 2000, "a second scan skips them straight away");

        // And the plug-in that scanned fine actually runs.
        if (gainFound)
        {
            Engine scanEngine (scanHost, pool, scanProps);
            FakeDevice dev (2, 2, 0, 0);
            scanEngine.audioDeviceAboutToStart (&dev);
            PluginDescription gainDesc;
            for (auto& d : scanHost.known.getTypes())
                if (d.name == "GainFixture")
                    gainDesc = d;
            String error;
            auto slot = scanEngine.makeSlot (gainDesc, {}, error);
            check (slot->instance != nullptr, "the scanned plug-in loads in Lathe " + error);
            if (slot->instance != nullptr)
            {
                AudioBuffer<float> buf (2, 256);
                buf.clear();
                buf.setSample (0, 10, 0.5f);
                MidiBuffer midi;
                slot->instance->processBlock (buf, midi);
                check (buf.getMagnitude (0, 0, 256) > 0.0f, "and processes audio");
            }
        }

        // --- "Run separately": a plug-in in its own process (LatheHost)
        std::cout << "Plug-ins run separately" << std::endl;
        PluginDescription flaky;
        for (auto& d : scanHost.known.getTypes())
            if (d.name == "FlakyFixture")
                flaky = d;
        check (RemotePluginInstance::hostExecutable().existsAsFile(), "LatheHost sits next to the executable");
        if (flaky.name.isNotEmpty() && RemotePluginInstance::hostExecutable().existsAsFile())
        {
            auto ones = [] { AudioBuffer<float> b (2, 256); for (int ch = 0; ch < 2; ++ch) FloatVectorOperations::fill (b.getWritePointer (ch), 1.0f, 256); return b; };
            String error;
            const auto started = Time::getMillisecondCounterHiRes();
            auto remote = RemotePluginInstance::create (flaky, 48000.0, 256, {}, error);
            check (remote != nullptr, "it starts in its own process (" + String (roundToInt (Time::getMillisecondCounterHiRes() - started)) + " ms) " + error);
            if (remote != nullptr)
            {
                remote->prepareToPlay (48000.0, 256);
                auto buf = ones();
                MidiBuffer midi;
                remote->processBlock (buf, midi);
                check (std::abs (buf.getSample (0, 100) - 0.5f) < 1.0e-6f && std::abs (buf.getSample (1, 255) - 0.5f) < 1.0e-6f,
                       "audio goes through it (its gain: 0.5, got " + String (buf.getSample (0, 100)) + ")");
                const auto params = remote->getParameters();
                StringArray names;
                for (auto* p : params) names.add (p->getName (32));
                check (params.size() >= 2 && params[0]->getName (32) == "Gain" && (params[1]->isBoolean() || params[1]->getNumSteps() == 2)
                           && params[0]->getCurrentValueAsText().isNotEmpty(),
                       "its settings are there, with names and text (" + names.joinIntoString (", ") + "; " + (params.size() > 0 ? params[0]->getCurrentValueAsText() : String()) + ")");
                params[0]->setValueNotifyingHost (0.25f);
                MemoryBlock state;
                remote->getStateInformation (state);   // (in order after the change)
                buf = ones();
                remote->processBlock (buf, midi);
                check (std::abs (buf.getSample (0, 10) - 0.25f) < 1.0e-6f && state.getSize() > 0,
                       "a setting changed from Lathe reaches it, and its state comes back (got " + String (buf.getSample (0, 10)) + ", state " + String ((int) state.getSize()) + " bytes)");

                {
                    auto second = RemotePluginInstance::create (flaky, 48000.0, 256, state, error);
                    check (second != nullptr && std::abs (second->getParameters()[0]->getValue() - 0.25f) < 1.0e-6f,
                           "a new one starts with the saved state");
                }

                // Now it crashes. Lathe (this test) carries on.
                bool told = false;
                remote->onStopped = [&] { told = true; };
                params[1]->setValueNotifyingHost (1.0f);
                MemoryBlock ignore;
                remote->getStateInformation (ignore);
                const auto crashAt = Time::getMillisecondCounterHiRes();
                float through = 0.0f;
                while (! told && Time::getMillisecondCounterHiRes() - crashAt < 10000)
                {
                    buf = ones();
                    remote->processBlock (buf, midi);
                    through = buf.getSample (0, 0);
                    MessageManager::getInstance()->runDispatchLoopUntil (5);
                }
                check (told && remote->hasStopped(), "it crashed, Lathe didn't, and Lathe was told ("
                           + String (roundToInt (Time::getMillisecondCounterHiRes() - crashAt)) + " ms)");
                buf = ones();
                remote->processBlock (buf, midi);
                check (std::abs (buf.getSample (0, 0) - 1.0f) < 1.0e-6f && std::abs (through - 1.0f) < 1.0e-6f,
                       "after that the effect lets the sound through untouched");
                MemoryBlock kept;
                remote->getStateInformation (kept);
                check (kept == state, "its last settings are kept for saving");

                check (remote->restart (error) && ! remote->hasStopped(), "Restart brings it back " + error);
                buf = ones();
                remote->processBlock (buf, midi);
                check (std::abs (buf.getSample (0, 10) - 0.25f) < 1.0e-6f && ! params[1]->getValue(),
                       "with its settings (got " + String (buf.getSample (0, 10)) + ")");
            }

            // Through the engine: a slot that runs separately, saved and loaded as such.
            Engine sepEngine (scanHost, pool, scanProps);
            FakeDevice sepDev (2, 2, 0, 0);
            sepEngine.audioDeviceAboutToStart (&sepDev);
            String why;
            auto slot = sepEngine.makeSlot (flaky, {}, why, true);
            check (slot->separate && dynamic_cast<RemotePluginInstance*> (slot->instance.get()) != nullptr, "makeSlot runs it separately " + why);
            Edit se;
            auto st = se.addTrack (TrackKind::audio, "Bass");
            st->plugins.push_back (slot);
            const auto saved = se.toVar (false);
            check ((bool) saved["tracks"][0]["plugins"][0]["separate"], "the project remembers it runs separately");
            bool askedSeparate = false;
            Edit back;
            back.loadFrom (saved, File(), [&] (const PluginDescription&, const MemoryBlock&, bool sep) { askedSeparate = sep; return std::make_shared<PluginSlot>(); });
            check (askedSeparate, "and loads it that way");
            auto inside = sepEngine.remakeSlot (*slot, false, why);
            check (! inside->separate && inside->instance != nullptr && dynamic_cast<RemotePluginInstance*> (inside->instance.get()) == nullptr,
                   "turning it off loads it inside Lathe, settings and all");
            sepEngine.audioDeviceStopped();
        }
    }
    else
    {
        std::cout << "  (skipped: set LATHE_TEST_VST3_DIR to a folder of test VST3s)" << std::endl;
    }

    std::cout << "Cue (monitor) mixes" << std::endl;
    {
        // A mono "hi-hat" input monitored live: DC at 0.5 on input 1.
        Edit e;
        e.sampleRate = 48000.0;
        auto hat = e.addTrack (TrackKind::audio, "Hats");
        hat->input = { 0, false };
        hat->monitor = (int) Monitor::in;
        hat->pan = 1.0f;   // main mix: hard right

        auto cue = e.addCue ("In-ears", 2);
        check (cue->channel.load() == 2 && e.cueLevel (*cue, *hat)->pan.load() == 1.0f, "a new cue mix starts as a copy of the main mix");
        e.cueLevel (*cue, *hat)->pan = -1.0f;   // in-ears: hard left

        Engine eng (host, pool, props);
        eng.setEdit (&e);
        FakeDevice dev (2, 4, 0, 0);
        eng.audioDeviceAboutToStart (&dev);
        eng.rebuild();

        AudioBuffer<float> ins (2, 256), outs (4, 256);
        auto run = [&] (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                ins.clear();
                FloatVectorOperations::fill (ins.getWritePointer (0), 0.5f, 256);
                eng.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 4, 256, {});
            }
        };
        auto level = [&] (int ch) { return outs.getSample (ch, 255); };
        auto near = [] (float a, float b) { return std::abs (a - b) < 0.01f; };

        run (4);
        check (near (level (0), 0.0f) && near (level (1), 0.5f), "main mix: hats on the right (L " + String (level (0)) + ", R " + String (level (1)) + ")");
        check (near (level (2), 0.5f) && near (level (3), 0.0f), "in-ears on outputs 3/4: hats on the left (L " + String (level (2)) + ", R " + String (level (3)) + ")");

        hat->volumeDb = kSilenceDb;
        run (4);
        check (near (level (1), 0.0f) && near (level (2), 0.5f), "main fader down doesn't touch the cue mix");
        hat->volumeDb = 0.0f;

        e.cueLevel (*cue, *hat)->levelDb = -6.0206f;
        run (4);
        check (near (level (2), 0.25f) && near (level (1), 0.5f), "cue level -6 dB halves the in-ears only (got " + String (level (2)) + ")");

        e.cueLevel (*cue, *hat)->mute = true;
        run (4);
        check (near (level (2), 0.0f) && near (level (1), 0.5f), "cue mute silences the in-ears only");
        e.cueLevel (*cue, *hat)->mute = false;
        e.cueLevel (*cue, *hat)->levelDb = 0.0f;

        cue->volumeDb = -6.0206f;
        run (4);
        check (near (level (2), 0.25f), "cue master volume (got " + String (level (2)) + ")");
        cue->volumeDb = 0.0f;

        // A return is heard in the cue at its own cue level, fed by the main send.
        auto verb = e.addTrack (TrackKind::bus, "Verb");
        hat->getSend (verb->id, true)->levelDb = 0.0f;   // post-fader: carries the main pan, so centre it
        hat->pan = 0.0f;
        eng.rebuild();
        e.cueLevel (*cue, *hat)->levelDb = kSilenceDb;
        run (4);
        check (near (level (2), 0.5f) && near (level (3), 0.5f), "return comes through the cue mix (L " + String (level (2)) + ", R " + String (level (3)) + ")");
        e.cueLevel (*cue, *verb)->levelDb = kSilenceDb;
        run (4);
        check (near (level (2), 0.0f), "and follows its own cue level");
        e.cueLevel (*cue, *hat)->levelDb = 0.0f;

        // Click only in the in-ears.
        hat->monitor = (int) Monitor::off;
        e.clickInMain = false;
        eng.metronomeOn = true;
        eng.play();
        float mainPeak = 0.0f, cuePeak = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            ins.clear();
            eng.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 4, 256, {});
            mainPeak = jmax (mainPeak, outs.getMagnitude (0, 0, 256), outs.getMagnitude (1, 0, 256));
            cuePeak = jmax (cuePeak, outs.getMagnitude (2, 0, 256));
        }
        eng.stop();
        eng.metronomeOn = false;
        check (mainPeak == 0.0f && cuePeak > 0.1f, "click in the in-ears but not the main outputs (main " + String (mainPeak) + ", cue " + String (cuePeak) + ")");

        // Save and load.
        Edit loaded;
        loaded.loadFrom (JSON::parse (JSON::toString (e.toVar (true))), File(), [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        auto lc = loaded.cues.empty() ? nullptr : loaded.cues[0];
        auto lhat = loaded.tracks.empty() ? nullptr : loaded.tracks[0];
        check (lc != nullptr && lhat != nullptr && lc->name == "In-ears" && lc->channel.load() == 2
                 && loaded.cueLevel (*lc, *lhat)->pan.load() == -1.0f && ! loaded.clickInMain.load(),
               "cue mixes survive a template save and load");
        eng.setEdit (nullptr);
    }

    std::cout << "Channel EQ" << std::endl;
    {
        ChannelEq eq;
        check (eq.isFlat() && std::abs (eq.responseDb (1000.0, 48000.0)) < 0.01f, "new EQ is flat");

        eq.bands[2].freq = 1000.0f;
        eq.bands[2].gainDb = 6.0f;
        check (std::abs (eq.responseDb (1000.0, 48000.0) - 6.0f) < 0.05f, "bell +6 dB at its centre (got " + String (eq.responseDb (1000.0, 48000.0)) + ")");

        // The processor matches the drawn curve: a 1 kHz sine comes out 6 dB louder.
        EqProcessor proc;
        AudioBuffer<float> sine (2, 48000);
        for (int i = 0; i < 48000; ++i)
            for (int ch = 0; ch < 2; ++ch)
                sine.setSample (ch, i, 0.25f * (float) std::sin (MathConstants<double>::twoPi * 1000.0 * i / 48000.0));
        for (int start = 0; start < 48000; start += 256)
        {
            AudioBuffer<float> view (sine.getArrayOfWritePointers(), 2, start, jmin (256, 48000 - start));
            proc.process (eq, view, view.getNumSamples(), 48000.0);
        }
        const float gain = sine.getMagnitude (0, 24000, 24000) / 0.25f;
        check (std::abs (Decibels::gainToDecibels (gain) - 6.0f) < 0.1f, "processed sine is +6 dB (got " + String (Decibels::gainToDecibels (gain), 2) + ")");

        eq.bands[0].on = true;
        eq.bands[0].freq = 100.0f;
        check (eq.responseDb (25.0, 48000.0) < -40.0f && std::abs (eq.responseDb (100.0, 48000.0) + 3.0f) < 0.2f,
               "low cut: 24 dB/oct, -3 dB at its frequency");

        eq.enabled = false;
        check (eq.responseDb (25.0, 48000.0) == 0.0f, "switching the EQ off makes it flat");
        eq.enabled = true;

        ChannelEq copy;
        copy.fromVar (eq.toVar());
        check (copy.bands[0].on.load() && copy.bands[2].gainDb.load() == 6.0f && copy.bands[0].freq.load() == 100.0f, "EQ settings save and load");

        // In the engine: a low cut removes DC from a monitored input.
        Edit e;
        auto t = e.addTrack (TrackKind::audio, "Bass");
        t->input = { 0, false };
        t->monitor = (int) Monitor::in;
        t->eq.bands[0].on = true;
        t->eq.bands[0].freq = 100.0f;
        Engine eng (host, pool, props);
        eng.setEdit (&e);
        FakeDevice dev (2, 2, 0, 0);
        eng.audioDeviceAboutToStart (&dev);
        eng.rebuild();
        AudioBuffer<float> ins (2, 256), outs (2, 256);
        for (int b = 0; b < 200; ++b)
        {
            ins.clear();
            FloatVectorOperations::fill (ins.getWritePointer (0), 0.5f, 256);
            eng.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, 256, {});
        }
        check (outs.getMagnitude (0, 0, 256) < 0.001f, "track EQ runs in the engine (DC removed, got " + String (outs.getMagnitude (0, 0, 256)) + ")");
        eng.setEdit (nullptr);
    }

    std::cout << "Lathe Patcher" << std::endl;
    {
        namespace up = lathe::updater;
        check (up::compareVersions ("v0.10.0", "0.9.2") > 0 && up::compareVersions ("0.2.0", "v0.2.0") == 0
                 && up::compareVersions ("0.2.0", "0.2.1") < 0, "version comparison");

        const auto json = JSON::parse (R"({"tag_name":"v0.3.0","body":"What's new","html_url":"https://example/release",
            "assets":[{"name":"Lathe-0.3.0-Windows.zip","browser_download_url":"https://example/w","size":10,"digest":"sha256:ABCD"},
                      {"name":"Lathe-0.3.0-macOS.zip","browser_download_url":"https://example/m","size":11},
                      {"name":"Lathe-0.3.0-Linux.tar.gz","browser_download_url":"https://example/l","size":12}]})");
        up::Release r;
        check (up::parseRelease (json, "macOS", r).wasOk() && r.assetUrl == "https://example/m" && r.version == "0.3.0" && r.notes == "What's new",
               "picks the macOS build from a release");
        check (up::parseRelease (json, "Windows", r).wasOk() && r.assetSize == 10 && r.sha256 == "abcd", "picks the Windows build, with its checksum");
        check (up::parseRelease (json, "Linux", r).wasOk() && r.assetName == "Lathe-0.3.0-Linux.tar.gz", "picks the Linux build");
        check (up::parseRelease (JSON::parse (R"({"tag_name":"v0.3.0","assets":[]})"), "Linux", r).failed(), "a release without a build for this platform is refused");

       #if JUCE_WINDOWS
        const String exeName = "Lathe.exe", scannerName = "LatheScanner.exe";
       #else
        const String exeName = "Lathe", scannerName = "LatheScanner";
       #endif

        // An installed copy, with a file of the user's that must survive.
        const auto root = dir.getChildFile ("patcher");
        const auto installDir = root.getChildFile ("Lathe");
        installDir.createDirectory();
        installDir.getChildFile (exeName).replaceWithText ("old");
        installDir.getChildFile (scannerName).replaceWithText ("old");
        installDir.getChildFile ("README.md").replaceWithText ("old");
        installDir.getChildFile ("my notes.txt").replaceWithText ("mine");
        check (up::isInstall (installDir, up::Layout::folder), "recognises a Lathe folder");

        // The release archive, packed like CI does.
        const auto build = root.getChildFile ("build").getChildFile ("Lathe-0.3.0-" + up::platformTag());
        build.createDirectory();
        for (auto name : { exeName, scannerName, String ("README.md"), String ("LICENSE") })
            build.getChildFile (name).replaceWithText ("new");
        build.getChildFile (exeName).setExecutePermission (true);
        const auto archive = root.getChildFile ("Lathe-0.3.0.tar.gz");
        {
            ChildProcess tar;
            tar.start (StringArray { "tar", "-czf", archive.getFullPathName(), "-C", build.getParentDirectory().getFullPathName(), build.getFileName() });
            tar.waitForProcessToFinish (30000);
        }

        up::Release fake;
        fake.assetSize = archive.getSize();
        fake.sha256 = SHA256 (archive).toHexString();
        check (archive.existsAsFile() && up::verify (fake, archive).wasOk(), "a good download passes size and checksum");
        auto corrupt = fake;
        corrupt.sha256 = String::repeatedString ("0", 64);
        check (up::verify (corrupt, archive).failed(), "a corrupted download is refused");

        const auto staging = up::stagingDirFor (installDir, up::Layout::folder);
        const auto unpacked = staging.getChildFile ("unpacked");
        check (up::unpack (archive, unpacked).wasOk(), "unpacks the release");

        // A download without Lathe in it changes nothing.
        const auto empty = staging.getChildFile ("empty");
        empty.getChildFile ("Lathe-0.3.0").createDirectory();
        check (up::install (empty, installDir, up::Layout::folder).failed()
                 && installDir.getChildFile (exeName).loadFileAsString() == "old", "a broken download leaves the install alone");

        check (up::install (unpacked, installDir, up::Layout::folder).wasOk(), "installs into the folder");
        check (installDir.getChildFile (exeName).loadFileAsString() == "new" && installDir.getChildFile (scannerName).loadFileAsString() == "new"
                 && installDir.getChildFile ("LICENSE").existsAsFile(), "every file in the release is replaced or added");
        check (installDir.getChildFile ("my notes.txt").loadFileAsString() == "mine", "files that aren't in the release are left alone");
       #if ! JUCE_WINDOWS
        check (access (installDir.getChildFile (exeName).getFullPathName().toRawUTF8(), X_OK) == 0, "the new Lathe keeps its execute permission");
       #endif
        check (installDir.findChildFiles (File::findFiles, false, "*.old").isEmpty(), "replaced files are cleaned up");

        // A file still "in use" from last time is cleaned up on the next run.
        installDir.getChildFile (exeName + ".old").replaceWithText ("stale");
        up::cleanUpOldFiles (installDir, up::Layout::folder);
        check (! installDir.getChildFile (exeName + ".old").exists(), "leftover .old files are removed later");
        staging.deleteRecursively();

        // macOS layout: Lathe.app replaced whole, wherever it lives.
        const auto apps = root.getChildFile ("Applications");
        const auto app = apps.getChildFile ("Lathe.app");
        app.getChildFile ("Contents/MacOS").createDirectory();
        app.getChildFile ("Contents/MacOS/Lathe").replaceWithText ("old");
        app.getChildFile ("Contents/Helpers/LathePatcher.app").createDirectory();
        const auto newApp = root.getChildFile ("mac-unpacked/Lathe-0.3.0-macOS/Lathe.app");
        newApp.getChildFile ("Contents/MacOS").createDirectory();
        newApp.getChildFile ("Contents/MacOS/Lathe").replaceWithText ("new");
        check (up::install (root.getChildFile ("mac-unpacked"), app, up::Layout::bundle).wasOk()
                 && app.getChildFile ("Contents/MacOS/Lathe").loadFileAsString() == "new"
                 && ! app.getChildFile ("Contents/Helpers").exists()
                 && ! apps.getChildFile (".Lathe.app.old").exists(),
               "replaces Lathe.app as a whole");
    }

    std::cout << "Group buses" << std::endl;
    {
        Edit e;
        e.sampleRate = 48000.0;
        auto kick = e.addTrack (TrackKind::audio, "Kick");
        auto snare = e.addTrack (TrackKind::audio, "Snare");
        auto bass = e.addTrack (TrackKind::audio, "Bass");
        for (auto& t : { kick, snare })
        {
            t->input = { 0, false };
            t->monitor = (int) Monitor::in;
        }
        auto drums = e.createGroup ({ kick->id, snare->id }, "Drums");
        auto bus = e.makeGroupBus (drums->id);
        check (bus != nullptr && bus->name == "Drums Bus" && bus->isBus() && drums->busId == bus->id
                 && kick->output.kind == OutputRoute::Kind::bus && kick->output.busId == bus->id
                 && snare->output.busId == bus->id && bass->output.kind == OutputRoute::Kind::master,
               "making a group bus routes the group's tracks (and only them) through it");
        check (e.makeGroupBus (drums->id) == bus, "asking again gives the same bus");

        // Audio: two monitored inputs at 0.25 each -> 0.5 on the bus -> bus fader -6 dB -> 0.25 out.
        Engine eng (host, pool, props);
        eng.setEdit (&e);
        FakeDevice dev (2, 2, 0, 0);
        eng.audioDeviceAboutToStart (&dev);
        eng.rebuild();
        AudioBuffer<float> ins (2, 256), outs (2, 256);
        auto run = [&] (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                ins.clear();
                FloatVectorOperations::fill (ins.getWritePointer (0), 0.25f, 256);
                eng.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, 256, {});
            }
        };
        run (4);
        check (std::abs (outs.getSample (0, 255) - 0.5f) < 0.01f, "both drums reach the master through the bus (got " + String (outs.getSample (0, 255)) + ")");
        bus->volumeDb = -6.0206f;
        run (4);
        check (std::abs (outs.getSample (0, 255) - 0.25f) < 0.01f, "the bus fader sets the whole group's level (got " + String (outs.getSample (0, 255)) + ")");
        bus->volumeDb = 0.0f;
        bus->eq.bands[0].on = true;   // a low cut on the bus removes the DC from both drums
        bus->eq.bands[0].freq = 100.0f;
        run (200);
        check (outs.getMagnitude (0, 0, 256) < 0.001f, "processing on the bus applies to the whole group");
        bus->eq.bands[0].on = false;
        eng.setEdit (nullptr);

        // Membership follows the bus.
        e.setTrackGroup (bass->id, drums->id);
        check (bass->output.kind == OutputRoute::Kind::bus && bass->output.busId == bus->id, "a track joining the group plays through its bus");
        e.setTrackGroup (bass->id, -1);
        check (bass->output.kind == OutputRoute::Kind::master, "and goes back to the master when it leaves");
        auto hats = e.addTrack (TrackKind::audio, "Hats");
        hats->output.kind = OutputRoute::Kind::hardware;
        hats->output.channel = 2;
        e.setTrackGroup (hats->id, drums->id);
        check (hats->output.kind == OutputRoute::Kind::hardware, "a track sent to hardware on purpose keeps its output");

        e.renameGroup (drums->id, "Kit");
        check (drums->name == "Kit" && bus->name == "Kit Bus", "renaming the group renames its bus");

        Edit loaded;
        loaded.loadFrom (JSON::parse (JSON::toString (e.toVar (true))), File(), [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); });
        auto lg = loaded.groups.empty() ? nullptr : loaded.groups[0];
        auto lbus = lg != nullptr ? loaded.findGroupBus (lg->id) : nullptr;
        check (lbus != nullptr && lbus->name == "Kit Bus" && loaded.tracks[0]->output.busId == lbus->id, "a group bus survives a template save and load");

        e.removeGroupBus (drums->id);
        check (e.buses.empty() && drums->busId == -1 && kick->output.kind == OutputRoute::Kind::master
                 && snare->output.kind == OutputRoute::Kind::master, "removing the bus sends the tracks straight to the master");

        auto b2 = e.makeGroupBus (drums->id);
        e.removeTrack (b2->id);
        check (drums->busId == -1 && e.findGroupBus (drums->id) == nullptr, "deleting the bus channel itself is handled too");
    }

    std::cout << "Cloud: change summaries" << std::endl;
    {
        Edit a;
        auto drums = a.addTrack (TrackKind::audio, "Drums");
        auto verb = a.addTrack (TrackKind::bus, "Verb");
        drums->volumeDb = -2.0f;
        const auto before = a.toVar (false);

        drums->volumeDb = -4.0f;
        drums->pan = -0.6f;
        drums->getSend (verb->id, true)->levelDb = -12.0f;
        drums->eq.bands[2].gainDb = 3.0f;
        drums->eq.bands[2].freq = 400.0f;
        a.addTrack (TrackKind::midi, "Keys");
        a.setTempo (124.0);
        const auto lines = cloud::describeChanges (before, a.toVar (false));
        const auto text = lines.joinIntoString ("\n");
        std::cout << "    " << text.replace ("\n", "\n    ") << std::endl;
        check (text.contains ("\"Drums\": fader -2.0 dB -> -4.0 dB") && text.contains ("pan C -> 30L"), "names fader and pan moves");
        check (text.contains ("send to Verb -inf dB -> -12.0 dB"), "names send changes by return");
        check (text.contains ("EQ Bell 1 400 Hz +3.0 dB (was 0.0 dB)"), "names EQ band changes");
        check (text.contains ("+ \"Keys\" (new midi track)") && text.contains ("Tempo 120.0 -> 124.0 BPM"), "new tracks and tempo");
        check (cloud::describeChanges (a.toVar (false), a.toVar (false)).isEmpty(), "no changes says nothing");

        // A kept snapshot (clips carry hashes, shared Audio/ paths) vs. the same project live.
        {
            const auto liveDir = dir.getChildFile ("cloud-diff");
            liveDir.getChildFile ("Audio").createDirectory();
            Edit live;
            live.projectFile = liveDir.getChildFile ("Song.lathe");
            live.workDir = liveDir;
            Clip take;
            take.id = live.newId();
            take.kind = ClipKind::audio;
            take.file = writeImpulseWav (liveDir.getChildFile ("Audio/Take.wav"), 4800, 5);
            live.addTrack (TrackKind::audio, "Gtr")->clips.push_back (take);
            cloud::LocalStore liveStore (live.projectFile);
            auto kept = cloud::Snapshot::capture (live);
            kept.finish (liveStore);
            check (cloud::describeChanges (cloud::Snapshot::capture (live).project, kept.project).isEmpty(),
                   "a kept snapshot of the same project shows no changes");
        }

        // Plug-in knobs, from the readable values a snapshot records.
        auto plug = [] (const String& decay)
        {
            DynamicObject::Ptr params = new DynamicObject();
            params->setProperty ("Decay", decay);
            params->setProperty ("Mix", "30 %");
            DynamicObject::Ptr p = new DynamicObject();
            p->setProperty ("description", "<PLUGIN name=\"BigVerb\"/>");
            p->setProperty ("state", "state-" + decay);
            p->setProperty ("params", var (params.get()));
            DynamicObject::Ptr t = new DynamicObject();
            t->setProperty ("id", 7);
            t->setProperty ("name", "Vox");
            t->setProperty ("plugins", Array<var> { var (p.get()) });
            DynamicObject::Ptr proj = new DynamicObject();
            proj->setProperty ("tracks", Array<var> { var (t.get()) });
            return var (proj.get());
        };
        const auto knob = cloud::summariseChanges (plug ("2.1 s"), plug ("3.4 s"));
        check (knob == "\"Vox\": BigVerb: Decay 2.1 s -> 3.4 s", "names the plug-in knob that moved (got " + knob + ")");
    }

    std::cout << "Cloud: packed (FLAC) storage of takes" << std::endl;
    {
        // A take-like signal: a few harmonics with a slow wobble and a little noise.
        auto writeTake = [&] (const File& f, int channels, int bits, double rate, int64 frames, bool noise, bool floatData,
                              const std::unordered_map<String, String>& meta)
        {
            f.deleteFile();
            std::unique_ptr<OutputStream> stream (f.createOutputStream().release());
            WavAudioFormat wav;
            auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels)
                                                      .withBitsPerSample (bits).withMetadataValues (meta)
                                                      .withSampleFormat (floatData ? AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                                   : AudioFormatWriterOptions::SampleFormat::integral));
            jassert (w != nullptr);
            Random rng (7);
            AudioBuffer<float> b (channels, 4096);
            for (int64 done = 0; done < frames;)
            {
                const int n = (int) jmin<int64> (4096, frames - done);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < n; ++i)
                    {
                        const double t = (double) (done + i) / rate;
                        const double v = noise ? rng.nextFloat() * 2.0 - 1.0
                                               : 0.4 * std::sin (2 * MathConstants<double>::pi * 110.0 * t * (1.0 + 0.002 * std::sin (t)))
                                                 + 0.15 * std::sin (2 * MathConstants<double>::pi * 330.0 * t + c)
                                                 + 0.002 * (rng.nextFloat() - 0.5);
                        b.setSample (c, i, (float) v);
                    }
                w->writeFromAudioSampleBuffer (b, 0, n);
                done += n;
            }
            w.reset();
            return f;
        };

        const std::unordered_map<String, String> none, bwav { { WavAudioFormat::bwavDescription, "A take with a broadcast-wave chunk" },
                                                              { WavAudioFormat::bwavOriginator, "Lathe tests" } };
        struct Case { const char* name; int channels, bits; double rate; int64 frames; bool noise, floatData, withMeta, expectPacked; };
        const Case cases[] = {
            { "24-bit mono 96k",            1, 24, 96000, 96000 * 4, false, false, false, true },
            { "16-bit stereo 44.1k",        2, 16, 44100, 44100 * 3, false, false, false, true },
            { "24-bit mono, odd length",    1, 24, 48000, 48001,     false, false, false, true },
            { "24-bit with extra chunks",   2, 24, 48000, 48000 * 2, false, false, true,  true },
            { "white noise (no gain)",      1, 24, 48000, 48000,     true,  false, false, false },
            { "32-bit float (not FLAC-able)", 1, 32, 48000, 48000,   false, true,  false, false },
        };
        for (auto& c : cases)
        {
            const auto wavFile = writeTake (dir.getChildFile (String ("pack-") + c.name + ".wav"), c.channels, c.bits, c.rate,
                                            c.frames, c.noise, c.floatData, c.withMeta ? bwav : none);
            const auto hash = cloud::sha256OfFile (wavFile);
            const auto packedFile = dir.getChildFile (String ("pack-") + c.name + ".lflac");
            const bool packed = cloud::packAudio (wavFile, packedFile);
            check (packed == c.expectPacked, String (c.name) + (c.expectPacked ? ": packed" : ": left as WAV"));
            if (! packed)
                continue;
            check (cloud::isPacked (packedFile) && ! cloud::isPacked (wavFile), String (c.name) + ": packed files are recognised");
            const auto back = dir.getChildFile (String ("pack-") + c.name + "-back.wav");
            const auto r = cloud::unpackAudio (packedFile, back);
            check (r.wasOk() && cloud::sha256OfFile (back) == hash,
                   String (c.name) + ": unpacking gives back the identical file (" + String (100.0 * (double) packedFile.getSize() / (double) wavFile.getSize(), 0) + "% of the size)");
        }

        // Speed: a minute of 24-bit mono at 96 kHz (the heaviest common case).
        const auto big = writeTake (dir.getChildFile ("pack-speed.wav"), 1, 24, 96000, 96000 * 60, false, false, none);
        const auto packedBig = dir.getChildFile ("pack-speed.lflac");
        auto t0 = Time::getMillisecondCounterHiRes();
        check (cloud::packAudio (big, packedBig), "a minute of 96 kHz audio packs");
        const double encodeMs = Time::getMillisecondCounterHiRes() - t0;
        t0 = Time::getMillisecondCounterHiRes();
        check (cloud::unpackAudio (packedBig, dir.getChildFile ("pack-speed-back.wav")).wasOk(), "and unpacks");
        const double decodeMs = Time::getMillisecondCounterHiRes() - t0;
        std::cout << "    1 min 96k/24 mono: pack " << String (encodeMs, 0) << " ms (" << String (60000.0 / encodeMs, 0)
                  << "x real time), unpack " << String (decodeMs, 0) << " ms (" << String (60000.0 / decodeMs, 0) << "x), "
                  << String (100.0 * (double) packedBig.getSize() / (double) big.getSize(), 0) << "% of the size" << std::endl;
        check (! cloud::packAudio (big, packedBig, [] (double p) { return p < 0.3; }) && ! packedBig.exists(),
               "packing can be cancelled, leaving nothing behind");
    }

    std::cout << "Cloud: hashing" << std::endl;
    {
        Random rng (42);
        bool allMatch = true;
        for (int size : { 0, 1, 55, 56, 63, 64, 65, 119, 128, 1000, (1 << 20) + 3, 3 * (1 << 20) })
        {
            MemoryBlock data ((size_t) size);
            for (int i = 0; i < size; ++i)
                static_cast<uint8*> (data.getData())[i] = (uint8) rng.nextInt (256);
            const auto f = dir.getChildFile ("hash-" + String (size) + ".bin");
            f.deleteFile();
            f.create();   // replaceWithData deletes the file when there's no data
            if (size > 0)
                f.replaceWithData (data.getData(), data.getSize());
            if (cloud::sha256OfFile (f) != SHA256 (data).toHexString().toLowerCase())
            {
                allMatch = false;
                std::cout << "    mismatch at " << size << " bytes" << std::endl;
            }
        }
        check (allMatch, "streaming SHA-256 matches JUCE's for every size, block edges included");
        check (cloud::sha256OfFile (dir.getChildFile ("hash-0.bin")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "and the empty file has the standard SHA-256");
        check (cloud::sha256OfFile (dir.getChildFile ("hash-3145728.bin"), [] (double p) { return p < 0.5; }).isEmpty(),
               "hashing a big file can be cancelled partway");
    }

    std::cout << "Cloud: snapshots and local history" << std::endl;
    {
        const auto projDir = dir.getChildFile ("cloud-local/Song");
        projDir.getChildFile ("Audio").createDirectory();
        Edit e;
        e.projectFile = projDir.getChildFile ("Song.lathe");
        e.workDir = projDir;
        auto t = e.addTrack (TrackKind::audio, "Kick");
        Clip c;
        c.id = e.newId();
        c.kind = ClipKind::audio;
        c.file = writeImpulseWav (projDir.getChildFile ("Audio/Kick 1.wav"), 4800, 10);
        t->clips.push_back (c);
        // A take that lives outside the project folder is shared too.
        Clip outside = c;
        outside.id = e.newId();
        dir.getChildFile ("cloud-local/elsewhere").createDirectory();
        outside.file = writeImpulseWav (dir.getChildFile ("cloud-local/elsewhere/Loop.wav"), 4800, 20);
        t->clips.push_back (outside);

        cloud::LocalStore store (e.projectFile);
        auto snap = cloud::Snapshot::capture (e);
        check (snap.finish (store).wasOk() && snap.hash.length() == 64, "snapshot hashes");
        const auto& clips = *snap.project["tracks"][0]["clips"].getArray();
        check (clips[0]["file"].toString() == "Audio/Kick 1.wav" && clips[1]["file"].toString() == "Audio/Loop.wav"
                 && clips[0]["hash"].toString() == cloud::sha256OfFile (c.file), "clips carry their hash and a shared Audio/ path");
        check (! snap.project.hasProperty ("insertMarker"), "where Play starts isn't part of a revision");

        auto again = cloud::Snapshot::capture (e);
        again.finish (store);
        check (again.hash == snap.hash, "the same project always hashes the same");

        {
            // A take split into pieces: three clips, one file. The progress counts files.
            Edit split;
            split.projectFile = e.projectFile;
            split.workDir = projDir;
            auto st = split.addTrack (TrackKind::audio, "Guitar");
            for (int i = 0; i < 3; ++i)
            {
                Clip piece = c;
                piece.id = split.newId();
                piece.start = i * 4.0;
                st->clips.push_back (piece);
            }
            StringArray seen;
            auto pieces = cloud::Snapshot::capture (split);
            check (pieces.finish (store, [&] (const String& what, double) { seen.addIfNotAlreadyThere (what); return true; }).wasOk(),
                   "a split take shares");
            check (seen.contains ("Checking audio files (1 of 1)") && ! seen.contains ("Checking audio files (1 of 3)"),
                   "the push counts audio files, not clips (" + seen.joinIntoString (" / ") + ")");
            const auto& pc = *pieces.project["tracks"][0]["clips"].getArray();
            check (pc.size() == 3 && pc[2]["hash"].toString() == cloud::sha256OfFile (c.file) && pc[2]["file"].toString() == "Audio/Kick 1.wav",
                   "and every clip using the file still gets its hash");
        }

        const auto local = cloud::keepWorkingCopy (store, snap, "me", "before switching");
        check (local.local && store.hasObject (local.snapshot) && store.getRevisions().size() == 1, "keeps the working copy as a local revision");
        check (cloud::keepWorkingCopy (store, again, "me", "again").id == local.id, "doesn't keep the same content twice");

        var opened;
        check (cloud::prepareRevision (nullptr, store, local, opened, {}).wasOk(), "opens a local revision offline");
        const auto& oc = *opened["tracks"][0]["clips"].getArray();
        check (oc[0]["file"].toString() == "Audio/Kick 1.wav" && File (oc[1]["file"].toString()) == outside.file,
               "finds the outside take where it really is");
    }

    if (SystemStats::getEnvironmentVariable ("LATHE_API_URL", {}).isNotEmpty())
    {
        std::cout << "Cloud: two people sharing a project (local backend)" << std::endl;
        const auto config = cloud::Config::load();
        auto user = [] (const char* env)
        {
            const auto spec = SystemStats::getEnvironmentVariable (env, {});
            return std::make_pair (spec.upToFirstOccurrenceOf (":", false, false), spec.fromFirstOccurrenceOf (":", false, false));
        };
        const auto [maraEmail, maraPass] = user ("LATHE_TEST_USER1");
        const auto [devEmail, devPass] = user ("LATHE_TEST_USER2");
        auto factory = [] (const PluginDescription&, const MemoryBlock&, bool) { return std::make_shared<PluginSlot>(); };

        cloud::Client mara (config), dev (config);
        check (mara.signIn (maraEmail, maraPass, {}).wasOk() && dev.signIn (devEmail, devPass, {}).wasOk(), "both sign in");
        check (mara.signIn (maraEmail, "wrong-password", {}).failed(), "a wrong password is refused");
        mara.signIn (maraEmail, maraPass, {});

        // Mara's project, with a recorded take.
        const auto maraDir = dir.getChildFile ("cloud-mara/Jam");
        Edit maraEdit;
        maraEdit.projectFile = maraDir.getChildFile ("Jam.lathe");
        maraEdit.workDir = maraDir;
        auto hats = maraEdit.addTrack (TrackKind::audio, "Hats");
        Clip take;
        take.id = maraEdit.newId();
        take.kind = ClipKind::audio;
        maraDir.getChildFile ("Audio").createDirectory();
        take.file = writeImpulseWav (maraDir.getChildFile ("Audio/Hats 1.wav"), 48000, 100);
        hats->clips.push_back (take);
        maraDir.createDirectory();
        maraEdit.projectFile.replaceWithText (JSON::toString (maraEdit.toVar (false)));

        cloud::LocalStore maraStore (maraEdit.projectFile);
        check (cloud::createCloudProject (mara, maraStore, "Jam").wasOk() && maraStore.isLinked(), "shares the project");
        auto s1 = cloud::Snapshot::capture (maraEdit);
        auto p1 = cloud::push (mara, maraStore, s1, {}, "First take", {});
        check (p1.result.wasOk() && p1.revision.seq == 1 && maraStore.getBase() == p1.revision.id, "pushes r1");
        cloud::ApiError err;
        // Sharing is only between friends: Mara asks, Dev accepts (asking back accepts).
        {
            DynamicObject::Ptr ask = new DynamicObject();
            ask->setProperty ("email", devEmail);
            mara.call ("POST", "/v2/friends", var (ask.get()), err);
            DynamicObject::Ptr back = new DynamicObject();
            back->setProperty ("email", maraEmail);
            const auto r = dev.call ("POST", "/v2/friends", var (back.get()), err);
            check (err.status == 0 && r["status"].toString() == "friends", "Mara and Dev become friends (" + err.message + ")");
        }
        DynamicObject::Ptr share = new DynamicObject();
        share->setProperty ("email", devEmail);
        mara.call ("POST", "/projects/" + maraStore.getProjectId() + "/members", var (share.get()), err);
        check (err.status == 0, "shares it with Dev");

        // Dev gets it.
        const auto devDir = dir.getChildFile ("cloud-dev/Jam");
        devDir.createDirectory();
        cloud::LocalStore devStore (devDir.getChildFile ("Jam.lathe"));
        devStore.link (maraStore.getProjectId(), "Jam");
        auto pulled = cloud::pull (dev, devStore, {});
        check (pulled.result.wasOk() && pulled.newRevisions.size() == 1 && pulled.head == p1.revision.id, "Dev pulls r1");
        var devProject;
        check (cloud::prepareRevision (&dev, devStore, pulled.newRevisions[0], devProject, {}).wasOk(), "r1 is ready to open");
        Edit te;
        te.projectFile = devStore.getProjectFile();
        te.workDir = devDir;
        te.loadFrom (devProject, devDir, factory);
        devStore.setBase (p1.revision.id);
        check (te.tracks.size() == 1 && te.tracks[0]->clips.size() == 1
                 && te.tracks[0]->clips[0].file == devDir.getChildFile ("Audio/Hats 1.wav")
                 && cloud::sha256OfFile (te.tracks[0]->clips[0].file) == cloud::sha256OfFile (take.file),
               "Dev has the take, byte for byte, in his Audio folder");

        // Dev turns the hats down and pushes r2.
        te.tracks[0]->volumeDb = -6.0f;
        auto ts = cloud::Snapshot::capture (te);
        auto p2 = cloud::push (dev, devStore, ts, devStore.getBase(), "Hats down", {});
        check (p2.result.wasOk() && p2.revision.seq == 2 && p2.revision.summary.contains ("fader 0.0 dB -> -6.0 dB"),
               "Dev pushes r2 with a summary: " + p2.revision.summary);

        // Mara pans them meanwhile: his push is refused, not merged over Dev's.
        maraEdit.tracks[0]->pan = -1.0f;
        auto s3 = cloud::Snapshot::capture (maraEdit);
        auto p3 = cloud::push (mara, maraStore, s3, maraStore.getBase(), "Hats left", {});
        check (p3.conflict && p3.theirHead == p2.revision.id, "Mara's push is refused: Dev pushed first");

        // Mara keeps his version, pulls Dev's, looks at it, then goes back to his own.
        const auto mine = cloud::keepWorkingCopy (maraStore, s3, maraEmail, "My work before opening r2");
        auto jp = cloud::pull (mara, maraStore, {});
        check (jp.result.wasOk() && jp.newRevisions.size() == 1, "Mara pulls r2");
        var theirs;
        cloud::prepareRevision (&mara, maraStore, jp.newRevisions[0], theirs, {});
        check (std::abs ((double) theirs["tracks"][0]["volume"] + 6.0) < 0.01 && std::abs ((double) theirs["tracks"][0]["pan"]) < 0.01,
               "r2 has Dev's fader and not Mara's pan");
        var back;
        cloud::prepareRevision (nullptr, maraStore, mine, back, {});
        check (std::abs ((double) back["tracks"][0]["pan"] + 1.0) < 0.01, "Mara's own version is still there, offline");

        // Mara decides his pan goes on top of Dev's revision: r3.
        auto p4 = cloud::push (mara, maraStore, s3, p2.revision.id, "Hats left, on top of Dev's", {});
        check (p4.result.wasOk() && p4.revision.seq == 3 && p4.revision.parent == p2.revision.id
                 && p4.revision.summary.contains ("fader -6.0 dB -> 0.0 dB") && p4.revision.summary.contains ("pan C -> 50L"),
               "pushes r3 on top, and the summary is against Dev's r2");
        int uploads = -1;
        mara.upload (maraStore.getProjectId(), { { cloud::sha256OfFile (take.file), take.file, {} } }, {}, uploads);
        check (uploads == 0, "the take was only ever uploaded once");

        // Several files at once, plus a big one in pieces; dropped connections are retried.
        {
            const auto batchDir = dir.getChildFile ("cloud-parallel");
            batchDir.createDirectory();
            std::vector<cloud::Client::UploadItem> batch;
            std::vector<std::pair<String, File>> back;
            for (int i = 0; i < 6; ++i)
            {
                MemoryBlock m ((size_t) 300000 + (size_t) i);
                for (size_t k = 0; k < m.getSize(); ++k)
                    static_cast<uint8*> (m.getData())[k] = (uint8) ((k * 31 + (size_t) i * 7) & 0xff);
                const auto f = batchDir.getChildFile ("take" + String (i) + ".wav");
                f.replaceWithData (m.getData(), m.getSize());
                batch.push_back ({ cloud::sha256OfFile (f), f, {} });
                back.push_back ({ batch.back().hash, batchDir.getChildFile ("back/take" + String (i) + ".wav") });
            }
            {
                // 110 MB: over the single-upload limit, so it goes up as two pieces.
                const auto big = batchDir.getChildFile ("long-take.wav");
                FileOutputStream out (big);
                HeapBlock<uint8> chunk (1 << 20);
                for (int mb = 0; mb < 110; ++mb)
                {
                    for (int k = 0; k < (1 << 20); ++k)
                        chunk[k] = (uint8) ((k ^ (mb * 131)) & 0xff);
                    out.write (chunk, 1 << 20);
                }
                out.flush();
                batch.push_back ({ cloud::sha256OfFile (big), big, {} });
                back.push_back ({ batch.back().hash, batchDir.getChildFile ("back/long-take.wav") });
            }

            mara.setParallelTransfers (4);
            cloud::testing::dropNextTransfers (2);
            std::vector<double> seen;
            int sent = 0;
            const auto r = mara.upload (maraStore.getProjectId(), batch, [&] (double p) { seen.push_back (p); return true; }, sent);
            check (r.wasOk() && sent == 7, "uploads 7 files, 4 at a time, through 2 dropped connections (" + r.getErrorMessage() + ")");
            check (! seen.empty() && seen.back() == 1.0 && std::is_sorted (seen.begin(), seen.end()), "progress only goes forward and ends at 100%");

            cloud::testing::dropNextTransfers (1);
            const auto d = mara.download (maraStore.getProjectId(), back, {});
            bool same = d.wasOk();
            for (size_t i = 0; i < back.size() && same; ++i)
                same = cloud::sha256OfFile (back[i].second) == batch[i].hash;
            check (same, "downloads them 4 at a time, every file intact (" + d.getErrorMessage() + ")");

            // Stopping partway: nothing new starts, and it says so.
            std::vector<std::pair<String, File>> again;
            for (auto& [h, f] : back)
                again.push_back ({ h, f.getSiblingFile ("again-" + f.getFileName()) });
            const auto c = mara.download (maraStore.getProjectId(), again, [] (double) { return false; });
            check (c.failed() && c.getErrorMessage() == "Cancelled.", "a transfer can be stopped");
        }

        // Signed-out access is refused by the API.
        cloud::Client nobody (config);
        nobody.call ("GET", "/projects", {}, err);
        check (err.status == 401, "the API refuses anyone not signed in");

        // Signing out and in.
        {
            cloud::Client c (config);
            const auto g0 = c.getAccountGeneration();
            check (c.signIn (maraEmail, maraPass, {}).wasOk() && c.getAccountGeneration() != g0, "signing in starts a new account generation");
            const auto g1 = c.getAccountGeneration();
            c.call ("GET", "/me", {}, err);
            check (err.status == 0 && c.getAccountGeneration() == g1, "an API call (and any token refresh) keeps it");

            const auto token = c.getSession().refreshToken;
            c.signOut();
            check (! c.isSignedIn() && c.getSession().idToken.isEmpty() && c.getAccountGeneration() != g1,
                   "signing out forgets the session and moves the generation on");
            c.call ("GET", "/me", {}, err);
            check (err.status == 401, "after signing out the API refuses the calls");

            // A refresh that was already on its way when you signed out doesn't sign you back in.
            for (int i = 0; i < 5; ++i)
            {
                check (c.signIn (maraEmail, maraPass, {}).wasOk(), "signs in again");
                auto s = c.getSession();
                s.expiresAtMs = 0;                    // the next call refreshes the token
                c.setSession (s);
                std::thread t ([&c] { cloud::ApiError e; c.call ("GET", "/me", {}, e); });
                c.signOut();
                t.join();
                if (c.isSignedIn() || c.getSession().idToken.isNotEmpty())
                {
                    check (false, "a refresh finishing after sign-out is dropped");
                    break;
                }
            }
            check (! c.isSignedIn(), "a refresh finishing after sign-out is dropped (5 tries)");

            // Sign-out on the server: the old refresh token stops working. (The local stand-in for
            // Cognito, moto, doesn't have RevokeToken; real Cognito does, with revocation enabled.)
            if (const auto rv = c.revoke (token); rv.failed())
                std::cout << "  (server-side sign-out not checked: this Cognito can't revoke tokens: "
                          << rv.getErrorMessage().substring (0, 60) << ")" << std::endl;
            else
            {
                cloud::Session old;
                old.email = maraEmail;
                old.refreshToken = token;
                c.setSession (old);
                c.call ("GET", "/me", {}, err);
                check (err.status == 401 && ! c.isSignedIn(), "a revoked sign-in can't be used again");
            }
        }

        // The Cloud menu and the account state behind it (what you see in Lathe).
        {
            std::cout << "Cloud: invite and listening links" << std::endl;
            {
                // Invite links: Mara makes one for the song, Dev (already a friend) pastes it.
                DynamicObject::Ptr ask = new DynamicObject();
                ask->setProperty ("song", maraStore.getProjectId());
                ask->setProperty ("role", "viewer");
                cloud::ApiError linkErr;
                const auto invite = mara.call ("POST", "/v2/invites", var (ask.get()), linkErr);
                const auto inviteUrl = invite["url"].toString();
                check (linkErr.status == 0 && inviteUrl.contains ("/join.html#"), "Mara makes an invite link (" + inviteUrl + linkErr.message + ")");
                const auto code = cloudlinks::inviteCode ("  " + inviteUrl + "\n");
                check (code.isNotEmpty() && code == invite["code"].toString(), "the code is read back out of a pasted link");
                check (cloudlinks::inviteCode ("https://getlathe.app/").isEmpty() && cloudlinks::inviteCode ("hello").isEmpty(),
                       "and something that isn't a link isn't mistaken for one");
                const auto joined = dev.call ("POST", "/v2/invites/" + code + "/accept", {}, linkErr);
                check (linkErr.status == 0 && joined["song"]["id"].toString() == maraStore.getProjectId(),
                       "Dev accepts it and gets the song (" + linkErr.message + ")");
                check (joined["song"]["role"].toString() == "editor", "keeping the editor role Dev already had, not the link's viewer role");

                // Listening links: render, FLAC, upload, register.
                Engine eng (host, pool, props);
                eng.setEdit (&maraEdit);
                FakeDevice dev2 (2, 2, 0, 0);
                eng.audioDeviceAboutToStart (&dev2);
                eng.rebuild();
                Engine::ExportSettings s;
                s.startBeat = 0.0;
                s.endBeat = 4.0;
                s.tailSeconds = 0.5;
                s.bitDepth = 16;
                s.folder = dir.getChildFile ("listen-temp");
                s.baseName = "mix";
                String listenUrl;
                const auto shared = cloudlinks::shareMix (mara, eng, s, maraEdit.tempo, maraStore.getProjectId(), {}, listenUrl);
                check (shared.wasOk() && listenUrl.contains ("/listen.html#"), "Mara shares the mix (" + listenUrl + shared.getErrorMessage() + ")");
                check (! s.folder.exists(), "the render's temporary folder is cleaned up");
                eng.audioDeviceStopped();
                eng.setEdit (nullptr);

                // What the website's listen.html sees, with no token.
                const auto token = listenUrl.fromLastOccurrenceOf ("#", false, false);
                const auto page = cloud::httpRequest ("GET", config.apiUrl + "/listen/" + token, nullptr, {});
                const auto info = page.json();
                check (page.ok() && info["song"].toString() == "Jam" && (double) info["seconds"] > 2.0,
                       "the listening page gets the song, without signing in (" + page.body.toString().substring (0, 200) + ")");
                const auto audio = cloud::httpRequest ("GET", info["audio"].toString(), nullptr, {});
                check (audio.ok() && audio.body.getSize() > 100 && audio.body.toString().startsWith ("fLaC"),
                       "and the mix itself, as FLAC (" + String ((int) audio.body.getSize()) + " bytes)");

                const MemoryBlock note (String ("{\"name\": \"Sam\", \"text\": \"Love the hats\", \"at\": 1.2}").toRawUTF8(),
                                        strlen ("{\"name\": \"Sam\", \"text\": \"Love the hats\", \"at\": 1.2}"));
                const auto posted = cloud::httpRequest ("POST", config.apiUrl + "/listen/" + token + "/notes", &note,
                                                        "Content-Type: application/json\r\n");
                const auto notesNow = mara.call ("GET", "/v2/songs/" + maraStore.getProjectId() + "/notes", {}, linkErr);
                bool found = false;
                if (auto* a = notesNow["notes"].getArray())
                    for (auto& n : *a)
                        found = found || (n["text"].toString() == "At 0:01: Love the hats" && n["author"].toString() == "Sam (listening link)");
                check (posted.status == 201 && found, "a listener's note lands on the song (" + String (posted.status) + ")");
            }

            std::cout << "Cloud: the Cloud menu, signing out" << std::endl;
            PropertiesFile cloudProps (dir.getChildFile ("cloud-ui.settings"), opts);
            {
                cloud::Client c (config);
                c.signIn (maraEmail, maraPass, {});
                cloudProps.setValue ("cloudRefreshToken", c.getSession().refreshToken);   // as if signed in last time
                cloudProps.setValue ("cloudEmail", maraEmail);
            }
            AppContext uiCtx { edit, engine, host, pool, cloudProps };
            int menuChanges = 0;
            StringArray statuses;
            CloudController::Hooks hooks;
            hooks.setStatus = [&] (const String& s) { statuses.add (s); };
            hooks.menuChanged = [&] { ++menuChanges; };
            hooks.projectsDir = [&] { return dir; };
            CloudController cc (uiCtx, cloudProps, hooks);

            auto menuText = [&]
            {
                PopupMenu m;
                cc.addMenuItems (m);
                StringArray items;
                for (PopupMenu::MenuItemIterator it (m); it.next();)
                    items.add (it.getItem().text);
                return items.joinIntoString (" | ");
            };
            auto pumpUntil = [] (std::function<bool()> done, int ms)
            {
                const auto end = Time::getMillisecondCounter() + (uint32) ms;
                while (! done() && Time::getMillisecondCounter() < end)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                return done();
            };

            check (cc.isSignedIn() && menuText().contains ("Signed in as " + maraEmail) && menuText().contains ("Sign Out"),
                   "a saved sign-in is restored, and the menu says who (" + menuText() + ")");
            // The first check-in (plan, friend requests) happens a second after start-up.
            check (pumpUntil ([&] { return cc.getPlanName().isNotEmpty(); }, 8000), "the plan arrives (" + cc.getPlanName() + ")");
            const int before = menuChanges;
            check (before > 0 && menuText().contains ("(" + cc.getPlanName() + ")"), "and the menu was told to show it");

            // An answer still on its way when you sign out is dropped.
            bool late = false;
            cc.request ("GET", "/me", {}, [&] (const var&, const cloud::ApiError&) { late = true; });
            cc.signOut();
            check (! cc.isSignedIn() && menuChanges > before, "Sign Out signs out and rebuilds the menu");
            check (menuText().contains ("Sign In...") && ! menuText().contains ("Signed in as") && ! menuText().contains ("Sign Out"),
                   "the menu offers Sign In again (" + menuText() + ")");
            check (cc.getPlanName().isEmpty() && cc.getEmail().isEmpty(), "the last account's plan and email are forgotten");
            check (cloudProps.getValue ("cloudRefreshToken").isEmpty(), "the saved sign-in is deleted");
            check (statuses.joinIntoString ("\n").contains ("Signed out of Lathe Cloud (" + maraEmail + ")"), "the status bar says so");
            pumpUntil ([] { return false; }, 1500);
            check (! late, "an answer meant for the signed-out account is dropped");
            check (! cc.isSignedIn() && cloudProps.getValue ("cloudRefreshToken").isEmpty(), "and nothing signs back in afterwards");
        }
    }
    else
    {
        std::cout << "  (cloud end-to-end skipped: run lathe-backend/scripts/local_stack.py and set LATHE_API_URL etc.)" << std::endl;
    }

    std::cout << "Join clips" << std::endl;
    {
        Edit j;
        auto jdir = dir.getChildFile ("join");
        jdir.createDirectory();
        j.workDir = jdir;
        j.sampleRate = 48000.0;
        j.tempo = 120.0;   // 24000 samples per beat

        // A steady 0.25 signal, so gains and gaps are easy to read back.
        const auto src = jdir.getChildFile ("steady.wav");
        {
            AudioBuffer<float> b (1, 96000);
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (0, i, 0.25f);
            std::unique_ptr<OutputStream> stream (src.createOutputStream());
            WavAudioFormat wav;
            auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }

        auto t = j.addTrack (TrackKind::audio, "Gtr");
        t->input.channel = -1;
        Clip a;
        a.id = j.newId(); a.kind = ClipKind::audio; a.name = "Gtr 1"; a.file = src;
        a.start = 0.0; a.length = 1.0;
        Clip b = a;
        b.id = j.newId(); b.name = "Gtr 2"; b.start = 2.0; b.length = 1.0; b.sourceOffset = 1000; b.gainDb = 6.0f;
        t->clips = { b, a };   // not in time order on purpose

        // What the two clips sound like through the engine...
        engine.setEdit (&j);
        engine.rebuild();
        j.insertMarker = 0.0;
        engine.play();
        const auto before = runCallbacks (engine, 320);
        engine.stop();

        j.beginClipEdit();
        int joinedId = -1;
        const auto r = joinClips (j, pool, *t, { a.id, b.id }, joinedId);
        check (r.wasOk() && t->clips.size() == 1 && t->clips[0].id == joinedId, "two audio clips join into one (" + r.getErrorMessage() + ")");
        if (t->clips.size() == 1)
        {
            const auto& c = t->clips[0];
            check (c.start == 0.0 && std::abs (c.length - 3.0) < 1.0e-9 && c.name == "Gtr 1", "the joined clip spans both and takes the first one's name");
            check (c.file.existsAsFile() && c.file.getParentDirectory() == j.getAudioDir(), "it's a new file in the project's Audio folder");

            std::unique_ptr<AudioFormatReader> rd (pool.formats.createReaderFor (c.file));
            AudioBuffer<float> got (1, (int) rd->lengthInSamples);
            rd->read (&got, 0, got.getNumSamples(), 0, true, false);
            check (got.getNumSamples() == 72000, "3 beats long (" + String (got.getNumSamples()) + " samples)");
            check (std::abs (got.getSample (0, 12000) - 0.25f) < 0.001f, "first clip's audio is there");
            check (got.getSample (0, 36000) == 0.0f, "the gap between them is silence");
            check (std::abs (got.getSample (0, 60000) - 0.25f * Decibels::decibelsToGain (6.0f)) < 0.001f, "second clip keeps its +6 dB clip gain");
        }

        // ...and the joined clip sounds the same.
        engine.rebuild();
        engine.play();
        const auto after = runCallbacks (engine, 320);
        engine.stop();
        float worst = 0.0f;
        for (size_t i = 0; i < jmin (before.size(), after.size()); ++i)
            worst = jmax (worst, std::abs (before[i] - after[i]));
        check (before.size() == after.size() && worst < 1.0e-4f, "playback is identical after joining (max difference " + String (worst) + ")");

        check (j.undo() && t->clips.size() == 2, "undo brings the two clips back");

        // MIDI: notes move into the new clip; what was hidden past a clip's end stays out.
        auto m = j.addTrack (TrackKind::midi, "Keys");
        Clip ma;
        ma.id = j.newId(); ma.kind = ClipKind::midi; ma.name = "Keys 1"; ma.start = 1.0; ma.length = 1.0;
        ma.notes = { { 60, 0.5, 0.25, 0.8f }, { 62, 0.9, 0.5, 0.8f }, { 64, 1.5, 0.25, 0.8f } };
        MidiEvent cc; cc.beat = 0.25; cc.size = 3; cc.data[0] = 0xb0; cc.data[1] = 1; cc.data[2] = 64;
        ma.events = { cc };
        Clip mb;
        mb.id = j.newId(); mb.kind = ClipKind::midi; mb.name = "Keys 2"; mb.start = 4.0; mb.length = 2.0;
        mb.notes = { { 67, 0.0, 1.0, 0.8f } };
        m->clips = { ma, mb };

        int midiId = -1;
        const auto mr = joinClips (j, pool, *m, { ma.id, mb.id }, midiId);
        check (mr.wasOk() && m->clips.size() == 1, "two MIDI clips join into one");
        if (m->clips.size() == 1)
        {
            const auto& c = m->clips[0];
            check (c.start == 1.0 && c.length == 5.0, "spans 1 to 6");
            check (c.notes.size() == 3, "the note hidden past the first clip's end is left out (" + String ((int) c.notes.size()) + " notes)");
            check (c.notes.size() == 3 && c.notes[0].start == 0.5 && std::abs (c.notes[1].length - 0.1) < 1.0e-9 && c.notes[2].start == 3.0 && c.notes[2].pitch == 67,
                   "notes keep their place, and one running past the clip end is cut there");
            check (c.events.size() == 1 && c.events[0].beat == 0.25, "controller data comes along");
        }

        Clip mc = m->clips[0];
        Clip md;
        md.id = j.newId(); md.kind = ClipKind::audio; md.file = src; md.start = 8.0; md.length = 1.0;
        int none = -1;
        check (joinClips (j, pool, *m, { mc.id }, none).failed(), "joining one clip is refused");
        t->clips.push_back (md);
        check (joinClips (j, pool, *t, { a.id, md.id, b.id }, none).wasOk(), "three audio clips with gaps join");

        engine.setEdit (&edit);
    }

    std::cout << "AI assistant (MCP)" << std::endl;
    {
        using namespace lathe::assistant;
        const auto k1 = makeToken(), k2 = makeToken();
        check (k1.length() == 64 && k1.containsOnly ("0123456789abcdef") && k1 != k2, "keys are 256 random bits, different every time");
        check (tokensMatch (k1, k1) && ! tokensMatch (k1, k2) && ! tokensMatch (k1, k1.dropLastCharacters (1)) && ! tokensMatch ({}, {}),
               "keys compare exactly (and an empty key never matches)");

        Edit e;
        e.tempo = 120.0;
        engine.setEdit (&e);
        auto gen = e.addTrack (TrackKind::midi, "Dice");
        {
            auto slot = std::make_shared<PluginSlot>();
            slot->description.name = "Gen";
            slot->instance = std::make_unique<NoteGenerator> (-1, -1);
            gen->plugins.push_back (slot);
            Clip c;
            c.id = e.newId();
            c.kind = ClipKind::midi;
            c.start = 4.0;   // bar 2
            c.length = 8.0;
            c.notes = { { 64, 2.0, 1.0, 1.0f }, { 60, 0.0, 0.5, 0.5f }, { 72, 9.0, 1.0, 0.8f } };   // the last is past the clip end
            gen->clips.push_back (c);
        }
        auto synth = e.addTrack (TrackKind::midi, "Synth");
        {
            auto slot = std::make_shared<PluginSlot>();
            slot->description.name = "Eater";
            slot->description.isInstrument = true;
            slot->instance = std::make_unique<NoteEater>();
            synth->plugins.push_back (slot);
        }
        synth->input.track = gen->id;
        engine.rebuild();

        Tools tools (e, engine);
        check (tools.findTrack ("synth") == synth && tools.findTrack (gen->id) == gen && tools.findTrack (String (gen->id)) == gen
               && tools.findTrack ("Master") == e.master && tools.findTrack ("nope") == nullptr,
               "finds tracks by name (any case), id and \"Master\"");

        const auto problems = tools.findProblems().joinIntoString ("\n");
        check (problems.contains ("\"Synth\" takes its input from \"Dice\", but only plays it while armed"),
               "explains that a track fed by another only plays it while armed");
        check (problems.contains ("\"Dice\"'s clips go into Gen"), "warns that a generator hides the clips it's fed");
        check (! problems.contains ("\"Dice\" has no instrument"), "a generator feeding another track isn't called silent");
        synth->monitor = (int) Monitor::in;
        gen->plugins[0]->bypassed = true;
        const auto after = tools.findProblems().joinIntoString ("\n");
        check (! after.contains ("only plays it while armed") && ! after.contains ("clips go into"),
               "...and stops once monitoring is In and the generator is bypassed");

        String status;
        auto session = tools.call ("describe_session", var (new DynamicObject()), status);
        const auto sessionText = session["content"][0]["text"].toString();
        const auto sessionJson = JSON::parse (sessionText);
        check (! (bool) session["isError"] && sessionJson["tracks"].size() == 2 && (double) sessionJson["tempo_bpm"] == 120.0
               && sessionJson["tracks"][1]["input"].toString() == "from track \"Dice\"" && sessionJson["tracks"][0]["plugins"][0]["role"].toString() == "note generator",
               "describe_session lists the tracks, their inputs and plug-in roles");

        // --- Working together: the assistant is told what the person changed, and can be paused
        {
            AppContext actx { e, engine, host, pool, props };
            StringArray said;
            actx.showStatus = [&] (const String& st) { said.add (st); };
            assistant::Service svc (actx);
            int activity = 0;
            svc.onActivity = [&] { ++activity; };
            auto txt = [] (const var& r)
            {
                StringArray parts;
                if (auto* c = r["content"].getArray())
                    for (auto& x : *c)
                        parts.add (x["text"].toString());
                return parts.joinIntoString ("\n");
            };
            auto change = [] (const String& json) { return JSON::parse (R"({"changes":)" + json + "}"); };

            svc.runTool ("describe_session", var (new DynamicObject()));
            check (activity == 1 && svc.isWorking(), "working together: a tool call counts as the assistant working");

            const auto oldName = synth->name;
            synth->name = "Lead";   // the person renames a track between the assistant's calls
            auto r = svc.runTool ("edit_session", change (R"([{"action":"set_mixer","track":"Dice","volume_db":-4}])"));
            check ((bool) r["isError"] && txt (r).contains ("Nothing was changed") && txt (r).contains ("Lead")
                       && std::abs (gen->volumeDb.load() + 4.0f) > 0.01f,
                   "working together: a change on an out-of-date picture is held back, saying what the person changed (" + txt (r).substring (0, 160) + ")");
            r = svc.runTool ("edit_session", change (R"([{"action":"set_mixer","track":"Dice","volume_db":-4}])"));
            check (! (bool) r["isError"] && std::abs (gen->volumeDb.load() + 4.0f) < 0.01f,
                   "working together: once told, the same change goes through");

            synth->volumeDb = -9.0f;   // the person rides a fader
            r = svc.runTool ("edit_session", change (R"([{"action":"set_mixer","track":"Dice","pan":0.2}])"));
            check (! (bool) r["isError"] && txt (r).contains ("Since you last looked") && txt (r).contains ("fader"),
                   "working together: a fader the person moved doesn't stop a change, but it's mentioned");

            svc.setPaused (true);
            r = svc.runTool ("describe_session", var (new DynamicObject()));
            check ((bool) r["isError"] && txt (r).contains ("Paused") && svc.isPaused(), "working together: paused, every tool answers that it's paused");
            r = svc.runTool ("edit_session", change (R"([{"action":"set_mixer","track":"Dice","pan":0.5}])"));
            check ((bool) r["isError"] && std::abs (gen->pan.load() - 0.2f) < 0.01f, "working together: and nothing changes while paused");
            svc.setPaused (false);
            r = svc.runTool ("edit_session", change (R"([{"action":"set_mixer","track":"Dice","pan":0.5}])"));
            check (! (bool) r["isError"] && std::abs (gen->pan.load() - 0.5f) < 0.01f, "working together: after Continue it carries on");

            // --- The channel EQ, through edit_session
            svc.runTool ("describe_session", var (new DynamicObject()));
            r = svc.runTool ("edit_session", change (R"([{"action":"set_eq","track":"Dice","band":"bell 1","freq_hz":300,"gain_db":-3,"q":1.4},
                                                          {"action":"set_eq","track":"Dice","band":"Low cut","freq_hz":90}])"));
            const auto& b1 = gen->eq.bands[2];
            const auto& lc = gen->eq.bands[0];
            check (! (bool) r["isError"] && std::abs (b1.freq.load() - 300.0f) < 0.01f && std::abs (b1.gainDb.load() + 3.0f) < 0.01f
                       && std::abs (b1.q.load() - 1.4f) < 0.01f && lc.on.load() && std::abs (lc.freq.load() - 90.0f) < 0.01f
                       && txt (r).contains ("Bell 1 300 Hz -3.0 dB"),
                   "set_eq: a bell cut and a low cut (switched on by setting it) (" + txt (r).substring (0, 200) + ")");
            r = svc.runTool ("edit_session", change (R"([{"action":"set_eq","track":"Dice","band":"high shelf","gain_db":2},
                                                          {"action":"set_eq","track":"Dice","band":"low cut","gain_db":-6}])"));
            check ((bool) r["isError"] && txt (r).contains ("has no gain") && std::abs (gen->eq.bands[4].gainDb.load()) < 0.01f,
                   "set_eq: a cut has no gain, and the whole list is held back (" + txt (r).substring (0, 160) + ")");
            r = svc.runTool ("edit_session", change (R"([{"action":"set_eq","track":"Dice","band":"mid","gain_db":2}])"));
            check ((bool) r["isError"] && txt (r).contains ("bell 1"), "set_eq: an unknown band says which there are");
            check (e.undo() && std::abs (gen->eq.bands[2].gainDb.load()) < 0.01f && ! gen->eq.bands[0].on.load(),
                   "set_eq: one undo takes the assistant's EQ back");
            e.redo();
            check (std::abs (gen->eq.bands[2].gainDb.load() + 3.0f) < 0.01f, "set_eq: and redo brings it back");
            check (e.getDoneSteps().back().byAssistant && e.getDoneSteps().back().label.contains ("Bell 1 300 Hz"),
                   "history: the assistant's change is marked as its own, in its words (" + e.getDoneSteps().back().label + ")");
            const auto described = JSON::parse (txt (svc.runTool ("describe_track", JSON::parse (R"({"track":"Dice"})"))));
            check (described["eq"]["bands"].toString().contains ("Bell 1 300 Hz -3.0 dB"), "describe_track shows the EQ it set");
            gen->eq.reset();
            e.commitUndoStep();

            synth->name = oldName;
            gen->pan = 0.0f;
            gen->volumeDb = 0.0f;
            synth->volumeDb = 0.0f;
        }

        auto args = var (new DynamicObject());
        args.getDynamicObject()->setProperty ("track", "dice");
        const auto notesJson = JSON::parse (tools.call ("get_notes", args, status)["content"][0]["text"].toString());
        const auto* list = notesJson["notes"].getArray();
        check (list != nullptr && list->size() == 2 && (int) (*list)[0]["pitch"] == 60 && (*list)[0]["name"].toString() == "C3"
               && (double) (*list)[0]["start_bar"] == 2.0 && (double) (*list)[1]["start_bar"] == 2.5 && (int) (*list)[0]["velocity"] == 64,
               "get_notes: time order, names (C3 = middle C), song bars, velocity 1-127, nothing past the clip end");
        args.getDynamicObject()->setProperty ("from_bar", 2.25);
        check (JSON::parse (tools.call ("get_notes", args, status)["content"][0]["text"].toString())["notes"].size() == 1, "get_notes from a bar");
        args.getDynamicObject()->setProperty ("track", "Nope");
        check ((bool) tools.call ("get_notes", args, status)["isError"], "an unknown track is an error the AI can read");

        args.getDynamicObject()->setProperty ("track", "Dice");
        const auto trackJson = JSON::parse (tools.call ("describe_track", args, status)["content"][0]["text"].toString());
        check (trackJson["clip_list"].size() == 1 && (int) trackJson["clip_list"][0]["notes"] == 2 && trackJson["feeds"].toString() == "Synth",
               "describe_track lists clips (with note counts) and who it feeds");

        auto from = var (new DynamicObject());
        from.getDynamicObject()->setProperty ("from_bar", 3);
        status = {};
        const auto played = tools.call ("play", from, status);
        check (! (bool) played["isError"] && engine.isPlaying() && std::abs (e.insertMarker - 8.0) < 1.0e-9 && status.contains ("bar 3"),
               "play from bar 3 (and says so in the status bar)");
        runCallbacks (engine, 4);
        status = {};
        tools.call ("stop", var (new DynamicObject()), status);
        check (! engine.isPlaying() && std::abs (e.insertMarker - 8.0) < 1.0e-9 && status.isNotEmpty(), "stop (the insert marker stays)");
        check (tools.call ("stop", var (new DynamicObject()), status)["content"][0]["text"].toString().contains ("wasn't playing")
               && std::abs (e.insertMarker - 8.0) < 1.0e-9, "stop while stopped does nothing (doesn't rewind)");

        auto args2 = [] (std::initializer_list<std::pair<const char*, var>> kv)
        {
            auto o = var (new DynamicObject());
            for (auto& [k, v] : kv)
                o.getDynamicObject()->setProperty (k, v);
            return o;
        };
        auto text = [] (const var& r) { return r["content"][0]["text"].toString(); };

        // --- write_notes: a four-on-the-floor bar, repeated
        {
            auto beat = e.addTrack (TrackKind::midi, "Beat");
            auto noteOf = [&] (var pitch, double bar, int velocity = 100)
            {
                return args2 ({ { "pitch", pitch }, { "start_bar", bar }, { "velocity", velocity } });
            };
            Array<var> bar1;
            for (int k = 0; k < 4; ++k)
                bar1.add (noteOf (36, 1.0 + k * 0.25));
            bar1.add (noteOf ("D1", 1.25));   // 38, the snare
            bar1.add (noteOf ("F#1", 1.125, 80));   // 42, a hat on the off-beat
            e.commitUndoStep();
            auto w = tools.call ("write_notes", args2 ({ { "track", "beat" }, { "from_bar", 1 }, { "to_bar", 2 },
                                                         { "notes", bar1 }, { "repeat", 4 } }), status);
            check (! (bool) w["isError"] && beat->clips.size() == 1 && beat->clips[0].notes.size() == 24
                       && std::abs (beat->clips[0].length - 16.0) < 1.0e-9 && text (w).contains ("Wrote 24 notes"),
                   "write_notes: one clip, four bars, 24 notes (" + text (w) + ")");
            if (beat->clips.size() == 1 && beat->clips[0].notes.size() == 24)
            {
                auto& n = beat->clips[0].notes;
                int kicks = 0, snares = 0, hats = 0;
                for (auto& x : n)
                    (x.pitch == 36 ? kicks : x.pitch == 38 ? snares : x.pitch == 42 ? hats : kicks) += 1;
                check (kicks == 16 && snares == 4 && hats == 4 && std::abs (n.back().start - 15.0) < 1.0e-9
                           && std::abs (n[0].length - 0.25) < 1.0e-9 && std::abs (n[0].velocity - 100.0f / 127.0f) < 1.0e-4,
                       "notes by number and name (C3 = middle C), repeated a bar apart, 16th notes by default");
            }

            Array<var> one { noteOf (39, 2.5) };
            w = tools.call ("write_notes", args2 ({ { "track", "Beat" }, { "from_bar", 2 }, { "to_bar", 3 }, { "notes", one }, { "mode", "add" } }), status);
            check (! (bool) w["isError"] && beat->clips.size() == 1 && beat->clips[0].notes.size() == 25, "mode add joins the clip that's there");

            w = tools.call ("write_notes", args2 ({ { "track", "Beat" }, { "from_bar", 2 }, { "to_bar", 3 }, { "notes", one } }), status);
            int inBar2 = 0;
            for (auto& c : beat->clips)
                for (auto& x : c.notes)
                    if (c.start + x.start >= 4.0 && c.start + x.start < 8.0) ++inBar2;
            check (! (bool) w["isError"] && beat->clips.size() == 3 && inBar2 == 1, "replace clears those bars first (" + String ((int) beat->clips.size()) + " clips)");

            check ((bool) tools.call ("write_notes", args2 ({ { "track", "Beat" }, { "from_bar", 1 }, { "to_bar", 2 },
                                                              { "notes", Array<var> { noteOf (36, 2.0) } } }), status)["isError"],
                   "a note past to_bar is an error");
            check ((bool) tools.call ("write_notes", args2 ({ { "track", "Beat" }, { "from_bar", 1 }, { "to_bar", 2 },
                                                              { "notes", Array<var> { noteOf ("H2", 1.0) } } }), status)["isError"],
                   "so is a pitch that isn't one");
            e.commitUndoStep();
            e.undo();
            check (beat->clips.size() == 1 && beat->clips[0].notes.size() == 25, "each write is one undo step");
            e.removeTrack (beat->id);
            e.commitUndoStep();
        }

        // --- get_plugin_params / set_plugin_params, on Lathe's own Drums and Delay
        {
            Array<var> plugins { "Drums", "Delay" };
            auto made = tools.call ("edit_session", args2 ({ { "changes", Array<var> { args2 ({ { "action", "add_track" }, { "kind", "midi" },
                                                                                              { "name", "Knobs" }, { "plugins", plugins } }) } } }), status);
            auto knobs = tools.findTrack ("Knobs");
            check (! (bool) made["isError"] && knobs != nullptr && knobs->plugins.size() == 2 && knobs->plugins[0]->instance != nullptr,
                   "a track with Drums and Delay for the settings tools (" + text (made) + ")");
            if (knobs != nullptr && knobs->plugins.size() == 2 && knobs->plugins[0]->instance != nullptr)
            {
                auto* drums = knobs->plugins[0]->instance.get();
                auto paramNamed = [] (AudioPluginInstance& inst, const String& name) -> AudioProcessorParameter*
                {
                    for (auto* p : inst.getParameters())
                        if (p->getName (64) == name)
                            return p;
                    return nullptr;
                };
                auto* kit = paramNamed (*drums, "Kit");
                auto* level = paramNamed (*drums, "Level");

                const auto listed = JSON::parse (text (tools.call ("get_plugin_params", args2 ({ { "track", "knobs" }, { "plugin", "drums" } }), status)));
                var kitInfo;
                for (auto& p : *listed["parameters"].getArray())
                    if (p["name"].toString() == "Kit")
                        kitInfo = p;
                check (listed["plugin"].toString() == "Drums" && kitInfo["value"].toString() == "Simple 909" && kitInfo["id"].toString() == "kit"
                           && kitInfo["choices"].size() == 4 && kitInfo["choices"][1].toString() == "Simple 808",
                       "get_plugin_params lists Drums' Kit with its id, value and choices");
                check (JSON::parse (text (tools.call ("get_plugin_params", args2 ({ { "track", "Knobs" }, { "plugin", 1 }, { "search", "tune" } }), status)))["parameters"].size() == 3,
                       "get_plugin_params: search narrows it, and the plug-in can be given by position");

                auto change = [&] (var param, var value) { return args2 ({ { "param", param }, { "value", value } }); };
                auto setParams = [&] (const String& plugin, Array<var> params)
                {
                    return tools.call ("set_plugin_params", args2 ({ { "track", "Knobs" }, { "plugin", plugin }, { "params", params } }), status);
                };
                status = {};
                auto s = setParams ("Drums", { change ("kit", "simple 808"), change ("Level", -10.0) });
                check (! (bool) s["isError"] && kit != nullptr && kit->getCurrentValueAsText() == "Simple 808"
                           && level != nullptr && level->getCurrentValueAsText().startsWith ("-10")
                           && text (s).contains ("Kit: Simple 909 -> Simple 808") && status.contains ("Drums on Knobs"),
                       "set_plugin_params picks a kit by name and sets a level in dB, and says what each was (" + text (s) + ")");
                s = setParams ("Drums", { change ("Kit", "Simple 606"), change ("Kit", "Simple 9000") });
                check ((bool) s["isError"] && kit->getCurrentValueAsText() == "Simple 808" && text (s).contains ("Simple 707"),
                       "a choice that doesn't exist changes nothing, not even the change before it, and lists the real ones");
                s = setParams ("Drums", { change ("Level", 40.0) });
                check (! (bool) s["isError"] && level->getCurrentValueAsText().startsWith ("+6") && text (s).contains ("outside"),
                       "a number past the range is held at its end, and the reply says so (" + text (s) + ")");
                s = setParams ("Drums", { change ("Snap", "75%") });
                auto* snap = paramNamed (*drums, "Snap");
                check (! (bool) s["isError"] && snap != nullptr && snap->getCurrentValueAsText() == "75%",
                       "a percentage as text on a 0-1 control (" + text (s) + ")");
                s = setParams ("Drums", { change ("tune", 2.0) });
                check ((bool) s["isError"] && text (s).contains ("Kick tune"), "an ambiguous name asks which one");
                s = setParams ("Delay", { change ("Time", 250.0) });
                auto* time = paramNamed (*knobs->plugins[1]->instance, "Time");
                check (! (bool) s["isError"] && time != nullptr && time->getCurrentValueAsText() == "250 ms",
                       "Delay's time in milliseconds (" + text (s) + ")");
                check ((bool) setParams ("Nope", { change ("Time", 1.0) })["isError"], "an unknown plug-in is an error");
            }
            if (knobs != nullptr)
                e.removeTrack (knobs->id);
            e.commitUndoStep();
        }

        // --- new_project / open_project / save_project, with stand-ins for the app's File menu
        {
            const auto pdir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-tools-projects-" + String (Random::getSystemRandom().nextInt64()));
            pdir.getChildFile ("Templates").createDirectory();
            pdir.getChildFile ("Templates").getChildFile ("8-Input Tracking.lathe").replaceWithText ("{}");
            for (auto name : { "Night Drive", "Night Bus", "Teaser 2" })
                pdir.getChildFile (name).getChildFile (String (name) + ".lathe").create();
            const auto oldFile = e.projectFile;
            const auto oldDir = e.workDir;

            StringArray calls;
            tools.project.projectsDir = pdir;
            tools.project.templatesDir = pdir.getChildFile ("Templates");
            tools.project.save = [&] (const File& as) -> String
            {
                const auto f = as == File() ? e.projectFile : as;
                f.getParentDirectory().createDirectory();
                f.replaceWithText (JSON::toString (e.toVar (false)));
                e.projectFile = f;
                e.workDir = f.getParentDirectory();
                e.dirty = false;
                calls.add ("save " + (as == File() ? String ("(here)") : as.getParentDirectory().getFileName() + "/" + as.getFileName()));
                return {};
            };
            tools.project.openProject = [&] (const File& f) -> String
            {
                calls.add ("open " + f.getFileNameWithoutExtension());
                e.projectFile = f;
                e.dirty = false;
                return {};
            };
            tools.project.newProject = [&] (const File& tmpl) -> String
            {
                calls.add ("new " + (tmpl == File() ? String ("(empty)") : tmpl.getFileNameWithoutExtension()));
                e.projectFile = File();
                e.dirty = false;
                return {};
            };

            e.projectFile = File();
            e.dirty = true;
            check ((bool) JSON::parse (text (tools.call ("describe_session", var (new DynamicObject()), status)))["unsaved_changes"],
                   "describe_session says when there are unsaved changes");
            auto r = tools.call ("save_project", args2 ({}), status);
            check ((bool) r["isError"] && text (r).contains ("give it a name") && calls.isEmpty(), "save_project on a never-saved project asks for a name");
            r = tools.call ("save_project", args2 ({ { "name", "Teaser 2" } }), status);
            check ((bool) r["isError"] && text (r).contains ("already a project") && calls.isEmpty(), "...and won't save over another project");
            r = tools.call ("save_project", args2 ({ { "name", "Odd Bounce 7/8" } }), status);
            check (! (bool) r["isError"] && calls.joinIntoString ("|") == "save Odd Bounce 78/Odd Bounce 78.lathe" && ! e.dirty,
                   "save_project with a name saves it as Name/Name.lathe in the projects folder (" + calls.joinIntoString ("|") + ")");
            calls.clear();
            e.dirty = true;
            r = tools.call ("save_project", args2 ({ { "name", "odd bounce 78" } }), status);
            check (! (bool) r["isError"] && calls.joinIntoString ("|") == "save (here)", "its own name again is an ordinary save");

            calls.clear();
            e.dirty = true;
            r = tools.call ("open_project", args2 ({ { "name", "drive" } }), status);
            check ((bool) r["isError"] && text (r).contains ("unsaved changes") && calls.isEmpty(),
                   "open_project won't replace unsaved changes unless told what to do with them");
            r = tools.call ("open_project", args2 ({ { "name", "drive" }, { "unsaved", "save" } }), status);
            check (! (bool) r["isError"] && calls.joinIntoString ("|") == "save (here)|open Night Drive",
                   "unsaved \"save\" saves first, then opens (by a unique part of the name)");
            calls.clear();
            r = tools.call ("open_project", args2 ({ { "name", "night" } }), status);
            check ((bool) r["isError"] && text (r).contains ("Night Bus") && calls.isEmpty(), "an ambiguous name lists what it could be");
            r = tools.call ("open_project", args2 ({}), status);
            check ((bool) r["isError"] && text (r).contains ("Teaser 2"), "without a name it lists the projects");
            r = tools.call ("open_project", args2 ({ { "name", "Night Drive" } }), status);
            check (! (bool) r["isError"] && text (r).contains ("already open") && calls.isEmpty(), "opening what's already open does nothing");

            e.dirty = true;
            r = tools.call ("new_project", args2 ({ { "template", "nope" } }), status);
            check ((bool) r["isError"] && text (r).contains ("8-Input Tracking") && calls.isEmpty(), "new_project names the templates there are");
            r = tools.call ("new_project", args2 ({ { "template", "8-input" }, { "unsaved", "discard" } }), status);
            check (! (bool) r["isError"] && calls.joinIntoString ("|") == "new 8-Input Tracking" && text (r).contains ("Untitled"),
                   "new_project from a template, discarding the changes when told to");
            calls.clear();
            e.dirty = true;
            r = tools.call ("new_project", args2 ({ { "unsaved", "save" } }), status);
            check ((bool) r["isError"] && text (r).contains ("never been saved") && calls.isEmpty(),
                   "\"save\" on a project that was never saved stops, rather than losing it");

            e.projectFile = oldFile;
            e.workDir = oldDir;
            e.dirty = false;
            tools.project = {};
            pdir.deleteRecursively();
        }

        // --- export_audio: a background render, then the mix's level
        {
            const auto oldDir = e.workDir;
            e.workDir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-tools-export-" + String (Random::getSystemRandom().nextInt64()));
            var immediate;
            auto job = tools.startLongTool ("export_audio", args2 ({ { "from_bar", 1 }, { "to_bar", 2 }, { "stems", true }, { "tail_seconds", 0 } }), immediate);
            check (job != nullptr, "export_audio starts" + (job == nullptr ? " (" + text (immediate) + ")" : String()));
            if (job != nullptr)
            {
                for (int i = 0; i < 6000 && ! job->done.wait (0); ++i)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                const auto out = job->done.wait (0) ? JSON::parse (text (job->result)) : var();
                const auto* files = out["files"].getArray();
                check (job->done.wait (0) && ! (bool) job->result["isError"] && files != nullptr && files->size() >= 1
                           && std::abs ((double) (*files)[0]["seconds"] - 2.0) < 0.05 && ((*files)[0].hasProperty ("integrated_lufs") || (*files)[0]["level"].toString() == "silent") && File (out["folder"].toString()).isDirectory(),
                       "export_audio writes to the Exports folder, with lengths and the mix's level ("
                           + (job->done.wait (0) ? text (job->result).substring (0, 300) : String ("still running")) + ")");
                var again;
                check (tools.startLongTool ("export_audio", args2 ({ { "from_bar", 3 }, { "to_bar", 2 } }), again) == nullptr
                           && text (again).contains ("after from_bar"), "bars the wrong way round are an error");
            }
            e.workDir.deleteRecursively();
            e.workDir = oldDir;
        }

        // --- analyze_mix: the same offline render, into the analysis instead of files
        {
            var immediate;
            auto job = tools.startLongTool ("analyze_mix", args2 ({ { "from_bar", 1 }, { "to_bar", 3 }, { "section_bars", 1 } }), immediate);
            check (job != nullptr, "analyze_mix starts" + (job == nullptr ? " (" + text (immediate) + ")" : String()));
            if (job != nullptr)
            {
                for (int i = 0; i < 6000 && ! job->done.wait (0); ++i)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                const auto out = job->done.wait (0) ? JSON::parse (text (job->result)) : var();
                const auto* trackList = out["tracks"].getArray();
                const auto* sections = out["sections"].getArray();
                check (job->done.wait (0) && ! (bool) job->result["isError"] && out["bars"].toString() == "1 to 3"
                           && out["mix"].isObject() && trackList != nullptr && trackList->size() == (int) (e.tracks.size() + e.buses.size())
                           && sections != nullptr && sections->size() == 2 && out["masking"].isArray() && out["notes"].isArray(),
                       "analyze_mix reports the mix, every track and return, each bar as a section ("
                           + (job->done.wait (0) ? text (job->result).substring (0, 400) : String ("still running")) + ")");
                var again;
                check (tools.startLongTool ("analyze_mix", args2 ({ { "from_bar", 3 }, { "to_bar", 2 } }), again) == nullptr
                           && text (again).contains ("after from_bar"), "analyze_mix: bars the wrong way round are an error");
            }
        }

        // --- Compare: Before, Now and a reference (compare_mix, and the window's model)
        {
            Compare cmp (engine, e, props);
            tools.compare = &cmp;
            auto waitJob = [] (const std::shared_ptr<Tools::Job>& j)
            {
                for (int i = 0; i < 6000 && j != nullptr && ! j->done.wait (0); ++i)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                return j != nullptr && j->done.wait (0);
            };
            auto waitIdle = [&cmp]
            {
                for (int i = 0; i < 6000 && cmp.isBusy(); ++i)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                return ! cmp.isBusy();
            };
            const bool oldLoop = e.loopEnabled;
            e.loopEnabled = false;
            // Something to hear: a track playing a tone for the first two bars.
            auto toneTrack = e.addTrack (TrackKind::audio, "Tone");
            {
                const auto toneFile = dir.getChildFile ("compare tone.wav");
                AudioBuffer<float> tone (1, 96000);
                for (int i = 0; i < 96000; ++i)
                    tone.setSample (0, i, 0.3f * (float) std::sin (2.0 * MathConstants<double>::pi * 440.0 * i / 48000.0));
                toneFile.deleteFile();
                std::unique_ptr<OutputStream> stream (toneFile.createOutputStream());
                WavAudioFormat wav;
                auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
                w->writeFromAudioSampleBuffer (tone, 0, 96000);
                w.reset();
                Clip toneClip;
                toneClip.id = e.newId();
                toneClip.kind = ClipKind::audio;
                toneClip.file = toneFile;
                toneClip.start = 0.0;
                toneClip.length = 4.0;
                toneTrack->clips.push_back (toneClip);
                engine.rebuild();
            }

            var immediate;
            auto job = tools.startLongTool ("compare_mix", args2 ({ { "mark_before", true } }), immediate);
            const bool markedOk = job != nullptr && waitJob (job);
            check (markedOk && ! (bool) job->result["isError"] && cmp.get (Compare::before).has()
                       && cmp.get (Compare::before).measures.valid && text (job->result).contains ("Before is set"),
                   "compare_mix mark_before renders and measures Before ("
                       + (markedOk ? text (job->result).substring (0, 200) : immediate.isObject() ? text (immediate) : String ("didn't finish")) + ")");

            e.master->volumeDb = -6.0f;   // the change to hear
            engine.rebuild();
            job = tools.startLongTool ("compare_mix", args2 ({}), immediate);
            const bool ran = job != nullptr && waitJob (job);
            const auto out = ran ? JSON::parse (text (job->result)) : var();
            const auto& b = cmp.get (Compare::before).measures;
            const auto& n = cmp.get (Compare::now).measures;
            check (ran && out["before"].isObject() && out["now"].isObject()
                       && cmp.get (Compare::now).audio->getNumSamples() == cmp.get (Compare::before).audio->getNumSamples(),
                   "compare_mix renders Now over the same bars and reports both (" + (ran ? text (job->result).substring (0, 240) : String ("didn't finish")) + ")");
            check (b.lufsValid && n.lufsValid && std::abs ((n.lufs - b.lufs) + 6.0f) < 0.3f && cmp.summary().contains ("loudness -6"),
                   "Compare hears a 6 dB fader move (" + cmp.summary() + ")");
            check (std::abs (cmp.gainFor (Compare::before) - (n.lufsValid && b.lufsValid ? n.lufs - b.lufs : 0.0f)) < 0.01f && cmp.gainFor (Compare::now) == 0.0f,
                   "match loudness turns the louder one down to the quieter");
            cmp.setMatchLoudness (false);
            check (cmp.gainFor (Compare::before) == 0.0f, "and off plays them as they are");
            cmp.setMatchLoudness (true);

            // Cancelling (the version's button turns into Cancel while it renders, or Escape).
            {
                const auto nowAudio = cmp.get (Compare::now).audio;
                e.master->volumeDb = -3.0f;
                engine.rebuild();
                auto cancelJob = tools.startLongTool ("compare_mix", args2 ({}), immediate);
                const bool wasBusy = cmp.isBusy() && cmp.busySlot() == Compare::now;
                cmp.cancel();
                const bool cancelFinished = cancelJob != nullptr && waitJob (cancelJob);
                check (wasBusy && cancelFinished && (bool) cancelJob->result["isError"]
                           && text (cancelJob->result).contains ("cancelled") && ! cmp.isBusy()
                           && cmp.get (Compare::now).audio == nowAudio,
                       "cancelling Update Now keeps the Now there was, and tells the assistant ("
                           + (cancelFinished ? text (cancelJob->result) : String ("didn't finish")) + ")");
                e.master->volumeDb = -6.0f;
                engine.rebuild();
            }

            // A reference at another sample rate, remembered for the song.
            const auto oldFile = e.projectFile;
            e.projectFile = dir.getChildFile ("compare-song.lathe");
            const auto refFile = dir.getChildFile ("reference 44k.wav");
            {
                AudioBuffer<float> sine (2, 44100);
                for (int i = 0; i < 44100; ++i)
                    sine.setSample (0, i, 0.25f * std::sin (2.0 * MathConstants<double>::pi * 220.0 * i / 44100.0)),
                    sine.setSample (1, i, sine.getSample (0, i));
                refFile.deleteFile();
                std::unique_ptr<OutputStream> stream (refFile.createOutputStream());
                WavAudioFormat wav;
                auto w = wav.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24));
                w->writeFromAudioSampleBuffer (sine, 0, 44100);
            }
            check (cmp.loadReference (refFile).wasOk() && waitIdle() && cmp.get (Compare::reference).has()
                       && std::abs (cmp.get (Compare::reference).audio->getNumSamples() - engine.getSampleRate()) < 4.0
                       && cmp.get (Compare::reference).measures.lufsValid,
                   "the reference is read, resampled to the device rate and measured");
            cmp.projectChanged();
            check (! cmp.get (Compare::before).has() && waitIdle() && cmp.get (Compare::reference).has() && cmp.get (Compare::reference).file == refFile,
                   "opening the song again forgets Before and Now and brings its reference back");
            check (cmp.loadReference (dir.getChildFile ("nope.wav")).failed(), "a missing file is refused");

            // Playing: what you hear replaces the song's output, and switching keeps the place.
            cmp.setHeard (Compare::reference);
            cmp.play();
            const auto heard = runCallbacks (engine, 40);
            float peak = 0.0f;
            for (auto v : heard) peak = jmax (peak, std::abs (v));
            check (cmp.isPlaying() && peak > 0.05f && cmp.getPositionSeconds() > 0.2, "Compare plays the reference through the device");
            const double at = cmp.getPositionSeconds();
            cmp.setHeard (Compare::now);
            runCallbacks (engine, 4);
            check (cmp.getPositionSeconds() > at, "switching what you hear keeps going from the same place");
            cmp.stop();
            check (! cmp.isPlaying(), "and it stops");

            e.projectFile = oldFile;
            e.master->volumeDb = 0.0f;
            e.loopEnabled = oldLoop;
            e.removeTrack (toneTrack->id);
            engine.rebuild();
            tools.compare = nullptr;
        }

        status = {};
        auto r = tools.call ("play", args2 ({ { "from_bar", 1 }, { "to_bar", 2 } }), status);
        check (! (bool) r["isError"] && engine.isPlaying() && std::abs (engine.getAutoStopBeats() - 4.0) < 1.0e-6
               && text (r).contains ("stops by itself at bar 2") && status.contains ("bars 1 to 2"),
               "play to_bar: Lathe gets the stop point (and says so)");
        check ((double) JSON::parse (text (tools.call ("describe_session", var (new DynamicObject()), status)))["transport"]["stops_at_bar"] == 2.0,
               "describe_session shows where playback will stop");
        runCallbacks (engine, 400);   // 102400 samples; bar 2 is 96000 in
        check (! engine.isPlaying() && std::abs (engine.getPlayheadBeats() - 4.0) < 1.0e-6, "...and stops exactly at bar 2");
        engine.stop();

        check (text (tools.call ("play", args2 ({ { "loop_times", 2 } }), status)).contains ("loop is off"),
               "loop_times with the loop off says to set it first");
        check ((bool) tools.call ("play", args2 ({ { "from_bar", 3 }, { "to_bar", 2 } }), status)["isError"],
               "to_bar before the start is an error");

        r = tools.call ("set_loop", args2 ({ { "from_bar", 1 }, { "to_bar", 2 } }), status);
        check (! (bool) r["isError"] && e.loopEnabled && e.loopStart == 0.0 && e.loopEnd == 4.0 && e.dirty
               && text (r).contains ("bars 1 to 2 (1 bar)"), "set_loop sets the region and turns it on (the project is changed)");
        engine.rebuild();   // (the app does this when the project changes)

        check (text (tools.call ("play", args2 ({ { "to_bar", 3 } }), status)).contains ("never gets to bar 3"),
               "to_bar past the end of a loop that's on is explained");
        r = tools.call ("play", args2 ({ { "loop_times", 2 } }), status);
        check (! (bool) r["isError"] && engine.isPlaying() && engine.getLoopPassesLeft() == 2 && e.insertMarker == 0.0
               && text (r).contains ("2 times"), "play loop_times starts at the loop start");
        runCallbacks (engine, 800);   // 204800 samples: room for two 96000-sample passes
        check (! engine.isPlaying() && std::abs (engine.getPlayheadBeats() - 4.0) < 1.0e-6, "...plays it twice and stops at the loop end");
        engine.stop();

        r = tools.call ("set_loop", args2 ({ { "on", false } }), status);
        check (! (bool) r["isError"] && ! e.loopEnabled && e.loopEnd == 4.0 && text (r).contains ("Loop off"),
               "set_loop on=false turns it off and keeps the region");
        check ((bool) tools.call ("set_loop", args2 ({ { "from_bar", 2 } }), status)["isError"], "set_loop needs both bars");
        engine.rebuild();
        e.insertMarker = 8.0;

        // --- building a session: list_plugins and edit_session
        {
            Edit ex;
            Tools tx (ex, engine);
            auto addType = [&] (const String& name, bool instrument, const String& category = {})
            {
                PluginDescription d;
                d.name = name;
                d.manufacturerName = "Test";
                d.pluginFormatName = "Test";
                d.fileOrIdentifier = "test:" + name;
                d.isInstrument = instrument;
                d.category = category;
                host.known.addType (d);
            };
            addType ("Arpeggiator", true, "Instrument|Generator");   // a note generator that scans as an instrument
            addType ("DrumSynth", true);
            addType ("Wavetable", true);
            addType ("BigVerb", false);
            addType ("Broken Verb", false);
            tx.loadPlugin = [] (const PluginDescription& d, String& error)
            {
                auto slot = std::make_shared<PluginSlot>();
                slot->description = d;
                if (d.name == "Arpeggiator")              slot->instance = std::make_unique<NoteGenerator> (-1, -1);
                else if (d.name != "Broken Verb")   slot->instance = std::make_unique<NoteEater>();
                else                                error = "it crashed";
                return slot;
            };
            auto change = [&] (std::initializer_list<std::pair<const char*, var>> kv) { return args2 (kv); };
            auto batchOf = [&] (std::initializer_list<var> list)
            {
                Array<var> a;
                for (auto& v : list)
                    a.add (v);
                return args2 ({ { "changes", a } });
            };
            auto names = [] (const Track& t)
            {
                StringArray n;
                for (auto& p : t.plugins)
                    n.add (p->getName());
                return n.joinIntoString (",");
            };

            const auto listed = JSON::parse (text (tx.call ("list_plugins", var (new DynamicObject()), status)));
            String diceyKind;
            for (auto& p : *listed["plugins"].getArray())
                if (p["name"].toString() == "Arpeggiator")
                    diceyKind = p["kind"].toString();
            check (listed["plugins"].size() == 5 + (int) devices::all().size() && diceyKind == "note generator"
                   && listed["plugins"][0]["format"].toString() == "built into Lathe",
                   "list_plugins lists Lathe's own first, then these, Arpeggiator as a note generator");
            check (JSON::parse (text (tx.call ("list_plugins", args2 ({ { "kind", "instrument" } }), status)))["plugins"].size() == 2 + 5,   // + Lathe's own five
                   "list_plugins filters by kind");
            {
                const auto gens = JSON::parse (text (tx.call ("list_plugins", args2 ({ { "kind", "note generator" } }), status)));
                bool hasRandomNotes = false;
                for (auto& g : *gens["plugins"].getArray())
                    hasRandomNotes = hasRandomNotes || g["name"].toString() == "Random Notes";
                check (hasRandomNotes, "Lathe's own Random Notes is listed as a note generator");
            }

            // One track: the generator goes before the instrument, whatever order it's given in.
            auto r = tx.call ("edit_session", batchOf ({ change ({ { "action", "add_track" }, { "kind", "midi" }, { "name", "Drums" },
                                                                   { "plugins", Array<var> { "DrumSynth", "arpeggiator" } } }) }), status);
            auto drums = tx.findTrack ("Drums");
            check (! (bool) r["isError"] && drums != nullptr && drums->isMidi() && names (*drums) == "Arpeggiator,DrumSynth",
                   "add_track with plugins: a MIDI track with Arpeggiator before DrumSynth (" + text (r) + ")");
            check (text (r).contains ("with Arpeggiator -> DrumSynth"), "the reply shows the chain in signal order");
            check (ex.getUndoDepth() == 1 && status.contains ("Cmd/Ctrl+Z"), "it's one undo step, and Lathe's status bar says so");

            // Two tracks, one feeding the other, plus a reverb return in the same call.
            r = tx.call ("edit_session", batchOf ({
                    change ({ { "action", "add_track" }, { "kind", "midi" }, { "name", "Seq" }, { "plugins", Array<var> { "Arpeggiator" } } }),
                    change ({ { "action", "add_track" }, { "kind", "midi" }, { "name", "Kit" }, { "plugins", Array<var> { "DrumSynth" } } }),
                    change ({ { "action", "set_input" }, { "track", "Kit" }, { "from_track", "Seq" } }),
                    change ({ { "action", "add_track" }, { "kind", "return" }, { "name", "Verb" }, { "plugins", Array<var> { "BigVerb" } } }),
                    change ({ { "action", "set_send" }, { "track", "Kit" }, { "return", "Verb" }, { "level_db", -12 } }) }), status);
            auto seq = tx.findTrack ("Seq"), kit = tx.findTrack ("Kit"), verb = tx.findTrack ("Verb");
            check (! (bool) r["isError"] && kit != nullptr && seq != nullptr && kit->input.track == seq->id
                   && kit->monitor.load() == (int) Monitor::in, "set_input from_track routes Seq into Kit, monitoring In (" + text (r) + ")");
            check (verb != nullptr && verb->isBus() && names (*verb) == "BigVerb" && kit->sends.size() == 1
                   && kit->sends[0]->busId == verb->id && kit->sends[0]->levelDb.load() == -12.0f, "a return with BigVerb, Kit sending to it at -12 dB");
            check (ex.getUndoDepth() == 2, "the whole batch is one more undo step");

            check (ex.undo() && ex.tracks.size() == 1 && ex.buses.empty() && ex.tracks[0] == drums,
                   "one undo takes the whole batch away");
            check (ex.redo() && ex.tracks.size() == 3 && ex.buses.size() == 1 && kit->input.track == seq->id, "redo brings it all back");

            // Nothing lands when a change can't be made.
            const int depth = ex.getUndoDepth();
            drums->volumeDb = 0.0f;
            r = tx.call ("edit_session", batchOf ({
                    change ({ { "action", "set_mixer" }, { "track", "Drums" }, { "volume_db", -10 } }),
                    change ({ { "action", "add_track" }, { "kind", "midi" }, { "name", "Lead" }, { "plugins", Array<var> { "Wavetable" } } }),
                    change ({ { "action", "add_plugin" }, { "track", "Lead" }, { "plugin", "Broken Verb" } }) }), status);
            check ((bool) r["isError"] && text (r).contains ("Change 3") && text (r).contains ("couldn't load Broken Verb: it crashed")
                   && text (r).contains ("Nothing was changed"),
                   "a change that fails says which one (" + text (r) + ")");
            check (tx.findTrack ("Lead") == nullptr && drums->volumeDb.load() == 0.0f && ex.getUndoDepth() == depth,
                   "...and nothing before it is kept, levels included");

            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "add_plugin" }, { "track", "Drums" }, { "plugin", "Wavetable" } }) }), status))
                       .contains ("already has an instrument (DrumSynth)"), "a second instrument on a MIDI track is refused");
            auto audio = ex.addTrack (TrackKind::audio, "Gtr");
            ex.commitUndoStep();
            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "add_plugin" }, { "track", "Gtr" }, { "plugin", "DrumSynth" } }) }), status))
                       .contains ("goes on a MIDI track"), "an instrument on an audio track is refused, with what to do instead");
            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "add_plugin" }, { "track", "Gtr" }, { "plugin", "Verb" } }) }), status))
                       .contains ("could be"), "an ambiguous plug-in name lists the candidates");
            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "set_input" }, { "track", "Seq" }, { "from_track", "Kit" } }) }), status))
                       .contains ("loop"), "routing that would loop is refused");
            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "add_track" }, { "name", "kit" } }) }), status))
                       .contains ("already a track called"), "track names stay distinct");
            check (text (tx.call ("edit_session", batchOf ({ change ({ { "action", "set_output" }, { "track", "Gtr" }, { "to", "Kit" } }) }), status))
                       .contains ("isn't a return"), "output goes to the master, a return or the interface");

            r = tx.call ("edit_session", batchOf ({
                    change ({ { "action", "set_output" }, { "track", "Gtr" }, { "to", "Verb" } }),
                    change ({ { "action", "add_plugin" }, { "track", "Gtr" }, { "plugin", "BigVerb" }, { "on", false } }),
                    change ({ { "action", "set_plugin_on" }, { "track", "Drums" }, { "plugin", 1 }, { "on", false } }),
                    change ({ { "action", "rename_track" }, { "track", "Gtr" }, { "name", "Guitar" } }),
                    change ({ { "action", "set_tempo" }, { "bpm", 96 } }) }), status);
            check (! (bool) r["isError"] && audio->name == "Guitar" && audio->output.kind == OutputRoute::Kind::bus
                   && audio->output.busId == verb->id && audio->plugins.size() == 1 && audio->plugins[0]->bypassed.load()
                   && drums->plugins[0]->bypassed.load() && ex.tempo == 96.0, "outputs, bypass, rename and tempo (" + text (r) + ")");
            check (ex.undo() && audio->name == "Gtr" && audio->plugins.empty() && ! drums->plugins[0]->bypassed.load() && ex.tempo == 120.0
                   && audio->output.kind == OutputRoute::Kind::master, "...all undone in one step");

            r = tx.call ("edit_session", batchOf ({ change ({ { "action", "remove_track" }, { "track", "Verb" } }) }), status);
            check (! (bool) r["isError"] && ex.buses.empty() && kit->sends.empty(), "remove_track takes the return and the sends to it");
            check (ex.undo() && ex.buses.size() == 1 && kit->sends.size() == 1, "undo brings them back");
        }

        // --- measuring: a thread plays the audio callbacks in real time, as a device would
        {
            Edit em;
            em.tempo = 120.0;
            engine.setEdit (&em);
            for (auto [name, cost] : { std::pair<const char*, double> { "Heavy", 600.0 }, { "Light", 150.0 } })
            {
                auto tr = em.addTrack (TrackKind::audio, name);
                auto slot = std::make_shared<PluginSlot>();
                slot->description.name = String (name) + " Load";
                slot->instance = std::make_unique<Spinner> (cost);
                tr->plugins.push_back (slot);
            }
            em.insertMarker = 2.0;
            engine.rebuild();
            Tools tm (em, engine);

            std::atomic<bool> running { true };
            std::thread audioThread ([&]
            {
                AudioBuffer<float> outs (2, 256), ins (2, 256);
                ins.clear();
                auto next = Time::getMillisecondCounterHiRes();
                while (running.load())
                {
                    engine.audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), 2, outs.getArrayOfWritePointers(), 2, 256, {});
                    next += 256.0 * 1000.0 / 48000.0;
                    const auto wait = next - Time::getMillisecondCounterHiRes();
                    if (wait > 0)
                        Thread::sleep ((int) wait);
                }
            });

            auto waitFor = [] (Tools::Job& job)
            {
                const auto start = Time::getMillisecondCounterHiRes();
                while (! job.done.wait (0) && Time::getMillisecondCounterHiRes() - start < 60000)
                    MessageManager::getInstance()->runDispatchLoopUntil (10);
                return job.done.wait (0);
            };

            const int threadsBefore = engine.getProcessingThreads();
            var immediate;
            auto job = tm.startLongTool ("measure_performance", args2 ({ { "from_bar", 1 }, { "to_bar", 2 }, { "seconds", 2 } }), immediate);
            check (job != nullptr, "measure_performance starts " + (job == nullptr ? text (immediate) : String()));
            if (job != nullptr && waitFor (*job))
            {
                const auto res = JSON::parse (text (job->result));
                if (res["per_track"].size() == 0)
                    std::cout << "    measure_performance said: " << text (job->result) << std::endl;
                const auto heavyPct = res["per_track"].size() > 0 ? (double) res["per_track"][0]["pct_of_deadline"] : 0.0;
                check (! (bool) job->result["isError"] && (double) res["result"]["mean_load_pct"] > 0.0
                       && (int) res["result"]["blocks_measured"] > 100, "it measures real blocks (" + text (job->result).substring (0, 400) + ")");
                check (res["per_track"].size() == 2 && res["per_track"].size() > 0 && res["per_track"][0]["track"].toString() == "Heavy"
                       && std::abs (heavyPct - 600.0 / 5333.3 * 100.0) < 6.0,
                       "per-track cost: the heavy track first, at about its real cost (" + String (heavyPct, 1) + "% of the deadline, want ~11.3%)");
                check (res["conditions"]["audio"]["buffer_samples"].toString() == "256" && res.hasProperty ("to_compare_elsewhere"),
                       "it reports the conditions and how to compare");
                check (! engine.isPlaying() && em.insertMarker == 2.0 && ! engine.isMeasuring(), "Lathe is stopped afterwards, insert marker untouched");
            }

            check (tm.call ("measure_performance", var (new DynamicObject()), status)["isError"], "a long tool can't run as a quick call");

            // Straight after "done", before the finished measurement has been tidied away (that comes
            // on the message thread a moment later): the next one must still start.
            job = tm.startLongTool ("compare_processing_cores", args2 ({ { "from_bar", 1 }, { "to_bar", 2 }, { "seconds", 2 }, { "rounds", 1 } }), immediate);
            check (job != nullptr, "compare_processing_cores starts " + (job == nullptr ? text (immediate) : String()));
            if (job != nullptr && waitFor (*job))
            {
                const auto res = JSON::parse (text (job->result));
                if (res["runs"].size() != 2)
                    std::cout << "    compare_processing_cores said: " << text (job->result) << std::endl;
                check (! (bool) job->result["isError"] && res["runs"].size() == 2
                       && (double) res["summary"]["one_core"]["mean_load_pct"] > 0.0 && (double) res["summary"]["all_cores"]["mean_load_pct"] > 0.0
                       && res["runs"].size() > 0 && res["runs"][0]["mode"].toString() == "1 core", "it measures 1 core, then all cores (" + text (job->result).substring (0, 300) + ")");
                check (engine.getProcessingThreads() == threadsBefore, "and puts the Processing cores setting back");
            }

            running = false;
            audioThread.join();
            engine.setEdit (&e);
            engine.rebuild();
        }

        // --- the HTTP server
        const auto endpoint = dir.getChildFile ("assistant.json");
        int connects = 0;
        Server::Handler h;
        h.callTool = [&] (const String& name, const var& a) { String st; return tools.call (name, a, st); };
        h.onConnect = [&] (const String&) { ++connects; };
        Server server (h, endpoint);
        const auto key = makeToken();
        check (server.start (key).wasOk() && server.getPort() >= kFirstPort, "the server starts on 127.0.0.1");
        const auto ep = readEndpoint (endpoint);
        check (ep.has_value() && ep->port == server.getPort() && ep->token == key, "and writes its port and key for LatheMCP");
       #if ! JUCE_WINDOWS
        check (endpoint.existsAsFile() && (File (endpoint).getFullPathName().isNotEmpty())
               && ([&] { struct stat st {}; return ::stat (endpoint.getFullPathName().toRawUTF8(), &st) == 0 && (st.st_mode & 0077) == 0; })(),
               "assistant.json is readable only by you");
       #endif

        auto raw = [&] (const String& request) -> HttpMessage
        {
            HttpMessage m;
            StreamingSocket s;
            if (s.connect ("127.0.0.1", server.getPort(), 2000) && writeAll (s, request))
                readHttp (s, m, false, 5000);
            return m;
        };
        auto post = [&] (const String& body, const String& extraHeaders, const String& host = {}) -> HttpMessage
        {
            String r;
            r << "POST /mcp HTTP/1.1\r\nHost: " << (host.isNotEmpty() ? host : "127.0.0.1:" + String (server.getPort())) << "\r\n"
              << extraHeaders << "Content-Type: application/json\r\nContent-Length: " << (int) body.getNumBytesAsUTF8() << "\r\n\r\n" << body;
            return raw (r);
        };
        const String init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"Test"}}})";
        const auto bearer = "Authorization: Bearer " + key + "\r\n";

        check (post (init, {}).status == 401, "no key: refused");
        check (post (init, "Authorization: Bearer " + makeToken() + "\r\n").status == 401, "wrong key: refused");
        check (post (init, bearer, "evil.example:" + String (server.getPort())).status == 403, "another host name (DNS rebinding): refused");
        check (post (init, bearer + "Origin: https://evil.example\r\n").status == 403, "a request from a web page: refused");
        check (post (init, bearer + "Origin: http://localhost.evil.example\r\n").status == 403, "...even one named like localhost");
        check (post (init, bearer + "Origin: null\r\n").status == 403, "...or a local file or sandboxed page (Origin: null)");
        check (server.getActivity().refused == 6 && connects == 0, "refused requests never reach Lathe");

        auto ok = post (init, bearer);
        const auto initReply = JSON::parse (ok.body);
        check (ok.status == 200 && initReply["result"]["protocolVersion"].toString() == "2025-03-26" && (int) initReply["id"] == 1
               && initReply["result"]["capabilities"]["tools"].isObject() && connects == 1,
               "initialize: agrees on the client's protocol version and offers tools");
        check (post (R"({"jsonrpc":"2.0","method":"notifications/initialized"})", bearer).status == 202, "notifications get 202, no body");
        const auto listed = JSON::parse (post (R"({"jsonrpc":"2.0","id":"a","method":"tools/list"})", bearer).body);
        check (listed["result"]["tools"].size() == 19 && listed["id"].toString() == "a", "tools/list has the nineteen tools");

        String body;
        const int code = postToLathe (*ep, R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"describe_track","arguments":{"track":"Synth"}}})", body, 5000);
        const auto called = JSON::parse (body);
        const auto content = called["result"]["content"];
        const auto calledText = content.isArray() && content.size() > 0 ? content[0]["text"].toString() : String();
        check (code == 200 && ! (bool) called["result"]["isError"] && JSON::parse (calledText)["name"].toString() == "Synth",
               "tools/call runs the tool (as LatheMCP sends it): " + String (code) + " " + body.substring (0, 200));
        check (JSON::parse (post (R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"rm_rf"}})", bearer).body)["error"].isObject(),
               "an unknown tool is a protocol error");
        check (JSON::parse (post (R"({"jsonrpc":"2.0","id":9,"method":"bogus"})", bearer).body)["error"]["code"].toString() == "-32601", "unknown methods too");
        check (post ("{nope", bearer).status == 400, "bad JSON: 400");
        check (raw ("GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + String (server.getPort()) + "\r\n" + bearer + "\r\n").status == 405, "GET (a stream): not offered");
        const auto huge = post (String::repeatedString ("x", kMaxBody + 10), bearer).status;
        check (huge == 0 || huge == 400, "a huge request is refused, and the closed connection doesn't kill anyone (SIGPIPE) (got "
                                          + String (huge) + ")");
        {
            // A client that hangs up before the answer: Lathe must carry on.
            StreamingSocket s;
            s.connect ("127.0.0.1", server.getPort(), 2000);
            writeAll (s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + String (server.getPort()) + "\r\n" + bearer
                             + "Content-Length: " + String (init.length()) + "\r\n\r\n" + init);
            s.close();
            Thread::sleep (300);
            check (post (R"({"jsonrpc":"2.0","id":10,"method":"ping"})", bearer).status == 200, "a client that hangs up early doesn't stop the server");
        }

        server.stop();
        check (! endpoint.existsAsFile(), "stopping removes assistant.json");
        String none;
        check (postToLathe (*ep, init, none, 2000) == 0, "and nothing listens any more");

        engine.setEdit (&edit);
    }

    engine.setEdit (nullptr);
    std::cout << (failures == 0 ? "\nAll tests passed." : "\n" + String (failures) + " test(s) FAILED.") << std::endl;
    return failures == 0 ? 0 : 1;
}
