# Code map

```
Source/
  Model/Edit.*          project model, clip split/trim/clear, undo (with step labels and who), JSON, monitor (cue) mixes
  Model/ChannelEq.*     the built-in channel EQ: settings, biquads, response curve
  Engine/Engine.*       audio callback, render snapshot, PDC, recording, metronome, export
  Engine/Mp3Writer.*    MP3 export through LAME (fetched by CMake, cmake/lame/config.h)
  Cloud/Milestones.*    the opt-in, anonymous one-time milestones (asked once, MilestonesPanel in UI/Dialogs.cpp)
  Engine/PluginHost.*   VST3 loading, background scan through LatheScanner, skipped list, plug-in menu
  Scanner/              LatheScanner: describes one plug-in in its own process
  Host/                 "Run separately": RemotePlugin (Lathe's side, an AudioPluginInstance), HostMain (LatheHost,
                        runs the plug-in), RemoteShared (the shared audio block and semaphore)
  Demo/                 the "Night Drive" demo song and pretend audio interface, for --screenshots; Welcome.*
                        writes "Welcome to Lathe" (Resources/Welcome.lathe), the song a new install opens
  Assistant/            AI assistant (MCP): Server (127.0.0.1 HTTP, key, JSON-RPC), Tools (what the AI can
                        read and do), MixAnalysis (what analyze_mix measures in an offline render), Service
                        (on/off, message thread), BridgeMain (LatheMCP, stdio <-> Lathe)
  Cloud/                cloud sharing: CloudClient (sign-in, API, uploads), Revisions (local history,
                        snapshots, push / pull), ProjectDiff (change summaries)
  Patcher/              Lathe Patcher: Updater.* (GitHub lookup, download, verify, swap in place) + its window
  Devices/              Lathe's built-in devices: Device.* (base class, shared panel, catalogue), Effects.cpp
                        (Reverb, Delay, Compressor, Saturator, Mastering), and their engines in
                        Reverb/, Delay/ and Master/; Instruments.* (Bass Synth, Poly Synth, Drums);
                        Mutable.* (Multi Synth, Resonator, Grains, Random Notes, and CoreRate, which
                        runs a fixed-rate core at any sample rate)
ThirdParty/mutable/     the vendored MIT DSP behind those four (see its README for source and changes)
  Engine/AudioPool.*    memory-mapped WAV playback, thumbnails, import/convert
  UI/CloudLinks.*       invite links and listening links (pages on getlathe.app), Cloud menu
  UI/UndoHistory.*      Edit > Undo History: every undo step, who made it (you or the assistant), click to go back
  Compare/Compare.*     View > Compare: renders Before/Now, reads a reference, measures them, plays them in step
  UI/CompareWindow.*    its window
  UI/                   transport, arrangement, mixer (+ mix tabs), device chain, EQ editor, piano roll, dialogs
Tests/EngineTests.cpp   headless engine checks
```

The audio thread never takes a blocking lock and never allocates. After any structural change, the message thread builds a new immutable render snapshot and swaps it in. Faders, mute, solo, arm, monitor, sends, and bypass are atomics, which the audio thread reads live.
