# Multi-core audio

**Status: done in 0.8.0** (the plan below, as built; see "As built" at the end). Before
0.8.0 Lathe rendered every track one after another on the single audio thread Core Audio
(or ASIO, or ALSA) calls. On an 8- or 10-core machine that means plug-in-heavy
sessions hit their limit on one core while the others sit idle, and a heavy synth shows up as
10% of the CPU meter where Live, which spreads tracks over several threads, shows a couple of
percent. This plan spreads the work over the cores without changing what anything sounds like.

## What the engine did before 0.8.0

Per audio block, `Engine::renderChunk`:

1. clears the bus, master and cue-mix buffers;
2. runs `processTrack` for each track in `g.order` (a track that takes another track as its
   input comes after its source). Each one renders its clips and live input, runs its plug-ins
   and EQ, then **mixes straight into shared places**: its sends and output into
   `g.buses[...]`, `g.master`, the cue mixes' `trackSum`, hardware outputs (`addToOutput`), and
   MIDI outputs (`sendBlockOfMessages`);
3. runs `processBus` for each return and group bus (each goes to the master or to hardware;
   buses don't feed other buses);
4. runs the master chain, the metronome and the cue mixes.

What stops step 2 running in parallel:

| Shared thing | Who writes it |
|---|---|
| bus, master and cue-sum buffers | every track (sends, output, cue taps) |
| `g.scratch` | `renderClips` and `processBus` use one temp buffer |
| hardware output buffers, MIDI outputs | tracks routed straight to hardware or a MIDI out |
| `tap` / `midiTap` of a source track | read by the tracks that take it as their input |
| summing order | floating-point sums depend on order, so a different order changes the last bits |

Per-track state (its buffer, plug-ins, delay lines, record take, meters) is already the track's
own, and each plug-in is only ever called by its own track, so plug-ins need no changes: they
are still never called by two threads at once, as in every other DAW.

## The design

**Process in parallel, mix in order.**

- **Parallel part:** each track renders its clips, input, plug-ins and EQ into **its own
  buffer only**, with its own scratch buffer. Nothing shared is written.
- **Serial part (cheap):** one pass in `g.order` adds each track's buffer into the buses,
  master, cue mixes, hardware outputs and MIDI outputs, exactly as today. Because the order is
  the same, **the output is bit-identical to the current engine.** Exports, tests and the
  cloud's change summaries can't tell the difference.

**Dependency levels.** `buildGraph` already knows which track feeds which (`inputTrack`). It
groups tracks into levels: level 0 has no track input, level 1 takes its input from level 0,
and so on. Tracks within a level run in parallel; levels run one after another. Then all
buses (independent of each other) run in parallel, then the master. Most sessions are one
level of tracks plus one level of buses.

**Workers.** A small pool of audio worker threads, started with the device and stopped with
it:

- count: performance cores minus one (the device thread works too), with a setting to cap it;
- **macOS:** each worker joins the device's `AudioWorkgroup` (JUCE 8:
  `AudioIODevice::getWorkgroup()`), so the scheduler keeps them on performance cores and
  treats them as part of the audio deadline; Windows and Linux: realtime-priority threads;
- per block: the audio thread publishes the level's task list, wakes the workers, and
  everyone (the audio thread included) takes tasks with one atomic counter, heaviest track
  first (from a running average of each track's cost), then meets at a barrier. No locks, no
  allocation, no waiting on the message thread. Workers spin briefly, then sleep on a
  lightweight semaphore between blocks;
- each worker sets `ScopedNoDenormals`, like the audio thread already does.

**Stay serial when it doesn't pay.** Waking threads costs a few microseconds. With one or two
light tracks, or when the last blocks were cheap, the audio thread just does it all itself.
The choice is per block, so it adapts as plug-ins are added.

**Export uses it too.** The export thread drives the same scheduler, so offline exports get
faster on the same machine, with the same bit-identical result.

## Steps (each one shippable, nothing changes for users until step 4)

1. **Measure.** A CPU-burner test plug-in (fixed work per sample) and a benchmark in the test
   build: N tracks at 128 and 256 samples, time per block. Gives a before/after number.
2. **Split render from mix.** Per-track scratch buffers; move every shared write out of
   `processTrack`/`processBus` into the ordered mix pass. Still one thread. Tests: every
   existing engine test passes unchanged, plus a check that renders and exports are
   bit-identical to the old path.
3. **Levels and the scheduler, off by default.** The worker pool, workgroup joining,
   heaviest-first tasks, the barrier, and the serial fallback, behind an `LATHE_MULTICORE`
   setting. Tests:
   - parallel vs serial renders **bit-identical** on every existing test project, and on
     hundreds of random graphs (tracks feeding tracks, sends, group buses, cue mixes, hardware
     outputs, MIDI tracks and generators);
   - a ThreadSanitizer CI job (Linux, clang) running the engine tests with threads forced on;
   - recording, count-in, loop and the AI auto-stop while parallel.
4. **Turn it on.** Default on, with **Options > Audio / MIDI Settings > Processing cores**
   (Auto, or a number; 1 = the old behaviour). The CPU meter keeps meaning "how much of the
   audio deadline was used", now across cores. Release notes, docs, a beta round.

## Risks and how they're handled

- **A plug-in that misbehaves off the main audio thread.** Hosts like Live and Reaper already
  run plug-ins on worker threads, so this is rare. The "Processing cores: 1" setting is the
  escape hatch, and a per-plug-in "run on the main audio thread" flag can follow if needed.
- **Wake-up overhead at tiny buffers.** The per-block serial fallback, and measuring in step 1.
- **Priority problems.** Workers never take locks the message thread holds; the graph swap
  keeps using the existing `graphLock`, held only by the audio thread while it renders.
- **Recording.** A track's take is only ever written by the thread rendering that track in
  that block, and the barrier orders blocks, so the disk writers see one writer at a time, as
  today.

## As built (0.8.0)

- `Engine::renderTrack` / `mixTrack` and `renderBus` / `mixBus` are the two halves.
  `renderTracks` hands out each dependency level (`RenderGraph::levels`, built in
  `buildGraph` from `inputTrack`) heaviest first; `renderBuses` does the same for the buses.
  The mix passes run in `g.order` and bus order, as before.
- `Source/Engine/RenderPool.{h,cpp}`: the workers. One atomic task counter; each worker has
  a state (idle, woken, busy) so `run()` can take back a wake-up that hasn't started and
  only waits for workers in the middle of a task. Workers spin for a short while after a
  block, then sleep on a semaphore. Realtime priority, `ScopedNoDenormals`, and on macOS
  they join the device's `AudioWorkgroup`.
- Per track, a running average of its render time (`RenderTrack::costUs`) orders the tasks
  and decides whether a block is worth sharing (60 µs of work or more).
- The pool is swapped only under `graphLock` and never during an export (the export thread
  uses it without the lock); a change asked for then waits for `handleAsyncUpdate`.
- `Options > Audio / MIDI Settings > Processing cores` (setting `processingThreads`,
  0 = Auto = `Engine::autoProcessingThreads()`, performance cores on Apple silicon).

**How it's tested**

- `LatheTests --dump <folder> [threads]` writes 30 outputs (four sessions covering every
  routing path at buffer sizes 64 to 512, an export mix and its stems). The old
  single-threaded engine's files and the new engine's, on 1, 2 and more threads, are
  identical byte for byte.
- `LatheTests --random [seeds] [threads]`: random sessions (chains of tracks feeding tracks,
  sends, group buses, hardware outputs, cue mixes, mutes, solos, live input) rendered on one
  thread and on several, compared sample for sample. In ctest: 200 sessions on 4 threads.
- `LATHE_TEST_THREADS=n` makes every engine test share out every block on n threads (ctest
  runs the whole suite that way on 4), and CI's **Thread check** job runs both of the
  above under ThreadSanitizer.
- `Lathe --benchmark` / `LatheTests --bench` time it (docs/performance.md).

**Results** (share of the audio deadline at 128 samples, 1 core → all cores)

| Session | 2-core Intel Xeon | Apple M1 Pro, 6 performance cores |
|---|---|---|
| 12 heavy synths | 92% (peaks 148%, dropouts) → 51% | 42% → 9% |
| 8 synths | 66% → 32% | 28% → 9% |
| Band: 8 audio + 4 synths + 2 returns | 51% → 26% | 22% → 8.5% |
| 4 synths printed to audio (chained) | 44% → 20% | 18% → 5.6% |
| 32-bar export | 33.4 s → 20.0 s | 16.3 s → 5.5 s |
