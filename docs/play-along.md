# Play Along

Tell Lathe what key the song is in, and **View > Play Along** shows you where those notes are on a guitar, bass, ukulele or keyboard, so you can jam along. When the key changes, the window changes with it.

## The key lane

![The key lane under the bar numbers: A minor pentatonic for four bars, then C major. The Bass track has its own key, A dorian from bar 3, on a strip at the bottom of its row.](https://getlathe.app/img/daw/daw-key-lane.jpg)

The **Key** strip under the bar numbers holds the song's keys. Each key change starts at a bar and lasts until the next one. Before the first key change, the song has no key.

- **Set the key:** click the lane and pick a root, then a scale ("A" > "A minor pentatonic").
- **Change a key:** click its block, then pick a new **Root** or **Scale**.
- **Add a key change:** click a block where the new key should start and choose **Change key at bar N**. Key changes go at the start of the bar you click; hold **Alt** to put one on the grid instead.
- **Move a key change:** drag the start of its block.
- **Remove a key change:** click its block and choose **Remove this key change**. The key before it carries on.

The scales are major, minor, major pentatonic, minor pentatonic, blues, dorian, mixolydian and harmonic minor. Each root has its own colour. The colours go round the circle of fifths, so closely related keys (C and G, A minor and E minor) get neighbouring colours.

Key changes are part of undo, and Undo History names them ("Key at bar 17: C major"). They're saved in the project file, but templates leave them out. **View > Key Lane** hides the strip.

## A track with its own key

Usually the whole band is in the song's key, even when the bass plays only roots. But a part can be in a different key or mode: a solo in D dorian over a D minor song, or a part written in another key on purpose. For that, a track can have keys of its own.

- **Give a track its own key:** right-click its header and choose **Own Key from Bar N** (the bar of the start marker). It starts as the song's key there, on a thin strip at the bottom of the track. Click the strip to change it or add key changes, and drag the start of one to move it, as on the song's lane.
- Until its first key change, the track follows the song's key. From then on it follows only its own, so the song's later key changes don't apply to it.
- **Back to the song's key:** right-click the header and choose **Follow the Song's Key**, or pick it from the strip's menu.

A track's keys are part of undo ("Key at bar 9 on "Bass": D dorian") and are saved with the track. Templates leave them out.

The AI assistant can set the keys for you. Ask it something like "work out the keys of this song for Play Along". It reads the notes of the MIDI tracks (and can convert audio to MIDI first), then fills in the lane. See [the AI assistant](ai-assistant.md).

## The Play Along window

**View > Play Along** opens a window you can keep beside Lathe.

![Play Along half a bar before the song changes to C major: the guitar shows A minor pentatonic with F and B fading in as rings, and the bass follows the Bass track's own key, A dorian.](https://getlathe.app/img/daw/daw-play-along.jpg)

It shows:

- **The key now**, with the bar it started at, and the next key change, if there is one.
- **Each instrument you pick** (Guitar, Bass, Ukulele, Keyboard), stacked. Click the buttons at the top right to turn them on and off. Lathe remembers your choice and where the window was.
- **Whose key each instrument shows**, once any track has a key of its own. The menu beside each instrument says: **Automatic** follows a track named after the instrument ("Bass" or "Bass DI" for Bass; "Guitar", "Gtr 2" for Guitar; "Uke"; "Keys" or "Piano" for Keyboard) if it has a key of its own, and the song's key otherwise. You can also pick **Song's key**, or any track with a key of its own. When an instrument follows a track, its key is shown beside the menu.

On each instrument, every note in the key is marked and named. The root is filled with the key's colour; the other notes are light dots ringed in it.

- **Guitar** is in standard tuning (E A D G B E), frets 0 to 15, with the low E string at the bottom, as in tab. Open strings are drawn left of the nut.
- **Bass** is a 4-string in standard tuning (E A D G), frets 0 to 15.
- **Ukulele** is in standard GCEA tuning, frets 0 to 12, with the G string at the bottom, as in ukulele tab.
- **Keyboard** shows two octaves from C3.

While the song plays, the window follows the playhead; when it's stopped, it follows the start marker. Click along the ruler to look at any part of the song.

In the last bar before a key change, the next key's new notes fade in as rings, and the notes that are leaving dim. That way the change doesn't catch you out on the downbeat.
