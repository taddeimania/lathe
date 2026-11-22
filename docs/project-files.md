# Project files

A Lathe project is a folder you can copy, move or back up whole: the project file, its audio and its exports, with nothing kept anywhere else.

```
Song/
  Song.lathe          the project (JSON, readable and diff-friendly)
  Song.lathe.bak      the previous save
  Audio/              recordings and imported audio (WAV at the project rate)
  Exports/
```

`.lathe` files are plain JSON. They hold tempo, time signature, loop, tracks (input, output, sends, fader, pan, mute, solo, arm, monitor, EQ), monitor mixes, key changes (`"keys"`: each a beat and a name such as `"A minor pentatonic"`), plug-ins (VST3 description plus base64 state), and clips. Audio paths are relative to the project folder, so you can move or copy the folder between your Mac and the Windows machine. Plug-ins are matched by identity, not by file path. A plug-in that isn't installed on the current machine keeps its saved settings and comes back when it's installed. Templates use the same format without clips.

Opening a project with plug-ins (other than Lathe's own) shows a progress bar naming each one as it loads ("Loading Kontakt 7 (3 of 12)..."). Big instruments can take a few seconds each. **Cancel** stops after the one that's loading and leaves the song you had open as it was.

**File > Save As** makes a new project folder and copies every audio file the project uses into its `Audio/` folder, so the new project stands on its own. A progress bar shows the file it's copying ("Copying Kick.wav (3 of 20)"); **Cancel** stops it, deletes what was copied, and leaves the project where it was.

Saves are atomic: the file is written beside the old one, then swapped in. While a saved project has unsaved changes, Lathe also writes `Song.lathe-autosave` every minute. Closing or quitting with unsaved changes asks whether to save them first. **Save** saves; **Don't save** drops them, autosave and all. So Lathe only offers to recover changes the next time you open the project if it quit unexpectedly.
