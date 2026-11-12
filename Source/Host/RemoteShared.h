#pragma once

// What Lathe and LatheHost (a plug-in "run separately") share: the block of memory the audio
// goes through, and the semaphore that wakes the host for each block. See RemotePlugin.h.

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <semaphore.h>
 #include <fcntl.h>
#endif

namespace lathe::remote
{
constexpr int kMaxChannels = 32;
constexpr int kMaxBlock = 8192;
constexpr int kMaxMidi = 2048;
constexpr uint32_t kMagic = 0x4c48'5354;   // "LHST"
/** The id on the host's command line (ChildProcessWorker). */
inline const char* commandLineId() { return "lathe-plugin-host"; }

struct MidiEvent
{
    int32_t sampleOffset;
    uint8_t size;
    uint8_t data[3];
};

/** The shared block. Lathe writes the input and bumps requestSeq; the host processes in
    place and sets doneSeq to the same number. */
struct Shared
{
    uint32_t magic;
    alignas (64) std::atomic<uint32_t> requestSeq;
    alignas (64) std::atomic<uint32_t> doneSeq;
    alignas (64) std::atomic<uint32_t> quit;
    int32_t numSamples;
    int32_t numChannels;
    int32_t midiInCount;
    int32_t midiOutCount;
    int32_t realtime;                       // 0 while rendering offline
    double ppq, bpm;                        // the host's transport, for tempo-synced plug-ins
    int64_t timeSamples;
    int32_t playing;
    MidiEvent midiIn[kMaxMidi];
    MidiEvent midiOut[kMaxMidi];
    float audio[kMaxChannels][kMaxBlock];   // in place: input, then output
};

static_assert (std::atomic<uint32_t>::is_always_lock_free, "the sequence numbers must be lock-free across processes");

/** A named semaphore both processes open: Lathe posts one per block, the host waits. */
class Semaphore
{
public:
    Semaphore() = default;
    ~Semaphore()   { close(); }

    bool create (const juce::String& name)   { return open (name, true); }
    bool attach (const juce::String& name)   { return open (name, false); }

    void post() noexcept
    {
       #if JUCE_WINDOWS
        if (handle != nullptr) ReleaseSemaphore (handle, 1, nullptr);
       #else
        if (sem != nullptr) sem_post (sem);
       #endif
    }

    /** Waits until posted (blocks: post once more to end a wait when quitting). */
    void wait() noexcept
    {
       #if JUCE_WINDOWS
        if (handle != nullptr) WaitForSingleObject (handle, INFINITE);
       #else
        if (sem != nullptr)
            while (sem_wait (sem) != 0 && errno == EINTR) {}
       #endif
    }

    void close()
    {
       #if JUCE_WINDOWS
        if (handle != nullptr) CloseHandle (handle);
        handle = nullptr;
       #else
        if (sem != nullptr) sem_close (sem);
        sem = nullptr;
        if (owner && posixName.isNotEmpty())
            sem_unlink (posixName.toRawUTF8());
        posixName.clear();
       #endif
        owner = false;
    }

private:
    bool owner = false;
   #if JUCE_WINDOWS
    HANDLE handle = nullptr;
   #else
    sem_t* sem = nullptr;
    juce::String posixName;
   #endif

    bool open (const juce::String& name, bool create)
    {
        close();
        owner = create;
       #if JUCE_WINDOWS
        const auto wide = ("Local\\" + name).toWideCharPointer();
        handle = create ? CreateSemaphoreW (nullptr, 0, 1 << 20, wide)
                        : OpenSemaphoreW (SEMAPHORE_ALL_ACCESS, FALSE, wide);
        return handle != nullptr;
       #else
        posixName = "/" + name;   // macOS: 31 characters at most
        if (create)
            sem_unlink (posixName.toRawUTF8());   // a leftover from a crash
        sem = create ? sem_open (posixName.toRawUTF8(), O_CREAT | O_EXCL, 0600, 0)
                     : sem_open (posixName.toRawUTF8(), 0);
        if (sem == SEM_FAILED)
            sem = nullptr;
        return sem != nullptr;
       #endif
    }

    JUCE_DECLARE_NON_COPYABLE (Semaphore)
};
} // namespace lathe::remote
