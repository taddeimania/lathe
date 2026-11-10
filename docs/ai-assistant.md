# AI assistant (MCP)

Lathe can let an AI app on your computer, like Claude Desktop or Claude Code, look at the open project and play or stop it. It talks to Lathe through MCP (the Model Context Protocol). Ask it things like "why can't I hear MIDI 2?", "what notes does the bass play in bar 5?" or "play from bar 9". For now it only reads the project: it can't change anything.

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
| `play` / `stop` | The transport, optionally from a bar. Stop while recording keeps the take, like the Stop button. |

**Privacy and safety:** Lathe only listens on this computer (127.0.0.1), never the network. Every request needs a secret key, which Lathe makes fresh each time the connection starts and writes to `assistant.json` in its settings folder, readable only by you. Requests from web pages (or anything claiming another host name) are refused. When you turn it off, or quit Lathe, the key is deleted. **New key** in the same window disconnects anything that had the old one. You see what the assistant does in the status bar ("AI assistant: playing from bar 9").
