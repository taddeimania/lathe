# AI assistant (MCP)

Lathe can let an AI app on your computer, like Claude Desktop or Claude Code, look at the open project, play any stretch of it, build the session, and measure how hard your computer is working. It talks to Lathe through MCP (the Model Context Protocol). Ask it things like:

- "why can't I hear MIDI 2?" or "what notes does the bass play in bar 5?"
- "play bars 9 to 13" or "loop the chorus and play it three times"
- "add a MIDI track with an arpeggiator into a synth, and a reverb return the drums send to"
- "what's using the most CPU in this song?" or "compare 1 core with all cores on bars 1 to 9"

Everything it changes in one go is a single undo step: Cmd/Ctrl+Z takes it all back.

**Turn it on:** Options > **AI Assistant...** > *Let AI apps on this computer connect to Lathe*. It's off until you do, and Lathe remembers your choice. The same window shows the setup to copy:

- **Claude Desktop:** Settings > Developer > Edit Config, add the `"lathe"` entry the window shows to `claude_desktop_config.json`, then restart Claude Desktop. It runs `LatheMCP`, a small helper inside Lathe (`Lathe.app/Contents/MacOS/LatheMCP` on the Mac).
- **Claude Code:** run the `claude mcp add lathe -- .../LatheMCP` command the window shows.
- **Other MCP apps:** add `LatheMCP` as a local (stdio) server.

What it can do:

| Tool | |
|---|---|
| `describe_session` | Tempo, transport, every track (input, output, monitoring, plug-ins in order and which ones generate notes, clips), returns and master, plus likely reasons a track can't be heard. |
| `describe_track` | One track in full: sends, EQ, who it feeds, every clip with its position. |
| `get_notes` | A MIDI track's notes (pitch, bar, length, velocity), for one clip or a range of bars. |
| `write_notes` | Writes notes into a MIDI track's bars, replacing what was there or adding to it, with an optional repeat. One undo step. |
| `play` | Starts playback, optionally from a bar. With `to_bar`, Lathe stops by itself exactly at that bar. With `loop_times`, it plays the loop that many times and stops at the loop end. Either way the playhead then goes back to the insert marker, as Stop does. |
| `stop` | Stops, like the Stop button. Stop while recording keeps the take. |
| `set_loop` | Sets the loop region (in bars) and turns it on, or turns it off. The loop is saved with the project, as when you drag the loop brackets. |
| `list_plugins` | The plug-ins installed on this computer, each marked instrument, note generator or effect. |
| `edit_session` | Builds or changes the session in one go: add, remove, rename and move tracks and returns; add, remove, move and bypass plug-ins; inputs (another track, an interface input, a MIDI device), outputs, sends, volume, pan, mute, solo, monitoring, arm, MIDI channel, tempo and time signature. All of it lands as one undo step, or, if one change can't be made, none of it does and the assistant is told why. |
| `measure_performance` | Plays some bars for real and times every audio block, as the CPU meter does: average, worst 1 in 100, peak, blocks that missed their deadline, and each track's own cost with its plug-ins. It also notes the conditions: computer, CPU and cores, interface and buffer size. |
| `compare_processing_cores` | The same bars on 1 core and on all cores, in turn (1, all, 1, all), side by side with the speed-up. Your Processing cores setting is put back afterwards. |

**Building a session.** Plug-ins go where they work: a note generator (an arpeggiator, a step sequencer, a chord generator) before the instrument it drives, an instrument before the effects. A MIDI track plays one instrument. "An arpeggiator into a synth" can be one track with both plug-ins, or two tracks, the synth's track taking the arpeggiator's notes as its input, which lets you record the arpeggiator's part or drive several instruments from it. A track that takes another track as its input gets monitoring set to In, so it plays without being armed. Fader, pan, mute, solo, send levels, monitoring and arm change live, as when you move them yourself, and stay where they are on undo.

**Measuring.** While it measures, Lathe plays the bars (the loop if it's on, otherwise the first four), so you hear them. The figures are the time each block took as a share of the time it lasts, the same thing Lathe's CPU meter shows. To compare with another app, use the same buffer size, sample rate, plug-ins and bars, and read that app's meter while it plays.

**Privacy and safety:** Lathe only listens on this computer (127.0.0.1), never the network. Every request needs a secret key, which Lathe makes fresh each time the connection starts and writes to `assistant.json` in its settings folder, readable only by you. Requests from web pages (or anything claiming another host name) are refused. When you turn it off, or quit Lathe, the key is deleted. **New key** in the same window disconnects anything that had the old one. You see what the assistant does in the status bar ("AI assistant: playing bars 9 to 13").
