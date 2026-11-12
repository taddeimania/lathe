#include "RemotePlugin.h"
#include "RemoteShared.h"

#if JUCE_INTEL
 #include <immintrin.h>
#endif
#include <thread>
#if ! JUCE_WINDOWS
 #include <unistd.h>
 #include <signal.h>
 #include <sys/wait.h>
 #include <cerrno>
#endif

namespace lathe
{
using remote::Shared;

namespace
{
inline void cpuRelax() noexcept
{
   #if JUCE_INTEL
    _mm_pause();
   #elif JUCE_ARM && (JUCE_GCC || JUCE_CLANG)
    asm volatile ("yield");
   #endif
}

MemoryBlock toBlock (const var& v)
{
    const auto text = JSON::toString (v, true);
    return MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8());
}

var fromBlock (const MemoryBlock& mb)
{
    return JSON::parse (String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize()));
}

var object (std::initializer_list<std::pair<const char*, var>> props)
{
    DynamicObject::Ptr o = new DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty (Identifier (k), v);
    return var (o.get());
}

std::atomic<int> nextConnection { 1 };
} // namespace

//==============================================================================
/** One running host: the control connection, the shared block and the semaphore. */
class RemotePluginInstance::Connection : private ChildProcessCoordinator,
                                          private Timer
{
public:
    Connection()
    {
        const int n = nextConnection++;
        const auto pid = String ((int) getpidPortable());
        name = "lathe-" + pid + "-" + String (n);
        shmFile = File::getSpecialLocation (File::tempDirectory).getChildFile ("lathe-host-" + pid + "-" + String (n) + ".shm");
    }

    ~Connection() override
    {
        stopTimer();
        if (shared != nullptr)
        {
            shared->quit = 1;
            wake.post();
        }
        killWorkerProcess();
       #if JUCE_WINDOWS
        if (process != nullptr) CloseHandle (process);
       #endif
        map.reset();
        wake.close();
        shmFile.deleteFile();
    }

    /** False once the host process has ended (any thread). */
    bool hostAlive()
    {
        const int id = pid.load();
        if (id <= 0)
            return true;   // not said who it is yet
       #if JUCE_WINDOWS
        if (process == nullptr)
            return true;
        DWORD code = 0;
        return ! (GetExitCodeProcess (process, &code) && code != STILL_ACTIVE);
       #else
        const ScopedLock sl (reapLock);
        if (reaped)
            return false;
        int status = 0;
        const auto r = waitpid ((pid_t) id, &status, WNOHANG);
        if (r == (pid_t) id || (r < 0 && errno == ECHILD && kill ((pid_t) id, 0) != 0))
            reaped = true;
        return ! reaped;
       #endif
    }

    /** Starts the host and loads the plug-in. The reply, or a void var and `error`. */
    var start (const PluginDescription& desc, double sr, int bs, const MemoryBlock& state, String& error)
    {
        const auto exe = RemotePluginInstance::hostExecutable();
        if (! exe.existsAsFile())
        {
            error = "LatheHost isn't next to Lathe, so plug-ins can't run separately. Reinstall Lathe.";
            return {};
        }
        // The shared block, as a file both processes map.
        shmFile.deleteFile();
        {
            FileOutputStream out (shmFile);
            if (! out.openedOk())
            {
                error = "Couldn't make the shared memory file for the plug-in.";
                return {};
            }
            HeapBlock<char> zeros ((size_t) 65536, true);
            for (size_t done = 0; done < sizeof (Shared); done += 65536)
                out.write (zeros, jmin ((size_t) 65536, sizeof (Shared) - done));
        }
        map = std::make_unique<MemoryMappedFile> (shmFile, MemoryMappedFile::readWrite, false);
        if (map->getData() == nullptr || map->getSize() < sizeof (Shared))
        {
            error = "Couldn't map the shared memory for the plug-in.";
            return {};
        }
        shared = new (map->getData()) Shared();
        shared->magic = remote::kMagic;
        if (! wake.create (name))
        {
            error = "Couldn't make the semaphore for the plug-in.";
            return {};
        }

        if (! launchWorkerProcess (exe, remote::commandLineId(), remote::pingTimeoutMs, 0))
        {
            error = "LatheHost didn't start.";
            return {};
        }
        startTimer (200);   // watch the process from here on
        auto reply = request (object ({ { "type", "load" },
                                        { "plugin", desc.createXml()->toString() },
                                        { "rate", sr }, { "block", bs },
                                        { "state", state.toBase64Encoding() },
                                        { "shm", shmFile.getFullPathName() },
                                        { "semaphore", name } }),
                              60000);
        if (! reply.isObject())
        {
            error = "LatheHost stopped while loading " + desc.name + ".";
            return {};
        }
        if (! (bool) reply["ok"])
        {
            error = reply["error"].toString();
            return {};
        }
        return reply;
    }

    /** Sends and waits for the answer (the message thread waits; the reply comes on the
        connection's thread). A void var if it doesn't come in time. */
    var request (var msg, int timeoutMs)
    {
        if (lost.load())
            return {};
        auto pending = std::make_shared<Pending>();
        int id;
        {
            const ScopedLock sl (lock);
            id = ++lastId;
            waiting[id] = pending;
        }
        msg.getDynamicObject()->setProperty ("id", id);
        if (! sendMessageToWorker (toBlock (msg)))
        {
            const ScopedLock sl (lock);
            waiting.erase (id);
            return {};
        }
        // Wait in short steps, so a host that dies meanwhile is noticed straight away.
        for (int waited = 0; waited < timeoutMs; waited += 50)
        {
            if (pending->done.wait (50))
                break;
            if (! hostAlive())
            {
                handleConnectionLost();
                break;
            }
        }
        const ScopedLock sl (lock);
        waiting.erase (id);
        return pending->reply;
    }

    void send (const var& msg)
    {
        if (! lost.load())
            sendMessageToWorker (toBlock (msg));
    }

    Shared* shared = nullptr;
    remote::Semaphore wake;
    std::atomic<bool> lost { false };
    String lastWords;   // what the host printed before it went (rarely anything)
    std::function<void (const var&)> onMessage;   // connection thread
    std::function<void()> onLost;                  // any thread

private:
    struct Pending
    {
        WaitableEvent done { true };
        var reply;
    };
    String name;
    File shmFile;
    std::atomic<int> pid { 0 };
   #if JUCE_WINDOWS
    HANDLE process = nullptr;
   #else
    CriticalSection reapLock;
    bool reaped = false;
   #endif

    void timerCallback() override
    {
        if (! lost.load() && ! hostAlive())
            handleConnectionLost();
    }
    std::unique_ptr<MemoryMappedFile> map;
    CriticalSection lock;
    std::map<int, std::shared_ptr<Pending>> waiting;
    int lastId = 0;

    static int getpidPortable()
    {
       #if JUCE_WINDOWS
        return (int) GetCurrentProcessId();
       #else
        return (int) getpid();
       #endif
    }

    void handleMessageFromWorker (const MemoryBlock& mb) override
    {
        const auto msg = fromBlock (mb);
        if (msg["type"].toString() == "hello")
        {
           #if JUCE_WINDOWS
            if (process == nullptr)
                process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) (int) msg["pid"]);
           #endif
            pid = (int) msg["pid"];
            return;
        }
        if (msg.hasProperty ("reply"))
        {
            const ScopedLock sl (lock);
            if (auto it = waiting.find ((int) msg["reply"]); it != waiting.end())
            {
                it->second->reply = msg;
                it->second->done.signal();
            }
            return;
        }
        if (onMessage)
            onMessage (msg);
    }

    void handleConnectionLost() override
    {
        if (lost.exchange (true))
            return;
        {
            const ScopedLock sl (lock);
            for (auto& [id, p] : waiting)
                p->done.signal();
        }
        if (onLost)
            onLost();
    }
};

//==============================================================================
/** A setting of the plug-in, as Lathe sees it: the value is kept here, changes are sent. */
class RemotePluginInstance::Parameter : public AudioPluginInstance::HostedParameter
{
public:
    Parameter (RemotePluginInstance& o, int i, const var& d) : owner (o), index (i)
    {
        update (d);
    }

    void update (const var& d)
    {
        name = d["name"].toString();
        label = d["label"].toString();
        defaultValue = (float) (double) d["default"];
        steps = (int) d.getProperty ("steps", AudioProcessor::getDefaultNumParameterSteps());
        discrete = (bool) d["discrete"];
        boolean = (bool) d["boolean"];
        automatable = (bool) d.getProperty ("automatable", true);
        strings.clear();
        if (auto* a = d["strings"].getArray())
            for (auto& s : *a)
                strings.add (s.toString());
        setFromHost ((float) (double) d["value"], d["text"].toString(), false);
    }

    /** A change from the plug-in's own window (or a load). */
    void setFromHost (float v, const String& t, bool tell)
    {
        value = v;
        {
            const ScopedLock sl (textLock);
            text = t;
        }
        if (tell)
            sendValueChangedMessageToListeners (v);
    }

    String getParameterID() const override     { return String (index); }
    float getValue() const override            { return value.load(); }
    void setValue (float v) override
    {
        value = v;
        owner.connection->send (object ({ { "type", "setParam" }, { "index", index }, { "value", v } }));
        // Lathe reads the text back (set_plugin_params, the snapshot): ask for it once it's set.
        if (MessageManager::existsAndIsCurrentThread())
            refreshText();
    }
    float getDefaultValue() const override     { return defaultValue; }
    String getName (int maxLength) const override   { return name.substring (0, maxLength); }
    String getLabel() const override           { return label; }
    int getNumSteps() const override           { return steps; }
    bool isDiscrete() const override           { return discrete; }
    bool isBoolean() const override            { return boolean; }
    bool isAutomatable() const override        { return automatable; }
    StringArray getAllValueStrings() const override   { return strings; }

    String getText (float v, int maxLength) const override
    {
        if (std::abs (v - value.load()) < 1.0e-7f)
        {
            const ScopedLock sl (textLock);
            return text.substring (0, maxLength);
        }
        if (MessageManager::existsAndIsCurrentThread() && ! owner.hasStopped())
        {
            const auto r = owner.connection->request (object ({ { "type", "paramText" }, { "index", index }, { "value", v } }), 1000);
            if (r.isObject())
                return r["text"].toString().substring (0, maxLength);
        }
        return String (v, 3);
    }

    float getValueForText (const String& t) const override
    {
        if (MessageManager::existsAndIsCurrentThread() && ! owner.hasStopped())
        {
            const auto r = owner.connection->request (object ({ { "type", "valueForText" }, { "index", index }, { "text", t } }), 1000);
            if (r.isObject())
                return (float) (double) r["value"];
        }
        return t.getFloatValue();
    }

    void refreshText()
    {
        const auto r = owner.connection->request (object ({ { "type", "paramText" }, { "index", index }, { "value", value.load() } }), 1000);
        if (r.isObject())
        {
            const ScopedLock sl (textLock);
            text = r["text"].toString();
        }
    }

private:
    RemotePluginInstance& owner;
    int index;
    String name, label;
    float defaultValue = 0.0f;
    int steps = 0x7fffffff;
    bool discrete = false, boolean = false, automatable = true;
    StringArray strings;
    std::atomic<float> value { 0.0f };
    CriticalSection textLock;
    String text;
};

//==============================================================================
AudioProcessor::BusesProperties RemotePluginInstance::busesFor (const var& loaded)
{
    BusesProperties b;
    const int ins = jlimit (0, remote::kMaxChannels, (int) loaded["inputs"]);
    const int outs = jlimit (0, remote::kMaxChannels, (int) loaded["outputs"]);
    if (ins > 0)
        b = b.withInput ("Input", ins == 2 ? AudioChannelSet::stereo() : AudioChannelSet::discreteChannels (ins), true);
    if (outs > 0)
        b = b.withOutput ("Output", outs == 2 ? AudioChannelSet::stereo() : AudioChannelSet::discreteChannels (outs), true);
    return b;
}

File RemotePluginInstance::hostExecutable()
{
    const auto dir = File::getSpecialLocation (File::currentExecutableFile).getParentDirectory();
   #if JUCE_WINDOWS
    return dir.getChildFile ("LatheHost.exe");
   #else
    return dir.getChildFile ("LatheHost");
   #endif
}

std::unique_ptr<RemotePluginInstance> RemotePluginInstance::create (const PluginDescription& desc, double sr, int bs,
                                                                    const MemoryBlock& state, String& error)
{
    auto connection = std::make_unique<Connection>();
    const auto loaded = connection->start (desc, sr, bs, state, error);
    if (! loaded.isObject())
        return {};
    std::unique_ptr<RemotePluginInstance> p (new RemotePluginInstance (busesFor (loaded), std::move (connection), loaded, desc));
    p->rate = sr;
    p->block = bs;
    p->lastState = state;
    return p;
}

RemotePluginInstance::RemotePluginInstance (const BusesProperties& buses, std::unique_ptr<Connection> c,
                                            const var& loaded, const PluginDescription& d)
    : AudioPluginInstance (buses), description (d)
{
    if (auto* list = loaded["params"].getArray())
        for (int i = 0; i < list->size(); ++i)
            addHostedParameter (std::make_unique<Parameter> (*this, i, list->getReference (i)));
    adopt (std::move (c), loaded);
}

void RemotePluginInstance::adopt (std::unique_ptr<Connection> c, const var& loaded)
{
    connection = std::move (c);
    info = loaded;
    lastSeq = connection->shared != nullptr ? connection->shared->requestSeq.load() : 0;
    setLatencySamples ((int) loaded["latency"]);

    std::weak_ptr<std::atomic<bool>> weak = alive;
    connection->onMessage = [this, weak] (const var& msg)
    {
        const auto type = msg["type"].toString();
        if (type == "param")
        {
            const int i = (int) msg["index"];
            const float v = (float) (double) msg["value"];
            const auto text = msg["text"].toString();
            MessageManager::callAsync ([this, weak, i, v, text]
            {
                auto a = weak.lock();
                if (a == nullptr || ! a->load())
                    return;
                if (auto* p = dynamic_cast<Parameter*> (getParameters()[i]))
                    p->setFromHost (v, text, true);
            });
        }
        else if (type == "latency")
        {
            const int samples = (int) msg["samples"];
            MessageManager::callAsync ([this, weak, samples]
            {
                if (auto a = weak.lock(); a != nullptr && a->load())
                    setLatencySamples (samples);
            });
        }
    };
    connection->onLost = [this, weak]
    {
        if (auto a = weak.lock(); a != nullptr && a->load())
            connectionLost();
    };
    stopped = false;
}

RemotePluginInstance::~RemotePluginInstance()
{
    alive->store (false);
    if (connection != nullptr)
    {
        connection->onLost = nullptr;
        connection->onMessage = nullptr;
        if (! connection->lost.load())
            connection->request (object ({ { "type", "quit" } }), 2000);
    }
    connection.reset();
}

void RemotePluginInstance::connectionLost()
{
    if (stopped.exchange (true))
        return;
    std::weak_ptr<std::atomic<bool>> weak = alive;
    MessageManager::callAsync ([this, weak]
    {
        if (auto a = weak.lock(); a != nullptr && a->load() && onStopped)
            onStopped();
    });
}

String RemotePluginInstance::whyStopped() const
{
    return description.name + " stopped working. It runs separately, so the rest of Lathe is fine; "
           "it's silent (an effect lets the sound through) until you restart it.";
}

bool RemotePluginInstance::restart (String& error)
{
    MemoryBlock state;
    {
        const ScopedLock sl (stateLock);
        state = lastState;
    }
    auto c = std::make_unique<Connection>();
    const auto loaded = c->start (description, rate, block, state, error);
    if (! loaded.isObject())
        return false;
    // The settings keep their objects (Lathe's windows hold them); only their values change.
    if (auto* list = loaded["params"].getArray())
        for (int i = 0; i < list->size() && i < getParameters().size(); ++i)
            if (auto* p = dynamic_cast<Parameter*> (getParameters()[i]))
                p->update (list->getReference (i));
    // Swap under the callback lock, so the audio thread never sees a half-made connection.
    {
        const ScopedLock sl (getCallbackLock());
        auto old = std::move (connection);
        adopt (std::move (c), loaded);
        if (old != nullptr)
        {
            old->onLost = nullptr;
            old->onMessage = nullptr;
        }
    }
    return true;
}

//==============================================================================
void RemotePluginInstance::fillInPluginDescription (PluginDescription& d) const   { d = description; }
const String RemotePluginInstance::getName() const   { return description.name; }
double RemotePluginInstance::getTailLengthSeconds() const   { return (double) info.getProperty ("tail", 0.0); }
bool RemotePluginInstance::acceptsMidi() const   { return (bool) info["acceptsMidi"]; }
bool RemotePluginInstance::producesMidi() const  { return (bool) info["producesMidi"]; }
bool RemotePluginInstance::hasEditor() const     { return (bool) info["hasEditor"]; }

void RemotePluginInstance::prepareToPlay (double sr, int bs)
{
    rate = sr;
    block = bs;
    if (! stopped.load())
        connection->request (object ({ { "type", "prepare" }, { "rate", sr }, { "block", bs } }), 20000);
}

void RemotePluginInstance::releaseResources()
{
    if (! stopped.load())
        connection->send (object ({ { "type", "release" } }));
}

void RemotePluginInstance::reset()
{
    if (! stopped.load())
        connection->send (object ({ { "type", "reset" } }));
}

void RemotePluginInstance::setNonRealtime (bool nonRealtime) noexcept
{
    AudioPluginInstance::setNonRealtime (nonRealtime);
    offline = nonRealtime;
    if (! stopped.load())
        connection->send (object ({ { "type", "nonRealtime" }, { "on", nonRealtime } }));
}

void RemotePluginInstance::getStateInformation (MemoryBlock& out)
{
    if (! stopped.load())
    {
        const auto r = connection->request (object ({ { "type", "getState" } }), 10000);
        if (r.isObject())
        {
            MemoryBlock state;
            state.fromBase64Encoding (r["state"].toString());
            const ScopedLock sl (stateLock);
            lastState = state;
        }
    }
    const ScopedLock sl (stateLock);
    out = lastState;   // after a crash: the last settings it had
}

void RemotePluginInstance::setStateInformation (const void* data, int size)
{
    MemoryBlock state (data, (size_t) jmax (0, size));
    {
        const ScopedLock sl (stateLock);
        lastState = state;
    }
    if (stopped.load())
        return;
    const auto r = connection->request (object ({ { "type", "setState" }, { "state", state.toBase64Encoding() } }), 10000);
    if (auto* list = r["params"].getArray())
        for (int i = 0; i < list->size() && i < getParameters().size(); ++i)
            if (auto* p = dynamic_cast<Parameter*> (getParameters()[i]))
                p->setFromHost ((float) (double) list->getReference (i)["value"], list->getReference (i)["text"].toString(), true);
}

void RemotePluginInstance::showWindow()
{
    if (! stopped.load())
        connection->send (object ({ { "type", "showEditor" } }));
}

void RemotePluginInstance::hideWindow()
{
    if (! stopped.load())
        connection->send (object ({ { "type", "hideEditor" } }));
}

//==============================================================================
void RemotePluginInstance::letThrough (AudioBuffer<float>& buffer, int start, int n, MidiBuffer&)
{
    // An instrument goes silent; an effect lets its input through.
    if (getTotalNumInputChannels() == 0)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, start, n);
}

void RemotePluginInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const int total = buffer.getNumSamples();
    auto* c = connection.get();
    auto* s = c != nullptr ? c->shared : nullptr;
    if (s == nullptr || stopped.load(std::memory_order_relaxed))
    {
        letThrough (buffer, 0, total, midi);
        midi.clear();
        return;
    }

    const int channels = jmin (buffer.getNumChannels(), remote::kMaxChannels);
    MidiBuffer midiOut;
    const bool realtime = ! offline.load (std::memory_order_relaxed);

    // The transport, for tempo-synced plug-ins.
    double ppq = 0.0, bpm = 120.0;
    int64 timeSamples = 0;
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            ppq = pos->getPpqPosition().orFallback (0.0);
            bpm = pos->getBpm().orFallback (120.0);
            timeSamples = pos->getTimeInSamples().orFallback (0);
            playing = pos->getIsPlaying();
        }

    for (int start = 0; start < total; start += remote::kMaxBlock)
    {
        const int n = jmin (remote::kMaxBlock, total - start);

        // Still busy with an earlier block it was too late for: skip this one.
        if (s->doneSeq.load (std::memory_order_acquire) != lastSeq)
        {
            letThrough (buffer, start, n, midi);
            continue;
        }

        s->numSamples = n;
        s->numChannels = channels;
        for (int ch = 0; ch < channels; ++ch)
            std::memcpy (s->audio[ch], buffer.getReadPointer (ch, start), sizeof (float) * (size_t) n);
        int count = 0;
        for (const auto m : midi)
        {
            if (m.samplePosition < start || m.samplePosition >= start + n || m.numBytes > 3 || count >= remote::kMaxMidi)
                continue;
            auto& e = s->midiIn[count++];
            e.sampleOffset = m.samplePosition - start;
            e.size = (uint8_t) m.numBytes;
            std::memcpy (e.data, m.data, (size_t) m.numBytes);
        }
        s->midiInCount = count;
        s->midiOutCount = 0;
        s->realtime = realtime ? 1 : 0;
        s->ppq = ppq + (double) start / jmax (1.0, rate) * bpm / 60.0;
        s->bpm = bpm;
        s->timeSamples = timeSamples + start;
        s->playing = playing ? 1 : 0;

        const uint32_t seq = lastSeq + 1;
        lastSeq = seq;
        s->requestSeq.store (seq, std::memory_order_release);
        c->wake.post();

        // Wait for it: about two blocks' time live (beyond that the block is lost anyway), long
        // enough for anything while rendering offline.
        const double limitMs = realtime ? jmax (10.0, 2000.0 * n / jmax (1.0, rate)) : 30000.0;
        const double startedMs = Time::getMillisecondCounterHiRes();
        bool done = false;
        for (int spins = 0;; ++spins)
        {
            if (s->doneSeq.load (std::memory_order_acquire) == seq) { done = true; break; }
            if (c->lost.load (std::memory_order_relaxed)) break;
            if ((spins & 63) == 63)
            {
                if (Time::getMillisecondCounterHiRes() - startedMs > limitMs)
                    break;
                if (spins > 4096)
                    std::this_thread::yield();
            }
            cpuRelax();
        }
        if (! done)
        {
            letThrough (buffer, start, n, midi);
            continue;
        }

        for (int ch = 0; ch < channels; ++ch)
            std::memcpy (buffer.getWritePointer (ch, start), s->audio[ch], sizeof (float) * (size_t) n);
        for (int i = 0; i < jmin (s->midiOutCount, remote::kMaxMidi); ++i)
        {
            const auto& e = s->midiOut[i];
            midiOut.addEvent (e.data, jlimit (1, 3, (int) e.size), start + e.sampleOffset);
        }
    }

    if (producesMidi() || ! midiOut.isEmpty())
        midi.swapWith (midiOut);
}

//==============================================================================
/** In Lathe's plug-in window: the plug-in's own window is a separate one, from the host. */
class RemoteEditor : public AudioProcessorEditor
{
public:
    explicit RemoteEditor (RemotePluginInstance& p) : AudioProcessorEditor (p), plugin (p)
    {
        text.setText (p.getName() + " runs separately, in its own window.", dontSendNotification);
        text.setJustificationType (Justification::centred);
        addAndMakeVisible (text);
        show.setButtonText ("Show its window");
        show.onClick = [this] { plugin.showWindow(); };
        addAndMakeVisible (show);
        setSize (360, 110);
        plugin.showWindow();
    }
    ~RemoteEditor() override   { plugin.hideWindow(); }

    void paint (Graphics& g) override   { g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId)); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        text.setBounds (r.removeFromTop (40));
        show.setBounds (r.removeFromTop (30).withSizeKeepingCentre (180, 30));
    }

private:
    RemotePluginInstance& plugin;
    Label text;
    TextButton show;
};

AudioProcessorEditor* RemotePluginInstance::createEditor()
{
    return new RemoteEditor (*this);
}
} // namespace lathe
