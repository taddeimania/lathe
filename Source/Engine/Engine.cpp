#include "Engine.h"

namespace lathe
{
//==============================================================================
// Render snapshot
//==============================================================================

/** Fixed delay used for plug-in delay compensation. Processes a stereo buffer in place. */
struct DelayLine
{
    AudioBuffer<float> buffer;
    int size = 0, writePos = 0, delay = 0;

    void prepare (int delaySamples)
    {
        delay = jmax (0, delaySamples);
        size = delay + 1;
        writePos = 0;
        if (delay > 0)
        {
            buffer.setSize (2, size);
            buffer.clear();
        }
    }

    void clear()   { if (delay > 0) buffer.clear(); writePos = 0; }

    void process (AudioBuffer<float>& io, int n) noexcept
    {
        if (delay <= 0)
            return;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = io.getWritePointer (ch);
            auto* ring = buffer.getWritePointer (ch);
            int w = writePos;

            for (int i = 0; i < n; ++i)
            {
                ring[w] = data[i];
                int r = w - delay;
                if (r < 0) r += size;
                data[i] = ring[r];
                if (++w == size) w = 0;
            }
        }

        writePos = (writePos + n) % size;
    }
};

struct RenderClip
{
    int64 start = 0, end = 0, sourceOffset = 0;
    float gain = 1.0f;
    std::shared_ptr<AudioFileSource> source;
};

struct RenderMidi
{
    int64 sample = 0;
    uint8 data[3] {};
    uint8 size = 0;
};

struct RenderPlugin
{
    std::shared_ptr<PluginSlot> slot;
    AudioPluginInstance* processor = nullptr;
    int numChannels = 2;
};

struct RenderSend
{
    std::shared_ptr<Send> send;
    int busIndex = 0;
};

struct RenderTrack
{
    std::shared_ptr<Track> track;
    std::shared_ptr<TrackGroup> group;   // read live: group mute/solo
    TrackKind kind = TrackKind::audio;
    std::vector<RenderClip> clips;
    std::vector<RenderMidi> midiEvents;
    std::vector<RenderPlugin> plugins;
    std::vector<RenderSend> sends;

    int inputChannel = -1;
    bool inputStereo = false;
    int midiInput = -1;            // -1 = all devices, -2 = none, else device index
    MidiOutput* midiOut = nullptr;
    int midiChannel = 1;

    OutputRoute::Kind outKind = OutputRoute::Kind::master;
    int outBus = -1, outChannel = 0;
    bool outStereo = true;

    int latency = 0;
    DelayLine align;     // lines this track up with the slowest one at its level
    DelayLine direct;    // adds the return-track latency for signals going straight out

    int numChannels = 2;
    AudioBuffer<float> buffer;
    MidiBuffer midi;

    RecordTake* take = nullptr;       // set while recording
    MidiTake* midiTake = nullptr;
};

struct RenderGraph
{
    double sampleRate = 0.0;
    int maxBlock = 0;
    double samplesPerBeat = 0.0;
    double samplesPerClick = 0.0;
    int clicksPerBar = 4;

    bool loop = false;
    int64 loopStart = 0, loopEnd = 0;

    std::vector<RenderTrack> tracks, buses;
    RenderTrack master;
    int trackLatency = 0, busLatency = 0, masterLatency = 0;

    AudioBuffer<float> scratch;
};

//==============================================================================
// Recording
//==============================================================================
struct RecordTake
{
    std::shared_ptr<Track> track;
    File file;
    std::unique_ptr<AudioFormatWriter::ThreadedWriter> writer;
    int channel = 0;
    bool stereo = false;
    int64 startSample = -1, numSamples = 0;
    bool overflow = false;
};

struct MidiTake
{
    struct Event { int64 sample; uint8 data[3]; uint8 size; };
    std::shared_ptr<Track> track;
    std::vector<Event> events;   // reserved up front: the audio thread never reallocates it
    int64 startSample = -1, endSample = 0;
};

struct RecordSession
{
    std::vector<std::unique_ptr<RecordTake>> audio;
    std::vector<std::unique_ptr<MidiTake>> midi;
    int audioOffset = 0;   // samples to skip: input + output + plug-in latency
    int midiOffset = 0;
};

//==============================================================================
Optional<AudioPlayHead::PositionInfo> HostPlayHead::getPosition() const
{
    PositionInfo info;
    info.setBpm (bpm);
    info.setTimeSignature (TimeSignature { num, den });
    info.setTimeInSamples (position);
    info.setTimeInSeconds ((double) position / sampleRate);

    const double ppq = (double) position / samplesPerBeat;
    const double barLength = num * 4.0 / den;
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / barLength) * barLength);
    info.setBarCount ((int64) std::floor (ppq / barLength));
    info.setIsPlaying (playing);
    info.setIsRecording (recording);
    info.setIsLooping (looping);
    if (looping)
        info.setLoopPoints (LoopPoints { loopStartBeats, loopEndBeats });
    return info;
}

//==============================================================================
Engine::Engine (PluginHost& h, AudioPool& p, PropertiesFile& settings)
    : host (h), pool (p), props (settings)
{
    diskThread.startThread (Thread::Priority::high);
    deviceManager.addMidiInputDeviceCallback ({}, this);
    midiDeviceConnection = MidiDeviceListConnection::make ([this] { refreshMidiDevices(); });
    startTimer (500);
}

Engine::~Engine()
{
    stopTimer();
    if (recording)
        finishRecording();

    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.closeAudioDevice();

    graph.reset();
    diskThread.stopThread (2000);
}

void Engine::status (const String& s)
{
    if (onStatus)
        onStatus (s);
}

//==============================================================================
void Engine::openDevice()
{
    auto saved = props.getXmlValue ("audioDevice");
    const auto error = deviceManager.initialise (256, 256, saved.get(), true);

    if (saved == nullptr)
    {
        // First run: look for the UMC (or any Behringer) interface and use it,
        // preferring ASIO on Windows.
        auto& types = deviceManager.getAvailableDeviceTypes();
        for (int pass = 0; pass < 2; ++pass)
        {
            for (auto* type : types)
            {
                if (pass == 0 && type->getTypeName() != "ASIO" && type->getTypeName() != "CoreAudio")
                    continue;

                type->scanForDevices();
                for (auto& name : type->getDeviceNames())
                {
                    if (name.containsIgnoreCase ("UMC") || name.containsIgnoreCase ("BEHRINGER"))
                    {
                        deviceManager.setCurrentAudioDeviceType (type->getTypeName(), true);
                        auto setup = deviceManager.getAudioDeviceSetup();
                        setup.inputDeviceName = setup.outputDeviceName = name;
                        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
                        setup.inputChannels.setRange (0, 256, true);
                        setup.outputChannels.setRange (0, 256, true);
                        setup.bufferSize = 128;
                        deviceManager.setAudioDeviceSetup (setup, true);
                        pass = 2;
                        break;
                    }
                }
                if (pass == 2) break;
            }
        }
    }

    if (error.isNotEmpty())
        status ("Audio device: " + error);

    deviceManager.addAudioCallback (this);
    refreshMidiDevices();
}

void Engine::saveDeviceSettings()
{
    if (auto xml = deviceManager.createStateXml())
        props.setValue ("audioDevice", xml.get());
    props.saveIfNeeded();
}

bool Engine::isDeviceOpen() const
{
    return deviceManager.getCurrentAudioDevice() != nullptr && deviceManager.getCurrentAudioDevice()->isPlaying();
}

StringArray Engine::getInputChannelNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getInputChannelNames();
    return {};
}

StringArray Engine::getOutputChannelNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getOutputChannelNames();
    return {};
}

StringArray Engine::getMidiOutputNames() const
{
    StringArray names;
    for (auto& d : MidiOutput::getAvailableDevices())
        names.add (d.name);
    return names;
}

String Engine::describeDevice() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr)
        return "No audio device";

    const auto sr = d->getCurrentSampleRate();
    const auto bs = d->getCurrentBufferSizeSamples();
    return d->getName() + "  " + String (sr / 1000.0, 1) + " kHz  " + String (bs) + " samples ("
           + String (1000.0 * bs / jmax (1.0, sr), 1) + " ms)";
}

int Engine::getRecordOffsetSamples() const
{
    const double sr = deviceRate.load();
    const double userMs = props.getDoubleValue ("recordOffsetMs", 0.0);
    return inputLatency + outputLatency + graphLatency + (int) std::lround (userMs * sr / 1000.0);
}

//==============================================================================
void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    deviceRate = device->getCurrentSampleRate();
    deviceBlock = device->getCurrentBufferSizeSamples();
    inputLatency = device->getInputLatencyInSamples();
    outputLatency = device->getOutputLatencyInSamples();

    auto mapChannels = [] (const BigInteger& active, int numDevice, int* map)
    {
        int index = 0;
        for (int ch = 0; ch < kMaxChannels; ++ch)
            map[ch] = (ch < numDevice && active[ch]) ? index++ : -1;
    };

    numDeviceInputs = jmin (kMaxChannels, device->getInputChannelNames().size());
    numDeviceOutputs = jmin (kMaxChannels, device->getOutputChannelNames().size());
    mapChannels (device->getActiveInputChannels(), numDeviceInputs, inputMap);
    mapChannels (device->getActiveOutputChannels(), numDeviceOutputs, outputMap);

    const int needed = jmax (8192, deviceBlock.load());
    if (needed > zerosSize)
    {
        zeros.calloc ((size_t) needed);
        zerosSize = needed;
    }

    clickRemaining = 0;
    triggerAsyncUpdate();
}

void Engine::audioDeviceStopped()
{
    triggerAsyncUpdate();
}

void Engine::handleAsyncUpdate()
{
    {
        SpinLock::ScopedLockType sl (midiLock);
        const double sr = deviceRate.load();
        for (auto& c : collectors)
            c->reset (sr > 0 ? sr : 44100.0);
    }

    rebuild();

    if (onDeviceChanged)
        onDeviceChanged();
}

void Engine::timerCallback()
{
    // Plug-ins can change their latency at any time (lookahead switches etc).
    if (edit != nullptr && ! exporting && latencySignature() != lastLatencySignature)
        rebuild();

    // If the interface went away (unplugged, driver reset), keep trying to get it back.
    if (deviceManager.getCurrentAudioDevice() == nullptr && ! exporting)
    {
        if (--reopenCountdown <= 0)
        {
            reopenCountdown = 4;
            if (auto saved = props.getXmlValue ("audioDevice"))
            {
                const auto err = deviceManager.initialise (256, 256, saved.get(), false);
                if (err.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr)
                    status ("Audio device reconnected: " + describeDevice());
            }
        }
    }
}

//==============================================================================
void Engine::refreshMidiDevices()
{
    const auto inputs = MidiInput::getAvailableDevices();
    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 44100.0;

    std::vector<std::unique_ptr<MidiMessageCollector>> newCollectors;
    std::vector<MidiBuffer> newBuffers;
    StringArray ids, names;

    for (auto& d : inputs)
    {
        if (! deviceManager.isMidiInputDeviceEnabled (d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);

        auto c = std::make_unique<MidiMessageCollector>();
        c->reset (sr);
        newCollectors.push_back (std::move (c));
        newBuffers.emplace_back();
        newBuffers.back().ensureSize (8192);
        ids.add (d.identifier);
        names.add (d.name);
    }

    {
        const ScopedLock sl (graphLock);
        SpinLock::ScopedLockType ml (midiLock);
        std::swap (collectors, newCollectors);
        std::swap (liveMidi, newBuffers);
        midiInputIds = ids;
    }

    midiInputNames = names;
    rebuild();

    if (onDeviceChanged)
        onDeviceChanged();
}

void Engine::handleIncomingMidiMessage (MidiInput* source, const MidiMessage& m)
{
    if (m.isActiveSense() || m.isMidiClock() || m.isSysEx() || source == nullptr)
        return;

    SpinLock::ScopedLockType sl (midiLock);
    const auto id = source->getIdentifier();
    for (int i = 0; i < midiInputIds.size(); ++i)
        if (midiInputIds[i] == id && (size_t) i < collectors.size())
            collectors[(size_t) i]->addMessageToQueue (m);
}

MidiOutput* Engine::getMidiOutput (const String& name)
{
    if (name.isEmpty())
        return nullptr;

    if (auto it = midiOutputs.find (name); it != midiOutputs.end())
        return it->second.get();

    for (auto& d : MidiOutput::getAvailableDevices())
    {
        if (d.name == name)
        {
            if (auto out = MidiOutput::openDevice (d.identifier))
            {
                out->startBackgroundThread();
                auto* raw = out.get();
                midiOutputs[name] = std::move (out);
                return raw;
            }
        }
    }
    return nullptr;
}

//==============================================================================
void Engine::prepareSlot (PluginSlot& slot)
{
    const double sr = deviceRate.load();
    const int bs = deviceBlock.load();

    if (slot.instance == nullptr || sr <= 0 || bs <= 0)
        return;

    if (slot.preparedRate == sr && slot.preparedBlock == bs)
        return;

    if (slot.preparedRate > 0)
        slot.instance->releaseResources();

    slot.instance->setRateAndBufferSizeDetails (sr, bs);
    slot.instance->prepareToPlay (sr, bs);
    slot.preparedRate = sr;
    slot.preparedBlock = bs;
}

std::shared_ptr<PluginSlot> Engine::makeSlot (const PluginDescription& saved, const MemoryBlock& state, String& error)
{
    auto slot = std::make_shared<PluginSlot>();
    slot->description = saved;

    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    const int bs = deviceBlock.load() > 0 ? deviceBlock.load() : 512;

    if (auto resolved = host.resolve (saved))
    {
        slot->instance = host.create (*resolved, sr, bs, error);
        if (slot->instance != nullptr)
        {
            slot->description = *resolved;
            slot->instance->setPlayHead (&playHead);
            if (state.getSize() > 0)
                slot->instance->setStateInformation (state.getData(), (int) state.getSize());
            prepareSlot (*slot);
        }
    }

    if (slot->instance == nullptr)
    {
        slot->savedState = state;
        if (error.isEmpty())
            error = saved.name + " isn't installed (or hasn't been scanned)";
    }

    return slot;
}

Array<int> Engine::latencySignature() const
{
    Array<int> sig;
    if (edit == nullptr)
        return sig;

    auto add = [&sig] (const Track& t)
    {
        int lat = 0;
        for (auto& p : t.plugins)
            if (p->instance != nullptr && ! p->bypassed)
                lat += p->instance->getLatencySamples();
        sig.add (lat);
    };

    for (auto& t : edit->tracks) add (*t);
    for (auto& t : edit->buses)  add (*t);
    add (*edit->master);
    return sig;
}

//==============================================================================
std::unique_ptr<RenderGraph> Engine::buildGraph()
{
    const double sr = deviceRate.load();
    const int bs = deviceBlock.load();

    if (edit == nullptr || sr <= 0 || bs <= 0)
        return {};

    auto g = std::make_unique<RenderGraph>();
    g->sampleRate = sr;
    g->maxBlock = bs;
    g->samplesPerBeat = edit->samplesPerBeat (sr);
    g->samplesPerClick = g->samplesPerBeat * 4.0 / edit->timeSigDen;
    g->clicksPerBar = edit->timeSigNum;
    g->loop = edit->loopEnabled && edit->loopEnd > edit->loopStart;
    g->loopStart = (int64) std::llround (edit->loopStart * g->samplesPerBeat);
    g->loopEnd = (int64) std::llround (edit->loopEnd * g->samplesPerBeat);
    g->scratch.setSize (2, bs);

    std::map<int, int> busIndex;
    for (size_t i = 0; i < edit->buses.size(); ++i)
        busIndex[edit->buses[i]->id] = (int) i;

    auto setup = [&] (RenderTrack& rt, const std::shared_ptr<Track>& t)
    {
        rt.track = t;
        rt.kind = t->kind;
        rt.numChannels = 2;

        for (auto& slot : t->plugins)
        {
            if (slot->instance == nullptr)
                continue;   // missing plug-in: audio passes through

            prepareSlot (*slot);
            slot->instance->setPlayHead (&playHead);

            RenderPlugin p;
            p.slot = slot;
            p.processor = slot->instance.get();
            p.numChannels = jmax (2, p.processor->getTotalNumInputChannels(), p.processor->getTotalNumOutputChannels());
            rt.numChannels = jmax (rt.numChannels, p.numChannels);
            if (! slot->bypassed)
                rt.latency += p.processor->getLatencySamples();
            rt.plugins.push_back (p);
        }

        rt.buffer.setSize (rt.numChannels, bs);
        rt.buffer.clear();
        rt.midi.ensureSize (4096);

        rt.outKind = t->output.kind;
        rt.outChannel = t->output.channel;
        rt.outStereo = t->output.stereo;
        if (rt.outKind == OutputRoute::Kind::bus)
        {
            auto it = busIndex.find (t->output.busId);
            if (it != busIndex.end() && ! t->isBus())
                rt.outBus = it->second;
            else
                rt.outKind = OutputRoute::Kind::master;
        }
    };

    // --- returns
    g->buses.resize (edit->buses.size());
    for (size_t i = 0; i < edit->buses.size(); ++i)
        setup (g->buses[i], edit->buses[i]);

    // --- master
    setup (g->master, edit->master);
    g->master.outKind = OutputRoute::Kind::hardware;

    // --- tracks
    StringArray midiNames = midiInputNames;
    g->tracks.resize (edit->tracks.size());
    for (size_t i = 0; i < edit->tracks.size(); ++i)
    {
        auto& t = edit->tracks[i];
        auto& rt = g->tracks[i];
        setup (rt, t);

        rt.group = edit->findGroup (t->groupId);
        rt.inputChannel = t->input.channel;
        rt.inputStereo = t->input.stereo;
        rt.midiChannel = jlimit (1, 16, t->midiChannel);
        rt.midiInput = t->midiInput == "All" ? -1 : t->midiInput == "None" ? -2 : midiNames.indexOf (t->midiInput);
        if (rt.midiInput < -2 || (rt.midiInput >= 0 && rt.midiInput >= (int) collectors.size()))
            rt.midiInput = -2;
        if (t->isMidi())
            rt.midiOut = getMidiOutput (t->midiOutput);

        for (auto& s : t->sends)
            if (auto it = busIndex.find (s->busId); it != busIndex.end())
                rt.sends.push_back ({ s, it->second });

        // audio clips
        for (auto& c : t->clips)
        {
            if (c.kind != ClipKind::audio)
                continue;

            if (auto src = pool.get (c.file))
            {
                RenderClip rc;
                rc.start = (int64) std::llround (c.start * g->samplesPerBeat);
                rc.end = (int64) std::llround (c.end() * g->samplesPerBeat);
                rc.sourceOffset = c.sourceOffset;
                rc.gain = Decibels::decibelsToGain (c.gainDb, -100.0f);
                rc.source = src;
                if (rc.end > rc.start)
                    rt.clips.push_back (rc);
            }
        }
        std::sort (rt.clips.begin(), rt.clips.end(), [] (auto& a, auto& b) { return a.start < b.start; });

        // MIDI clips flattened to timestamped events
        const auto chan = (uint8) (rt.midiChannel - 1);
        for (auto& c : t->clips)
        {
            if (c.kind != ClipKind::midi)
                continue;

            for (auto& n : c.notes)
            {
                if (n.start >= c.length || n.start < 0.0)
                    continue;

                const double endBeat = jmin (n.start + n.length, c.length);
                const auto vel = (uint8) jlimit (1, 127, roundToInt (n.velocity * 127.0f));
                RenderMidi on  { (int64) std::llround ((c.start + n.start) * g->samplesPerBeat), { (uint8) (0x90 | chan), (uint8) n.pitch, vel }, 3 };
                RenderMidi off { (int64) std::llround ((c.start + endBeat) * g->samplesPerBeat), { (uint8) (0x80 | chan), (uint8) n.pitch, 0 }, 3 };
                if (off.sample <= on.sample)
                    off.sample = on.sample + 1;
                rt.midiEvents.push_back (on);
                rt.midiEvents.push_back (off);
            }

            for (auto& e : c.events)
            {
                if (e.beat < 0.0 || e.beat >= c.length || e.size <= 0)
                    continue;

                RenderMidi m;
                m.sample = (int64) std::llround ((c.start + e.beat) * g->samplesPerBeat);
                m.size = (uint8) e.size;
                for (int b = 0; b < e.size; ++b)
                    m.data[b] = e.data[b];
                if (m.data[0] >= 0x80 && m.data[0] < 0xf0)
                    m.data[0] = (uint8) ((m.data[0] & 0xf0) | chan);
                rt.midiEvents.push_back (m);
            }
        }

        // Sort by time; at equal times note-offs go first so repeated notes retrigger.
        std::sort (rt.midiEvents.begin(), rt.midiEvents.end(), [] (const RenderMidi& a, const RenderMidi& b)
        {
            if (a.sample != b.sample)
                return a.sample < b.sample;
            const bool aOff = (a.data[0] & 0xf0) == 0x80;
            const bool bOff = (b.data[0] & 0xf0) == 0x80;
            return aOff && ! bOff;
        });
    }

    // --- plug-in delay compensation
    for (auto& rt : g->tracks) g->trackLatency = jmax (g->trackLatency, rt.latency);
    for (auto& rt : g->buses)  g->busLatency = jmax (g->busLatency, rt.latency);
    g->masterLatency = g->master.latency;

    for (auto& rt : g->tracks)
    {
        rt.align.prepare (g->trackLatency - rt.latency);
        rt.direct.prepare (g->busLatency);
    }
    for (auto& rt : g->buses)
        rt.align.prepare (g->busLatency - rt.latency);

    graphLatency = g->trackLatency + g->busLatency + g->masterLatency;
    lastLatencySignature = latencySignature();

    // --- plug-in transport info
    playHead.sampleRate = sr;
    playHead.bpm = edit->tempo;
    playHead.samplesPerBeat = g->samplesPerBeat;
    playHead.num = edit->timeSigNum;
    playHead.den = edit->timeSigDen;
    playHead.looping = g->loop;
    playHead.loopStartBeats = edit->loopStart;
    playHead.loopEndBeats = edit->loopEnd;

    return g;
}

void Engine::installGraph (std::unique_ptr<RenderGraph> g)
{
    std::unique_ptr<RenderGraph> old;
    {
        const ScopedLock sl (graphLock);
        if (exporting)
        {
            rebuildAfterExport = true;
            old = std::move (g);   // export owns the plug-ins right now
        }
        else
        {
            // Hook any active recording takes onto the new snapshot.
            if (g != nullptr && recordSession != nullptr)
            {
                for (auto& rt : g->tracks)
                {
                    for (auto& t : recordSession->audio) if (t->track == rt.track) rt.take = t.get();
                    for (auto& t : recordSession->midi)  if (t->track == rt.track) rt.midiTake = t.get();
                }
            }

            old = std::move (graph);
            graph = std::move (g);
        }
    }
    // `old` is destroyed here, on the message thread, outside the lock.
}

void Engine::rebuild()
{
    {
        const ScopedLock sl (graphLock);
        if (exporting)
        {
            rebuildAfterExport = true;
            return;
        }
    }

    // A plug-in that's already running needs re-preparing when the device rate or
    // block size changed. Take the current snapshot away from the audio thread first.
    bool reprepare = false;
    const double sr = deviceRate.load();
    const int bs = deviceBlock.load();
    if (edit != nullptr)
    {
        auto check = [&] (const Track& t)
        {
            for (auto& p : t.plugins)
                if (p->instance != nullptr && p->preparedRate > 0 && (p->preparedRate != sr || p->preparedBlock != bs))
                    reprepare = true;
        };
        for (auto& t : edit->tracks) check (*t);
        for (auto& t : edit->buses)  check (*t);
        check (*edit->master);
    }

    if (reprepare)
        installGraph (nullptr);

    installGraph (buildGraph());
}

void Engine::setEdit (Edit* e)
{
    if (recording)
        finishRecording();

    playing = false;
    recording = false;
    installGraph (nullptr);
    edit = e;
    positionSamples = 0;
    rebuild();
}

//==============================================================================
// Transport
//==============================================================================
double Engine::getPlayheadBeats() const
{
    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    if (edit == nullptr)
        return 0.0;
    return (double) positionSamples.load() / edit->samplesPerBeat (sr);
}

void Engine::seekTo (double beats)
{
    if (edit == nullptr)
        return;
    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    const auto s = (int64) std::llround (jmax (0.0, beats) * edit->samplesPerBeat (sr));
    positionSamples = s;
    seekRequest = s;
}

void Engine::play()
{
    if (edit == nullptr || playing)
        return;

    seekTo (edit->insertMarker);
    playing = true;
}

void Engine::stop()
{
    if (edit == nullptr)
        return;

    if (recording)
        finishRecording();

    if (playing)
    {
        playing = false;
        notesOffRequest = true;
        seekTo (edit->insertMarker);
    }
    else
    {
        // Stop again: back to the start.
        edit->insertMarker = 0.0;
        seekTo (0.0);
        edit->sendChangeMessage();
    }
}

void Engine::toggleRecord()
{
    if (recording)
        finishRecording();
    else
        startRecording();
}

void Engine::startRecording()
{
    if (edit == nullptr || recording)
        return;

    if (deviceRate.load() <= 0.0 || graph == nullptr)
    {
        status ("Can't record: no audio device is running. Check Options > Audio/MIDI Settings.");
        return;
    }

    edit->ensureWorkDir();
    const double sr = deviceRate.load();
    auto session = std::make_unique<RecordSession>();
    session->audioOffset = getRecordOffsetSamples();
    session->midiOffset = outputLatency + graphLatency;

    for (auto& t : edit->tracks)
    {
        if (! t->arm)
            continue;

        if (t->isAudio())
        {
            if (t->input.channel < 0 || t->input.channel >= numDeviceInputs)
            {
                status (t->name + " has no input selected, so it won't record.");
                continue;
            }

            auto take = std::make_unique<RecordTake>();
            take->track = t;
            take->channel = t->input.channel;
            take->stereo = t->input.stereo && t->input.channel + 1 < numDeviceInputs;

            int takeNumber = 1;
            for (auto& c : t->clips)
                if (c.kind == ClipKind::audio)
                    ++takeNumber;

            take->file = edit->getAudioDir().getNonexistentChildFile (File::createLegalFileName (t->name) + " " + String (takeNumber), ".wav", false);

            std::unique_ptr<OutputStream> stream (take->file.createOutputStream());
            WavAudioFormat wav;
            auto writer = stream != nullptr ? wav.createWriterFor (stream, AudioFormatWriterOptions()
                                                                              .withSampleRate (sr)
                                                                              .withNumChannels (take->stereo ? 2 : 1)
                                                                              .withBitsPerSample (24))
                                            : nullptr;
            if (writer == nullptr)
            {
                status ("Couldn't create " + take->file.getFullPathName());
                continue;
            }

            take->writer = std::make_unique<AudioFormatWriter::ThreadedWriter> (writer.release(), diskThread, (int) sr * 4);
            session->audio.push_back (std::move (take));
        }
        else if (t->isMidi())
        {
            auto take = std::make_unique<MidiTake>();
            take->track = t;
            take->events.reserve (200000);
            session->midi.push_back (std::move (take));
        }
    }

    if (session->audio.empty() && session->midi.empty())
    {
        status ("Arm a track to record (the round button on the track).");
        return;
    }

    {
        const ScopedLock sl (graphLock);
        recordSession = std::move (session);
        if (graph != nullptr)
        {
            for (auto& rt : graph->tracks)
            {
                for (auto& t : recordSession->audio) if (t->track == rt.track) rt.take = t.get();
                for (auto& t : recordSession->midi)  if (t->track == rt.track) rt.midiTake = t.get();
            }
        }
        recording = true;
    }

    if (! playing)
        play();
}

void Engine::finishRecording()
{
    std::unique_ptr<RecordSession> session;
    {
        const ScopedLock sl (graphLock);
        recording = false;
        session = std::move (recordSession);
        if (graph != nullptr)
            for (auto& rt : graph->tracks)
                rt.take = nullptr, rt.midiTake = nullptr;
    }

    if (session == nullptr || edit == nullptr)
        return;

    const double spb = edit->samplesPerBeat (deviceRate.load() > 0 ? deviceRate.load() : edit->sampleRate);
    bool addedAny = false;
    bool overflowed = false;

    for (auto& take : session->audio)
    {
        take->writer.reset();   // flushes to disk

        const int64 usable = take->numSamples - session->audioOffset;
        if (take->startSample < 0 || usable <= 0)
        {
            take->file.deleteFile();
            continue;
        }

        if (! addedAny)
            edit->beginClipEdit();

        Clip c;
        c.id = edit->newId();
        c.kind = ClipKind::audio;
        c.name = take->file.getFileNameWithoutExtension();
        c.file = take->file;
        c.sourceOffset = session->audioOffset;
        c.start = (double) take->startSample / spb;
        c.length = (double) usable / spb;

        edit->clearRange (*take->track, c.start, c.end());
        take->track->clips.push_back (c);
        addedAny = true;
        overflowed |= take->overflow;
    }

    for (auto& take : session->midi)
    {
        if (take->startSample < 0 || take->events.empty())
            continue;

        const int64 clipStart = take->startSample;
        const double length = (double) (take->endSample - clipStart) / spb;
        if (length <= 0.0)
            continue;

        if (! addedAny)
            edit->beginClipEdit();

        Clip c;
        c.id = edit->newId();
        c.kind = ClipKind::midi;
        c.name = take->track->name;
        c.start = (double) clipStart / spb;
        c.length = length;

        // Pair note-ons with note-offs.
        struct Open { double start = -1.0; float velocity = 0.0f; };
        Open open[16][128];

        auto toBeat = [&] (int64 s) { return jlimit (0.0, length, (double) (s - session->midiOffset - clipStart) / spb); };

        for (auto& e : take->events)
        {
            const uint8 status = e.data[0];
            const int ch = status & 0x0f;
            const int type = status & 0xf0;
            const double beat = toBeat (e.sample);

            if (type == 0x90 && e.data[2] > 0)
            {
                open[ch][e.data[1]] = { beat, e.data[2] / 127.0f };
            }
            else if (type == 0x80 || (type == 0x90 && e.data[2] == 0))
            {
                auto& o = open[ch][e.data[1]];
                if (o.start >= 0.0)
                {
                    c.notes.push_back ({ (int) e.data[1], o.start, jmax (0.01, beat - o.start), o.velocity });
                    o.start = -1.0;
                }
            }
            else if (type >= 0xa0 && type <= 0xe0)
            {
                MidiEvent me;
                me.beat = beat;
                me.size = e.size;
                std::memcpy (me.data, e.data, 3);
                c.events.push_back (me);
            }
        }

        for (int ch = 0; ch < 16; ++ch)
            for (int p = 0; p < 128; ++p)
                if (open[ch][p].start >= 0.0)
                    c.notes.push_back ({ p, open[ch][p].start, jmax (0.01, length - open[ch][p].start), open[ch][p].velocity });

        std::sort (c.notes.begin(), c.notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });

        edit->clearRange (*take->track, c.start, c.end());
        take->track->clips.push_back (c);
        addedAny = true;
    }

    if (overflowed)
        status ("Warning: the disk couldn't keep up while recording; parts of the take may be missing.");

    if (addedAny)
        edit->changed();

    if (onRecordingFinished)
        onRecordingFinished();
}

//==============================================================================
// Audio callback
//==============================================================================
void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                               float* const* out, int numOut,
                                               int n, const AudioIODeviceCallbackContext&)
{
    ScopedNoDenormals noDenormals;

    for (int i = 0; i < numOut; ++i)
        if (out[i] != nullptr)
            FloatVectorOperations::clear (out[i], n);

    const ScopedTryLock sl (graphLock);
    if (! sl.isLocked() || graph == nullptr)
        return;

    auto& g = *graph;
    if (g.sampleRate != deviceRate.load() || g.maxBlock <= 0)
        return;

    const float* inDev[kMaxChannels] {};
    float* outDev[kMaxChannels] {};

    for (int d = 0; d < numDeviceInputs; ++d)
        inDev[d] = (inputMap[d] >= 0 && inputMap[d] < numIn) ? in[inputMap[d]] : nullptr;
    for (int d = 0; d < numDeviceOutputs; ++d)
        outDev[d] = (outputMap[d] >= 0 && outputMap[d] < numOut) ? out[outputMap[d]] : nullptr;

    for (size_t i = 0; i < collectors.size() && i < liveMidi.size(); ++i)
    {
        liveMidi[i].clear();
        collectors[i]->removeNextBlockOfMessages (liveMidi[i], n);
    }

    bool notesOff = notesOffRequest.exchange (false);
    int64 pos = positionSamples.load();
    if (const auto s = seekRequest.exchange (-1); s >= 0)
    {
        pos = s;
        notesOff = true;
    }

    const bool rolling = playing.load();
    const bool rec = recording.load();

    playHead.playing = rolling;
    playHead.recording = rec;

    int done = 0;
    while (done < n)
    {
        int len = jmin (n - done, g.maxBlock);
        if (rolling && g.loop && ! rec && pos < g.loopEnd)
            len = (int) jmin ((int64) len, g.loopEnd - pos);

        playHead.position = pos;
        renderChunk (g, pos, len, done, rolling, inDev, numDeviceInputs, outDev, numDeviceOutputs, true, notesOff);
        notesOff = false;
        done += len;

        if (rolling)
        {
            pos += len;
            if (g.loop && ! rec && pos == g.loopEnd)
            {
                pos = g.loopStart;
                notesOff = true;
            }
        }
    }

    positionSamples = pos;
}

void Engine::renderChunk (RenderGraph& g, int64 pos, int len, int offset, bool rolling,
                          const float* const* inDev, int numIn, float* const* outDev, int numOut,
                          bool realtime, bool notesOff)
{
    bool anySoloTrack = false, anySoloBus = false;
    for (auto& rt : g.tracks) anySoloTrack = anySoloTrack || rt.track->solo.load() || (rt.group != nullptr && rt.group->solo.load());
    for (auto& rt : g.buses)  anySoloBus = anySoloBus || rt.track->solo.load();

    for (auto& rt : g.buses)
        for (int ch = 0; ch < rt.numChannels; ++ch)
            rt.buffer.clear (ch, 0, len);
    for (int ch = 0; ch < g.master.numChannels; ++ch)
        g.master.buffer.clear (ch, 0, len);

    for (auto& rt : g.tracks)
        processTrack (g, rt, pos, len, offset, rolling, inDev, numIn, outDev, numOut, realtime, notesOff, anySoloTrack);

    for (auto& rt : g.buses)
        processBus (g, rt, len, offset, outDev, numOut, anySoloBus);

    // master
    auto& m = g.master;
    m.midi.clear();
    for (auto& p : m.plugins)
    {
        if (p.slot->bypassed.load())
            continue;
        AudioBuffer<float> view (m.buffer.getArrayOfWritePointers(), p.numChannels, len);
        p.processor->processBlock (view, m.midi);
    }
    applyFader (m, len, m.track->mute.load());
    addToOutput (m.buffer, len, offset, m.outChannel, true, outDev, numOut);

    if (realtime && rolling && metronomeOn.load())
        renderMetronome (g, pos, len, offset, outDev, numOut);
}

void Engine::applyFader (RenderTrack& rt, int len, bool silenced)
{
    auto& t = *rt.track;
    const float gain = silenced ? 0.0f : dbToGain (t.volumeDb.load());
    const float theta = (jlimit (-1.0f, 1.0f, t.pan.load()) + 1.0f) * MathConstants<float>::pi * 0.25f;
    const float targetL = gain * jmin (1.0f, MathConstants<float>::sqrt2 * std::cos (theta));
    const float targetR = gain * jmin (1.0f, MathConstants<float>::sqrt2 * std::sin (theta));

    if (! t.rtGainValid)
    {
        t.rtGainL = targetL;
        t.rtGainR = targetR;
        t.rtGainValid = true;
    }

    rt.buffer.applyGainRamp (0, 0, len, t.rtGainL, targetL);
    rt.buffer.applyGainRamp (1, 0, len, t.rtGainR, targetR);
    t.rtGainL = targetL;
    t.rtGainR = targetR;

    const float peakL = rt.buffer.getMagnitude (0, 0, len);
    const float peakR = rt.buffer.getMagnitude (1, 0, len);
    if (peakL > t.meterL.load()) t.meterL = peakL;
    if (peakR > t.meterR.load()) t.meterR = peakR;
}

void Engine::addToOutput (const AudioBuffer<float>& buf, int len, int offset, int channel, bool stereo,
                          float* const* outDev, int numOut)
{
    if (outDev == nullptr || channel < 0)
        return;

    if (stereo)
    {
        for (int ch = 0; ch < 2; ++ch)
            if (channel + ch < numOut && outDev[channel + ch] != nullptr)
                FloatVectorOperations::add (outDev[channel + ch] + offset, buf.getReadPointer (ch), len);
    }
    else if (channel < numOut && outDev[channel] != nullptr)
    {
        FloatVectorOperations::addWithMultiply (outDev[channel] + offset, buf.getReadPointer (0), 0.7071f, len);
        FloatVectorOperations::addWithMultiply (outDev[channel] + offset, buf.getReadPointer (1), 0.7071f, len);
    }
}

void Engine::processTrack (RenderGraph& g, RenderTrack& rt, int64 pos, int len, int offset, bool rolling,
                           const float* const* inDev, int numIn, float* const* outDev, int numOut,
                           bool realtime, bool notesOff, bool anySolo)
{
    auto& t = *rt.track;
    auto& buf = rt.buffer;

    for (int ch = 0; ch < rt.numChannels; ++ch)
        buf.clear (ch, 0, len);
    rt.midi.clear();

    const bool recordingHere = realtime && rolling && recording.load() && (rt.take != nullptr || rt.midiTake != nullptr);
    const int mon = t.monitor.load();
    const bool monitorInput = realtime && (mon == (int) Monitor::in || (mon == (int) Monitor::automatic && t.arm.load()));

    if (rt.kind == TrackKind::audio)
    {
        if (rolling && ! recordingHere)
            renderClips (g, rt, pos, len);

        if (realtime && (monitorInput || recordingHere) && rt.inputChannel >= 0)
        {
            const float* left = rt.inputChannel < numIn ? inDev[rt.inputChannel] : nullptr;
            const float* right = rt.inputStereo ? (rt.inputChannel + 1 < numIn ? inDev[rt.inputChannel + 1] : nullptr) : left;

            if (recordingHere && rt.take != nullptr && rt.take->writer != nullptr)
            {
                auto* take = rt.take;
                if (take->startSample < 0)
                    take->startSample = pos;

                const float* data[2] = { left != nullptr ? left + offset : zeros.get(),
                                         right != nullptr ? right + offset : zeros.get() };
                if (! take->writer->write (data, len))
                    take->overflow = true;
                take->numSamples += len;
            }

            if (monitorInput)
            {
                if (left != nullptr)  buf.addFrom (0, 0, left + offset, len);
                if (right != nullptr) buf.addFrom (1, 0, right + offset, len);
            }
        }
    }
    else if (rt.kind == TrackKind::midi)
    {
        const int ch = rt.midiChannel;
        if (notesOff)
        {
            rt.midi.addEvent (MidiMessage::controllerEvent (ch, 64, 0), 0);
            rt.midi.addEvent (MidiMessage::allNotesOff (ch), 0);
        }

        if (rolling && ! recordingHere)
        {
            auto& ev = rt.midiEvents;
            auto it = std::lower_bound (ev.begin(), ev.end(), pos, [] (const RenderMidi& e, int64 p) { return e.sample < p; });
            for (; it != ev.end() && it->sample < pos + len; ++it)
                rt.midi.addEvent (it->data, it->size, (int) (it->sample - pos));
        }

        if (realtime && (monitorInput || recordingHere) && rt.midiInput != -2)
        {
            auto* take = recordingHere ? rt.midiTake : nullptr;
            if (take != nullptr && take->startSample < 0)
                take->startSample = pos;

            for (size_t i = 0; i < liveMidi.size(); ++i)
            {
                if (rt.midiInput >= 0 && (size_t) rt.midiInput != i)
                    continue;

                for (const auto meta : liveMidi[i])
                {
                    if (meta.samplePosition < offset || meta.samplePosition >= offset + len || meta.numBytes > 3 || meta.numBytes < 1)
                        continue;

                    uint8 data[3] {};
                    std::memcpy (data, meta.data, (size_t) meta.numBytes);
                    if (data[0] >= 0x80 && data[0] < 0xf0)
                        data[0] = (uint8) ((data[0] & 0xf0) | (ch - 1));

                    const int s = meta.samplePosition - offset;
                    if (monitorInput || recordingHere)
                        rt.midi.addEvent (data, meta.numBytes, s);

                    if (take != nullptr && take->events.size() < take->events.capacity())
                    {
                        MidiTake::Event e { pos + s, { data[0], data[1], data[2] }, (uint8) meta.numBytes };
                        take->events.push_back (e);
                    }
                }
            }

            if (take != nullptr)
                take->endSample = pos + len;
        }

        if (realtime && rt.midiOut != nullptr && ! rt.midi.isEmpty())
            rt.midiOut->sendBlockOfMessages (rt.midi,
                                             Time::getMillisecondCounterHiRes() + 1000.0 * (offset + outputLatency) / g.sampleRate,
                                             g.sampleRate);
    }

    // inserts
    for (auto& p : rt.plugins)
    {
        if (p.slot->bypassed.load())
            continue;
        AudioBuffer<float> view (buf.getArrayOfWritePointers(), p.numChannels, len);
        p.processor->processBlock (view, rt.midi);
    }

    rt.align.process (buf, len);

    const bool groupMuted = rt.group != nullptr && rt.group->mute.load();
    const bool soloed = t.solo.load() || (rt.group != nullptr && rt.group->solo.load());
    const bool silenced = t.mute.load() || groupMuted || (anySolo && ! soloed);

    if (! silenced)
    {
        for (auto& s : rt.sends)
        {
            if (! s.send->preFader.load())
                continue;
            const float gain = dbToGain (s.send->levelDb.load());
            if (gain > 0.0f)
                for (int ch = 0; ch < 2; ++ch)
                    g.buses[(size_t) s.busIndex].buffer.addFrom (ch, 0, buf, ch, 0, len, gain);
        }
    }

    applyFader (rt, len, silenced);

    for (auto& s : rt.sends)
    {
        if (s.send->preFader.load())
            continue;
        const float gain = dbToGain (s.send->levelDb.load());
        if (gain > 0.0f)
            for (int ch = 0; ch < 2; ++ch)
                g.buses[(size_t) s.busIndex].buffer.addFrom (ch, 0, buf, ch, 0, len, gain);
    }

    if (rt.outKind == OutputRoute::Kind::bus && rt.outBus >= 0)
        for (int ch = 0; ch < 2; ++ch)
            g.buses[(size_t) rt.outBus].buffer.addFrom (ch, 0, buf, ch, 0, len);

    // After this the buffer is aligned with the master bus (it's also the stem).
    rt.direct.process (buf, len);

    if (rt.outKind == OutputRoute::Kind::master)
    {
        for (int ch = 0; ch < 2; ++ch)
            g.master.buffer.addFrom (ch, 0, buf, ch, 0, len);
    }
    else if (rt.outKind == OutputRoute::Kind::hardware && realtime)
    {
        addToOutput (buf, len, offset, rt.outChannel, rt.outStereo, outDev, numOut);
    }
}

void Engine::processBus (RenderGraph& g, RenderTrack& rt, int len, int offset, float* const* outDev, int numOut, bool anySolo)
{
    auto& t = *rt.track;
    rt.midi.clear();

    for (auto& p : rt.plugins)
    {
        if (p.slot->bypassed.load())
            continue;
        AudioBuffer<float> view (rt.buffer.getArrayOfWritePointers(), p.numChannels, len);
        p.processor->processBlock (view, rt.midi);
    }

    rt.align.process (rt.buffer, len);
    applyFader (rt, len, t.mute.load() || (anySolo && ! t.solo.load()));

    if (rt.outKind == OutputRoute::Kind::hardware)
        addToOutput (rt.buffer, len, offset, rt.outChannel, rt.outStereo, outDev, numOut);
    else
        for (int ch = 0; ch < 2; ++ch)
            g.master.buffer.addFrom (ch, 0, rt.buffer, ch, 0, len);
}

void Engine::renderClips (RenderGraph& g, RenderTrack& rt, int64 pos, int len)
{
    const int64 windowEnd = pos + len;
    float* tmp[2] = { g.scratch.getWritePointer (0), g.scratch.getWritePointer (1) };

    for (auto& c : rt.clips)
    {
        if (c.start >= windowEnd)
            break;
        if (c.end <= pos)
            continue;

        const int64 from = jmax (pos, c.start);
        const int64 to = jmin (windowEnd, c.end);
        const int n = (int) (to - from);
        const int dst = (int) (from - pos);
        const int64 src = c.sourceOffset + (from - c.start);

        if (n <= 0 || src >= c.source->lengthInSamples)
            continue;

        const int numCh = jmin (2, c.source->numChannels);
        c.source->reader->read (tmp, numCh, src, n);
        if (numCh == 1)
            FloatVectorOperations::copy (tmp[1], tmp[0], n);

        // Short fades at clip edges so cuts never click.
        const int64 fade = jmin ((int64) 64, (c.end - c.start) / 2);
        if (fade > 0 && (from < c.start + fade || to > c.end - fade))
        {
            for (int i = 0; i < n; ++i)
            {
                const int64 s = from + i;
                float gain = 1.0f;
                if (s - c.start < fade) gain = (float) (s - c.start) / (float) fade;
                if (c.end - s < fade)   gain = jmin (gain, (float) (c.end - s) / (float) fade);
                tmp[0][i] *= gain;
                tmp[1][i] *= gain;
            }
        }

        rt.buffer.addFrom (0, dst, tmp[0], n, c.gain);
        rt.buffer.addFrom (1, dst, tmp[1], n, c.gain);
    }
}

void Engine::renderMetronome (RenderGraph& g, int64 pos, int len, int offset, float* const* outDev, int numOut)
{
    const int ch = g.master.outChannel;
    float* left = ch < numOut ? outDev[ch] : nullptr;
    float* right = ch + 1 < numOut ? outDev[ch + 1] : nullptr;
    if (left == nullptr && right == nullptr)
        return;

    const double interval = g.samplesPerClick;
    int64 k = (int64) std::ceil ((double) pos / interval - 1.0e-9);
    int nextStart = len;
    bool nextAccent = false;

    auto findNext = [&]
    {
        const auto s = (int64) std::llround ((double) k * interval);
        nextStart = (s < pos + len) ? (int) jmax ((int64) 0, s - pos) : len;
        nextAccent = (k % jmax (1, g.clicksPerBar)) == 0;
    };
    findNext();

    const double sr = g.sampleRate;
    for (int i = 0; i < len; ++i)
    {
        if (i == nextStart)
        {
            clickFreq = nextAccent ? 1600.0 : 1100.0;
            clickAmp = nextAccent ? 0.45f : 0.3f;
            clickRemaining = (int) (0.03 * sr);
            clickPhase = 0.0;
            ++k;
            findNext();
        }

        if (clickRemaining > 0)
        {
            const float env = clickAmp * (float) clickRemaining / (float) (0.03 * sr);
            const float s = env * (float) std::sin (clickPhase);
            clickPhase += MathConstants<double>::twoPi * clickFreq / sr;
            --clickRemaining;
            if (left != nullptr)  left[offset + i] += s;
            if (right != nullptr) right[offset + i] += s;
        }
    }
}

//==============================================================================
// Export
//==============================================================================
Result Engine::exportAudio (const ExportSettings& s, const std::function<bool (double)>& progress, StringArray& written)
{
    if (playing || recording)
        return Result::fail ("Stop playback before exporting.");

    std::unique_ptr<RenderGraph> g;
    {
        const ScopedLock sl (graphLock);
        g = std::move (graph);
        exporting = true;
    }

    auto restore = [this, &g]
    {
        const ScopedLock sl (graphLock);
        exporting = false;
        if (graph == nullptr)
            graph = std::move (g);
        if (rebuildAfterExport)
        {
            rebuildAfterExport = false;
            triggerAsyncUpdate();
        }
    };

    if (g == nullptr)
    {
        restore();
        return Result::fail ("The audio engine isn't running. Open an audio device first.");
    }

    std::vector<AudioPluginInstance*> processors;
    auto collect = [&] (RenderTrack& rt)
    {
        rt.align.clear();
        rt.direct.clear();
        for (auto& p : rt.plugins)
            processors.push_back (p.processor);
    };
    for (auto& rt : g->tracks) collect (rt);
    for (auto& rt : g->buses)  collect (rt);
    collect (g->master);

    for (auto* p : processors)
    {
        p->setNonRealtime (true);
        p->reset();
    }

    const double sr = g->sampleRate;
    const int64 start = (int64) std::llround (s.startBeat * g->samplesPerBeat);
    const int64 length = (int64) std::llround ((s.endBeat - s.startBeat) * g->samplesPerBeat) + (int64) (s.tailSeconds * sr);
    const int mixLatency = g->trackLatency + g->busLatency + g->masterLatency;
    const int stemLatency = g->trackLatency + g->busLatency;
    const int64 total = length + mixLatency;

    s.folder.createDirectory();

    auto makeWriter = [&] (const File& f) -> std::unique_ptr<AudioFormatWriter>
    {
        f.deleteFile();
        std::unique_ptr<OutputStream> stream (f.createOutputStream());
        if (stream == nullptr)
            return {};
        WavAudioFormat wav;
        auto options = AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (s.bitDepth);
        if (s.bitDepth == 32)
            options = options.withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint);
        return wav.createWriterFor (stream, options);
    };

    struct Output
    {
        const AudioBuffer<float>* buffer;
        File file;
        std::unique_ptr<AudioFormatWriter> writer;
        int latency;
        float peak = 0.0f;
        bool isStem;
    };
    std::vector<Output> outputs;

    if (s.mix)
    {
        auto f = s.folder.getChildFile (File::createLegalFileName (s.baseName) + ".wav");
        outputs.push_back ({ &g->master.buffer, f, makeWriter (f), mixLatency, 0.0f, false });
    }

    if (s.stems)
    {
        int index = 1;
        auto addStem = [&] (RenderTrack& rt)
        {
            const auto name = String (index++).paddedLeft ('0', 2) + " " + rt.track->name;
            auto f = s.folder.getChildFile (File::createLegalFileName (s.baseName + " - " + name) + ".wav");
            outputs.push_back ({ &rt.buffer, f, makeWriter (f), stemLatency, 0.0f, true });
        };
        for (auto& rt : g->tracks) addStem (rt);
        for (auto& rt : g->buses)  addStem (rt);
    }

    for (auto& o : outputs)
    {
        if (o.writer == nullptr)
        {
            for (auto* p : processors) p->setNonRealtime (false);
            restore();
            return Result::fail ("Couldn't write " + o.file.getFullPathName());
        }
    }

    float* noOutputs[1] = { nullptr };
    const float* noInputs[1] = { nullptr };
    bool cancelled = false;

    playHead.playing = true;
    playHead.recording = false;

    int64 rendered = 0;
    int64 pos = start;
    while (rendered < total)
    {
        const int len = (int) jmin ((int64) g->maxBlock, total - rendered);
        playHead.position = pos;
        renderChunk (*g, pos, len, 0, true, noInputs, 0, noOutputs, 0, false, rendered == 0);

        for (auto& o : outputs)
        {
            // Skip the plug-in latency at the start so files line up with the timeline.
            const int64 from = jmax (rendered, (int64) o.latency);
            const int64 to = jmin (rendered + len, (int64) o.latency + length);
            if (to > from)
            {
                const int bufStart = (int) (from - rendered);
                const int count = (int) (to - from);
                o.writer->writeFromAudioSampleBuffer (*o.buffer, bufStart, count);
                o.peak = jmax (o.peak, o.buffer->getMagnitude (bufStart, count));
            }
        }

        pos += len;
        rendered += len;

        if (! progress ((double) rendered / (double) total))
        {
            cancelled = true;
            break;
        }
    }

    for (auto& o : outputs)
    {
        o.writer.reset();
        // Silent stems (empty tracks, unused returns) aren't worth a file.
        if (cancelled || (o.isStem && o.peak <= 0.0f))
            o.file.deleteFile();
        else
            written.add (o.file.getFullPathName());
    }

    for (auto* p : processors)
    {
        p->setNonRealtime (false);
        p->reset();
    }
    for (auto& rt : g->tracks) rt.align.clear(), rt.direct.clear();
    for (auto& rt : g->buses)  rt.align.clear();

    playHead.playing = false;
    restore();

    return cancelled ? Result::fail ("Export cancelled.") : Result::ok();
}
} // namespace lathe
