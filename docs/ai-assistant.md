# AI assistant (MCP)

Lathe can let an AI app on your computer, like Claude Desktop or Claude Code, look at the open project, play any stretch of it, build the session, and measure how hard your computer is working. It talks to Lathe through MCP (the Model Context Protocol). Ask it things like:

- "why can't I hear MIDI 2?" or "what notes does the bass play in bar 5?"
- "play bars 9 to 13" or "loop the chorus and play it three times"
- "add a MIDI track with an arpeggiator into a synth, and a reverb return the drums send to"
- "what's using the most CPU in this song?" or "compare 1 core with all cores on bars 1 to 9"
- "is anything clipping, and what's fighting the bass?" or "mix this so the vocal sits on top, then check it"

Everything it changes in one go is a single undo step: Cmd/Ctrl+Z takes it all back. Edit > Undo History (Cmd/Ctrl+Alt+Z) lists every step with who made it, you or the assistant, in its own words, and when; click a step to go back to just before it, or use "Undo the assistant's last changes" to take back its latest run.

**Turn it on:** Options > **AI Assistant...** > *Let AI apps on this computer connect to Lathe*. It's off until you do, and Lathe remembers your choice. The same window shows the setup to copy:

- **Claude Desktop:** Settings > Developer > Edit Config, add the `"lathe"` entry the window shows to `claude_desktop_config.json`, then restart Claude Desktop. It runs `LatheMCP`, a small helper inside Lathe (`Lathe.app/Contents/MacOS/LatheMCP` on the Mac).
- **Claude Code:** run the `claude mcp add lathe -- .../LatheMCP` command the window shows.
- **Other MCP apps:** add `LatheMCP` as a local (stdio) server.

What it can do:

| Tool | |
|---|---|
| `describe_session` | Tempo, transport, every track (input, output, monitoring, plug-ins in order and which ones generate notes, clips), returns and master, plus likely reasons a track can't be heard. |
| `describe_track` | One track in full: sends, EQ, who it feeds, every clip with its position. |
| `get_notes` | A MIDI track's notes for one clip or a range of bars, in Lathe's [note notation](notation.md) (or as a JSON list). |
| `write_notes` | Writes notes into a MIDI track's bars, as a [notation](notation.md) pattern or a JSON list, replacing what was there or adding to it, with an optional repeat. One undo step. |
| `notation_guide` | The full reference for the note notation, for the assistant to read before writing a detailed part. |
| `play` | Starts playback, optionally from a bar. With `to_bar`, Lathe stops by itself exactly at that bar. With `loop_times`, it plays the loop that many times and stops at the loop end. Either way the playhead then goes back to the insert marker, as Stop does. |
| `stop` | Stops, like the Stop button. Stop while recording keeps the take. |
| `set_loop` | Sets the loop region (in bars) and turns it on, or turns it off. The loop is saved with the project, as when you drag the loop brackets. |
| `list_plugins` | The plug-ins installed on this computer, each marked instrument, note generator or effect. |
| `edit_session` | Builds or changes the session in one go: add, remove, rename and move tracks and returns; add, remove, move and bypass plug-ins; inputs (another track, an interface input, a MIDI device), outputs, sends, volume, pan, the channel EQ, mute, solo, monitoring, arm, MIDI channel, tempo and time signature. All of it lands as one undo step, or, if one change can't be made, none of it does and the assistant is told why. |
| `get_plugin_params` | Every setting a plug-in on a track publishes (Lathe's own devices and VST3s): name, id, current value, and its choices or its range. |
| `set_plugin_params` | Turns a plug-in's knobs, flips its switches and picks from its menus ("Kit" to "Simple 808", Delay "Time" to 250 ms). Numbers are in the setting's own units. All the changes for one plug-in land, or none do. Like turning a knob by hand, it isn't an undo step; the reply gives the old values. |
| `new_project` | Starts a new project, empty or from one of your templates by name, like File > New. |
| `open_project` | Opens a project from your projects folder by name (or any project by path), like File > Open. Without a name it lists them, newest first. |
| `save_project` | Saves the project. With a name it saves it as a new project in your projects folder (its own folder, audio copied in), like Save As; it never saves over another project. |
| `export_audio` | Renders the mix (and stems, if asked) to WAV, AIFF, FLAC, MP3 or Ogg Vorbis in the project's Exports folder, like File > Export, and reports each file's length and the mix's peak and loudness (LUFS). |
| `analyze_mix` | Listens to the mix without playing it: renders the loop (or the song, or the bars given) offline and measures the mix and every track. The mix: peak, true peak, loudness (LUFS), crest factor, balance across seven frequency bands, stereo width and correlation, also below 250 Hz. Each section: its loudness and loudest tracks. Each track and return: its loudness (LUFS) and level while it plays, its share of the tracks' loudness (K-weighted, as ears and meters hear it, not raw energy, which the bass always wins), the bars it plays in, where most of its energy is, and its width. Masking: pairs of tracks loud in the same band at the same time, within 6 dB of each other, and for how much of the time. And notes on anything measurably off: clipping, a true peak above -1 dBTP, a wide or out-of-phase low end. Nothing is written. |
| `compare_mix` | Works with View > Compare. With `mark_before`, renders the mix as it is as Before (the assistant does this before it changes a mix). Without it, renders Now over the same bars and replies with each version's loudness, true peak, crest, width and seven-band balance, plus a few sentences on what changed and how both stand against the reference you chose. You then switch between them by ear. |
| `measure_performance` | Plays some bars for real and times every audio block, as the CPU meter does: average, worst 1 in 100, peak, blocks that missed their deadline, and each track's own cost with its plug-ins. It also notes the conditions: computer, CPU and cores, interface and buffer size. |
| `compare_processing_cores` | The same bars on 1 core and on all cores, in turn (1, all, 1, all), side by side with the speed-up. Your Processing cores setting is put back afterwards. |

New project and open project replace what's open, so if there are unsaved changes they stop and ask: the assistant has to say whether to save them first or throw them away. Ask it to check with you before it throws anything away.

**Building a session.** Plug-ins go where they work: a note generator (an arpeggiator, a step sequencer, a chord generator) before the instrument it drives, an instrument before the effects. A MIDI track plays one instrument. "An arpeggiator into a synth" can be one track with both plug-ins, or two tracks, the synth's track taking the arpeggiator's notes as its input, which lets you record the arpeggiator's part or drive several instruments from it. A track that takes another track as its input gets monitoring set to In, so it plays without being armed. Fader, pan and send levels are part of the undo step, as when you move them yourself; mute, solo, monitoring and arm change live and stay where they are on undo.

**Working at the same time.** You can keep working in Lathe while the assistant does. Lathe remembers the song as the assistant last saw it, and before each change it makes, checks whether you changed anything in between:
- If you changed tracks, plug-ins and their settings, clips or notes, routing or tempo, its change is held back and nothing happens. It's told what you changed, in the words History uses ("\"Bass\": fader -8.0 dB -> -6.0 dB"), so it can look again and decide whether its plan still fits, or ask you.
- Faders, pans, mutes, solos, sends, record arm, monitoring and the loop don't stop it (you move those while listening). It's told about them, and its change is made on top of yours.
- While it works, a bar under the transport says what it did last, with **Pause**. Paused, every tool answers that it's paused and nothing changes: it tells you where it got to and waits. Press **Continue**, then tell it to carry on. The bar goes away a little after its last step.

**Measuring.** While it measures, Lathe plays the bars (the loop if it's on, otherwise the first four), so you hear them. The figures are the time each block took as a share of the time it lasts, the same thing Lathe's CPU meter shows. To compare with another app, use the same buffer size, sample rate, plug-ins and bars, and read that app's meter while it plays.

**Privacy and safety:** Lathe only listens on this computer (127.0.0.1), never the network. Every request needs a secret key, which Lathe makes fresh each time the connection starts and writes to `assistant.json` in its settings folder, readable only by you. Requests from web pages (or anything claiming another host name) are refused. When you turn it off, or quit Lathe, the key is deleted. **New key** in the same window disconnects anything that had the old one. You see what the assistant does in the status bar ("AI assistant: playing bars 9 to 13").
