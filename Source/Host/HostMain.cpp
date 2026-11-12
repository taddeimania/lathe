// LatheHost: runs one plug-in for Lathe in its own process ("Run separately" on a plug-in),
// so a plug-in that crashes takes only this process down.
//
// Lathe starts it (ChildProcessCoordinator) and sends JSON messages: load, prepare, state,
// settings, its window. Audio goes through a block of shared memory: Lathe writes a block,
// posts the semaphore; this process's audio thread wakes, runs the plug-in on the block in
// place, and says it's done. See RemotePlugin.h and RemoteShared.h.
//
// It ends when Lathe goes away or says "quit".

#include "RemoteShared.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#if ! JUCE_WINDOWS
 #include <unistd.h>
 #include <fcntl.h>
 #include <csignal>
#endif

using namespace juce;
using namespace lathe;

#if JUCE_MAC
// JUCE makes the NSApplication only in JUCEApplication's own main(). This process has its
// own main(), so without this NSApp is nil, [NSApp run] returns at once and the host quits
// straight after connecting (the macOS "LatheHost stopped while loading" failure).
namespace juce { void initialiseNSApplication(); }
#endif

namespace
{
//==============================================================================
// A log of how far this process got, one line per step, in the file Lathe names in
// LATHE_HOST_LOG. Lathe reads it if this process goes quiet: the first line gives it our
// process id before we can say hello, and the last line says where we stopped.
#if JUCE_WINDOWS
HANDLE logFile = INVALID_HANDLE_VALUE;
#else
int logFd = -1;
#endif

void hostLog (const char* text) noexcept
{
    char line[512];
    const auto n = std::snprintf (line, sizeof (line), "%s\n", text);
    if (n <= 0)
        return;
   #if JUCE_WINDOWS
    DWORD written = 0;
    if (logFile != INVALID_HANDLE_VALUE)
        WriteFile (logFile, line, (DWORD) jmin (n, (int) sizeof (line) - 1), &written, nullptr);
   #else
    if (logFd >= 0)
        (void) ::write (logFd, line, (size_t) jmin (n, (int) sizeof (line) - 1));
   #endif
}

void hostLog (const String& text) noexcept   { hostLog (text.toRawUTF8()); }

#if ! JUCE_WINDOWS
/** A crash says so in the log first, so Lathe knows at once: macOS keeps a crashed process
    alive while it writes the crash report, and until then Lathe couldn't tell it had gone.
    Then the crash goes on as usual, so the system's crash report (with the stack) is kept. */
void crashed (int sig)
{
    char line[64];
    const auto n = std::snprintf (line, sizeof (line), "crashed (signal %d)\n", sig);
    if (logFd >= 0 && n > 0)
        (void) ::write (logFd, line, (size_t) n);
    std::signal (sig, SIG_DFL);
    std::raise (sig);
}
#endif

void openLog()
{
   #if JUCE_WINDOWS
    wchar_t path[2048] = {};
    if (const auto len = GetEnvironmentVariableW (L"LATHE_HOST_LOG", path, 2048); len > 0 && len < 2048)
        logFile = CreateFileW (path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char line[64];
    std::snprintf (line, sizeof (line), "pid %lu", (unsigned long) GetCurrentProcessId());
   #else
    if (const auto* path = std::getenv ("LATHE_HOST_LOG"); path != nullptr && *path != 0)
        logFd = ::open (path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
        std::signal (sig, crashed);
    char line[64];
    std::snprintf (line, sizeof (line), "pid %d", (int) getpid());
   #endif
    hostLog (line);
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

/** The transport Lathe sent with the block. */
struct SharedPlayHead : public AudioPlayHead
{
    remote::Shared* s = nullptr;
    double rate = 48000.0;
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        if (s == nullptr)
            return p;
        p.setPpqPosition (s->ppq);
        p.setBpm (s->bpm);
        p.setTimeInSamples (s->timeSamples);
        p.setTimeInSeconds ((double) s->timeSamples / rate);
        p.setIsPlaying (s->playing != 0);
        return p;
    }
};

class Host;

/** The plug-in's own window. */
class EditorWindow : public DocumentWindow
{
public:
    EditorWindow (AudioProcessorEditor* editor, const String& title)
        : DocumentWindow (title, Colours::darkgrey, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
    }
    void closeButtonPressed() override   { setVisible (false); }
};

//==============================================================================
class Host : public ChildProcessWorker,
             private Thread,
             private AudioProcessorListener
{
public:
    Host() : Thread ("LatheHost audio") {}

    ~Host() override
    {
        stopAudio();
        window.reset();
        if (plugin != nullptr)
            plugin->removeListener (this);
        plugin.reset();
        map.reset();
    }

    void handleMessageFromCoordinator (const MemoryBlock& mb) override
    {
        auto msg = fromBlock (mb);
        // Plug-ins are loaded and asked things on the message thread.
        MessageManager::callAsync ([this, msg] { handle (msg); });
    }

    void handleConnectionMade() override
    {
        // First thing: who we are, so Lathe can tell at once if this process goes away.
        auto msg = obj();
        msg.getDynamicObject()->setProperty ("type", "hello");
       #if JUCE_WINDOWS
        msg.getDynamicObject()->setProperty ("pid", (int) GetCurrentProcessId());
       #else
        msg.getDynamicObject()->setProperty ("pid", (int) getpid());
       #endif
        sendMessageToCoordinator (toBlock (msg));
        hostLog ("said hello");
    }

    void handleConnectionLost() override
    {
        hostLog ("Lathe went away");
        stopAudio();
        MessageManager::getInstance()->stopDispatchLoop();
    }

private:
    std::unique_ptr<AudioPluginInstance> plugin;
    std::unique_ptr<MemoryMappedFile> map;
    remote::Shared* shared = nullptr;
    remote::Semaphore wake;
    SharedPlayHead playHead;
    std::unique_ptr<EditorWindow> window;
    double rate = 48000.0;
    int block = 512;
    std::atomic<bool> fromLathe { false };   // a setting Lathe is changing: don't echo it back
    int lastLatency = 0;

    void reply (const var& msg, var out)
    {
        if (! out.isObject())
            out = var (new DynamicObject());
        out.getDynamicObject()->setProperty ("reply", msg["id"]);
        sendMessageToCoordinator (toBlock (out));
    }

    static var obj()   { return var (new DynamicObject()); }

    var describeParams() const
    {
        Array<var> list;
        int i = 0;
        for (auto* p : plugin->getParameters())
        {
            auto d = obj();
            auto* o = d.getDynamicObject();
            o->setProperty ("name", p->getName (128));
            o->setProperty ("label", p->getLabel());
            o->setProperty ("default", p->getDefaultValue());
            o->setProperty ("value", p->getValue());
            o->setProperty ("text", p->getCurrentValueAsText());
            o->setProperty ("steps", p->getNumSteps());
            o->setProperty ("discrete", p->isDiscrete());
            o->setProperty ("boolean", p->isBoolean());
            o->setProperty ("automatable", p->isAutomatable());
            if (p->isDiscrete() && p->getNumSteps() <= 128)
            {
                Array<var> strings;
                for (auto& s : p->getAllValueStrings())
                    strings.add (s);
                o->setProperty ("strings", strings);
            }
            list.add (d);
            if (++i >= 4096)
                break;
        }
        return list;
    }

    void handle (const var& msg)
    {
        const auto type = msg["type"].toString();
        if (type == "load")        { hostLog ("load"); load (msg); hostLog ("load answered"); return; }
        if (type == "quit")
        {
            reply (msg, {});
            stopAudio();
            MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        if (plugin == nullptr)
        {
            auto out = obj();
            out.getDynamicObject()->setProperty ("error", "no plug-in loaded");
            reply (msg, out);
            return;
        }
        auto params = plugin->getParameters();
        if (type == "prepare")
        {
            rate = (double) msg["rate"];
            block = (int) msg["block"];
            playHead.rate = rate;
            const ScopedLock sl (plugin->getCallbackLock());
            plugin->releaseResources();
            plugin->setRateAndBufferSizeDetails (rate, block);
            plugin->prepareToPlay (rate, block);
            reply (msg, {});
        }
        else if (type == "release")  plugin->releaseResources();
        else if (type == "reset")    plugin->reset();
        else if (type == "nonRealtime") plugin->setNonRealtime ((bool) msg["on"]);
        else if (type == "getState")
        {
            MemoryBlock state;
            plugin->getStateInformation (state);
            auto out = obj();
            out.getDynamicObject()->setProperty ("state", state.toBase64Encoding());
            reply (msg, out);
        }
        else if (type == "setState")
        {
            MemoryBlock state;
            state.fromBase64Encoding (msg["state"].toString());
            {
                fromLathe = true;
                plugin->setStateInformation (state.getData(), (int) state.getSize());
                fromLathe = false;
            }
            auto out = obj();
            out.getDynamicObject()->setProperty ("params", describeParams());
            reply (msg, out);
            checkLatency();
        }
        else if (type == "setParam")
        {
            if (auto* p = params[(int) msg["index"]])
            {
                fromLathe = true;
                p->setValueNotifyingHost ((float) (double) msg["value"]);
                fromLathe = false;
            }
        }
        else if (type == "paramText")
        {
            auto out = obj();
            if (auto* p = params[(int) msg["index"]])
                out.getDynamicObject()->setProperty ("text", p->getText ((float) (double) msg["value"], 128));
            reply (msg, out);
        }
        else if (type == "valueForText")
        {
            auto out = obj();
            if (auto* p = params[(int) msg["index"]])
                out.getDynamicObject()->setProperty ("value", p->getValueForText (msg["text"].toString()));
            reply (msg, out);
        }
        else if (type == "showEditor")  showEditor();
        else if (type == "hideEditor")  { if (window != nullptr) window->setVisible (false); }
    }

    void load (const var& msg)
    {
        auto out = obj();
        auto* o = out.getDynamicObject();
        auto fail = [&] (const String& why)
        {
            o->setProperty ("ok", false);
            o->setProperty ("error", why);
            reply (msg, out);
        };

        rate = (double) msg["rate"];
        block = (int) msg["block"];
        playHead.rate = rate;

        map = std::make_unique<MemoryMappedFile> (File (msg["shm"].toString()), MemoryMappedFile::readWrite, false);
        if (map->getData() == nullptr || map->getSize() < sizeof (remote::Shared))
            return fail ("LatheHost couldn't open the shared memory.");
        shared = static_cast<remote::Shared*> (map->getData());
        if (shared->magic != remote::kMagic)
            return fail ("LatheHost found the wrong shared memory.");
        if (! wake.attach (msg["semaphore"].toString()))
            return fail ("LatheHost couldn't open the semaphore.");
        playHead.s = shared;

        PluginDescription desc;
        if (auto xml = parseXML (msg["plugin"].toString()); xml == nullptr || ! desc.loadFromXml (*xml))
            return fail ("LatheHost didn't understand which plug-in to load.");

        AudioPluginFormatManager formats;
        formats.addDefaultFormats();
        String error;
        hostLog ("loading " + desc.name + " from " + desc.fileOrIdentifier);
        plugin = formats.createPluginInstance (desc, rate, block, error);
        hostLog (plugin != nullptr ? "plug-in loaded" : "plug-in didn't load: " + error);
        if (plugin == nullptr)
            return fail (error.isNotEmpty() ? error : desc.name + " didn't load.");

        // As in Lathe: plain stereo on the main buses if the plug-in takes it.
        auto layout = plugin->getBusesLayout();
        if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0) = AudioChannelSet::stereo();
        if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = AudioChannelSet::stereo();
        if (plugin->checkBusesLayoutSupported (layout))
            plugin->setBusesLayout (layout);

        MemoryBlock state;
        state.fromBase64Encoding (msg["state"].toString());
        if (state.getSize() > 0)
            plugin->setStateInformation (state.getData(), (int) state.getSize());
        plugin->setPlayHead (&playHead);
        plugin->setRateAndBufferSizeDetails (rate, block);
        plugin->prepareToPlay (rate, block);
        plugin->addListener (this);
        lastLatency = plugin->getLatencySamples();

        o->setProperty ("ok", true);
        o->setProperty ("name", plugin->getName());
        o->setProperty ("inputs", plugin->getTotalNumInputChannels());
        o->setProperty ("outputs", plugin->getTotalNumOutputChannels());
        o->setProperty ("acceptsMidi", plugin->acceptsMidi());
        o->setProperty ("producesMidi", plugin->producesMidi());
        o->setProperty ("latency", lastLatency);
        o->setProperty ("tail", plugin->getTailLengthSeconds());
        o->setProperty ("hasEditor", plugin->hasEditor());
        o->setProperty ("params", describeParams());

        // Realtime priority where the system allows it (Linux often doesn't, without permission).
        if (startRealtimeThread (Thread::RealtimeOptions{}.withApproximateAudioProcessingTime (jmax (16, block), rate)))
            hostLog ("audio thread started (realtime)");
        else
        {
            startThread (Thread::Priority::highest);
            hostLog ("audio thread started");
        }
        reply (msg, out);
    }

    void showEditor()
    {
        if (window == nullptr)
        {
            auto* editor = plugin->createEditorIfNeeded();
            if (editor == nullptr)
                editor = new GenericAudioProcessorEditor (*plugin);
            window = std::make_unique<EditorWindow> (editor, plugin->getName() + " (running separately)");
        }
       #if JUCE_MAC
        Process::setDockIconVisible (true);   // so the window can come to the front
       #endif
        window->setVisible (true);
        window->toFront (true);
    }

    void checkLatency()
    {
        const int now = plugin->getLatencySamples();
        if (now == lastLatency)
            return;
        lastLatency = now;
        auto msg = obj();
        msg.getDynamicObject()->setProperty ("type", "latency");
        msg.getDynamicObject()->setProperty ("samples", now);
        sendMessageToCoordinator (toBlock (msg));
    }

    // --- the plug-in tells us
    void audioProcessorParameterChanged (AudioProcessor*, int index, float value) override
    {
        if (fromLathe.load())
            return;
        auto msg = obj();
        auto* o = msg.getDynamicObject();
        o->setProperty ("type", "param");
        o->setProperty ("index", index);
        o->setProperty ("value", value);
        if (auto* p = plugin->getParameters()[index])
            o->setProperty ("text", p->getText (value, 128));
        sendMessageToCoordinator (toBlock (msg));
    }

    void audioProcessorChanged (AudioProcessor*, const ChangeDetails& d) override
    {
        if (d.latencyChanged)
            MessageManager::callAsync ([this] { if (plugin != nullptr) checkLatency(); });
    }

    // --- audio
    void stopAudio()
    {
        if (shared != nullptr)
            shared->quit = 1;
        signalThreadShouldExit();
        wake.post();
        stopThread (3000);
    }

    void run() override
    {
        uint32_t lastDone = shared->doneSeq.load();
        MidiBuffer midi;
        midi.ensureSize (remote::kMaxMidi * 8);
        float* channels[remote::kMaxChannels] {};
        for (int ch = 0; ch < remote::kMaxChannels; ++ch)
            channels[ch] = shared->audio[ch];

        while (! threadShouldExit())
        {
            wake.wait();
            if (threadShouldExit() || shared->quit.load() != 0)
                break;
            const uint32_t seq = shared->requestSeq.load (std::memory_order_acquire);
            if (seq == lastDone)
                continue;   // a spare wake-up

            const int n = jlimit (0, remote::kMaxBlock, (int) shared->numSamples);
            const int need = jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
            const int numCh = jlimit (0, remote::kMaxChannels, jmax ((int) shared->numChannels, need));
            for (int ch = shared->numChannels; ch < numCh; ++ch)
                FloatVectorOperations::clear (channels[ch], n);

            midi.clear();
            for (int i = 0; i < jmin ((int) shared->midiInCount, remote::kMaxMidi); ++i)
            {
                const auto& e = shared->midiIn[i];
                midi.addEvent (e.data, jlimit (1, 3, (int) e.size), jlimit (0, jmax (0, n - 1), (int) e.sampleOffset));
            }

            {
                AudioBuffer<float> view (channels, numCh, n);
                const ScopedLock sl (plugin->getCallbackLock());
                if (plugin->isSuspended())
                    view.clear();
                else
                    plugin->processBlock (view, midi);
            }

            int out = 0;
            for (const auto m : midi)
            {
                if (m.numBytes > 3 || out >= remote::kMaxMidi)
                    continue;
                auto& e = shared->midiOut[out++];
                e.sampleOffset = m.samplePosition;
                e.size = (uint8_t) m.numBytes;
                std::memcpy (e.data, m.data, (size_t) m.numBytes);
            }
            shared->midiOutCount = out;
            lastDone = seq;
            shared->doneSeq.store (seq, std::memory_order_release);
        }
    }
};
} // namespace

//==============================================================================
static int runHost (const String& commandLine)
{
    openLog();
   #if JUCE_MAC
    initialiseNSApplication();
   #endif
    ScopedJuceInitialiser_GUI juceInit;
    hostLog ("JUCE started");
    auto host = std::make_unique<Host>();
    if (! host->initialiseFromCommandLine (commandLine, remote::commandLineId(), remote::pingTimeoutMs))
    {
        hostLog ("couldn't connect to Lathe (" + commandLine + ")");
        return 2;
    }
    hostLog ("connected to Lathe");
    MessageManager::callAsync ([] { hostLog ("running"); });
    MessageManager::getInstance()->runDispatchLoop();
    hostLog ("message loop ended");
    host.reset();
    std::_Exit (0);   // skip static teardown: some plug-ins misbehave when unloaded at exit
}

#if JUCE_WINDOWS
int wmain (int argc, wchar_t* argv[])
{
    // A crashing plug-in should just end this process, without an error box.
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (String (argv[i]));
    return runHost (args.joinIntoString (" "));
}
#else
int main (int argc, char* argv[])
{
    StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (String::fromUTF8 (argv[i]));
    return runHost (args.joinIntoString (" "));
}
#endif
