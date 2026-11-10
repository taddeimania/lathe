# Plan: multi-core audio

**Status: planned.** Today Lathe renders every track one after another on the single audio
thread Core Audio (or ASIO, or ALSA) calls. On an 8- or 10-core machine that means plug-in-heavy
sessions hit their limit on one core while the others sit idle, and a heavy synth shows up as
10% of the CPU meter where Live, which spreads tracks over several threads, shows a couple of
percent. This plan spreads the work over the cores without changing what anything sounds like.

## What the engine does today

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
