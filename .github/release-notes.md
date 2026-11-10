Lathe is a small, dependable DAW. It opens, finds your audio interface, loads
a template, and records. It hosts VST3 plug-ins, has a mixer with return
tracks and sends, routes audio and MIDI to and from your interface, and exports
a mix and stems. It doesn't try to do everything else.

## New in 0.10.0

- **Lathe comes with its own instruments.** Bass Synth (mono, with a sub and glide),
  Poly Synth (eight voices for pads, keys, brass and plucks) and Drums, with four
  synthesized kits: Simple 909, Simple 808, Simple 606 and Simple 707, on the usual drum
  notes.
- **And its own effects.** Reverb (small room, room, chamber, hall, plate, spring), Delay
  (digital, tape, analog, lo-fi, synced to the song), Compressor, Saturator (tape, tube,
  clip) and Mastering (five styles, four loudness targets, and Learn to set the levels
  for your mix).
- Every one has presets and the same simple panel. Nothing to install: they're built in,
  so a bandmate who opens your project has them too.
- Put Reverb or Delay on a return and it starts 100% wet.
- **Hear notes in the piano roll.** Click or drag the keyboard on the left to play a
  pitch through the track's instrument, and notes sound as you add, grab, drag and
  transpose them. No need to press play. Hear notes in the toolbar turns it off.
- **The plug-in browser** (Cmd/Ctrl+B) lists Lathe's own devices, then everything
  installed on your computer. It no longer downloads plug-ins.
- The AI assistant knows the built-in devices: "add a return with Reverb and send the
  drums to it" just works.
- **The AI assistant can write notes.** "Put a four-on-the-floor beat on a new 909 track"
  writes the clip, as one undo step.

## New in 0.9.1

- The AI assistant's CPU measurements report the computer, its cores, the interface and
  the buffer size. The power, Low Power Mode and temperature readings are gone for now.

## New in 0.9.0

- **Undo covers the whole session now**, not just clips and notes: adding, deleting and
  moving tracks and returns, groups, plug-ins (added, removed, reordered, bypassed),
  inputs, outputs, sends, names, tempo and time signature. A deleted track comes back
  with its plug-ins still loaded and set up. Faders, pans and send levels aren't part of
  undo yet: they stay where you left them.
- **Your AI assistant can build the session.** "Add a MIDI track with an arpeggiator into
  a synth, and a reverb return the drums send to" makes the tracks, loads the plug-ins in
  the right order, and sets the routing and sends. Everything from one
  request is a single undo step.
- **It can measure what your session costs.** It plays some bars and reports the CPU
  load (average, worst moments, missed deadlines), each track's own cost with its
  plug-ins, and the conditions (computer, cores, interface, buffer size).
  It can also compare 1 core with all cores on your own song.
- A new send to a return was silent until something else changed the session. Fixed.

## New in 0.8.0

- **Lathe uses all your cores.** Tracks and their plug-ins now play on several cores at
  once instead of one after another, so big sessions fit where one core used to run out.
  On an M1 Pro (6 performance cores), twelve heavy synths went from 42% of the audio
  deadline to 9%, and a 32-bar export from 16.3 s to 5.5 s. Nothing sounds different:
  the output is identical to the sample.
- **Options > Audio / MIDI Settings > Processing cores**: Auto (one per performance core) or
  a number. 1 plays one track at a time, as before, in case a plug-in misbehaves.
- **`Lathe --benchmark`** times the audio engine on one core and on all of them, with
  made-up sessions full of heavy plug-ins, and saves the results as a CSV. See
  docs/performance.md.

## New in 0.7.4

- **Your AI assistant can play exactly what you ask for.** "Play bars 9 to 13" stops
  right at bar 13, to the sample: Lathe stops by itself rather than waiting for the
  AI to say so.
- **It can set the loop and play it a number of times**: "loop bars 17 to 25 and play
  it three times" sets the loop brackets, plays three passes and stops at the loop end.
- `describe_session` shows where playback will stop, and how many loop passes are left.

## New in 0.7.3

- **The Mac version is signed and notarized by Apple.** Download it, unzip,
  and double-click to open.
- Downloads now come from [getlathe.app](https://getlathe.app) as well as here.
- The user guide is in the download, in the `docs` folder.

## New in 0.7.2

- **`Lathe --screenshots <folder>`** draws a made-up demo song's windows (arrangement,
  an open note, mixer, History) to image files and quits, for the website. It
  doesn't touch your settings, audio device or plug-ins.
- Fixed: notes and History said "1 days ago". They say "yesterday" now.

## New in 0.7.1

- **Lathe picks your audio interface on first run**, whichever brand it is: the
  device with the most inputs (ASIO on Windows, Core Audio on the Mac), with all
  its channels on. Before, it only looked for one maker's interfaces.
- The eight-input tracking template is now called **8-Input Tracking**.

## New in 0.7.0

- **AI assistant connection (MCP).** Let an AI app on your computer, like Claude
  Desktop or Claude Code, look at your project and play or stop it: ask "why
  can't I hear MIDI 2?" or "what does the bass play in bar 5?". It sees tracks,
  routing, monitoring, plug-ins (and which ones generate notes), clips and notes,
  and points out likely reasons a track is silent. It can't change the project
  yet. Turn it on in **Options > AI Assistant...**, which also shows the setup to
  copy into Claude Desktop or Claude Code. It's off until you turn it on, only
  listens on this computer, and needs a secret key that Lathe makes fresh each
  time.

## New in 0.6.1

- **Record the notes a plug-in generates** (an arpeggiator, or a generator like
  a note generator). Arm its MIDI track and record: Lathe records the notes the generator
  hands to the instrument, exactly in time (then bypass the generator so it
  doesn't play along with the clip). Or pick its track under **From a track** in
  another MIDI track's input box, and record there.

## New in 0.6.0

Everyone sharing songs should update: songs pushed from 0.6.0 can't be pulled by
older versions of Lathe.

- **Takes go up compressed**, losslessly (FLAC): about half the size, so pushes
  and pulls take about half as long. What you pull back is identical, byte for
  byte, to what was recorded, and every download is checked.
- **Plans.** Free accounts can join songs shared with them (and push, as
  editors). Starter: your own songs in the cloud, up to 5 GB each, 50 GB in all.
  Pro: 25 GB songs, 250 GB in all. Your plan shows in the Cloud menu.
- **The cloud keeps each song's newest 5 revisions** (20 on Pro). In History,
  **Keep** a revision (a mix you sent, say) and it stays however many come after.
  Revisions the cloud let go stay in History on your computer.

## New in 0.5.7

- **Fixed: New Project could crash on the Mac** when your audio input and output
  are different devices (for example the interface's inputs and the Mac's
  speakers). Switching the sample rate (new projects use 48 kHz, and opening a
  project uses its own rate) now closes the audio device and opens it again at
  the new rate, instead of changing it while it runs.

## New in 0.5.6

- **New projects record at 48 kHz.** Before, a new project used whatever rate
  the interface was last at, often 96 kHz, whose files are twice the size. Change
  it in Options > Audio/MIDI Settings > **New projects record at**. Projects you
  already have keep their own rate.

## New in 0.5.5

- **Record a track into an audio track.** An audio track's input can now be
  another track (**From a track**, at the top of the input box). Put an
  instrument and its effects on a MIDI track, pick it as an audio track's input,
  arm the audio track and play: you record the instrument's sound, live, as
  audio. The instrument plays your keyboard without being armed itself, and you
  hear it once, through the audio track.
- Fixed: after **New Project** from a big, scrolled-down project, the new
  project's track names didn't show until you clicked somewhere.
- Lathe now connects only to Lathe Cloud: **Cloud > Cloud Settings** is gone.
- Clearer messages when something isn't available on Lathe Cloud yet.

## New in 0.5.4

- **Notes work offline.** No internet (or the cloud is down)? Add notes, reply,
  resolve, edit and delete as usual. They show at once, faded until they're
  sent, and go to the cloud by themselves when it's reachable again (Lathe tries
  every 15 seconds), even if you quit and reopen in between.
- **You can see when notes aren't syncing.** The Notes button says **Notes
  offline** (hover it for why), the Notes window says how many changes are
  waiting, and the status bar tells you when it stops and starts again.

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

**macOS**: unzip and move `Lathe.app` to Applications, then double-click it.
Allow microphone access when asked, or nothing will record.

**Windows**: unzip anywhere and keep `Lathe.exe` and `LatheScanner.exe`
together in the same folder. Install your audio interface's driver from its
maker first and choose its ASIO driver in **Options > Audio / MIDI Settings**. SmartScreen may warn the first time: **More info > Run anyway**.

**Linux**: unpack and run `./Lathe`, keeping `LatheScanner` next to it.
