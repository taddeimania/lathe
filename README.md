# Lathe

A small, dependable DAW. It opens, finds your interface, loads a template, and records. It hosts VST3 plug-ins, has a mixer with returns and sends, routes audio and MIDI to and from your interface, and exports a mix and stems. It doesn't try to do everything else.

Built with C++20 and JUCE 8.0.9, the same setup as the plug-in projects. It runs on macOS (CoreAudio), Windows (ASIO, WASAPI) and Linux.

Download it at **[getlathe.app](https://getlathe.app)**. How to use it is in **[docs/](docs/README.md)**.

---

## Downloads on getlathe.app

Publishing a release also puts it on the website: `.github/workflows/publish-downloads.yml` copies the three archives to the site's bucket, so the download buttons on [getlathe.app](https://getlathe.app) always get the newest published release.

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

These are headless engine checks that run a fake audio device through the real audio callback. They cover clip timing, plug-in delay compensation, mute, solo and faders, sends, looping, record-latency offset, export, project round-trip, clip splitting, undo (clips, tracks, plug-ins and routing), the AI assistant's tools (building a session, measuring CPU on a real-time audio thread), and joining clips (the joined audio plays back sample-identical). The test build also makes three tiny VST3s (one that works, one that crashes while loading, and one that hangs). With those, the tests check that scanning skips the bad plug-ins without freezing or crashing Lathe. They also test the plug-in store: release parsing against the real release format, picking the right platform archive, zip (with Unix permissions) and tar.gz installs, updates over an existing copy, staged updates, and the full download-and-install flow.

The engine renders tracks on several cores (docs/multicore.md). `ctest` also runs every test with each block shared over 4 threads (`LATHE_TEST_THREADS=4`), and compares 200 random sessions rendered on 1 and 4 threads sample for sample. CI's **Thread check** job runs both under ThreadSanitizer. For the speed: `LatheTests --bench out.csv 1,full`, or `Lathe --benchmark` on any machine (docs/performance.md).

## Documentation

The user guide and the code map are in [docs/](docs/README.md): getting started, recording and mixing, keys, Lathe Cloud, the AI assistant, the built-in devices and plug-in browser, updating, project files, and where things live in the source.
