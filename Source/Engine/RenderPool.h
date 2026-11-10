#pragma once

#include "../Common.h"
#include <semaphore>

namespace lathe
{
//==============================================================================
/** The audio worker threads that render tracks in parallel (docs/multicore.md).

    The audio thread calls run() with a list of tasks; it and the workers take tasks with one
    atomic counter until they're gone, then run() returns. Nothing in run() locks or allocates.

    Each worker is woken for one job at a time and checks in before touching it, so a worker
    that wakes up late (the OS was slow to schedule it) can't wander into a later job: run()
    takes back any wake-up that hasn't started yet once the tasks are done, and only waits for
    workers that are in the middle of a task.

    Workers run at realtime priority, with denormals off, and on macOS they join the audio
    device's workgroup so the scheduler keeps them on the performance cores and counts them
    towards the audio deadline. */
class RenderPool
{
public:
    /** `workers` extra threads (the calling audio thread works too). */
    RenderPool (int workers, AudioWorkgroup, double sampleRate, int blockSize);
    ~RenderPool();

    int getNumWorkers() const noexcept   { return (int) workers.size(); }

    using Task = void (*) (void* context, int index);
    /** Runs task(context, i) for every i in [0, count), on this thread and the workers, and
        returns when all are done. Audio thread only (or the export thread, never both). */
    void run (int count, Task, void* context) noexcept;

private:
    struct Worker;
    void work (Worker&) noexcept;
    void takeTasks() noexcept;

    // the current job
    Task task = nullptr;
    void* context = nullptr;
    int count = 0;
    std::atomic<int> next { 0 }, done { 0 };

    std::vector<std::unique_ptr<Worker>> workers;
    AudioWorkgroup workgroup;
    std::atomic<bool> quitting { false };

    JUCE_DECLARE_NON_COPYABLE (RenderPool)
};
} // namespace lathe
