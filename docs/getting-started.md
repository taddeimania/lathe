# Getting started

1. **Audio device.** On first launch Lathe picks your audio interface (the device with the most inputs) and selects it: ASIO on Windows, CoreAudio on Mac, 128-sample buffer, all channels on. If it picks the wrong thing, click the device name at the top right (or **Options > Audio / MIDI Settings**). Your choice is saved and restored on every launch. If the interface is unplugged while Lathe runs, Lathe keeps trying to reconnect to it.
2. **Plug-ins.** On first launch Lathe scans the standard VST3 folders in the background, and you can keep working while it does. Each plug-in is opened by a small helper program, `LatheScanner`, which the build places next to Lathe. If a plug-in crashes or hangs (more than a minute) while it's being checked, only the helper dies: Lathe skips that plug-in and says so in the status bar. Settings lists the skipped plug-ins and has **Retry skipped plug-ins** for after you update one. VCV Rack 2's VST3 is a known example: it crashes when unloaded straight after loading.

   A plug-in that scans fine but crashes while you work can be **run separately**: right-click it on the track and tick **Run Separately**. It then runs in its own helper program, `LatheHost`, so if it crashes, Lathe keeps playing: the plug-in goes silent (an effect lets the sound through untouched) and the status bar says so. Right-click it > **Restart** brings it back with its settings. Its window opens on its own, from the helper. The project remembers the choice. It costs a little more CPU than running inside Lathe, so leave it off for plug-ins that behave. If a plug-in crashes Lathe while a song is opening (some can't take settings saved by another version, or on someone else's computer), Lathe remembers which one it was and runs it separately from then on, so the song opens next time instead of crashing again. Turning Run Separately off undoes that.
3. **Templates.** Two templates are created in `Documents/Lathe/Templates`:
   - **Basic**: two audio tracks (inputs 1 and 2), one MIDI track, two returns.
   - **8-Input Tracking**: eight mono tracks on inputs 1–8, a MIDI track, and two returns. Monitoring is **Off** on these tracks, so you hear yourself through your interface's direct monitoring with zero latency.

Set up a session the way you like it (tracks, inputs, plug-ins, sends), then use **File > Save as Template**. In Settings you can choose to open the last project or start from your default template on launch.

## The question on first launch

The first time you open Lathe after installing it, it asks whether it may share a few anonymous milestones, like the first time you record something, so we can see where new people get stuck. Each is sent once, with Lathe's version and Mac, Windows or Linux, and nothing else: no ID, name, song or track. **Don't share** (or closing the window) sends nothing, and Lathe doesn't ask again. Updating Lathe never shows the question; it only appears when Lathe has no settings yet. Change it any time in Settings, under **Privacy**. The full list, the exact message and what's kept are on [getlathe.app/privacy](https://getlathe.app/privacy.html#app); the code is `Source/Cloud/Milestones.cpp`.

## If Lathe crashes

Lathe writes a short report when it crashes, and the next time it opens it shows you the report and asks whether to send it. It holds Lathe's version, your system, how long Lathe was open and where in Lathe's code it stopped, and nothing from your songs: no names, paths or account. **Don't send** deletes it; **Remember my answer** makes it always send or never. Change it in Settings, under **Privacy**. Details: [getlathe.app/privacy](https://getlathe.app/privacy.html#crashes); the code is `Source/Cloud/CrashReports.cpp`.
