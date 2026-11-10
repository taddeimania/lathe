// Headless engine tests: a fake audio device drives the real audio callback.
// Build with -DLATHE_BUILD_TESTS=ON and run LatheTests.

#include "Engine/Engine.h"
#include "Engine/PluginStore.h"
#include "Engine/ClipJoin.h"
#include "Patcher/Updater.h"
#include "Cloud/ProjectDiff.h"
#include "Cloud/Revisions.h"
#include "Cloud/Packing.h"
#include "Assistant/AssistantServer.h"
#include "Assistant/AssistantTools.h"
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
    FakeDevice (int ins, int outs, int inLat, int outLat)
        : AudioIODevice ("Fake", "Fake"), numIns (ins), numOuts (outs), inLatency (inLat), outLatency (outLat) {}

    StringArray getOutputChannelNames() override  { StringArray s; for (int i = 0; i < numOuts; ++i) s.add ("Out " + String (i + 1)); return s; }
    StringArray getInputChannelNames() override   { StringArray s; for (int i = 0; i < numIns; ++i) s.add ("In " + String (i + 1)); return s; }
    Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    Array<int> getAvailableBufferSizes() override  { return { 256 }; }
    int getDefaultBufferSize() override            { return 256; }
    String open (const BigInteger&, const BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override                         { return true; }
    void start (AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override                      { return true; }
    String getLastError() override                 { return {}; }
    int getCurrentBufferSizeSamples() override     { return 256; }
    double getCurrentSampleRate() override         { return 48000.0; }
    int getCurrentBitDepth() override              { return 24; }
    BigInteger getActiveOutputChannels() const override { BigInteger b; b.setRange (0, numOuts, true); return b; }
    BigInteger getActiveInputChannels() const override  { BigInteger b; b.setRange (0, numIns, true); return b; }
    int getOutputLatencyInSamples() override       { return outLatency; }
    int getInputLatencyInSamples() override        { return inLatency; }

private:
    int numIns, numOuts, inLatency, outLatency;
};

/** Pure delay "plug-in" that reports its latency, for testing compensation. */
/** A MIDI generator (like an arpeggiator, or Dicey): plays one note on its own, from
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

/** Writes a stored (uncompressed) zip with Unix modes, like the macOS release archives. */
void writeZipWithModes (const File& zipFile, const std::vector<std::tuple<String, String, int>>& entries)   // path, content, mode
{
    MemoryOutputStream out, central;
    int count = 0;
    for (auto& [path, content, mode] : entries)
    {
        const auto offset = (uint32) out.getDataSize();
        const auto name = path.toRawUTF8();
        const auto nameLen = (uint16) strlen (name);
        const auto size = (uint32) content.getNumBytesAsUTF8();
        const bool isDir = path.endsWithChar ('/');

        out.writeInt (0x04034b50); out.writeShort (20); out.writeShort (0); out.writeShort (0);
        out.writeShort (0); out.writeShort (0); out.writeInt (0); out.writeInt ((int) size); out.writeInt ((int) size);
        out.writeShort ((short) nameLen); out.writeShort (0);
        out.write (name, nameLen);
        out.write (content.toRawUTF8(), size);

        const uint32 attrs = ((uint32) ((isDir ? 040000 : 0100000) | mode) << 16) | (isDir ? 0x10u : 0u);
        central.writeInt (0x02014b50); central.writeShort (0x0314); central.writeShort (20); central.writeShort (0); central.writeShort (0);
        central.writeShort (0); central.writeShort (0); central.writeInt (0); central.writeInt ((int) size); central.writeInt ((int) size);
        central.writeShort ((short) nameLen); central.writeShort (0); central.writeShort (0); central.writeShort (0); central.writeShort (0);
        central.writeInt ((int) attrs); central.writeInt ((int) offset);
        central.write (name, nameLen);
        ++count;
    }
    const auto centralOffset = (uint32) out.getDataSize();
    out.write (central.getData(), central.getDataSize());
    out.writeInt (0x06054b50); out.writeShort (0); out.writeShort (0); out.writeShort ((short) count); out.writeShort ((short) count);
    out.writeInt ((int) central.getDataSize()); out.writeInt ((int) centralOffset); out.writeShort (0);
    zipFile.replaceWithData (out.getData(), out.getDataSize());
}

/** A fake release folder: Name-ver-Platform/{README.md, Name.vst3/Contents/...}. */
File makeReleaseFolder (const File& dir, const String& name, const String& version, const String& binaryContent)
{
    auto root = dir.getChildFile (name + "-" + version + "-Linux");
    root.deleteRecursively();
    auto bin = root.getChildFile (name + ".vst3/Contents/x86_64-linux/" + name + ".so");
    bin.getParentDirectory().createDirectory();
    bin.replaceWithText (binaryContent);
    root.getChildFile ("README.md").replaceWithText ("readme");
    return root;
}
} // namespace

int main()
{
    ScopedJuceInitialiser_GUI juce;
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
        loaded.loadFrom (JSON::parse (json), File(), [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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
        auto gen = edit.addTrack (TrackKind::midi, "Dicey");
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

        // Keyboard input still records as before on a track without a generator.
        rec->arm = false;
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
        copy.loadFrom (edit.toVar (false), dir, [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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
        // A template like "Terry Jam Start": Kick on input 1, Snare on input 2, two returns.
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
        fresh.loadFrom (JSON::parse (templateJson), File(), [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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
        auto result = loaded.loadFrom (JSON::parse (text), dir, [] (const PluginDescription& d, const MemoryBlock& state)
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
        n.replies.push_back ({ "r1", "terry@example.com", {}, "On it", 5 });
        e.localNotes.push_back (n);
        SongNote song;
        song.id = "2-b";
        song.text = "Where we are";
        e.localNotes.push_back (song);

        const auto saved = e.toVar (false);
        Edit back;
        back.loadFrom (saved, File(), [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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


    std::cout << "Plug-in store" << std::endl;
    {
        // parseRelease against the shape of a real release (taddeimania/tarry v0.1.0)
        auto json = JSON::parse (R"({"tag_name":"v0.1.0","name":"Tarry 0.1.0","html_url":"https://github.com/taddeimania/tarry/releases/tag/v0.1.0",
            "published_at":"2026-10-26T22:56:09Z","draft":false,"prerelease":false,"body":"## What's in 0.1.0\n- **12 types**",
            "assets":[{"name":"Tarry-0.1.0-Linux.tar.gz","size":2528340,"browser_download_url":"https://github.com/taddeimania/tarry/releases/download/v0.1.0/Tarry-0.1.0-Linux.tar.gz"},
                      {"name":"Tarry-0.1.0-macOS.zip","size":3726297,"browser_download_url":"https://github.com/taddeimania/tarry/releases/download/v0.1.0/Tarry-0.1.0-macOS.zip"},
                      {"name":"Tarry-0.1.0-Windows.zip","size":2697478,"browser_download_url":"https://github.com/taddeimania/tarry/releases/download/v0.1.0/Tarry-0.1.0-Windows.zip"}]})");
        StoreEntry e;
        e.repo = "taddeimania/tarry";
        PluginStore::parseRelease (json, e);
        check (e.version == "0.1.0" && e.name == "Tarry", "release parsed: version " + e.version + ", name from asset " + e.name);
        check (e.assetName == "Tarry-0.1.0-" + PluginStore::platformTag() + (PluginStore::platformTag() == "Linux" ? ".tar.gz" : ".zip"),
               "picks this platform's archive (" + e.assetName + ")");

        StoreEntry dicey;
        PluginStore::parseRelease (JSON::parse (R"({"tag_name":"v1.0.1","draft":false,"assets":[{"name":"Dicey-1.0.1-macOS-2044f29.zip","size":1,"browser_download_url":"x"},{"name":"Dicey-1.0.1-Windows-2044f29.zip","size":1,"browser_download_url":"y"},{"name":"Dicey-1.0.1-Linux-2044f29.tar.gz","size":1,"browser_download_url":"z"}]})"), dicey);
        check (dicey.hasRelease(), "asset names with a commit suffix still match (Dicey)");

        StoreEntry draft;
        PluginStore::parseRelease (JSON::parse (R"({"tag_name":"v9","draft":true,"assets":[]})"), draft);
        check (! draft.hasRelease() && draft.version.isEmpty(), "draft releases are ignored");

        check (PluginStore::compareVersions ("0.10.0", "0.9.1") > 0 && PluginStore::compareVersions ("v1.0", "1.0.0") == 0
               && PluginStore::compareVersions ("1.2.0", "1.3.0") < 0, "version comparison");
        check (PluginStore::defaultCatalog().size() == 14, "catalog lists all 14 plug-ins");

        // Install from a zip that carries Unix modes, like the macOS builds.
        auto work = dir.getChildFile ("store");
        auto vst3 = work.getChildFile ("VST3");
        auto data = work.getChildFile ("data");
        work.deleteRecursively();
        data.createDirectory();

        auto zip = work.getChildFile ("Downloads/Foo-1.0.0-macOS.zip");
        zip.getParentDirectory().createDirectory();
        writeZipWithModes (zip, { { "Foo-1.0.0-macOS/", "", 0755 },
                                  { "Foo-1.0.0-macOS/README.md", "readme", 0644 },
                                  { "Foo-1.0.0-macOS/Foo.vst3/", "", 0755 },
                                  { "Foo-1.0.0-macOS/Foo.vst3/Contents/", "", 0755 },
                                  { "Foo-1.0.0-macOS/Foo.vst3/Contents/MacOS/", "", 0755 },
                                  { "Foo-1.0.0-macOS/Foo.vst3/Contents/MacOS/Foo", "binary v1", 0755 },
                                  { "Foo-1.0.0-macOS/Foo.vst3/Contents/Info.plist", "plist", 0644 } });
        File bundle;
        bool pending = false;
        auto r = PluginStore::installArchive (zip, "Foo", vst3, data, bundle, pending);
        check (r.wasOk() && ! pending, "zip release installs " + r.getErrorMessage());
        check (bundle == vst3.getChildFile ("Foo.vst3") && bundle.getChildFile ("Contents/MacOS/Foo").loadFileAsString() == "binary v1",
               "only the .vst3 bundle lands in the VST3 folder");
        check (! vst3.getChildFile ("README.md").exists() && vst3.getNumberOfChildFiles (File::findFilesAndDirectories) == 1, "nothing else is copied");
       #if ! JUCE_WINDOWS
        check (access (bundle.getChildFile ("Contents/MacOS/Foo").getFullPathName().toRawUTF8(), X_OK) == 0, "the plug-in binary keeps its execute permission");
       #endif

        // Update over the top: replaced in place, no leftovers.
        auto folder = makeReleaseFolder (work, "Foo", "1.1.0", "binary v2");
        auto tgz = work.getChildFile ("Downloads/Foo-1.1.0-Linux.tar.gz");
        ChildProcess tar;
        const bool tarOk = tar.start (StringArray { "tar", "-czf", tgz.getFullPathName(), "-C", work.getFullPathName(), folder.getFileName() })
                           && tar.waitForProcessToFinish (30000) && tar.getExitCode() == 0;
        check (tarOk, "made a tar.gz fixture");
        r = PluginStore::installArchive (tgz, "Foo", vst3, data, bundle, pending);
        check (r.wasOk() && ! pending && bundle.getChildFile ("Contents/x86_64-linux/Foo.so").loadFileAsString() == "binary v2",
               "tar.gz update replaces the installed bundle " + r.getErrorMessage());
        check (! bundle.getChildFile ("Contents/MacOS").exists(), "files from the old version are gone");
        check (vst3.findChildFiles (File::findFilesAndDirectories, false, "*.lathe-old").isEmpty(), "no .lathe-old copy left behind");

        // A staged update (what happens when Windows has the old one locked) is applied on the next launch.
        auto staged = data.getChildFile ("Pending/Foo.vst3");
        staged.getChildFile ("Contents").createDirectory();
        staged.getChildFile ("Contents/marker").replaceWithText ("v3");
        DynamicObject::Ptr p = new DynamicObject();
        p->setProperty ("source", staged.getFullPathName());
        p->setProperty ("target", bundle.getFullPathName());
        data.getChildFile ("Pending Installs.json").replaceWithText (JSON::toString (Array<var> { var (p.get()) }));
        auto applied = PluginStore::applyPendingInstalls (data);
        check (applied.size() == 1 && bundle.getChildFile ("Contents/marker").loadFileAsString() == "v3"
               && ! data.getChildFile ("Pending Installs.json").exists(), "staged update applied at startup");

        // The whole flow: download (from a file:// URL here), unpack, install, record.
        PluginStore store (host, data);
        store.vst3DirOverride = vst3;
        StoreEntry fake;
        fake.repo = "test/bar";
        fake.name = "Bar";
        fake.kind = "Effect";
        auto barFolder = makeReleaseFolder (work, "Bar", "2.0.0", "bar");
        auto barTgz = work.getChildFile ("Bar-2.0.0-Linux.tar.gz");
        ChildProcess tar2;
        tar2.start (StringArray { "tar", "-czf", barTgz.getFullPathName(), "-C", work.getFullPathName(), barFolder.getFileName() });
        tar2.waitForProcessToFinish (30000);
        fake.version = "2.0.0";
        fake.assetName = barTgz.getFileName();
        fake.assetUrl = URL (barTgz).toString (false);
        fake.assetSize = barTgz.getSize();
        store.entries.push_back (fake);

        bool finished = false;
        Result installResult = Result::ok();
        store.install ("test/bar", [&] (const Result& res, const PluginDescription*) { installResult = res; finished = true; });
        for (int i = 0; i < 200 && ! finished; ++i)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

        auto* bar = store.find ("test/bar");
        check (finished && installResult.wasOk(), "install() downloads and installs " + installResult.getErrorMessage());
        check (bar != nullptr && bar->isInstalled() && bar->installedVersion == "2.0.0" && ! bar->busy, "store shows it installed at 2.0.0");
        check (vst3.getChildFile ("Bar.vst3/Contents/x86_64-linux/Bar.so").existsAsFile(), "bundle is in the VST3 folder");
        check (! data.getChildFile ("Downloads/" + fake.assetName).exists(), "the downloaded archive is cleaned up");
        bar->version = "2.1.0";
        check (bar->updateAvailable(), "a newer release shows as an update");
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
        loaded.loadFrom (JSON::parse (JSON::toString (e.toVar (true))), File(), [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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
        loaded.loadFrom (JSON::parse (JSON::toString (e.toVar (true))), File(), [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); });
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
            p->setProperty ("description", "<PLUGIN name=\"Nave\"/>");
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
        check (knob == "\"Vox\": Nave: Decay 2.1 s -> 3.4 s", "names the plug-in knob that moved (got " + knob + ")");
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
        const auto [joelEmail, joelPass] = user ("LATHE_TEST_USER1");
        const auto [terryEmail, terryPass] = user ("LATHE_TEST_USER2");
        auto factory = [] (const PluginDescription&, const MemoryBlock&) { return std::make_shared<PluginSlot>(); };

        cloud::Client joel (config), terry (config);
        check (joel.signIn (joelEmail, joelPass, {}).wasOk() && terry.signIn (terryEmail, terryPass, {}).wasOk(), "both sign in");
        check (joel.signIn (joelEmail, "wrong-password", {}).failed(), "a wrong password is refused");
        joel.signIn (joelEmail, joelPass, {});

        // Joel's project, with a recorded take.
        const auto joelDir = dir.getChildFile ("cloud-joel/Jam");
        Edit je;
        je.projectFile = joelDir.getChildFile ("Jam.lathe");
        je.workDir = joelDir;
        auto hats = je.addTrack (TrackKind::audio, "Hats");
        Clip take;
        take.id = je.newId();
        take.kind = ClipKind::audio;
        joelDir.getChildFile ("Audio").createDirectory();
        take.file = writeImpulseWav (joelDir.getChildFile ("Audio/Hats 1.wav"), 48000, 100);
        hats->clips.push_back (take);
        joelDir.createDirectory();
        je.projectFile.replaceWithText (JSON::toString (je.toVar (false)));

        cloud::LocalStore joelStore (je.projectFile);
        check (cloud::createCloudProject (joel, joelStore, "Jam").wasOk() && joelStore.isLinked(), "shares the project");
        auto s1 = cloud::Snapshot::capture (je);
        auto p1 = cloud::push (joel, joelStore, s1, {}, "First take", {});
        check (p1.result.wasOk() && p1.revision.seq == 1 && joelStore.getBase() == p1.revision.id, "pushes r1");
        cloud::ApiError err;
        DynamicObject::Ptr share = new DynamicObject();
        share->setProperty ("email", terryEmail);
        joel.call ("POST", "/projects/" + joelStore.getProjectId() + "/members", var (share.get()), err);
        check (err.status == 0, "shares it with Terry");

        // Terry gets it.
        const auto terryDir = dir.getChildFile ("cloud-terry/Jam");
        terryDir.createDirectory();
        cloud::LocalStore terryStore (terryDir.getChildFile ("Jam.lathe"));
        terryStore.link (joelStore.getProjectId(), "Jam");
        auto pulled = cloud::pull (terry, terryStore, {});
        check (pulled.result.wasOk() && pulled.newRevisions.size() == 1 && pulled.head == p1.revision.id, "Terry pulls r1");
        var terryProject;
        check (cloud::prepareRevision (&terry, terryStore, pulled.newRevisions[0], terryProject, {}).wasOk(), "r1 is ready to open");
        Edit te;
        te.projectFile = terryStore.getProjectFile();
        te.workDir = terryDir;
        te.loadFrom (terryProject, terryDir, factory);
        terryStore.setBase (p1.revision.id);
        check (te.tracks.size() == 1 && te.tracks[0]->clips.size() == 1
                 && te.tracks[0]->clips[0].file == terryDir.getChildFile ("Audio/Hats 1.wav")
                 && cloud::sha256OfFile (te.tracks[0]->clips[0].file) == cloud::sha256OfFile (take.file),
               "Terry has the take, byte for byte, in his Audio folder");

        // Terry turns the hats down and pushes r2.
        te.tracks[0]->volumeDb = -6.0f;
        auto ts = cloud::Snapshot::capture (te);
        auto p2 = cloud::push (terry, terryStore, ts, terryStore.getBase(), "Hats down", {});
        check (p2.result.wasOk() && p2.revision.seq == 2 && p2.revision.summary.contains ("fader 0.0 dB -> -6.0 dB"),
               "Terry pushes r2 with a summary: " + p2.revision.summary);

        // Joel pans them meanwhile: his push is refused, not merged over Terry's.
        je.tracks[0]->pan = -1.0f;
        auto s3 = cloud::Snapshot::capture (je);
        auto p3 = cloud::push (joel, joelStore, s3, joelStore.getBase(), "Hats left", {});
        check (p3.conflict && p3.theirHead == p2.revision.id, "Joel's push is refused: Terry pushed first");

        // Joel keeps his version, pulls Terry's, looks at it, then goes back to his own.
        const auto mine = cloud::keepWorkingCopy (joelStore, s3, joelEmail, "My work before opening r2");
        auto jp = cloud::pull (joel, joelStore, {});
        check (jp.result.wasOk() && jp.newRevisions.size() == 1, "Joel pulls r2");
        var theirs;
        cloud::prepareRevision (&joel, joelStore, jp.newRevisions[0], theirs, {});
        check (std::abs ((double) theirs["tracks"][0]["volume"] + 6.0) < 0.01 && std::abs ((double) theirs["tracks"][0]["pan"]) < 0.01,
               "r2 has Terry's fader and not Joel's pan");
        var back;
        cloud::prepareRevision (nullptr, joelStore, mine, back, {});
        check (std::abs ((double) back["tracks"][0]["pan"] + 1.0) < 0.01, "Joel's own version is still there, offline");

        // Joel decides his pan goes on top of Terry's revision: r3.
        auto p4 = cloud::push (joel, joelStore, s3, p2.revision.id, "Hats left, on top of Terry's", {});
        check (p4.result.wasOk() && p4.revision.seq == 3 && p4.revision.parent == p2.revision.id
                 && p4.revision.summary.contains ("fader -6.0 dB -> 0.0 dB") && p4.revision.summary.contains ("pan C -> 50L"),
               "pushes r3 on top, and the summary is against Terry's r2");
        int uploads = -1;
        joel.upload (joelStore.getProjectId(), { { cloud::sha256OfFile (take.file), take.file, {} } }, {}, uploads);
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

            joel.setParallelTransfers (4);
            cloud::testing::dropNextTransfers (2);
            std::vector<double> seen;
            int sent = 0;
            const auto r = joel.upload (joelStore.getProjectId(), batch, [&] (double p) { seen.push_back (p); return true; }, sent);
            check (r.wasOk() && sent == 7, "uploads 7 files, 4 at a time, through 2 dropped connections (" + r.getErrorMessage() + ")");
            check (! seen.empty() && seen.back() == 1.0 && std::is_sorted (seen.begin(), seen.end()), "progress only goes forward and ends at 100%");

            cloud::testing::dropNextTransfers (1);
            const auto d = joel.download (joelStore.getProjectId(), back, {});
            bool same = d.wasOk();
            for (size_t i = 0; i < back.size() && same; ++i)
                same = cloud::sha256OfFile (back[i].second) == batch[i].hash;
            check (same, "downloads them 4 at a time, every file intact (" + d.getErrorMessage() + ")");

            // Stopping partway: nothing new starts, and it says so.
            std::vector<std::pair<String, File>> again;
            for (auto& [h, f] : back)
                again.push_back ({ h, f.getSiblingFile ("again-" + f.getFileName()) });
            const auto c = joel.download (joelStore.getProjectId(), again, [] (double) { return false; });
            check (c.failed() && c.getErrorMessage() == "Cancelled.", "a transfer can be stopped");
        }

        // Signed-out access is refused by the API.
        cloud::Client nobody (config);
        nobody.call ("GET", "/projects", {}, err);
        check (err.status == 401, "the API refuses anyone not signed in");
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
        check (listed["result"]["tools"].size() == 6 && listed["id"].toString() == "a", "tools/list has the six tools");

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
