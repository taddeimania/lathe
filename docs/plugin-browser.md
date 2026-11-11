# Built-in devices and the plug-in browser

Lathe comes with its own instruments, effects and a note generator, so a new project has synths, drums, a reverb, a delay and the rest before you've installed anything. They load like any plug-in (same chain, undo, saving and AI assistant tools) but nothing needs installing or scanning, and a bandmate who opens your Lathe Cloud project has them too.

| Device | What it does |
|---|---|
| **Bass Synth** | A mono synth for basslines: saw, square or triangle, a sub an octave down, detune and glide, a 24 dB filter with its own envelope and drive, and an amp envelope. |
| **Poly Synth** | Eight voices for pads, keys, brass and plucks: two oscillators (unison, octave or fifth apart), filter, envelope, vibrato (plus the mod wheel) and chorus. |
| **Drums** | Synthesized kits in the style of the classic drum machines: Simple 909, Simple 808, Simple 606 and Simple 707, with tune and decay for the kick, tune and snap for the snare, hat decay and tone, and tom tune. It plays the usual drum notes: kick 36, rim 37, snare 38, clap 39, closed hat 42 (which chokes the open hat, 46), toms 41, 45 and 48, crash 49, ride 51, cowbell 56. |
| **Multi Synth** | Eight voices, each one of 24 models: analog, waveshaped, FM (two-op and six-op keys, bells and basses), phase distortion, wavetables, wave terrain, harmonics, formants, chords, string ensemble, chiptune, speech, swarm, filtered noise, particles, plucked string, struck bell, kick, snare and hi-hat. Every model has the same three tone controls (Harmonics, Timbre, Morph), an envelope that can move Timbre, Morph and pitch, and an alternate output with a different version of the sound. Drums, strings, bells and six-op FM ring on their own and Decay sets how long; the rest follow Attack and Release. |
| **Resonator** | Strikes and plucks a resonating body: bells and bars, sympathetic strings, a single string, FM tones, chord strings, or a string into reverb. Structure, brightness, damping and position shape it, and playing harder sounds louder and brighter. One, two or four notes at once, with stereo width. |
| **Reverb** | Small room, room, chamber, hall, plate and spring. Decay, size, pre-delay, tone, low end and mix. |
| **Delay** | Digital, tape, analog and lo-fi echoes, synced to the song (a note value) or free (milliseconds). Repeats, tone, grit, wobble, stereo or ping-pong, and mix. |
| **Grains** | Cuts what you play into grains and plays them back: shorter or longer, earlier or later, shifted up to two octaves either way, regular or scattered, each with its own amount of randomness. **Freeze** holds what's in it. Four qualities, from clean to worn tape, plus feedback, its own reverb, and mix. |
| **Compressor** | Threshold, ratio, attack, release, makeup (with auto makeup) and a parallel mix, with a gain-reduction meter. |
| **Saturator** | Tape, tube or clip, oversampled. Drive (level-matched), tone, mix and output. |
| **Mastering** | Finishes a mix to a loudness target (streaming, download, CD or vinyl) in one of five styles. Press **Learn** and play the song so it sets its levels for this mix; **Amount** and **Push** do the rest. |
| **Random Notes** | A note generator: put it before an instrument on a MIDI track and press play. It writes a melody in time with the song, from seven rhythms and a chosen scale (major, minor, pentatonic and three others), over a range you set. **Déjà vu** decides how much it repeats itself (all the way up, it loops the last 1 to 16 steps), and **Variation** picks a different melody. The same Variation plays the same notes every time you press play. With **Follow keys** on, it only plays the notes you're holding. |

Each one has presets (the menu and arrows at the top of its window). Put Reverb or Delay on a return and it starts at Mix 100%, since the return's dry sound is already in the mix.

## The browser

**Options > Plug-in Browser** (Cmd/Ctrl+B), or **Browse...** next to "+ Add plug-in", lists Lathe's own devices and then every plug-in Lathe has found on this computer, with a search box and Effects / Instruments filters. Note generators such as Random Notes are listed with the instruments.

- **Add** (or double-click) puts an effect on the selected track (or the master), and an instrument on the selected MIDI track, or on a new MIDI track if that one already has an instrument. A note generator goes before the instrument.
- Right-click an installed plug-in to show it in Finder or Explorer.
- To use a plug-in you've just installed, run **Options > Scan for New Plug-ins**.
