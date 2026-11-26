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

![Play Along half a bar before the song changes to C major: the guitar shows A minor pentatonic with F and B fading in as rings, and the C major card is outlined in its colour, two beats away.](https://getlathe.app/img/daw/daw-play-along.jpg)

At the top:

- **Where you are:** the bar at the playhead while the song plays, or at the start marker while it's stopped.
- **The key now**, with the bar it started at and its notes (the root in the key's colour).
- **What's next:** the next key change and its bar, and how far off it is ("in 3 bars", "in 2 beats"). In the last bar before it, the card is outlined in the new key's colour and a bar fills as the change nears.
- **The buttons at the top right:** pick one instrument (**Guitar**, **Bass**, **Ukulele** or **Keyboard**; click it again to hide it), and turn **Chords** on or off beside it. Lathe remembers your choice and where the window was. If what's on doesn't fit, the window scrolls.

The instrument is drawn with its tuning above it. Every note in the key is marked and named. The root is filled with the key's colour; the other notes are light dots ringed in it.

- **Guitar** is in standard tuning (E A D G B E), frets 0 to 15, with the low E string at the bottom, as in tab. Open strings are drawn left of the nut.
- **Bass** is a 4-string in standard tuning (E A D G), frets 0 to 15.
- **Ukulele** is in standard GCEA tuning, frets 0 to 12, with the G string at the bottom, as in ukulele tab.
- **Keyboard** shows two octaves from C3.

In a seven-note key, each letter comes once, as the key is written: C# in D harmonic minor, not Db.

**Whose key the instrument and the chords show**, once any track has a key of its own: the menu beside each says. **Automatic** follows a track named after the instrument ("Bass" or "Bass DI" for Bass; "Guitar", "Gtr 2" for Guitar; "Uke"; "Keys" or "Piano" for Keyboard; for Chords, one named after the instrument shown) if it has a key of its own, and the song's key otherwise. You can also pick **Song's key**, or any track with a key of its own. When one follows a track, its key is shown beside the menu.

While the song plays, the window follows the playhead; when it's stopped, it follows the start marker. Click along the ruler to look at any part of the song.

In the last bar before a key change, the next key's new notes fade in as rings, and the notes that are leaving dim. That way the change doesn't catch you out on the downbeat.

## The chord sheet

Turn on **Chords** to see the chords that belong to the key, and ways to play them on the instrument you've picked (on a guitar if none is picked).

![The chord sheet in A minor pentatonic, with Am7 picked: its open voicing is outlined on the guitar, and each chord shows what it becomes in C major, the key coming up.](https://getlathe.app/img/daw/daw-play-along-chords.jpg)

- **The key's chords**, one for each step of the scale, each in its root's colour, with its numeral: I, ii, iii, IV, V, vi and vii° in a major key; i, ii°, bIII, iv, v, bVI and bVII in a minor one. The numerals are counted from the major scale, so a flat means the step is a semitone lower than in major. The modes and harmonic minor get their own chords: D dorian has a major IV (G), E mixolydian a bVII (D), harmonic minor a major V and a vii°7.
- **Pentatonic keys** use the chords of their parent scale, major or minor. **Blues** uses the minor chords and adds the dominant 7ths a blues is played on (I7, IV7 and V7).
- **Click a chord** to pick it. The row below it has its variants: the triad, the 7th, sus2 and sus4 (when their notes are in the key) and the power chord (5). Picking a variant changes every chord in the row, so you can read the key's 7ths at a glance. On the right are the chord's notes and what each is in the chord (R, b3, 5, b7...).
- **Voicings** are drawn for the instrument shown. On guitar, bass and ukulele they're chord diagrams: strings upright (the guitar's low E on the left, the ukulele's G, as chord charts draw them), a cross over a string you leave out, a ring over one you play open, and the fret number beside the first fret shown. Each dot names its note in the chord, and a bar across the strings is a barre. Scroll sideways for more, and click one to pick it.
  - **Guitar:** **Open and barre** has the open chord and the barre shapes up the neck (the E, A, C and D shapes and their minor and 7th versions); **Up the neck** has triads on three strings in each inversion, or four-note shapes for 7ths.
  - **Ukulele:** shapes on all four strings (a power chord can leave some out): open chords first, then up the neck.
  - **Bass:** the root on the lowest string you play, with the 3rd (and the 7th, for 7ths) above it, open and up the neck.
  - **Keyboard:** the chord close together in each inversion, then **Spread**, with the root an octave below the rest, all within the two octaves Play Along draws. Each is drawn as keys, with its notes underneath ("G3 B3 D4 F4").
- **On the instrument**, the voicing you picked is outlined (with crosses on the strings it leaves out), and the rest of the key steps back.

When the key changes, the sheet keeps what you picked by its place in the key: if you're playing the V7, you get the new key's V7, in the voicing nearest where your hand is. In the last bar before a key change, each chord shows the one it becomes in the next key ("Am7 → Cmaj7").

The chord sheet follows the song's key, or the own key of a track named after the instrument shown: pick another in its menu.
