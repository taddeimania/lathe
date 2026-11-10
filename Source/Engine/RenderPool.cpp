#include "RenderPool.h"

#if JUCE_INTEL
 #include <immintrin.h>
#endif

namespace lathe
{
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

// A worker's state. Only the audio thread moves idle -> woken; only the worker moves
// woken -> busy; the worker moves busy -> idle when it runs out of tasks; and the audio
// thread may take back a wake-up (woken -> idle) that hasn't started.
enum State : int { idle = 0, woken = 1, busy = 2 };
}

struct RenderPool::Worker : public Thread
{
    Worker (RenderPool& p, int index) : Thread ("Lathe audio worker " + String (index + 1)), pool (p) {}
    void run() override  { pool.work (*this); }

    RenderPool& pool;
    std::atomic<int> state { idle };
    std::atomic<bool> sleeping { false };
    std::counting_semaphore<> wake { 0 };
};

RenderPool::RenderPool (int numWorkers, AudioWorkgroup wg, double sampleRate, int blockSize)
    : workgroup (std::move (wg))
{
    const auto options = Thread::RealtimeOptions{}
                             .withApproximateAudioProcessingTime (jmax (16, blockSize), sampleRate > 0 ? sampleRate : 48000.0);
    for (int i = 0; i < numWorkers; ++i)
    {
        auto w = std::make_unique<Worker> (*this, i);
        if (! w->startRealtimeThread (options))   // e.g. Linux without realtime permission
            w->startThread (Thread::Priority::highest);
        workers.push_back (std::move (w));
    }
}

RenderPool::~RenderPool()
{
    quitting = true;
    for (auto& w : workers)
    {
        w->signalThreadShouldExit();
        w->wake.release();
    }
    for (auto& w : workers)
        w->stopThread (2000);
}

void RenderPool::work (Worker& w) noexcept
{
    ScopedNoDenormals noDenormals;
    WorkgroupToken token;
    if (workgroup)
        workgroup.join (token);

    while (! quitting.load())
    {
        // Spin a little first: the next block's job usually comes within a few hundred
        // microseconds, and waking from the semaphore costs more than that.
        for (int i = 0; i < 4000 && w.state.load (std::memory_order_acquire) != woken && ! quitting.load (std::memory_order_relaxed); ++i)
            cpuRelax();

        if (w.state.load (std::memory_order_acquire) != woken)
        {
            w.sleeping.store (true);
            if (w.state.load() != woken && ! quitting.load())
                w.wake.acquire();
            w.sleeping.store (false);
            continue;   // check again (a wake-up may be stale, or taken back)
        }

        int expected = woken;
        if (! w.state.compare_exchange_strong (expected, busy, std::memory_order_acq_rel))
            continue;   // the audio thread took this wake-up back: the job is finished

        takeTasks();
        w.state.store (idle, std::memory_order_release);
    }
}

void RenderPool::takeTasks() noexcept
{
    for (int i = next.fetch_add (1, std::memory_order_relaxed); i < count; i = next.fetch_add (1, std::memory_order_relaxed))
    {
        task (context, i);
        done.fetch_add (1, std::memory_order_release);
    }
}

void RenderPool::run (int numTasks, Task t, void* ctx) noexcept
{
    if (numTasks <= 0)
        return;

    task = t;
    context = ctx;
    count = numTasks;
    next.store (0, std::memory_order_relaxed);
    done.store (0, std::memory_order_relaxed);

    // Wake as many workers as there are tasks to share (the audio thread takes one too).
    const int toWake = jmin ((int) workers.size(), numTasks - 1);
    for (int i = 0; i < toWake; ++i)
    {
        auto& w = *workers[(size_t) i];
        // Sequentially consistent, like the worker's sleeping/state pair: one of the two
        // always sees the other, so a worker can't fall asleep on a job it was given.
        w.state.store (woken);   // also publishes the job's fields
        if (w.sleeping.load())
            w.wake.release();
    }

    takeTasks();

    while (done.load (std::memory_order_acquire) < numTasks)
        cpuRelax();

    // All tasks are done. Take back wake-ups that never started, and wait for any worker
    // still on its way out of takeTasks(), so none of them sees the next job's fields early.
    for (int i = 0; i < toWake; ++i)
    {
        auto& w = *workers[(size_t) i];
        int expected = woken;
        if (! w.state.compare_exchange_strong (expected, idle, std::memory_order_acq_rel))
            while (w.state.load (std::memory_order_acquire) == busy)
                cpuRelax();
    }
}
} // namespace lathe
