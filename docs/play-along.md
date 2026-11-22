# Play Along

Tell Lathe what key the song is in, and **View > Play Along** shows you where those notes are on a guitar, bass, ukulele or keyboard, so you can jam along. When the key changes, the window changes with it.

## The key lane

![The key lane under the bar numbers: A minor pentatonic for four bars, then C major.](https://getlathe.app/img/daw/daw-key-lane.jpg)

The **Key** strip under the bar numbers holds the song's keys. Each key change starts at a bar and lasts until the next one. Before the first key change, the song has no key.

- **Set the key:** click the lane and pick a root, then a scale ("A" > "A minor pentatonic").
- **Change a key:** click its block, then pick a new **Root** or **Scale**.
- **Add a key change:** click a block where the new key should start and choose **Change key at bar N**. Key changes go at the start of the bar you click; hold **Alt** to put one on the grid instead.
- **Move a key change:** drag the start of its block.
- **Remove a key change:** click its block and choose **Remove this key change**. The key before it carries on.

The scales are major, minor, major pentatonic, minor pentatonic, blues, dorian, mixolydian and harmonic minor. Each root has its own colour. The colours go round the circle of fifths, so closely related keys (C and G, A minor and E minor) get neighbouring colours.

Key changes are part of undo, and Undo History names them ("Key at bar 17: C major"). They're saved in the project file, but templates leave them out. **View > Key Lane** hides the strip.

The AI assistant can set the keys for you. Ask it something like "work out the keys of this song for Play Along". It reads the notes of the MIDI tracks (and can convert audio to MIDI first), then fills in the lane. See [the AI assistant](ai-assistant.md).

## The Play Along window

**View > Play Along** opens a window you can keep beside Lathe.

![Play Along in A minor pentatonic on guitar and keyboard, half a bar before a change to C major: F and B, the new notes, are fading in as rings.](https://getlathe.app/img/daw/daw-play-along.jpg)

It shows:

- **The key now**, with the bar it started at, and the next key change, if there is one.
- **Each instrument you pick** (Guitar, Bass, Ukulele, Keyboard), stacked. Click the buttons at the top right to turn them on and off. Lathe remembers your choice and where the window was.

On each instrument, every note in the key is marked and named. The root is filled with the key's colour; the other notes are light dots ringed in it.

- **Guitar** is in standard tuning (E A D G B E), frets 0 to 15, with the low E string at the bottom, as in tab. Open strings are drawn left of the nut.
- **Bass** is a 4-string in standard tuning (E A D G), frets 0 to 15.
- **Ukulele** is in standard GCEA tuning, frets 0 to 12, with the G string at the bottom, as in ukulele tab.
- **Keyboard** shows two octaves from C3.

While the song plays, the window follows the playhead; when it's stopped, it follows the start marker. Click along the ruler to look at any part of the song.

In the last bar before a key change, the next key's new notes fade in as rings, and the notes that are leaving dim. That way the change doesn't catch you out on the downbeat.
