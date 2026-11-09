Lathe is a small, dependable DAW. It opens, finds your audio interface, loads
a template, and records. It hosts VST3 plug-ins, has a mixer with return
tracks and sends, routes audio and MIDI to and from your interface, and exports
a mix and stems. It doesn't try to do everything else.

## New in 0.5.4

- **Notes work offline.** No internet (or the cloud is down)? Add notes, reply,
  resolve, edit and delete as usual. They show at once, faded until they're
  sent, and go to the cloud by themselves when it's reachable again (Lathe tries
  every 15 seconds), even if you quit and reopen in between.
- **You can see when notes aren't syncing.** The Notes button says **Notes
  offline** (hover it for why), the Notes window says how many changes are
  waiting, and the status bar tells you when it stops and starts again.
- Clearer messages when something isn't available on Lathe Cloud yet.
- Lathe now connects only to Lathe Cloud: **Cloud > Cloud Settings** is gone.

## New in 0.5.3

- **Fixed: File > Open Recent opened the wrong project** on the Mac (often the
  same one, whichever you picked). The menu now opens exactly the file it shows.
  New from Template and Delete Template had the same problem and are fixed too.
- **A tidier top bar.** Count-in moved into the metronome's menu: click the
  arrow next to **Click** (or right-click the record button) to choose Off, 1, 2
  or 4 bars. When count-in is on, the record button shows the number of bars.

## New in 0.5.2

- **Notes on tracks.** Right-click a track at a moment in the song: **Add Note
  Here** ("The bass part doesn't sit right here, can you rework it?"). It shows
  as a speech bubble on the track.
- **Song notes** for the whole song: the plan so far, what's left, anything
  useful.
- **Conversations.** Click a note to reply. Anyone can mark it **Resolved** (or
  reopen it); authors can edit and delete their own, and the song's owner can
  delete any.
- **Notes** in the top bar lists them all and lights up when someone left
  something new. Show every note on the tracks, hide resolved ones, or hide
  them all.
- Shared songs' notes appear for everyone within a minute, no Push needed.

## New in 0.5.1

- **Count-in.** Turn on **Count-in** next to the record button and pick 1, 2 or
  4 bars. Pressing record from a stop clicks that many bars first (even with the
  metronome off), then records from the downbeat, exactly in time. The counter
  shows the beats left; Stop cancels it.
- Fixed the compiler warnings in Lathe's own code on macOS.

## New in 0.5.0

- **Friends.** Cloud > Friends: send a request to someone's email. Once they
  accept, you can share with each other. You can only share with friends.
- **Projects hold songs.** A project is a group of songs (an album, a band).
  Cloud > Projects shows your projects, their songs, and songs shared with you.
  Songs you shared before 0.5 each become a project of their own, still shared
  with the same people.
- **Share a project or one song**, with each friend able to **edit** (push) or
  only **view** (pull). Viewers can still change their own copy; they just
  can't push.
- **The owner manages it:** stop sharing (with one person or everyone), change
  someone's access, rename (shares stay), move a song to another project (people
  who only had it through the old project lose it), or delete from the cloud.
  Anyone can leave.
- **Notices.** When you're given access, lose it, or become a viewer, Lathe
  tells you. Your copy on your computer is never touched.
- **Delete templates**, built-in ones too: File > New from Template > Delete
  Template. Restore Built-in Templates brings those back.
- Cloud > Put This Song in the Cloud replaces Share This Project.

## New in 0.4.5

- **Export specific tracks.** Right-click a track and choose **Export This
  Track...** (or **Export N Selected Tracks...**, or **Export This Group...** on a
  group header). There's also **File > Export Selected Tracks...**. The Export
  window now has a **Tracks** section: all tracks as you hear them, or only the
  ones you tick. Ticked tracks play even if they're muted or something else is
  soloed, and reverb returns keep what those tracks send them. The mix is named
  after what you picked, like "Song - Bass.wav", and each ticked track or return
  can get its own file too.

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
