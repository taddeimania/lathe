# Recording and mixing

## Recording

- **Sample rate**: new projects record at **48 kHz** unless you change **New projects record at** in Options > Audio/MIDI Settings (44.1, 48, 88.2 or 96 kHz, or whatever the interface is set to). 96 kHz files are twice the size, which adds up in the cloud. A project you open always runs at its own rate (Lathe switches the interface to it), and you can still change the rate for the open project in the same window.
- Arm tracks with the round button. **R** or the record button starts recording from the start marker.
- If nothing is armed, record arms the selected track.
- **Count-in**: click the arrow next to **Click** (or right-click the record button) and choose 1, 2 or 4 bars. When it's on, the record button shows the number of bars. Pressing record from a stop then clicks that many bars (accent on each downbeat, whether or not the metronome is on) and records from the downbeat. The counter shows the beats left, and armed inputs are monitored so you can play along. Recording while playing punches in at once. Stop (or record again) during the count-in cancels it.
- Takes are shifted by the round-trip latency the driver reports, plus any plug-in latency, so they land in time. If your setup still lands early or late, adjust **Recording offset** in Settings. To measure it, record a click through a cable from an output back into an input.
- A new take replaces whatever was underneath it on that track. Undo (Cmd/Ctrl+Z) brings the old material back.
- MIDI tracks record notes, CC, and pitch bend from any MIDI input (or the one you pick). To play hardware from a MIDI track, set **MIDI to** in the mixer, for example to your interface's MIDI OUT.
- **Recording one track into another** (an instrument with its effects, played live, as audio):
  1. Put the instrument and any effects on a MIDI track. Leave it unarmed; it doesn't need clips (a generator that plays by itself works too).
  2. On an audio track, open the input box and pick that track under **From a track** (e.g. "From MIDI 1").
  3. Arm the audio track. The MIDI track now plays your keyboard live, and you hear it through the audio track (not twice), so the audio track's fader sets how loud it is.
  4. Record. The audio track gets what the instrument played after its plug-ins and EQ (before its fader, mute and solo), in stereo, lined up the way a MIDI recording is.
  Arm the MIDI track too if you also want the notes. Any audio or MIDI track can be the source, so you can resample an audio track through its effects the same way.
- **Recording what a plug-in generates** (an arpeggiator, or a generator like Dicey):
  - **On its own track:** put the generator before the instrument on a MIDI track, arm it, and record. Lathe records the notes the generator hands to the instrument (not what you play into it), exactly in time. Afterwards, bypass the generator (the status bar reminds you), or it plays along with the new clip.
  - **From a track:** on another MIDI track, pick the generator's track under **From a track** in the input box, put your instrument there, arm it and record. That track plays and records the generator's notes live, and the generator's track stays as it is, so nothing doubles on playback.

## Track groups

Select tracks (Cmd/Ctrl-click or Shift-click their names), then press **Cmd/Ctrl+G** or right-click → **Group**. A group gets a header row in the arrangement with:

- **Arm** arms every track in the group, or disarms them all if they're all armed.
- **S / M** solo or mute the whole group, on top of each track's own buttons. The members' own states are kept, so un-muting the group brings back exactly what was there before.
- **The triangle** collapses the group to its header, to save space on a big session.
- **FX** gives the group its own bus (a drum bus, say): a channel every track in the group plays through. It opens in the device panel, so you can add plug-ins (a compressor, a reverb), the channel EQ and a fader that process the whole group at once. Tracks that join the group later play through it too; tracks that leave go back to the master. The bus also shows in the mixer with the returns. Right-click the group header → **Remove Group Bus** to send the tracks straight to the master again.

Click a group's header to select all its tracks. Double-click its name to rename it, and right-click it for colour, arm all, collapse and **Ungroup** (Cmd/Ctrl+Shift+G), which keeps the tracks. In the mixer, members show the group's name above their own. Groups (and group buses) are saved in projects and templates. Without a bus a group only controls its tracks together; with one, their audio is mixed and processed together too.

Double-click any track or group name to rename it in place (Return keeps, Escape cancels). The scroll wheel scrolls the track list wherever the pointer is over the arrangement.

## Routing

- **Inputs**: any mono input (In 1, In 2, …) or stereo pair your interface has, or **another track** (see Recording).
- **Outputs**: each track goes to Master, to a return, or straight to an interface output, which works for re-amping or outboard gear. The master goes to whichever output pair you choose (1/2 by default).
- **Sends**: every audio and MIDI track has one send knob per return track (A, B, …), post-fader by default.
- **Plug-in delay compensation** is automatic, including in exports.

## Monitor mixes (in-ears, headphone feeds)

A monitor mix is a second mix of the same channels, on its own pair of outputs. For example, you can have hi-hats on the left in your in-ears while they sit on the right in the main mix.

- In the mixer, click **+ Monitor mix**. It starts as a copy of the main mix and goes to the first free output pair (3/4 on most interfaces, where your in-ear or headphone amp plugs in).
- Switch between **Main** and your monitor mixes with the tabs above the strips. In a monitor mix:
  - The faders, pans and mute buttons set that mix's own levels. The faders and the strip edges turn the mix's colour so you know which mix you're editing.
  - The master strip becomes the mix's volume, output pair and mute.
  - The main mix doesn't change.
- What a monitor mix hears:
  - Each channel after its plug-ins and EQ, and before its main fader. So the main fader, pan, mute and solo don't affect it.
  - Returns at their own level in the mix, fed by the main sends. Tracks routed into a return are heard through that return.
- **Click**: each monitor mix has its own metronome level. Turn off **Click in main** to hear the click only in your monitor mixes.
- Right-click a mix tab to rename it, copy the main mix's levels into it again, or delete it.
- Monitor mixes are saved with projects and templates. A tracking template can carry your in-ear mix.
- Monitor mixes are for listening only. They aren't exported.

## Channel EQ

Every track, return and the master has a built-in EQ after its plug-ins and before its fader. It has six bands: a low cut (24 dB/oct), a low shelf, two bells, a high shelf and a high cut.

- Each mixer strip shows the EQ curve. Click it to open the EQ in the device panel.
- **Drag** a numbered point to set its frequency and gain.
- **Scroll** (or Alt-drag) on a bell to set its width.
- **Double-click** a bell or shelf to set it back to 0 dB, or double-click a cut to turn it on or off.
- The power button bypasses the whole EQ. **Reset** flattens it.

## Export

**File > Export Mix / Stems** (Cmd/Ctrl+Shift+E). You can export the whole song or the loop region, as 16-bit, 24-bit, or 32-bit float WAV, with a tail. Stems are post-fader, one per track and return, and they line up sample-for-sample with the mix. Silent stems are skipped. Files go to `<project>/Exports/<name> <date>/` unless you choose another folder.
