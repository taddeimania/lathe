# Plug-in browser (your GitHub plug-ins)

**Options > Plug-in Browser** (Cmd/Ctrl+B), or **Browse...** next to "+ Add plug-in". It lists your 14 plug-ins from `github.com/taddeimania`, grouped as effects, instruments and MIDI tools, with the latest published version of each. Below them are the other installed plug-ins.

- **Get & add** downloads the latest release for this computer (`<Name>-<version>-macOS.zip` or `-Windows.zip`), installs the `.vst3` into your own VST3 folder, scans it, and adds it:
  - Effects go on the selected track (or the master).
  - Instruments go on the selected MIDI track, or a new MIDI track if it already has one.
  - MIDI tools (Beadwork, Dicey, Posy) go at the front of a MIDI track's chain, ahead of the instrument they drive.
- **Add** on a plug-in that's already installed just adds it. **Update to x.y.z** appears (in orange) when GitHub has a newer release. On launch Lathe checks quietly and mentions available updates in the status bar.
- Install folders don't need admin rights: `~/Library/Audio/Plug-Ins/VST3` on the Mac and `%LOCALAPPDATA%\Programs\Common\VST3` on Windows.
- On Windows, a plug-in that's loaded can't be replaced. In that case the update is staged and finished the next time Lathe starts, before any plug-in loads. On either system, restart Lathe to use an updated plug-in in a project that already has it open.
- Only *published* releases are used, so draft releases from the CI workflow don't show up until you publish them.
- GitHub allows 60 anonymous lookups an hour, so results are cached for an hour. **Check for updates** forces a fresh look.
- **Add repository...** takes any `owner/name` whose releases follow the same pattern. The catalog is plain JSON at `Documents/Lathe/Plugin Catalog.json` if you'd rather edit it by hand. New plug-ins built into Lathe's list are merged in automatically.
