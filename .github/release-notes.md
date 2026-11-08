Lathe is a small, dependable DAW. It opens, finds your audio interface, loads
a template, and records. It hosts VST3 plug-ins, has a mixer with return
tracks and sends, routes audio and MIDI to and from your interface, and exports
a mix and stems. It doesn't try to do everything else.

## New in 0.4.4

- **Copy and paste clips.** Select a clip, press Cmd/Ctrl+C, click an empty
  spot on a track (that puts the start marker there), and press Cmd/Ctrl+V.
  The start marker moves past what you pasted, so pressing paste again lays
  copies end to end. Cmd/Ctrl+X cuts.
- **Select several clips.** Cmd/Ctrl- or Shift-click clips, drag a box around
  them on empty lane space, or press Cmd/Ctrl+A. Copy, paste, duplicate, split
  and delete work on all of them. Clips copied from several tracks paste in the
  same layout, starting from the selected track.
- **Join clips.** Cmd/Ctrl+J (or right-click > Join) turns the selected clips on
  a track into one clip. Gaps become silence. For audio, Lathe writes a new file
  that sounds exactly like the pieces did, clip gain included. Undo brings the
  pieces back.
- **You can see when you're recording.** The record button turns solid red and
  pulses, the counters turn red, and a red line runs under the transport. A red
  "Recording" clip grows on every armed track as you play.

## New in 0.4.3

- **Lathe tells you when there's an update.** A little after it opens (and then
  twice a day), Lathe asks GitHub for the latest release. If there's a newer
  one, a quiet orange **Update available** appears at the bottom right. No
  pop-ups. Click it to update with Lathe Patcher. Right-click it to see what's
  new, skip that version, or stop the checks. **Options > Check for Updates
  Automatically** turns the checks off or back on.

## New in 0.4.2

- **Group buses.** Click **FX** on a group's header (or right-click it → Add
  Plug-ins to the Whole Group) and the group gets its own bus: a channel every
  track in the group plays through. Put a compressor, a reverb, the channel EQ
  and a fader on it to process, say, all your drums at once. Tracks that join the
  group later go through it too.
- **Faster cloud transfers.** Up to 4 files (or pieces of a big file) upload and
  download at once, and a dropped connection is retried instead of failing the
  whole push. Change it under Cloud > Transfers at Once.
- **An app icon** for Lathe and Lathe Patcher.

## New in 0.4.1

- **Fixed: Lathe crashed a few seconds after opening a project whose sharing
  hadn't finished** (for example after quitting during a big upload), every time.
  If that happened to you, update and open it again: the status bar says "not
  uploaded yet", and Cloud > Push (finish sharing) uploads the rest without
  re-sending files that already went up.
- Cloud work now shows what it's doing and how far along it is in the status bar
  (checking audio files, uploading, downloading), and quitting in the middle asks
  first and stops cleanly.
- A dropped connection or timeout is now reported as an error instead of looking
  like a successful push.

## New in 0.4.0

- **Share projects through the cloud** (the new Cloud menu). Works like git, not
  like a shared document: everyone works offline, then pushes and pulls revisions.
  - **Share This Project** puts it online as r1; **Invite Someone** by email.
  - **Open Shared Project** downloads a project someone shared with you, audio included.
  - **Push** uploads a new revision with an automatic summary of what changed
    (faders, pans, sends, EQ bands, plug-in knobs, clips, tracks). Audio is only
    ever uploaded once.
  - **Pull** downloads collaborators' revisions without touching what you have open.
  - **History** lists every revision. Open any of them; what you had open is kept
    first as "Local", so nothing is ever lost.
  - If someone pushed before you, Lathe asks: pull theirs first, or push yours on
    top (theirs stays in History).
- The status bar shows which revision you're on, and tells you when a
  collaborator pushes a new one.

## New in 0.3.0

- **Lathe Patcher** (Options > Check for Updates): checks GitHub for a newer
  Lathe and installs it in place, so later updates don't need a manual
  download. This release has to be downloaded by hand one last time.

## New in 0.2.0

- **Monitor mixes** for in-ears and headphone feeds: a second mix of the same
  channels on its own output pair, with its own level, pan and mute per
  channel, its own click level, and an option to keep the click out of the
  main outputs. They're saved in projects and templates.
- **Channel EQ** on every track, return and the master: a 24 dB/oct low cut,
  a low shelf, two bells, a high shelf and a high cut, with a curve on each
  mixer strip.
- **Track groups** (Cmd/Ctrl+G): arm, solo and mute a group together, and
  fold it up in the arrangement.
- Double-click a track name to rename it. The scroll wheel scrolls the track
  list.
- Fixed: after File > New from Template, meters stayed empty and only one
  armed track recorded.

## Also in Lathe

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
