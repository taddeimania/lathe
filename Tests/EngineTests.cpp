// Headless engine tests: a fake audio device drives the real audio callback.
// Build with -DLATHE_BUILD_TESTS=ON and run LatheTests.

#include "Engine/Engine.h"
#include "Engine/PluginStore.h"
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
        check (longestStall < 250.0, "UI never froze during the scan (longest stall " + String (roundToInt (longestStall)) + " ms)");

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

    engine.setEdit (nullptr);
    std::cout << (failures == 0 ? "\nAll tests passed." : "\n" + String (failures) + " test(s) FAILED.") << std::endl;
    return failures == 0 ? 0 : 1;
}
