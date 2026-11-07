Lathe is a small, dependable DAW. It opens, finds your audio interface, loads
a template, and records. It hosts VST3 plug-ins, has a mixer with return
tracks and sends, routes audio and MIDI to and from your interface, and exports
a mix and stems. It doesn't try to do everything else.

## What's in 0.1.0

- **Arrangement**: audio and MIDI tracks, clips with waveforms, a snap grid,
  a loop region, split, duplicate and trim, and drag-and-drop audio import.
- **Recording** on any number of armed tracks. Takes are lined up
  automatically using the latency your driver reports, plus plug-in latency.
- **Mixer**: per-track input and output routing (master, return tracks or any
  interface output), monitor Auto/In/Off, inserts, sends, pan, faders and
  meters.
- **VST3 hosting** with automatic plug-in delay compensation. Plug-ins are
  scanned in a separate helper process, so one that crashes or hangs is
  skipped instead of taking Lathe down.
- **Plug-in browser** (Cmd/Ctrl+B): installs and updates the taddeimania
  plug-ins straight from their GitHub releases.
- **MIDI**: record from any controller, edit in the piano roll, play VST3
  instruments or send to hardware.
- **Export** a mix and/or post-fader stems as 16-bit, 24-bit or 32-bit float
  WAV.
- **Templates**, plain-JSON project files with relative audio paths, atomic
  saves, and autosave with recovery.

## Install

**macOS**: unzip and move `Lathe.app` to Applications. The app isn't
notarized, so the first time, right-click it and choose **Open** (or run
`xattr -dr com.apple.quarantine /Applications/Lathe.app`). Allow microphone
access when asked, or nothing will record.

**Windows**: unzip anywhere and keep `Lathe.exe` and `LatheScanner.exe`
together in the same folder. For the Behringer UMC1820, install its driver from
behringer.com first and choose the ASIO driver in **Options > Audio / MIDI
Settings**. SmartScreen may warn the first time: **More info > Run anyway**.

**Linux**: unpack and run `./Lathe`, keeping `LatheScanner` next to it.
