# MIDI and the piano roll

Write and edit notes in the piano roll, bring in MIDI files from other programs and sample libraries, and send your parts back out.

## A MIDI track

A MIDI track's notes play the instrument in its device panel: one of Lathe's own (see [Instruments and effects](devices.md)) or a VST3 instrument. A note generator, like an arpeggiator or Random Notes, goes before the instrument it drives.

To play a hardware synth instead, set **MIDI to** in the track's mixer strip to the MIDI output it's connected to.

To get notes onto a track you can record them ([Recording](recording.md#midi)), draw them in the piano roll, import a MIDI file, or ask the [AI assistant](ai-assistant.md) to write them.

## The piano roll

Double-click a MIDI clip (or right-click it > **Edit notes...**) to open it. **Escape** closes it.

![The piano roll on the lead line, with the keyboard on the left, the notes in the middle and the velocity lane underneath.](https://getlathe.app/img/daw/daw-pianoroll.jpg)

### Writing notes

| | |
|---|---|
| Double-click empty space | Add a note. Keep the button down and drag to set how long it is. |
| **Draw** (B) | Click or drag on empty space to add notes, without double-clicking |
| Double-click a note | Delete it |

A new note is as long as the last one you drew or resized (one grid step at first).

### Selecting

| | |
|---|---|
| Click a note | Select it. Shift-click adds or removes one. |
| Drag on empty space | Select the notes in the box (with Shift, add them) |
| Click a key on the keyboard | Hear it and select every note of that pitch (with Shift, add them) |
| Cmd/Ctrl+A | Select every note in the clip |

Selected notes are lit with a light outline, and the rest dim a little.

### Editing notes

| | |
|---|---|
| Drag a note | Move it (and the rest of the selection) |
| Alt-drag a note | Copy it: the copies move and the notes stay |
| Drag a note's left or right end | Make it start earlier or later, or end earlier or later |
| ↑ ↓ | Move the selected notes up or down a semitone. With Shift, an octave. |
| ← → | Move them earlier or later by a grid step. With Alt, by a 1/128 note. |
| Shift+← → | Make them shorter or longer by a grid step |
| Cmd/Ctrl+C, X, V | Copy, cut and paste notes |
| Cmd/Ctrl+D | Duplicate the selected notes just after themselves |
| Cmd/Ctrl+E | Split the selected notes at every grid line |
| Cmd/Ctrl+J | Glue: join the selected notes of each pitch into one |
| Q | Quantize the selected notes (or all of them) |
| Delete | Delete the selected notes |
| Cmd/Ctrl while dragging | Ignore the grid |

Paste puts the notes at the start marker when it's inside the clip, and moves the marker on past them so pasting again adds the next copy. While the piano roll has the keys, the **Edit** menu's commands (Cut, Copy, Paste, Duplicate, Split, Glue, Delete, Select All) act on notes rather than clips.

Right-click a note or empty space for the same commands, and for **Legato** (each note runs on to the next), **Reverse** (play the selection backwards), **Invert** (turn it upside down, the lowest note swapping with the highest) and **Select All** of one pitch. Every edit is one undo step.

### The grid and quantize

The **grid** in the toolbar sets the step for drawing, moving, nudging and quantizing: a bar, 1/2 to 1/64, 1/8 and 1/16 triplets, **Adaptive** (finer as you zoom in), or **Off**. Lathe remembers it.

**Quantize** (or Q) moves notes to the grid. The **...** button beside it sets how:

- **Strength:** how far the notes move towards the grid, from 25% (a little tighter, still human) to 100%.
- **Swing:** pushes every other grid step later. At 100%, straight eighths (on a 1/8 grid) play as a triplet's long-short.
- **Move note ends too:** on, the ends snap to the grid as well; off, the notes keep their lengths.

### Finding your way

The ruler above the notes counts the song's bars. Click it to move the start marker there (and the playhead); drag the clip's end in the ruler to make the clip longer or shorter, up to the next clip on the track.

| | |
|---|---|
| Cmd/Ctrl+scroll, or pinch | Zoom in and out in time |
| Alt+scroll | Taller or shorter rows |
| Z | Zoom to fit the selected notes (or all of them) |
| Shift+scroll | Scroll sideways |

The line at the top of the piano roll shows the note under the mouse, or the one you're dragging: its pitch, where it starts (bar.beat.sixteenth), its length and velocity. When the song has a key (or the track has its own), rows outside the key are darker and the root's rows are tinted, so you can see which notes fit.

### Velocity

The lane under the notes shows how hard each note is played. Louder notes are drawn brighter.

- **Drag a note's stem** up or down to change it. With several notes selected, they all move by the same amount, so the louder ones stay louder.
- **Drag across empty space in the lane** to draw a line: the notes under it (the selected ones, or all of them) ramp from where you started to where you let go. A crescendo in one stroke.

### Hearing notes

Click or drag the keyboard on the left to hear a pitch. Notes also sound through the track's instrument as you add, grab, drag and transpose them. **Hear notes** in the toolbar turns that off.

Notes are named with C3 as middle C.

## MIDI files

### Importing

Drag a MIDI file (.mid) onto the arrangement, or choose **File > Import MIDI File...**:

- **Onto a MIDI track:** the whole file becomes one clip on that track, where you dropped it (or, from the menu, at the start marker on the selected track). Anything already there in those bars is replaced. This suits a drum groove from a library, even one spread over several tracks of the file. Several files dropped together play one after another.
- **Anywhere else:** each part of the file (a track of it, or a channel of a one-track file) gets its own new MIDI track, named after it. New tracks have no instrument yet: add one to hear them.

Clips run to the end of the bar their last note ends in. Notes keep their place in beats, so the file plays at the song's tempo; if the file was written at another tempo, the status bar says so. Notes, velocities, controllers, pitch bend and aftertouch come in. An import is one undo step.

### Exporting

To take MIDI the other way, out to another program or to someone else:

- **File > Export MIDI File...** saves every MIDI track that has notes, one track each in the file, from the start of the song.
- **Right-click a MIDI clip > Export as MIDI File...** saves that clip, or every selected MIDI clip, from the bar the first one starts in.
- **Drag MIDI clips out of Lathe's window**, to the Finder or Explorer or into another program, to drop them there as a .mid file. The clips stay where they were.

The file has the song's tempo and time signature, each track's name and MIDI channel, and the notes, controllers and pitch bend the clips play. A clip's hidden notes past its end are left out. A MIDI file holds notes, not sound: to send the sound, [export audio](exporting.md).

## Converting audio to MIDI

Turn a recording into notes: right-click an audio clip and choose **Convert to MIDI**, then what's on it:

- **Melody**, for one note at a time: a voice, a bass, a lead line. Each note gets its pitch, where it starts, how long it is and how loud it was. Slides and vibrato are held as one note at the nearest pitch.
- **Drums**, for a drum part: each hit becomes a kick, a snare or a closed hi-hat (General MIDI's notes 36, 38 and 42, which EZdrummer and other drum plug-ins follow). Crashes and rides come out as hi-hat notes too. A hat or cymbal played on a kick gives both notes. A whole kit works, close mics and overheads together (a [bounced drum group](arranging.md#bounce-a-group-to-one-audio-track), say): the ring of the drums and the room isn't taken for hits, and soft notes are kept when they're struck, not when they're the tail of the last hit.
- **One Drum**, for a track of a single drum (a kick mic, a snare mic): every hit becomes the same note.

The notes go on a new MIDI track right under the audio track, named after it ("Vocal MIDI"), one MIDI clip for each audio clip at the same place. Select several clips on a track first to convert them all. The audio is left as it is. Add an instrument to the new track to hear it: a drum plug-in for drums. It's one undo step.

Timing is kept as played, not snapped to the grid, and softer notes get lower velocities.

> **Note:** Convert to MIDI hears one note at a time, so chords (a strummed guitar, a piano) don't convert. On drums, toms come out as a kick or a snare, and a cymbal hit at the same moment as the snare is usually missed: the snare's wires sound like it. A drum recording converts well; a full band mix doesn't.

The AI assistant can convert a track too (`convert_to_midi`), and then read and rework the notes.

## Writing parts in text

The AI assistant writes and reads parts in a short text form, a drum grid or a line of notes per bar. You can read the same form: see [Note notation](notation.md).
