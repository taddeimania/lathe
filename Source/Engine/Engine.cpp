#include "Engine.h"
#include "Mp3Writer.h"
#include "RenderPool.h"
#include "../Host/RemotePlugin.h"
#include "../Devices/Device.h"
#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

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
    int inputTrack = -1;           // index in RenderGraph::tracks: record/monitor what that track plays
    std::vector<int> receivers;    // tracks taking their input from this one
    AudioBuffer<float> tap;        // this block's sound after plug-ins and EQ (when an audio track listens)

    // MIDI tracks: notes are "tapped" right after the last plug-in that generates MIDI (an
    // arpeggiator, a step sequencer...) and before the first instrument; -1 = before any plug-in.
    int midiTapAfter = -1;
    bool recordGenerated = false;  // a plug-in generates notes: record what it hands on, not the input
    String generatorName;
    bool tapMidi = false;          // a MIDI track takes its input from this one
    MidiBuffer midiTap;            // this block's notes at the tap
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
    AudioBuffer<float> scratch;     // this track's own temp buffer (clip reading), so tracks can render at once
    bool heardElsewhere = false;    // set while rendering, used while mixing (see renderTrack)
    float costUs = 0.0f;            // how long rendering takes (running average), to hand out heavy tracks first

    RecordTake* take = nullptr;       // set while recording
    MidiTake* midiTake = nullptr;
};

/** A cue mix: channels are tapped after their plug-ins, before their main fader. */
struct RenderCue
{
    std::shared_ptr<CueMix> cue;
    std::vector<std::shared_ptr<CueLevel>> trackLevels;   // parallel to RenderGraph::tracks; null = not heard directly
    std::vector<std::shared_ptr<CueLevel>> busLevels;     // parallel to RenderGraph::buses
    AudioBuffer<float> trackSum, sum;
    DelayLine trackDelay;   // lines tracks up with the returns (return plug-in latency)
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
    std::vector<size_t> order;     // processing order of tracks: a track's input source comes first
    // For rendering in parallel: tracks grouped by dependency. Level 0 takes no other track as
    // its input; level n takes its input from level n-1. Each level is sorted heaviest first
    // before it's handed out (audio thread). Buses all depend only on the tracks.
    std::vector<std::vector<size_t>> levels;
    std::vector<size_t> busTasks;
    RenderTrack master;
    int trackLatency = 0, busLatency = 0, masterLatency = 0;

    std::vector<RenderCue> cues;
    AudioBuffer<float> scratch, click;
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
    int offset = 0;              // samples to skip at the start (latency)
    int64 startSample = -1, numSamples = 0;
    bool overflow = false;
};

struct MidiTake
{
    struct Event { int64 sample; uint8 data[3]; uint8 size; };
    std::shared_ptr<Track> track;
    std::vector<Event> events;   // reserved up front: the audio thread never reallocates it
    int64 startSample = -1, endSample = 0;
    int64 offset = 0;            // latency to take off: played notes only (generated ones are on time)
    String generatedBy;          // the plug-in whose notes this is, when recorded on its own track
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

    // For the thread checks (CI's ThreadSanitizer job): LATHE_TEST_THREADS=n renders on n
    // threads and shares out every block, however small, so every test goes through the
    // parallel path.
    if (const auto forced = SystemStats::getEnvironmentVariable ("LATHE_TEST_THREADS", {}).getIntValue(); forced > 0)
    {
        requestedThreads = forced;
        parallelMinimumUs = -1.0;
    }
}

Engine::~Engine()
{
    stopTimer();
    if (recording)
        finishRecording();

    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.closeAudioDevice();

    renderPool.reset();
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
        // First run: pick the audio interface, meaning the device (ASIO on Windows, Core Audio
        // on the Mac) with the most inputs, if it has more than a laptop's built-in mic. Use all
        // its channels at a 128-sample buffer. Otherwise keep the system default.
        String bestType, bestName;
        int bestInputs = 2;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            if (type->getTypeName() != "ASIO" && type->getTypeName() != "CoreAudio")
                continue;
            type->scanForDevices();
            for (auto& name : type->getDeviceNames (true))
            {
                std::unique_ptr<AudioIODevice> probe (type->createDevice (name, name));
                const int inputs = probe != nullptr ? probe->getInputChannelNames().size() : 0;
                if (inputs > bestInputs)
                {
                    bestInputs = inputs;
                    bestType = type->getTypeName();
                    bestName = name;
                }
            }
        }

        if (bestName.isNotEmpty())
        {
            deviceManager.setCurrentAudioDeviceType (bestType, true);
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.inputDeviceName = setup.outputDeviceName = bestName;
            setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
            setup.inputChannels.setRange (0, 256, true);
            setup.outputChannels.setRange (0, 256, true);
            setup.bufferSize = 128;
            deviceManager.setAudioDeviceSetup (setup, true);
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

bool Engine::setDeviceRate (double rate)
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr || rate <= 0.0)
        return false;
    if (std::abs (device->getCurrentSampleRate() - rate) <= 0.5)
        return true;
    if (! device->getAvailableSampleRates().contains (rate))
        return false;
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = rate;
    // Close the device completely, then open it again at the new rate. Changing the rate
    // of a running device in place races on macOS when input and output are different
    // devices: JUCE's combined device restarts itself from a Core Audio notification
    // thread while the message thread is reopening it, and crashes.
    deviceManager.closeAudioDevice();
    deviceManager.setAudioDeviceSetup (setup, true);
    return deviceManager.getCurrentAudioDevice() != nullptr
           && std::abs (deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() - rate) <= 0.5;
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

    // The workers render to this device's deadline (and on macOS join its workgroup).
    if (auto wg = device->getWorkgroup(); ! (wg == poolWorkgroup))
    {
        poolWorkgroup = wg;
        poolWorkgroupChanged = true;
    }
    if (MessageManager::getInstanceWithoutCreating() != nullptr && MessageManager::getInstance()->isThisTheMessageThread())
        updatePool();
    else
        poolDirty = true;
    triggerAsyncUpdate();
}

void Engine::audioDeviceStopped()
{
    triggerAsyncUpdate();
}

void Engine::handleAsyncUpdate()
{
    if (poolDirty.exchange (false))
        updatePool();

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
    // Playback stopped by itself (setAutoStop): back to the insert marker, as Stop does.
    if (autoStopped.exchange (false) && edit != nullptr && ! playing)
    {
        notesOffRequest = true;
        seekTo (edit->insertMarker);
        edit->sendChangeMessage();
    }

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

    if (exactlyEqual (slot.preparedRate, sr) && slot.preparedBlock == bs)
        return;

    if (slot.preparedRate > 0)
        slot.instance->releaseResources();

    slot.instance->setRateAndBufferSizeDetails (sr, bs);
    slot.instance->prepareToPlay (sr, bs);
    slot.preparedRate = sr;
    slot.preparedBlock = bs;
}

std::shared_ptr<PluginSlot> Engine::makeSlot (const PluginDescription& saved, const MemoryBlock& state, String& error, bool separate)
{
    auto slot = std::make_shared<PluginSlot>();
    slot->description = saved;

    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    const int bs = deviceBlock.load() > 0 ? deviceBlock.load() : 512;

    if (auto resolved = host.resolve (saved); resolved && separate && ! devices::isBuiltIn (*resolved))
    {
        slot->separate = true;
        String why;
        if (auto remote = RemotePluginInstance::create (*resolved, sr, bs, state, why))
        {
            auto* r = remote.get();
            const auto name = resolved->name;
            r->onStopped = [this, name]
            {
                if (onStatus)
                    onStatus (name + " stopped working. It runs separately, so the rest of Lathe is fine. "
                              "Right-click it > Restart to bring it back.");
            };
            slot->description = *resolved;
            slot->instance = std::move (remote);
            slot->instance->setPlayHead (&playHead);
            prepareSlot (*slot);
            return slot;
        }
        // Not loaded inside Lathe instead: a plug-in someone runs separately may be one that
        // crashes. It stays as a missing plug-in (settings kept) until it starts or they turn
        // "Run separately" off.
        slot->savedState = state;
        error = resolved->name + " couldn't run separately: " + why;
        return slot;
    }

    if (auto resolved = host.resolve (saved))
    {
        String createError;
        slot->instance = host.create (*resolved, sr, bs, createError);
        if (error.isEmpty())
            error = createError;
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

std::shared_ptr<PluginSlot> Engine::remakeSlot (PluginSlot& slot, bool separate, String& error)
{
    MemoryBlock state;
    if (slot.instance != nullptr)
        slot.instance->getStateInformation (state);
    else
        state = slot.savedState;
    auto fresh = makeSlot (slot.description, state, error, separate);
    fresh->bypassed = slot.bypassed.load();
    return fresh;
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
        rt.scratch.setSize (2, bs);

        if (t->isMidi())
        {
            for (size_t i = 0; i < rt.plugins.size(); ++i)
            {
                if (rt.plugins[i].processor->producesMidi())
                {
                    rt.midiTapAfter = (int) i;
                    rt.generatorName = rt.plugins[i].slot->getName();
                }
                else if (rt.plugins[i].slot->isInstrument())
                {
                    break;   // the instrument: notes after it aren't the track's notes anymore
                }
            }
            rt.recordGenerated = rt.midiTapAfter >= 0;
        }

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

    // --- tracks taking another track as their input. Sources are processed first; a
    // loop (A from B, B from A) is cut where it closes.
    {
        std::map<int, size_t> indexOf;
        for (size_t i = 0; i < g->tracks.size(); ++i)
            indexOf[g->tracks[i].track->id] = i;
        for (size_t i = 0; i < g->tracks.size(); ++i)
        {
            auto& rt = g->tracks[i];
            if (! rt.track->holdsClips() || ! rt.track->input.isTrack())
                continue;
            if (auto it = indexOf.find (rt.track->input.track); it != indexOf.end() && it->second != i
                && (rt.track->isAudio() || g->tracks[it->second].track->isMidi()))
                rt.inputTrack = (int) it->second;
        }

        std::vector<int> state (g->tracks.size(), 0);   // 0 new, 1 visiting, 2 placed
        std::function<void (size_t)> place = [&] (size_t i)
        {
            if (state[i] != 0)
                return;
            state[i] = 1;
            auto& rt = g->tracks[i];
            if (rt.inputTrack >= 0)
            {
                if (state[(size_t) rt.inputTrack] == 1)
                    rt.inputTrack = -1;                  // closes a loop
                else
                    place ((size_t) rt.inputTrack);
            }
            state[i] = 2;
            g->order.push_back (i);
        };
        for (size_t i = 0; i < g->tracks.size(); ++i)
            place (i);

        for (size_t i = 0; i < g->tracks.size(); ++i)
            if (const int src = g->tracks[i].inputTrack; src >= 0)
            {
                auto& s = g->tracks[(size_t) src];
                s.receivers.push_back ((int) i);
                if (g->tracks[i].kind == TrackKind::midi)
                {
                    s.tapMidi = true;
                    s.midiTap.ensureSize (4096);
                }
                else if (s.tap.getNumSamples() == 0)
                {
                    s.tap.setSize (2, bs);
                    s.tap.clear();
                }
            }
    }

    // --- dependency levels for parallel rendering (sources come first in g->order)
    {
        std::vector<int> level (g->tracks.size(), 0);
        for (auto i : g->order)
        {
            const int src = g->tracks[i].inputTrack;
            level[i] = src >= 0 ? level[(size_t) src] + 1 : 0;
            if ((size_t) level[i] >= g->levels.size())
                g->levels.resize ((size_t) level[i] + 1);
            g->levels[(size_t) level[i]].push_back (i);
        }
        for (size_t b = 0; b < g->buses.size(); ++b)
            g->busTasks.push_back (b);
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

    // --- cue mixes
    g->click.setSize (1, bs);
    g->cues.resize (edit->cues.size());
    for (size_t c = 0; c < edit->cues.size(); ++c)
    {
        auto& rc = g->cues[c];
        rc.cue = edit->cues[c];
        rc.trackSum.setSize (2, bs);
        rc.sum.setSize (2, bs);
        rc.trackDelay.prepare (g->busLatency);

        // A track routed into a return is heard through that return, not twice.
        for (size_t i = 0; i < g->tracks.size(); ++i)
            rc.trackLevels.push_back (g->tracks[i].outKind == OutputRoute::Kind::bus ? nullptr
                                                                                       : edit->cueLevel (*rc.cue, *edit->tracks[i]));
        for (size_t i = 0; i < g->buses.size(); ++i)
            rc.busLevels.push_back (edit->cueLevel (*rc.cue, *edit->buses[i]));
    }

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
                if (p->instance != nullptr && p->preparedRate > 0 && (! exactlyEqual (p->preparedRate, sr) || p->preparedBlock != bs))
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
    clearAutoStop();
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
    clearAutoStop();   // a seek means someone else took over the transport
}

void Engine::setProcessingThreads (int threads)
{
    requestedThreads = jlimit (0, 64, threads);
    if (MessageManager::getInstanceWithoutCreating() == nullptr || MessageManager::getInstance()->isThisTheMessageThread())
        updatePool();
    else
        poolDirty = true;
}

int Engine::autoProcessingThreads()
{
    int cores = SystemStats::getNumPhysicalCpus();
   #if JUCE_MAC
    // Apple silicon: only the performance cores (hw.perflevel0); efficiency cores would set the pace.
    int perf = 0;
    size_t size = sizeof (perf);
    if (sysctlbyname ("hw.perflevel0.physicalcpu", &perf, &size, nullptr, 0) == 0 && perf > 0)
        cores = perf;
   #endif
    return jlimit (1, 16, cores);
}

void Engine::setAutoStop (double stopAtBeats, int loopPasses)
{
    if (edit == nullptr)
        return;
    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    stopAtSamples = stopAtBeats < 0.0 ? (int64) -1 : (int64) std::llround (stopAtBeats * edit->samplesPerBeat (sr));
    loopPassesLeft = jmax (0, loopPasses);
}

double Engine::getAutoStopBeats() const
{
    const auto s = stopAtSamples.load();
    if (s < 0 || edit == nullptr)
        return -1.0;
    const double sr = deviceRate.load() > 0 ? deviceRate.load() : 48000.0;
    return (double) s / edit->samplesPerBeat (sr);
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

    clearAutoStop();

    if (recording || isCountingIn())
    {
        const bool wasCounting = isCountingIn();
        countInLeft = 0;
        finishRecording();
        if (wasCounting)
            return;   // stopped during the count-in: nothing was playing
    }

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
    if (recording || isCountingIn())
    {
        countInLeft = 0;     // (pressed again during the count-in: cancel it)
        finishRecording();
    }
    else
        startRecording();
}

int Engine::getCountInBeatsLeft() const
{
    const auto left = countInLeft.load();
    if (left <= 0 || countInSamplesPerClick <= 0.0)
        return 0;
    return (int) std::ceil ((double) left / countInSamplesPerClick - 1.0e-6);
}

void Engine::startRecording()
{
    if (edit == nullptr || recording || isCountingIn())
        return;

    clearAutoStop();   // recording runs until you stop it

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
            auto take = std::make_unique<RecordTake>();
            take->track = t;

            if (t->input.isTrack())
            {
                // Another track's sound, taken inside Lathe: no interface latency. Line it up
                // the way a live MIDI performance is (you play to what you hear), plus the
                // source's own plug-in latency.
                const RenderTrack* src = nullptr;
                for (auto& rt : graph->tracks)
                    if (rt.track->id == t->input.track)
                        src = &rt;
                if (src == nullptr)
                {
                    status (t->name + " takes its input from a track that isn't there anymore, so it won't record.");
                    continue;
                }
                take->stereo = true;
                take->offset = session->midiOffset + src->latency;
            }
            else
            {
                if (t->input.channel < 0 || t->input.channel >= numDeviceInputs)
                {
                    status (t->name + " has no input selected, so it won't record.");
                    continue;
                }
                take->channel = t->input.channel;
                take->stereo = t->input.stereo && t->input.channel + 1 < numDeviceInputs;
                take->offset = session->audioOffset;
            }

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
            take->offset = session->midiOffset;   // played: you play to what you hear
            for (auto& rt : graph->tracks)
                if (rt.track == t)
                {
                    if (rt.recordGenerated)
                    {
                        take->offset = 0;          // generated in step with the playhead
                        take->generatedBy = rt.generatorName;
                    }
                    else if (rt.inputTrack >= 0 && graph->tracks[(size_t) rt.inputTrack].recordGenerated)
                    {
                        take->offset = 0;
                    }
                }
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
        recordStartBeats = playing ? getPlayheadBeats() : edit->insertMarker;

        // From a stop, with count-in on: click first. The audio thread starts playing and
        // recording on the downbeat, in the same block, so the take lines up exactly.
        const int bars = countInBars.load();
        if (! playing && bars > 0 && graph != nullptr && graph->samplesPerClick > 0.0)
        {
            countInSamplesPerClick = graph->samplesPerClick;
            countInTotal = (int64) std::llround (graph->samplesPerClick * jmax (1, graph->clicksPerBar) * bars);
            seekTo (edit->insertMarker);
            countInLeft = countInTotal;
            return;
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
    String generatedNote;

    for (auto& take : session->audio)
    {
        take->writer.reset();   // flushes to disk

        const int64 usable = take->numSamples - take->offset;
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
        c.sourceOffset = take->offset;
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

        auto toBeat = [&] (int64 s) { return jlimit (0.0, length, (double) (s - take->offset - clipStart) / spb); };

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
        if (take->generatedBy.isNotEmpty())
            generatedNote = "Recorded the notes " + take->generatedBy + " played on \"" + take->track->name + "\". Bypass "
                            + take->generatedBy + " now, or it plays along with the new clip.";
    }

    if (generatedNote.isNotEmpty())
        status (generatedNote);
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
    if (! exactlyEqual (g.sampleRate, deviceRate.load()) || g.maxBlock <= 0)
        return;

    // Measuring: time the whole block (what the CPU meter shows), after the warm-up.
    struct BlockTimer
    {
        Engine& e;
        int64 start = 0;
        bool on = false;
        explicit BlockTimer (Engine& en) : e (en)
        {
            if (e.measuring.load (std::memory_order_acquire))
            {
                if (e.measureSkip.load (std::memory_order_relaxed) > 0)
                    e.measureSkip.fetch_sub (1, std::memory_order_relaxed);
                else
                {
                    on = true;
                    e.measureWriting.store (true, std::memory_order_relaxed);
                    start = Time::getHighResolutionTicks();
                }
            }
            e.measureThisBlock = on;
        }
        ~BlockTimer()
        {
            e.measureThisBlock = false;
            if (! on)
                return;
            const int i = e.measureCount.load (std::memory_order_relaxed);
            if (i < (int) e.measureBlocks.size())
            {
                e.measureBlocks[(size_t) i] = (float) ((double) (Time::getHighResolutionTicks() - start)
                                                       * 1.0e6 / (double) Time::getHighResolutionTicksPerSecond());
                e.measureCount.store (i + 1, std::memory_order_release);
            }
            e.measureTrackBlocks.fetch_add (1, std::memory_order_relaxed);
            e.measureWriting.store (false, std::memory_order_release);
        }
    } blockTimer (*this);

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

    // Notes to hear (the piano roll), for this block.
    previewCount = 0;
    if (const int ready = previewFifo.getNumReady(); ready > 0)
    {
        const auto scope = previewFifo.read (jmin (ready, (int) previewBlock.size()));
        scope.forEach ([this] (int index) { previewBlock[(size_t) previewCount++] = previewQueue[(size_t) index]; });
    }

    bool notesOff = notesOffRequest.exchange (false);
    int64 pos = positionSamples.load();
    if (const auto s = seekRequest.exchange (-1); s >= 0)
    {
        pos = s;
        notesOff = true;
    }

    bool rolling = playing.load();
    bool rec = recording.load();

    playHead.playing = rolling;
    playHead.recording = rec;

    int done = 0;

    // Count-in: click (with the song stopped) until the downbeat, then roll and record.
    if (int64 left = countInLeft.load(); left > 0 && ! rolling)
    {
        while (done < n && left > 0)
        {
            const int len = (int) jmin ((int64) (n - done), (int64) g.maxBlock, left);
            countInClickPos = countInTotal - left;
            playHead.position = pos;
            renderChunk (g, pos, len, done, false, inDev, numDeviceInputs, outDev, numDeviceOutputs, true, notesOff);
            countInClickPos = -1;
            notesOff = false;
            done += len;
            const int64 before = left;
            left -= len;
            // Hand the new count back, unless Stop cancelled it meanwhile.
            int64 expected = before;
            if (! countInLeft.compare_exchange_strong (expected, left))
            {
                left = 0;
                done = n;
                break;
            }
            if (left == 0)
            {
                recording = true;
                playing = true;
                rolling = rec = true;
                playHead.playing = playHead.recording = true;
            }
        }
    }

    // Auto-stop (setAutoStop): never while recording.
    int64 stopAt = rec ? (int64) -1 : stopAtSamples.load();
    auto stopHere = [&]
    {
        playing = false;
        rolling = false;
        playHead.playing = false;
        notesOff = true;
        stopAt = -1;
        stopAtSamples = -1;
        loopPassesLeft = 0;
        autoStopped = true;
    };

    while (done < n)
    {
        if (rolling && stopAt >= 0 && pos >= stopAt)
            stopHere();

        int len = jmin (n - done, g.maxBlock);
        if (rolling && g.loop && ! rec && pos < g.loopEnd)
            len = (int) jmin ((int64) len, g.loopEnd - pos);
        if (rolling && stopAt >= 0)
            len = (int) jmin ((int64) len, stopAt - pos);

        playHead.position = pos;
        renderChunk (g, pos, len, done, rolling, inDev, numDeviceInputs, outDev, numDeviceOutputs, true, notesOff);
        notesOff = false;
        done += len;

        if (rolling)
        {
            pos += len;
            if (g.loop && ! rec && pos == g.loopEnd)
            {
                int passes = loopPassesLeft.load();
                if (passes == 1)
                    stopHere();   // the last pass: stop at the loop end
                else
                {
                    if (passes > 1)
                        loopPassesLeft.compare_exchange_strong (passes, passes - 1);
                    pos = g.loopStart;
                    notesOff = true;
                }
            }
        }
    }

    positionSamples = pos;

    // The Compare window playing: it replaces what goes out of the master's outputs.
    if (auto* outside = outsideLive.load (std::memory_order_acquire); outside != nullptr && outside->isPlaying() && ! playing.load())
    {
        for (int d = 0; d < numDeviceOutputs; ++d)
            if (outDev[d] != nullptr)
                FloatVectorOperations::clear (outDev[d], n);
        for (int at = 0; at < n; at += kPreviewBlock)
        {
            const int len = jmin (kPreviewBlock, n - at);
            outside->render (outsideScratch, len);
            addToOutput (outsideScratch, len, at, g.master.outChannel, true, outDev, numDeviceOutputs);
        }
    }
}

void Engine::setOutsidePlayer (std::shared_ptr<OutsidePlayer> p)
{
    std::shared_ptr<OutsidePlayer> old;
    {
        const ScopedLock sl (graphLock);   // the audio callback holds it while it runs
        old = std::move (outsideOwner);
        outsideOwner = std::move (p);
        outsideLive.store (outsideOwner.get(), std::memory_order_release);
    }
    // `old` goes here, outside the lock.
}

namespace
{
/** What one block's parallel tasks need (lives on the rendering thread's stack). */
struct BlockJob
{
    Engine* engine;
    RenderGraph* g;
    const std::vector<size_t>* tasks;
    int64 pos;
    int len, offset, numIn;
    bool rolling, realtime, notesOff;
    const float* const* inDev;
};

inline double ticksToUs (int64 ticks) noexcept
{
    static const double scale = 1.0e6 / (double) Time::getHighResolutionTicksPerSecond();
    return (double) ticks * scale;
}

/** Heaviest first, so the long tasks start early and the short ones fill the gaps. */
void sortByCost (std::vector<size_t>& tasks, const std::vector<RenderTrack>& tracks) noexcept
{
    for (size_t i = 1; i < tasks.size(); ++i)   // insertion sort: no allocation, few items
        for (size_t j = i; j > 0 && tracks[tasks[j]].costUs > tracks[tasks[j - 1]].costUs; --j)
            std::swap (tasks[j], tasks[j - 1]);
}

inline void noteCost (RenderTrack& rt, int64 start, bool measure) noexcept
{
    const auto us = (float) ticksToUs (Time::getHighResolutionTicks() - start);
    rt.costUs = rt.costUs <= 0.0f ? us : 0.9f * rt.costUs + 0.1f * us;
    if (measure)   // one thread renders a track in a given block, so a plain add is enough
    {
        auto& m = rt.track->measuredUs;
        m.store (m.load (std::memory_order_relaxed) + us, std::memory_order_relaxed);
    }
}
}

void Engine::renderTrackTimed (RenderGraph& g, RenderTrack& rt, int64 pos, int len, int offset, bool rolling,
                               const float* const* inDev, int numIn, bool realtime, bool notesOff)
{
    const auto start = Time::getHighResolutionTicks();
    renderTrack (g, rt, pos, len, offset, rolling, inDev, numIn, realtime, notesOff);
    noteCost (rt, start, realtime && measureThisBlock.load (std::memory_order_relaxed));
}

void Engine::renderBusTimed (RenderGraph& g, RenderTrack& rt, int len)
{
    const auto start = Time::getHighResolutionTicks();
    renderBus (g, rt, len);
    noteCost (rt, start, measureThisBlock.load (std::memory_order_relaxed));
}

bool Engine::worthSharing (const std::vector<RenderTrack>& tracks) const noexcept
{
    if (renderPool == nullptr || renderPool->getNumWorkers() == 0 || tracks.size() < 2)
        return false;
    // Waking the workers costs a few tens of microseconds: only worth it for real work.
    double total = 0.0;
    for (auto& rt : tracks)
        total += rt.costUs;
    return total >= parallelMinimumUs.load (std::memory_order_relaxed);
}

void Engine::renderTracks (RenderGraph& g, int64 pos, int len, int offset, bool rolling,
                           const float* const* inDev, int numIn, bool realtime, bool notesOff)
{
    if (! worthSharing (g.tracks))
    {
        for (auto i : g.order)   // sources before the tracks that take them as their input
            renderTrackTimed (g, g.tracks[i], pos, len, offset, rolling, inDev, numIn, realtime, notesOff);
        return;
    }

    BlockJob job { this, &g, nullptr, pos, len, offset, numIn, rolling, realtime, notesOff, inDev };
    for (auto& level : g.levels)   // a level only starts once the one it listens to is done
    {
        if (level.size() == 1)
        {
            renderTrackTimed (g, g.tracks[level[0]], pos, len, offset, rolling, inDev, numIn, realtime, notesOff);
            continue;
        }
        sortByCost (level, g.tracks);
        job.tasks = &level;
        renderPool->run ((int) level.size(), [] (void* context, int i)
        {
            auto& j = *static_cast<BlockJob*> (context);
            j.engine->renderTrackTimed (*j.g, j.g->tracks[(*j.tasks)[(size_t) i]], j.pos, j.len, j.offset, j.rolling,
                                        j.inDev, j.numIn, j.realtime, j.notesOff);
        }, &job);
    }
}

void Engine::renderBuses (RenderGraph& g, int len)
{
    if (! worthSharing (g.buses))
    {
        for (auto& rt : g.buses)
            renderBusTimed (g, rt, len);
        return;
    }

    BlockJob job { this, &g, &g.busTasks, 0, len, 0, 0, false, false, false, nullptr };
    sortByCost (g.busTasks, g.buses);
    renderPool->run ((int) g.busTasks.size(), [] (void* context, int i)
    {
        auto& j = *static_cast<BlockJob*> (context);
        j.engine->renderBusTimed (*j.g, j.g->buses[(*j.tasks)[(size_t) i]], j.len);
    }, &job);
}

//==============================================================================
void Engine::previewNote (int trackId, int note, float velocity)
{
    PreviewEvent e;
    e.trackId = trackId;
    e.note = (uint8) jlimit (0, 127, note);
    e.velocity = velocity <= 0.0f ? (uint8) 0 : (uint8) jlimit (1, 127, roundToInt (velocity * 127.0f));
    const auto scope = previewFifo.write (1);
    scope.forEach ([this, &e] (int index) { previewQueue[(size_t) index] = e; });
}

//==============================================================================
void Engine::beginMeasurement (int skipBlocks)
{
    measuring = false;
    while (measureWriting.load (std::memory_order_acquire))   // a block still being written down
        Thread::yield();
    if (edit != nullptr)
    {
        for (auto* list : { &edit->tracks, &edit->buses })
            for (auto& t : *list)
                t->measuredUs = 0.0;
    }
    const int block = jmax (16, deviceBlock.load());
    const double rate = jmax (8000.0, deviceRate.load());
    measureBlocks.assign ((size_t) (rate * 120.0 / block) + 16, 0.0f);   // room for two minutes
    measureCount = 0;
    measureTrackBlocks = 0;
    measureSkip = jmax (0, skipBlocks);
    measuring.store (true, std::memory_order_release);
}

Engine::Measured Engine::endMeasurement()
{
    measuring = false;
    Measured m;
    const int count = measureCount.load (std::memory_order_acquire);
    m.blockUs.assign (measureBlocks.begin(), measureBlocks.begin() + jmin (count, (int) measureBlocks.size()));
    m.blockSize = deviceBlock.load();
    m.sampleRate = deviceRate.load();
    m.threads = getActiveProcessingThreads();
    m.trackBlocks = measureTrackBlocks.load();
    return m;
}

//==============================================================================
int Engine::getActiveProcessingThreads() const noexcept
{
    return 1 + poolWorkers;
}

void Engine::updatePool()
{
    const int wanted = requestedThreads.load() == 0 ? autoProcessingThreads() : requestedThreads.load();
    const int workers = jmax (0, wanted - 1);
    const double sr = deviceRate.load();
    const int bs = deviceBlock.load();
    if (workers == poolWorkers && poolRate == sr && poolBlock == bs && ! poolWorkgroupChanged)
        return;

    std::unique_ptr<RenderPool> fresh = workers > 0 ? std::make_unique<RenderPool> (workers, poolWorkgroup, sr, bs) : nullptr;
    {
        const ScopedLock sl (graphLock);
        if (exporting)
        {
            poolDirty = true;   // the export thread is using the pool: swap after it's done
            return;             // (fresh is discarded outside the lock)
        }
        std::swap (renderPool, fresh);
        poolWorkers = workers;
        poolRate = sr;
        poolBlock = bs;
        poolWorkgroupChanged = false;
    }
    // the old pool's threads stop here, outside the lock
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
    if (realtime)
        for (auto& c : g.cues)
        {
            c.trackSum.clear (0, len);
            c.sum.clear (0, len);
        }

    renderTracks (g, pos, len, offset, rolling, inDev, numIn, realtime, notesOff);
    for (auto i : g.order)
        mixTrack (g, g.tracks[i], len, offset, outDev, numOut, realtime, anySoloTrack);

    if (realtime)
        for (auto& c : g.cues)
        {
            c.trackDelay.process (c.trackSum, len);
            for (int ch = 0; ch < 2; ++ch)
                c.sum.addFrom (ch, 0, c.trackSum, ch, 0, len);
        }

    renderBuses (g, len);
    for (auto& rt : g.buses)
        mixBus (g, rt, len, offset, outDev, numOut, anySoloBus, realtime);

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
    m.track->rtEq.process (m.track->eq, m.buffer, len, g.sampleRate);
    applyFader (m, len, m.track->mute.load());
    addToOutput (m.buffer, len, offset, m.outChannel, true, outDev, numOut);

    if (! realtime)
        return;

    const bool countingIn = countInClickPos >= 0;
    const bool click = countingIn || (rolling && metronomeOn.load());
    if (click)
    {
        renderMetronome (g, countingIn ? countInClickPos : pos, len);
        if (edit == nullptr || edit->clickInMain.load())
        {
            const auto* c = g.click.getReadPointer (0);
            for (int ch = m.outChannel; ch < m.outChannel + 2; ++ch)
                if (ch >= 0 && ch < numOut && outDev[ch] != nullptr)
                    FloatVectorOperations::add (outDev[ch] + offset, c, len);
        }
    }

    // cue mixes
    for (auto& rc : g.cues)
    {
        auto& cue = *rc.cue;
        if (click)
        {
            const float clickGain = dbToGain (cue.clickDb.load());
            if (clickGain > 0.0f)
                for (int ch = 0; ch < 2; ++ch)
                    rc.sum.addFrom (ch, 0, g.click, 0, 0, len, clickGain);
        }

        const float target = cue.mute.load() ? 0.0f : dbToGain (cue.volumeDb.load());
        if (! cue.rtGainValid)
        {
            cue.rtGain = target;
            cue.rtGainValid = true;
        }
        rc.sum.applyGainRamp (0, len, cue.rtGain, target);
        cue.rtGain = target;

        const float peakL = rc.sum.getMagnitude (0, 0, len);
        const float peakR = rc.sum.getMagnitude (1, 0, len);
        if (peakL > cue.meterL.load()) cue.meterL = peakL;
        if (peakR > cue.meterR.load()) cue.meterR = peakR;

        addToOutput (rc.sum, len, offset, cue.channel.load(), true, outDev, numOut);
    }
}

namespace
{
void panGains (float gain, float pan, float& l, float& r) noexcept
{
    const float theta = (jlimit (-1.0f, 1.0f, pan) + 1.0f) * MathConstants<float>::pi * 0.25f;
    l = gain * jmin (1.0f, MathConstants<float>::sqrt2 * std::cos (theta));
    r = gain * jmin (1.0f, MathConstants<float>::sqrt2 * std::sin (theta));
}

/** Adds a channel to a cue mix at the channel's own cue level and pan. */
void mixIntoCue (AudioBuffer<float>& dest, const AudioBuffer<float>& src, int len, CueLevel& l) noexcept
{
    float targetL, targetR;
    panGains (l.mute.load() ? 0.0f : dbToGain (l.levelDb.load()), l.pan.load(), targetL, targetR);
    if (! l.rtGainValid)
    {
        l.rtGainL = targetL;
        l.rtGainR = targetR;
        l.rtGainValid = true;
    }

    if (l.rtGainL > 0.0f || targetL > 0.0f)
        dest.addFromWithRamp (0, 0, src.getReadPointer (0), len, l.rtGainL, targetL);
    if (l.rtGainR > 0.0f || targetR > 0.0f)
        dest.addFromWithRamp (1, 0, src.getReadPointer (1), len, l.rtGainR, targetR);
    l.rtGainL = targetL;
    l.rtGainR = targetR;
}
} // namespace

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

// A track's block is two halves. renderTrack makes its sound (clips, input, plug-ins, EQ) and
// writes only to the track itself, plus its own tap and record take, so tracks can render on
// different threads at once. mixTrack then adds it into the shared places (cue mixes, sends,
// buses, master, hardware) and has to run in g.order, one track at a time: always summing in
// the same order keeps the output identical to the bit, however the rendering was shared out.
void Engine::renderTrack (RenderGraph& g, RenderTrack& rt, int64 pos, int len, int offset, bool rolling,
                          const float* const* inDev, int numIn, bool realtime, bool notesOff)
{
    auto& t = *rt.track;
    auto& buf = rt.buffer;

    for (int ch = 0; ch < rt.numChannels; ++ch)
        buf.clear (ch, 0, len);
    rt.midi.clear();

    const bool recordingHere = realtime && rolling && recording.load() && (rt.take != nullptr || rt.midiTake != nullptr);
    const int mon = t.monitor.load();
    bool monitorInput = realtime && (mon == (int) Monitor::in || (mon == (int) Monitor::automatic && t.arm.load()));
    // Feeding an armed track (an audio track taking this one as its input): play live, as if
    // armed, so you can perform into the recording without also recording here.
    // While a receiving track monitors this one you hear it there, so it's silent here
    // (otherwise it would play twice).
    bool heardElsewhere = false;
    if (realtime)
        for (int r : rt.receivers)
        {
            auto& rcv = *g.tracks[(size_t) r].track;
            const int rmon = rcv.monitor.load();
            if (rcv.isAudio() && (rmon == (int) Monitor::in || (rmon == (int) Monitor::automatic && rcv.arm.load())))
                heardElsewhere = true;
            if (rcv.arm.load() || rmon == (int) Monitor::in)
                monitorInput = monitorInput || mon == (int) Monitor::automatic;
        }

    if (rt.kind == TrackKind::audio)
    {
        if (rolling && ! recordingHere)
            renderClips (g, rt, pos, len);

        if (realtime && (monitorInput || recordingHere) && (rt.inputChannel >= 0 || rt.inputTrack >= 0))
        {
            // From another track (already processed this block), or from the interface.
            const float* left = nullptr;
            const float* right = nullptr;
            if (rt.inputTrack >= 0)
            {
                const auto& src = g.tracks[(size_t) rt.inputTrack].tap;
                left = src.getReadPointer (0);
                right = src.getReadPointer (1);
            }
            else
            {
                left = rt.inputChannel < numIn && inDev[rt.inputChannel] != nullptr ? inDev[rt.inputChannel] + offset : nullptr;
                right = rt.inputStereo ? (rt.inputChannel + 1 < numIn && inDev[rt.inputChannel + 1] != nullptr ? inDev[rt.inputChannel + 1] + offset : nullptr)
                                       : left;
            }

            if (recordingHere && rt.take != nullptr && rt.take->writer != nullptr)
            {
                auto* take = rt.take;
                if (take->startSample < 0)
                    take->startSample = pos;

                const float* data[2] = { left != nullptr ? left : zeros.get(),
                                         right != nullptr ? right : zeros.get() };
                if (! take->writer->write (data, len))
                    take->overflow = true;
                take->numSamples += len;
            }

            if (monitorInput)
            {
                if (left != nullptr)  buf.addFrom (0, 0, left, len);
                if (right != nullptr) buf.addFrom (1, 0, right, len);
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

        // Notes the piano roll wants heard: straight to the instrument, never recorded.
        if (realtime && offset == 0)
            for (int i = 0; i < previewCount; ++i)
                if (const auto& e = previewBlock[(size_t) i]; e.trackId == rt.track->id)
                    rt.midi.addEvent (e.velocity > 0 ? MidiMessage::noteOn (ch, e.note, e.velocity)
                                                     : MidiMessage::noteOff (ch, e.note), 0);

        if (rolling && ! recordingHere)
        {
            auto& ev = rt.midiEvents;
            auto it = std::lower_bound (ev.begin(), ev.end(), pos, [] (const RenderMidi& e, int64 p) { return e.sample < p; });
            for (; it != ev.end() && it->sample < pos + len; ++it)
                rt.midi.addEvent (it->data, it->size, (int) (it->sample - pos));
        }

        auto* take = recordingHere ? rt.midiTake : nullptr;
        if (take != nullptr)
        {
            if (take->startSample < 0)
                take->startSample = pos;
            take->endSample = pos + len;
        }
        // Played notes are recorded as they come in; generated ones where the generator
        // hands them on (after the plug-ins, below).
        auto* inputTake = rt.recordGenerated ? nullptr : take;
        auto addIncoming = [&] (const uint8* src, int size, int s)
        {
            uint8 data[3] {};
            std::memcpy (data, src, (size_t) size);
            if (data[0] >= 0x80 && data[0] < 0xf0)
                data[0] = (uint8) ((data[0] & 0xf0) | (ch - 1));
            rt.midi.addEvent (data, size, s);
            if (inputTake != nullptr && inputTake->events.size() < inputTake->events.capacity())
                inputTake->events.push_back ({ pos + s, { data[0], data[1], data[2] }, (uint8) size });
        };

        if (realtime && (monitorInput || recordingHere))
        {
            if (rt.inputTrack >= 0)
            {
                // Another MIDI track's notes (processed earlier this block).
                for (const auto meta : g.tracks[(size_t) rt.inputTrack].midiTap)
                    if (meta.numBytes >= 1 && meta.numBytes <= 3 && meta.samplePosition >= 0 && meta.samplePosition < len)
                        addIncoming (meta.data, meta.numBytes, meta.samplePosition);
            }
            else if (rt.midiInput != -2)
            {
                for (size_t i = 0; i < liveMidi.size(); ++i)
                {
                    if (rt.midiInput >= 0 && (size_t) rt.midiInput != i)
                        continue;
                    for (const auto meta : liveMidi[i])
                        if (meta.samplePosition >= offset && meta.samplePosition < offset + len && meta.numBytes >= 1 && meta.numBytes <= 3)
                            addIncoming (meta.data, meta.numBytes, meta.samplePosition - offset);
                }
            }
        }

        if (realtime && rt.midiOut != nullptr && ! rt.midi.isEmpty())
            rt.midiOut->sendBlockOfMessages (rt.midi,
                                             Time::getMillisecondCounterHiRes() + 1000.0 * (offset + outputLatency) / g.sampleRate,
                                             g.sampleRate);
    }

    // The notes at the tap: for MIDI tracks listening to this one, and for a recording of
    // what a generator plug-in plays.
    auto captureMidi = [&]
    {
        if (rt.kind != TrackKind::midi)
            return;
        if (rt.tapMidi)
        {
            rt.midiTap.clear();
            rt.midiTap.addEvents (rt.midi, 0, len, 0);
        }
        auto* take = recordingHere && rt.recordGenerated ? rt.midiTake : nullptr;
        if (take != nullptr)
            for (const auto meta : rt.midi)
                if (meta.numBytes >= 1 && meta.numBytes <= 3 && take->events.size() < take->events.capacity())
                {
                    MidiTake::Event e { pos + meta.samplePosition, { 0, 0, 0 }, (uint8) meta.numBytes };
                    std::memcpy (e.data, meta.data, (size_t) meta.numBytes);
                    take->events.push_back (e);
                }
    };
    if (rt.midiTapAfter < 0)
        captureMidi();

    // inserts
    for (size_t i = 0; i < rt.plugins.size(); ++i)
    {
        auto& p = rt.plugins[i];
        if (! p.slot->bypassed.load())
        {
            AudioBuffer<float> view (buf.getArrayOfWritePointers(), p.numChannels, len);
            p.processor->processBlock (view, rt.midi);
        }
        if ((int) i == rt.midiTapAfter)
            captureMidi();
    }

    t.rtEq.process (t.eq, buf, len, g.sampleRate);
    if (rt.tap.getNumSamples() >= len)   // an audio track takes this one's sound as its input
        for (int ch = 0; ch < 2; ++ch)
            rt.tap.copyFrom (ch, 0, buf, ch, 0, len);
    rt.align.process (buf, len);
    rt.heardElsewhere = heardElsewhere;
}

void Engine::mixTrack (RenderGraph& g, RenderTrack& rt, int len, int offset, float* const* outDev, int numOut,
                       bool realtime, bool anySolo)
{
    auto& t = *rt.track;
    auto& buf = rt.buffer;
    const bool heardElsewhere = rt.heardElsewhere;

    if (realtime && ! g.cues.empty())
    {
        const auto index = (size_t) (&rt - g.tracks.data());
        for (auto& c : g.cues)
            if (index < c.trackLevels.size() && c.trackLevels[index] != nullptr)
                mixIntoCue (c.trackSum, buf, len, *c.trackLevels[index]);
    }

    const bool groupMuted = rt.group != nullptr && rt.group->mute.load();
    const bool soloed = t.solo.load() || (rt.group != nullptr && rt.group->solo.load())
                        || (rt.inputTrack >= 0 && g.tracks[(size_t) rt.inputTrack].track->solo.load());   // solo the source: hear it here
    // Exporting some tracks: exactly those play, whatever mute and solo say.
    const bool silenced = (! realtime && ! exportOnly.empty()) ? exportOnly.count (t.id) == 0
                                                               : (t.mute.load() || groupMuted || (anySolo && ! soloed) || heardElsewhere);

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

// A bus's block, in the same two halves as a track's: renderBus (its plug-ins, on any thread,
// once every track has been mixed into it) and mixBus (into the cue mixes and the master, in
// order).
void Engine::renderBus (RenderGraph& g, RenderTrack& rt, int len)
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

    t.rtEq.process (t.eq, rt.buffer, len, g.sampleRate);
    rt.align.process (rt.buffer, len);
}

void Engine::mixBus (RenderGraph& g, RenderTrack& rt, int len, int offset, float* const* outDev, int numOut, bool anySolo, bool realtime)
{
    auto& t = *rt.track;

    if (realtime && ! g.cues.empty())
    {
        const auto index = (size_t) (&rt - g.buses.data());
        for (auto& c : g.cues)
            if (index < c.busLevels.size() && c.busLevels[index] != nullptr)
                mixIntoCue (c.sum, rt.buffer, len, *c.busLevels[index]);
    }

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
    float* tmp[2] = { rt.scratch.getWritePointer (0), rt.scratch.getWritePointer (1) };

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

void Engine::renderMetronome (RenderGraph& g, int64 pos, int len)
{
    auto* out = g.click.getWritePointer (0);
    FloatVectorOperations::clear (out, len);

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
            out[i] = s;
        }
    }
}

//==============================================================================
// Export
//==============================================================================
String Engine::exportMixName (const String& base, const Edit& edit, const std::set<int>& ids)
{
    StringArray names;
    for (auto& t : edit.tracks)
        if (ids.count (t->id) > 0 && t->holdsClips())
            names.add (t->name);

    if (names.isEmpty())
        return base;
    if (names.size() <= 3)
        return base + " - " + names.joinIntoString (" + ");
    return base + " (" + String (names.size()) + " tracks)";
}

String Engine::formatName (ExportSettings::Format f)
{
    using F = ExportSettings::Format;
    switch (f)
    {
        case F::aiff: return "aiff";
        case F::flac: return "flac";
        case F::mp3:  return "mp3";
        case F::ogg:  return "ogg";
        case F::wav:  break;
    }
    return "wav";
}

bool Engine::parseFormat (const String& name, ExportSettings::Format& out)
{
    using F = ExportSettings::Format;
    const auto n = name.trim().trimCharactersAtStart (".").toLowerCase();
    for (auto f : { F::wav, F::aiff, F::flac, F::mp3, F::ogg })
        if (n == formatName (f))
        {
            out = f;
            return true;
        }
    if (n == "aif")  { out = F::aiff; return true; }
    if (n == "wave") { out = F::wav;  return true; }
    if (n == "vorbis" || n == "ogg vorbis") { out = F::ogg; return true; }
    return false;
}

Result Engine::exportAudio (const ExportSettings& s, const std::function<bool (double)>& progress, StringArray& written)
{
    struct Output
    {
        File file;
        std::unique_ptr<AudioFormatWriter> writer;
        float peak = 0.0f;
        bool isStem = false;
    };
    std::vector<Output> outputs;
    double sr = 0.0;

    auto makeWriter = [&] (const File& f) -> std::unique_ptr<AudioFormatWriter>
    {
        f.deleteFile();
        std::unique_ptr<OutputStream> stream (f.createOutputStream());
        if (stream == nullptr)
            return {};
        using F = ExportSettings::Format;
        auto options = AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2);
        // Only WAV keeps 32-bit float; AIFF and FLAC top out at 24.
        const auto bits = s.format == F::wav ? s.bitDepth : jmin (24, s.bitDepth);
        switch (s.format)
        {
            case F::mp3:
                return makeMp3Writer (std::move (stream), sr, 2, s.kbps, f.getFileNameWithoutExtension());
            case F::ogg:
            {
                // JUCE's Vorbis qualities run 0-10 (64 to 500 kbps on average); these match the bit rates.
                const auto quality = s.kbps >= 320 ? 9 : s.kbps >= 256 ? 8 : s.kbps >= 224 ? 7
                                   : s.kbps >= 192 ? 6 : s.kbps >= 160 ? 5 : 4;
                OggVorbisAudioFormat ogg;
                return ogg.createWriterFor (stream, options.withBitsPerSample (32).withQualityOptionIndex (quality));
            }
            case F::flac:
            {
                FlacAudioFormat flac;
                return flac.createWriterFor (stream, options.withBitsPerSample (bits).withQualityOptionIndex (5));
            }
            case F::aiff:
            {
                AiffAudioFormat aiff;
                return aiff.createWriterFor (stream, options.withBitsPerSample (bits));
            }
            case F::wav:
                break;
        }
        WavAudioFormat wav;
        options = options.withBitsPerSample (bits);
        if (bits == 32)
            options = options.withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint);
        return wav.createWriterFor (stream, options);
    };
    const auto extension = "." + formatName (s.format);

    auto prepare = [&] (const std::vector<RenderTap>& taps, int64) -> Result
    {
        sr = getSampleRate();
        s.folder.createDirectory();
        // Stems are numbered by position in the whole song, so a track's file name never changes.
        std::map<int, int> number;
        int index = 1;
        for (auto& t : edit->tracks) number[t->id] = index++;
        for (auto& t : edit->buses)  number[t->id] = index++;
        for (auto& tap : taps)
        {
            Output o;
            o.isStem = ! tap.isMix;
            if (tap.isMix)
            {
                const auto name = s.trackIds.empty() ? s.baseName : exportMixName (s.baseName, *edit, s.trackIds);
                o.file = s.folder.getChildFile (File::createLegalFileName (name) + extension);
            }
            else
            {
                const auto name = String (number[tap.trackId]).paddedLeft ('0', 2) + " " + tap.name;
                o.file = s.folder.getChildFile (File::createLegalFileName (s.baseName + " - " + name) + extension);
            }
            // Never write over a file that's already there (an earlier export in a folder you
            // chose): "Song (2).wav" instead.
            {
                const auto wanted = o.file;
                for (int n = 2; o.file.exists(); ++n)
                    o.file = wanted.getSiblingFile (wanted.getFileNameWithoutExtension() + " (" + String (n) + ")" + wanted.getFileExtension());
            }
            o.writer = makeWriter (o.file);
            if (o.writer == nullptr)
                return Result::fail ("Couldn't write " + o.file.getFullPathName());
            outputs.push_back (std::move (o));
        }
        return Result::ok();
    };

    auto listen = [&] (int tap, const AudioBuffer<float>& buffer, int start, int count, int64)
    {
        auto& o = outputs[(size_t) tap];
        o.writer->writeFromAudioSampleBuffer (buffer, start, count);
        o.peak = jmax (o.peak, buffer.getMagnitude (start, count));
    };

    const auto result = renderOffline (s, progress, prepare, listen);

    for (auto& o : outputs)
    {
        o.writer.reset();
        // Silent stems (empty tracks, unused returns) aren't worth a file.
        if (result.failed() || (o.isStem && o.peak <= 0.0f))
            o.file.deleteFile();
        else
            written.add (o.file.getFullPathName());
    }
    return result;
}

Result Engine::renderOffline (const ExportSettings& s, const std::function<bool (double)>& progress,
                              const std::function<Result (const std::vector<RenderTap>&, int64)>& prepare,
                              const RenderListener& listen)
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
        if (rebuildAfterExport || poolDirty)
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

    const int64 start = (int64) std::llround (s.startBeat * g->samplesPerBeat);
    const int64 length = (int64) std::llround ((s.endBeat - s.startBeat) * g->samplesPerBeat) + (int64) (s.tailSeconds * g->sampleRate);
    const int mixLatency = g->trackLatency + g->busLatency + g->masterLatency;
    const int stemLatency = g->trackLatency + g->busLatency;
    const int64 total = length + mixLatency;

    struct Output
    {
        const AudioBuffer<float>* buffer;
        int latency;
    };
    std::vector<Output> outputs;
    std::vector<RenderTap> taps;

    // Limit what plays only if audio/MIDI tracks were picked. Just a return picked:
    // everything plays into it, and only its file is written.
    exportOnly.clear();
    for (auto& t : edit->tracks)
        if (s.trackIds.count (t->id) > 0)
            exportOnly = s.trackIds;

    if (s.mix)
    {
        outputs.push_back ({ &g->master.buffer, mixLatency });
        taps.push_back ({ "Mix", -1, true, false });
    }
    if (s.stems)
    {
        auto addStem = [&] (RenderTrack& rt, bool isReturn)
        {
            if (! s.trackIds.empty() && s.trackIds.count (rt.track->id) == 0)
                return;
            outputs.push_back ({ &rt.buffer, stemLatency });
            taps.push_back ({ rt.track->name, rt.track->id, false, isReturn });
        };
        for (auto& rt : g->tracks) addStem (rt, false);
        for (auto& rt : g->buses)  addStem (rt, true);
    }

    if (auto ready = prepare (taps, length); ready.failed())
    {
        exportOnly.clear();
        restore();
        return ready;
    }

    for (auto* p : processors)
    {
        p->setNonRealtime (true);
        p->reset();
    }
    // Faders start where they're set, not ramping from wherever the live mix last had them
    // (a render that begins with a ramp measures a peak that isn't in the song).
    for (auto& rt : g->tracks) rt.track->rtGainValid = false;
    for (auto& rt : g->buses)  rt.track->rtGainValid = false;
    g->master.track->rtGainValid = false;

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

        for (size_t i = 0; i < outputs.size(); ++i)
        {
            auto& o = outputs[i];
            // Skip the plug-in latency at the start so everything lines up with the timeline.
            const int64 from = jmax (rendered, (int64) o.latency);
            const int64 to = jmin (rendered + len, (int64) o.latency + length);
            if (to > from)
                listen ((int) i, *o.buffer, (int) (from - rendered), (int) (to - from), from - o.latency);
        }

        pos += len;
        rendered += len;

        if (! progress ((double) rendered / (double) total))
        {
            cancelled = true;
            break;
        }
    }

    for (auto* p : processors)
    {
        p->setNonRealtime (false);
        p->reset();
    }
    for (auto& rt : g->tracks) rt.align.clear(), rt.direct.clear();
    for (auto& rt : g->buses)  rt.align.clear();

    playHead.playing = false;
    exportOnly.clear();
    restore();

    return cancelled ? Result::fail ("Export cancelled.") : Result::ok();
}
} // namespace lathe
