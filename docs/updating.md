# Updating Lathe (Lathe Patcher)

Lathe comes with a small updater, **Lathe Patcher**. Open it from **Options > Check for Updates…** in Lathe. Lathe also checks GitHub on its own, shortly after it starts and then every 12 hours. When there's a newer release it shows **Update available** in orange at the bottom right of the window (no pop-up). Click it to open the patcher. Right-click it to skip that version or turn the checks off. **Options > Check for Updates Automatically** also turns them off or back on. The check uses `LATHE_UPDATE_URL` too. On Windows and Linux you can also double-click `LathePatcher` in the Lathe folder.

- It asks GitHub for the latest **published** release of `taddeimania/lathe` and compares it with the installed version.
- If there's a newer one, it shows the release notes. **Update** then:
  - downloads that release's build for your platform,
  - checks the download's size and the SHA-256 checksum GitHub publishes,
  - replaces Lathe in place.
- Your projects, templates, settings and plug-ins aren't touched.
- On the Mac it replaces `Lathe.app` wherever you keep it (the patcher lives inside it, in `Contents/Helpers`).
- On Windows and Linux it replaces the files in Lathe's folder. Files that are in use, such as a running `Lathe.exe`, are renamed to `*.old` and removed the next time Lathe starts. Quit Lathe and reopen it to use the new version.
- If anything fails partway, the old files are put back.

Notes:
- The repository must be public, and the release must be published (not a draft) for the patcher to see it.
- The first release that includes the patcher has to be downloaded by hand once. After that, use the patcher.
- `LATHE_UPDATE_URL=<url of a release JSON>` points the patcher at another feed, for testing.
