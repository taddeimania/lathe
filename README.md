# Lathe

A small, dependable DAW. It opens, finds your interface, loads a template, and records. It hosts VST3 plug-ins, has a mixer with returns and sends, routes audio and MIDI to and from your interface, and exports a mix and stems. It doesn't try to do everything else.

Built with C++20 and JUCE 8.0.9, the same setup as the plug-in projects. It runs on Windows (ASIO, WASAPI) and macOS (CoreAudio).

---

## Build

### macOS

You need the Xcode command line tools and CMake (`brew install cmake`).

```sh
cd ~/Projects/lathe
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
open build/Lathe_artefacts/Release/Lathe.app
```

The first configure downloads JUCE, which takes a minute. macOS asks for microphone access the first time Lathe opens the interface. Allow it, or nothing will record.

### Windows

You need Visual Studio 2022 (with "Desktop development with C++") and CMake. Install the **Behringer UMC1820 driver** from behringer.com first, since its ASIO driver is what gives low latency.

```bat
cd lathe
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Lathe_artefacts\Release\Lathe.exe
```

ASIO support is built in automatically. Steinberg's ASIO SDK has been GPLv3 since October 2025, and CMake fetches it. To build without it, pass `-DLATHE_ASIO=OFF`. To use your own copy, pass `-DLATHE_ASIO_SDK_DIR=...`.

### Builds on GitHub

`.github/workflows/build.yml` builds and tests Lathe on macOS (universal), Windows and Linux. It works like the plug-in repos' workflow: it runs only when you start it (**Actions > Build > Run workflow**).

- On a branch, it builds, runs the tests, and keeps the three packaged apps as run artifacts for 30 days.
- On a `v*` tag, it does the same and then creates a **draft release** with the three archives and the text from `.github/release-notes.md`. Review the draft and publish it. The tag must match `project(Lathe VERSION ...)` in `CMakeLists.txt`.

Each archive holds the app, `LatheScanner` (next to `Lathe.exe`, or inside `Lathe.app`), `README.md` and `LICENSE`.

### Tests

```sh
cmake -B build -DLATHE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

These are headless engine checks that run a fake audio device through the real audio callback. They cover clip timing, plug-in delay compensation, mute, solo and faders, sends, looping, record-latency offset, export, project round-trip, and clip splitting and undo. The test build also makes three tiny VST3s (one that works, one that crashes while loading, and one that hangs). With those, the tests check that scanning skips the bad plug-ins without freezing or crashing Lathe. They also test the plug-in store: release parsing against the real release format, picking the right platform archive, zip (with Unix permissions) and tar.gz installs, updates over an existing copy, staged updates, and the full download-and-install flow.

---

## First run

1. **Audio device.** On first launch Lathe looks for a UMC or Behringer device and selects it: ASIO on Windows, CoreAudio on Mac, 128-sample buffer, all channels on. If it picks the wrong thing, click the device name at the top right (or **Options > Audio / MIDI Settings**). Your choice is saved and restored on every launch. If the interface is unplugged while Lathe runs, Lathe keeps trying to reconnect to it.
2. **Plug-ins.** On first launch Lathe scans the standard VST3 folders in the background, and you can keep working while it does. Each plug-in is opened by a small helper program, `LatheScanner`, which the build places next to Lathe. If a plug-in crashes or hangs (more than a minute) while it's being checked, only the helper dies: Lathe skips that plug-in and says so in the status bar. Settings lists the skipped plug-ins and has **Retry skipped plug-ins** for after you update one. VCV Rack 2's VST3 is a known example: it crashes when unloaded straight after loading.
3. **Templates.** Two templates are created in `Documents/Lathe/Templates`:
   - **Basic**: two audio tracks (inputs 1 and 2), one MIDI track, two returns.
   - **UMC1820 Tracking**: eight mono tracks on preamps 1–8, a MIDI track, and two returns. Monitoring is **Off** on these tracks, so you hear yourself through the UMC's **MONITORING** knob with zero latency (set it toward IN or MIX).

Set up a session the way you like it (tracks, inputs, plug-ins, sends), then use **File > Save as Template**. In Settings you can choose to open the last project or start from your default template on launch.

## Plug-in browser (your GitHub plug-ins)

**Options > Plug-in Browser** (Cmd/Ctrl+B), or **Browse...** next to "+ Add plug-in". It lists your 14 plug-ins from `github.com/taddeimania`, grouped as effects, instruments and MIDI tools, with the latest published version of each. Below them are the other installed plug-ins.

- **Get & add** downloads the latest release for this computer (`<Name>-<version>-macOS.zip` or `-Windows.zip`), installs the `.vst3` into your own VST3 folder, scans it, and adds it:
  - Effects go on the selected track (or the master).
  - Instruments go on the selected MIDI track, or a new MIDI track if it already has one.
  - MIDI tools (Beadwork, Dicey, Posy) go at the front of a MIDI track's chain, ahead of the instrument they drive.
- **Add** on a plug-in that's already installed just adds it. **Update to x.y.z** appears (in orange) when GitHub has a newer release. On launch Lathe checks quietly and mentions available updates in the status bar.
- Install folders don't need admin rights: `~/Library/Audio/Plug-Ins/VST3` on the Mac and `%LOCALAPPDATA%\Programs\Common\VST3` on Windows.
- On Windows, a plug-in that's loaded can't be replaced. In that case the update is staged and finished the next time Lathe starts, before any plug-in loads. On either system, restart Lathe to use an updated plug-in in a project that already has it open.
- Only *published* releases are used, so draft releases from the CI workflow don't show up until you publish them.
- GitHub allows 60 anonymous lookups an hour, so results are cached for an hour. **Check for updates** forces a fresh look.
- **Add repository...** takes any `owner/name` whose releases follow the same pattern. The catalog is plain JSON at `Documents/Lathe/Plugin Catalog.json` if you'd rather edit it by hand. New plug-ins built into Lathe's list are merged in automatically.

## Recording

- Arm tracks with the round button. **R** or the record button starts recording from the start marker.
- If nothing is armed, record arms the selected track.
- Takes are shifted by the round-trip latency the driver reports, plus any plug-in latency, so they land in time. If your setup still lands early or late, adjust **Recording offset** in Settings. To measure it, record a click through a cable from an output back into an input.
- A new take replaces whatever was underneath it on that track. Undo (Cmd/Ctrl+Z) brings the old material back.
- MIDI tracks record notes, CC, and pitch bend from any MIDI input (or the one you pick). To play hardware from a MIDI track, set **MIDI to** in the mixer, for example to the UMC's MIDI OUT.

## Track groups

Select tracks (Cmd/Ctrl-click or Shift-click their names), then press **Cmd/Ctrl+G** or right-click → **Group**. A group gets a header row in the arrangement with:

- **Arm** arms every track in the group, or disarms them all if they're all armed.
- **S / M** solo or mute the whole group, on top of each track's own buttons. The members' own states are kept, so un-muting the group brings back exactly what was there before.
- **The triangle** collapses the group to its header, to save space on a big session.

Click a group's header to select all its tracks. Double-click its name to rename it, and right-click it for colour, arm all, collapse and **Ungroup** (Cmd/Ctrl+Shift+G), which keeps the tracks. In the mixer, members show the group's name above their own. Groups are saved in projects and templates. They control tracks together but don't change the audio routing; to process a group's audio together, send its tracks to a return.

Double-click any track or group name to rename it in place (Return keeps, Escape cancels). The scroll wheel scrolls the track list wherever the pointer is over the arrangement.

## Routing

- **Inputs**: any mono input (In 1…18) or stereo pair. On the UMC1820, 1–8 are the preamps, 9–10 are S/PDIF, and 11–18 are ADAT (at 44.1 or 48 kHz).
- **Outputs**: each track goes to Master, to a return, or straight to an interface output. On the UMC1820, outputs 1/2 are MAIN OUT and 3–10 are the line outs, which work for re-amping or outboard gear. The master goes to whichever output pair you choose (1/2 by default).
- **Sends**: every audio and MIDI track has one send knob per return track (A, B, …), post-fader by default.
- **Plug-in delay compensation** is automatic, including in exports.

## Export

**File > Export Mix / Stems** (Cmd/Ctrl+Shift+E). You can export the whole song or the loop region, as 16-bit, 24-bit, or 32-bit float WAV, with a tail. Stems are post-fader, one per track and return, and they line up sample-for-sample with the mix. Silent stems are skipped. Files go to `<project>/Exports/<name> <date>/` unless you choose another folder.

## Keys

| | |
|---|---|
| Space | Play / stop. Stop twice to return to the start. |
| R | Record |
| C | Metronome |
| L | Loop on/off (drag in the top strip of the ruler to set the loop) |
| Tab | Arrangement / mixer |
| Cmd/Ctrl+B | Plug-in browser |
| Cmd/Ctrl+G, +Shift+G | Group the selected tracks / ungroup |
| Cmd/Ctrl+T, +Shift+T, +Alt+T | Add audio / MIDI / return track |
| Cmd/Ctrl+D | Duplicate clip |
| Cmd/Ctrl+E | Split clip at the start marker |
| Delete | Delete clip (or notes in the piano roll) |
| Cmd/Ctrl+Z, Shift+Z | Undo / redo for clip and note edits |
| Cmd/Ctrl+scroll | Zoom. Shift+scroll scrolls sideways. Hold Alt while dragging to ignore the grid. |
| Piano roll | Double-click to add or delete a note, drag to move, drag a note's end to resize, drag on empty space to select, ↑↓ to transpose (Shift moves an octave), Q to quantize |

## Project files

A project is a folder:

```
Song/
  Song.lathe          the project (JSON, readable and diff-friendly)
  Song.lathe.bak      the previous save
  Audio/              recordings and imported audio (WAV at the project rate)
  Exports/
```

`.lathe` files are plain JSON. They hold tempo, time signature, loop, tracks (input, output, sends, fader, pan, mute, solo, arm, monitor), plug-ins (VST3 description plus base64 state), and clips. Audio paths are relative to the project folder, so you can move or copy the folder between your Mac and the Windows machine. Plug-ins are matched by identity, not by file path. A plug-in that isn't installed on the current machine keeps its saved settings and comes back when it's installed. Templates use the same format without clips.

Saves are atomic: the file is written beside the old one, then swapped in. While a saved project has unsaved changes, Lathe also writes `Song.lathe-autosave` every minute. If Lathe quits unexpectedly, it offers to recover those changes the next time you open the project.

## Deliberately left out

These were left out to keep Lathe small and predictable. Some could be added later if you miss them.

- Time-stretching and warping. Audio plays at its recorded speed, the tempo is fixed per project, and changing the tempo keeps audio clips the same length in seconds.
- Automation lanes, tempo changes, punch-in, and loop/comping record. While recording, the loop is ignored and playback runs straight through.
- Undo only covers clip and note edits. Track, plug-in, and routing changes aren't undoable, and deleting a track that has clips asks first.
- Scanning is crash-proof, but loaded plug-ins still run inside Lathe's process (like in most DAWs). A plug-in that crashes while it's playing will take Lathe down with it.
- Offline export can't capture external hardware synths driven by MIDI out. Record their audio onto a track first.
- 16-bit export is truncated, not dithered.

## Code map

```
Source/
  Model/Edit.*          project model, clip split/trim/clear, undo, JSON
  Engine/Engine.*       audio callback, render snapshot, PDC, recording, metronome, export
  Engine/PluginHost.*   VST3 loading, background scan through LatheScanner, skipped list, plug-in menu
  Scanner/              LatheScanner: describes one plug-in in its own process
  Engine/PluginStore.*  GitHub releases: catalog, lookup + cache, download, unpack, install, staged updates
  Engine/AudioPool.*    memory-mapped WAV playback, thumbnails, import/convert
  UI/                   transport, arrangement, mixer, device chain, piano roll, dialogs
Tests/EngineTests.cpp   headless engine checks
```

The audio thread never takes a blocking lock and never allocates. After any structural change, the message thread builds a new immutable render snapshot and swaps it in. Faders, mute, solo, arm, monitor, sends, and bypass are atomics, which the audio thread reads live.
