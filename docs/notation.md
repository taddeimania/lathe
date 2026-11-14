# Lathe's note notation

A short text format for writing and reading MIDI parts through the AI assistant (MCP) tools.
`write_notes` takes it as `pattern`, `get_notes` answers in it, and `notation_guide` gives the
assistant the full reference below.

## Why

`write_notes` took one JSON object per note:
`{"pitch": "F#2", "start_bar": 3.375, "length_beats": 0.25, "velocity": 96}`. That is about 25
tokens a note, so a bar of 16th-note drums cost around 600, and every position was a fraction to
work out (3.375 is bar 3, 16th 7). Assistants dealt with it by writing a Python script to produce
the JSON: the music was in the script, and the session paid for the script, the output and the
reasoning about both.

The notation is built from formats models already know: drum-machine step grids, Strudel/Tidal
"mini-notation" for melodies, and note names. A bar of drums is three short lines. Positions come
from the layout, so there's no arithmetic. Microtiming and per-note velocity are optional details
on any note, so a plain part stays short and a detailed one costs only what it needs.

## Goals

- Write any part Lathe can hold: every pitch, start, length and velocity, to a small fraction of a
  millisecond.
- Read back losslessly: `get_notes` output, written back with `write_notes` over the same bars,
  gives the same notes.
- Familiar first. Where a symbol means something in Strudel, it means the same here, or it isn't
  used.
- Errors say where: line, label, bar and step, and what was expected. Nothing is written if any
  line fails.
- The JSON note list stays for anything the notation can't say.

Not covered yet: controller data (CC), pitch bend, per-note expression (MPE), chord symbols, and
tempo or time-signature changes inside a pattern (Lathe has one tempo per song). See "Later".

## The format

A pattern is lines. `#` starts a comment. Blank lines are ignored.

A pattern covers the bars given to `write_notes` (`from_bar` to `to_bar`). Each line is a loop:
a one-bar line fills every bar of the range, a two-bar line alternates, and so on. A line longer
than the range is an error. `write_notes`'s `repeat` still repeats the whole range.

Pitches are note names or MIDI numbers, as in Lathe: C3 = 60 = middle C, `F#2`, `Bb1`, `C-1`.
Any case. Drum names stand for the General MIDI notes Lathe's Drums plays:

| Name (and short forms) | Note |
|---|---|
| kick (bd) | 36 |
| rim (rs) | 37 |
| snare (sd) | 38 |
| clap (cp) | 39 |
| hat (hh, chh) | 42 |
| phat (ph) | 44 (pedal hat) |
| ohat (oh, ohh) | 46 |
| ltom (lt), mtom (mt), htom (ht) | 41, 45, 48 |
| crash (cr) | 49 |
| ride (rd) | 51 |
| tamb | 54 |
| cowbell (cb) | 56 |

### Grid lines (drums, or any one pitch)

```
grid 16
kick   x... ..x. x... ....
snare  .... x... .... x..o
hat    8.6. 9.6. 8.6. 9.67
```

The first word is the lane: the pitch every hit plays. Then one character per step:

| | |
|---|---|
| `.` or `-` | rest |
| `x` | hit, velocity 100 |
| `X` | accent, 120 |
| `o` | ghost, 45 |
| `1`-`9` | hit at that level: 14, 28, 42, 56, 71, 85, 99, 113, 127 |

Spaces are ignored. `|` marks bar lines; with them, every bar must have exactly `grid` steps.
Without them, the steps must fill whole bars. A hit lasts one step.

`grid N` sets the steps per bar for the lines after it. The default is 16th notes (16 in 4/4,
14 in 7/8); `grid 12` gives 8th-note triplets in 4/4, `grid 24` 16th triplets.

A line is a grid line when it has a run of two or more step characters (`x...`). `kick x x x x`
is a token line: four quarter notes.

### Token lines (melodies, chords, exact timing)

```
bass:  c2:110 ~ [c2:80 c2:70-6ms] ~ | f1:105=2 _ ~ g1:90+4ms
keys:  [c3,e3,g3] _ _ ~ | <[a2,c3,e3] [f2,a2,c3]> _ _ ~
snare: ~ x:96+6ms ~ x:100+8ms
```

The label is optional and ends with `:`. When it's a pitch or drum name, `x`, `X` and `o` in
the line play it. Otherwise it's only a name for the line.

`|` separates bars. Each bar is divided evenly among its top-level tokens, as a Strudel cycle is.
A line without `|` is one bar.

| | |
|---|---|
| `c3`, `F#2`, `36`, `snare` | a note |
| `x` `X` `o` | a hit on the line's lane (100, 120, 45) |
| `~` | rest |
| `_` | hold: the note before lasts one more step |
| `[a b c]` | the three share one step |
| `[a,b,c]` | played together (a chord); `[a b, c d]` two sequences at once |
| `<a b>` | in turn, each time the line plays: a the first time, b the second, then a again. A one-bar line plays every bar, so it alternates bar by bar |
| `a*3` | a, three times in its step |

Any note, hit or group can carry up to four suffixes, in this order, no spaces:

| | |
|---|---|
| `:96` | velocity 1-127 (default 100; `X` 120, `o` 45) |
| `+8ms`, `-5ms` | later or earlier, in milliseconds |
| `+20%`, `-10%` | later or earlier, as a share of the note's own step |
| `=1.5` | length in beats (default: its step, plus any holds) |
| `*3` | repeat within the step (last) |

`c3:96+8ms=1.5`. On a group, suffixes apply to every note inside that doesn't set its own:
`[c3,e3,g3]:80`.

### Feel

```
swing 58%            # every second 16th, 58% of the way through its 8th (50% is straight)
swing 62% 8          # every second 8th instead
humanize 6ms 8vel seed 3
swing off
humanize off
```

They apply to the lines after them until changed. Swing moves notes that start exactly on the
off-beat it names, before any `+ms`. Humanize nudges timing by up to ± the given milliseconds and
velocity by up to ± the given amount, but leaves alone what a note sets itself (its own offset,
its own velocity). The same text and seed always give the same notes, so undo, the cloud and a
re-run all agree.

## Reading back (get_notes)

`get_notes` answers in the notation by default (`format: "list"` gives the old JSON). The first
line says which bars it covers and how to write it back:

```
# "Beat", bars 1 to 3 (4/4, 120 BPM). write_notes from_bar 1, to_bar 3 with this pattern gives the same notes.
kick   x... ..x. x... .... | x... ..x. x... ..x.
snare  .... x... .... x... | .... x... .... x..o
hat    x.x. x.x. x.x. x.x. | x.x. x.x. x.x. xXx.
```

- On a track whose instrument is a drum machine (its name has "drum" in it), each pitch is a lane,
  named when it's a drum name. A lane whose hits all sit on one grid, last one step and use the
  grid's velocities is a grid line; otherwise it's a token line with suffixes only where needed.
- On other tracks, notes go into as few token lines ("voices") as keep them from overlapping:
  chords are `[ , ]`, lengths are holds where they fit the bar's steps and `=` where they don't,
  and anything off the grid gets a `%` offset, which is exact at any tempo.
- Swing and humanize are never guessed back; their effect shows as offsets and velocities.

Round trip: the same notes, starts and lengths within 0.0001 beats (well under a millisecond),
velocities exact.

## In the MCP tools

- `write_notes`: new `pattern` (string), instead of `notes`. Same `from_bar`, `to_bar`, `repeat`
  and `mode`. Its description carries a few lines and two examples; that's paid in every
  session, so it stays short.
- `get_notes`: new `format`, `"pattern"` (default) or `"list"`.
- `notation_guide`: no arguments; returns the reference above (symbols, velocity table, feel,
  common mistakes). Read-only, so it doesn't count as the assistant changing the song.

## Errors

`Line 3 (hat), bar 2: 15 steps, but grid is 16. Each bar between | needs exactly 16.`
`Line 2 (bass), bar 2: "q3" isn't a note (C3 is middle C; drums: kick, snare, hat...).`
`Line 4 (snare): this line is 3 bars but bars 1 to 3 are 2. Shorten it or widen the range.`

## Code

- `Source/Assistant/Notation.h/.cpp`: `parse` (text to notes, for a range) and `format` (notes to
  text), plus the drum names and the guide text. No dependence on the engine, so it's tested on
  its own.
- `AssistantTools.cpp`: `write_notes` takes `pattern`, `get_notes` gives it, `notation_guide`.
- Tests: parsing each symbol, errors, swing and humanize, and round trips of random parts
  (drums, chords, offsets, odd meters).

## Later

- Controller lanes: `cc74  0 . . 64 . . 127` (a grid of values) and pitch bend.
- Chord symbols in token lines: `Am7`, `F/A`.
- Compact read-back of repeated bars (`!` or a repeat count).
- Measure: tokens and mistakes per song in the weekly videos task, before and after.
