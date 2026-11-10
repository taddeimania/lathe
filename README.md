# Lathe

A small, dependable DAW. It opens, finds your interface, loads a template, and records. It hosts VST3 plug-ins, has a mixer with returns and sends, routes audio and MIDI to and from your interface, and exports a mix and stems. It doesn't try to do everything else.

Built with C++20 and JUCE 8.0.9, the same setup as the plug-in projects. It runs on macOS (CoreAudio), Windows (ASIO, WASAPI) and Linux.

Download it at **[getlathe.app](https://getlathe.app)**. How to use it is in **[docs/](docs/README.md)**.

---

## Downloads on getlathe.app

Publishing a release also puts it on the website: `.github/workflows/publish-downloads.yml` copies the three archives to the site's bucket, so the download buttons on [getlathe.app](https://getlathe.app) always get the newest published release:

- `https://getlathe.app/downloads/Lathe-macOS.zip`, `Lathe-Windows.zip` and `Lathe-Linux.tar.gz`. These addresses stay the same every release, and the file saves under its real name, e.g. `Lathe-0.7.2-macOS.zip`.
- `https://getlathe.app/downloads/latest.json`: the version, release date and file sizes. The page reads it to show "Version 0.7.2, released …".
- `https://getlathe.app/downloads/v0.7.2/…`: every version, kept.

Drafts and pre-releases never go to the site. To send a release again (or the current one for the first time), use **Actions > Publish downloads > Run workflow** from `main`, with a tag or empty for the latest. An older tag only fills in its own folder.

It signs in to AWS with GitHub's OIDC token, so no AWS keys are stored in GitHub. It needs the secret `AWS_ROLE_ARN` (a secret so your AWS account ID is masked in the public logs) and the variables `SITE_BUCKET` and `SITE_DISTRIBUTION_ID`. `terraform output github_variables` in lathe-site prints the `gh` commands. The AWS sign-in action is pinned to a commit, since it runs with the publish role.

## Builds on GitHub

`.github/workflows/build.yml` builds and tests Lathe on macOS (universal), Windows and Linux. It works like the plug-in repos' workflow: it runs only when you start it (**Actions > Build > Run workflow**).

- On a branch, it builds, runs the tests, and keeps the three packaged apps as run artifacts for 30 days.
- On a `v*` tag, it does the same and then creates a **draft release** with the three archives and the text from `.github/release-notes.md`. Review the draft and publish it. The tag must match `project(Lathe VERSION ...)` in `CMakeLists.txt`.

Each archive holds the app, `LatheScanner` (next to `Lathe.exe`, or inside `Lathe.app`), `README.md`, `LICENSE` and the user guide in `docs/`.

## Build

### macOS

You need the Xcode command line tools and CMake (`brew install cmake`).

```sh
cd ~/Projects/lathe
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
open build/Lathe_artefacts/Release/Lathe.app
```

The first configure downloads JUCE, which takes a minute. macOS asks for microphone access the first time Lathe opens the interface. Allow it, or nothing will record.

### Windows

You need Visual Studio 2022 (with "Desktop development with C++") and CMake. Install your audio interface's driver from its maker first, since its ASIO driver is what gives low latency.

```bat
cd lathe
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Lathe_artefacts\Release\Lathe.exe
```

ASIO support is built in automatically. Steinberg's ASIO SDK has been GPLv3 since October 2025, and CMake fetches it. To build without it, pass `-DLATHE_ASIO=OFF`. To use your own copy, pass `-DLATHE_ASIO_SDK_DIR=...`.

### Tests

```sh
cmake -B build -DLATHE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

These are headless engine checks that run a fake audio device through the real audio callback. They cover clip timing, plug-in delay compensation, mute, solo and faders, sends, looping, record-latency offset, export, project round-trip, clip splitting and undo, and joining clips (the joined audio plays back sample-identical). The test build also makes three tiny VST3s (one that works, one that crashes while loading, and one that hangs). With those, the tests check that scanning skips the bad plug-ins without freezing or crashing Lathe. They also test the plug-in store: release parsing against the real release format, picking the right platform archive, zip (with Unix permissions) and tar.gz installs, updates over an existing copy, staged updates, and the full download-and-install flow.

## Documentation

The user guide and the code map are in [docs/](docs/README.md): getting started, recording and mixing, keys, Lathe Cloud, the AI assistant, the plug-in browser, updating, project files, and where things live in the source.
