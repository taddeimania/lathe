# Code map

```
Source/
  Model/Edit.*          project model, clip split/trim/clear, undo, JSON, monitor (cue) mixes
  Model/ChannelEq.*     the built-in channel EQ: settings, biquads, response curve
  Engine/Engine.*       audio callback, render snapshot, PDC, recording, metronome, export
  Engine/PluginHost.*   VST3 loading, background scan through LatheScanner, skipped list, plug-in menu
  Scanner/              LatheScanner: describes one plug-in in its own process
  Demo/                 the "Night Drive" demo song and pretend audio interface, for --screenshots
  Assistant/            AI assistant (MCP): Server (127.0.0.1 HTTP, key, JSON-RPC), Tools (what the AI can
                        read and do), Service (on/off, message thread), BridgeMain (LatheMCP, stdio <-> Lathe)
  Cloud/                cloud sharing: CloudClient (sign-in, API, uploads), Revisions (local history,
                        snapshots, push / pull), ProjectDiff (change summaries)
  Patcher/              Lathe Patcher: Updater.* (GitHub lookup, download, verify, swap in place) + its window
  Engine/PluginStore.*  GitHub releases: catalog, lookup + cache, download, unpack, install, staged updates
  Engine/AudioPool.*    memory-mapped WAV playback, thumbnails, import/convert
  UI/                   transport, arrangement, mixer (+ mix tabs), device chain, EQ editor, piano roll, dialogs
Tests/EngineTests.cpp   headless engine checks
```

The audio thread never takes a blocking lock and never allocates. After any structural change, the message thread builds a new immutable render snapshot and swaps it in. Faders, mute, solo, arm, monitor, sends, and bypass are atomics, which the audio thread reads live.
