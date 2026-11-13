cmake_minimum_required(VERSION 3.22)

# macOS 11+. Builds for the machine you're on by default; for a universal
# binary pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64". Set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Lathe VERSION 0.12.7 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: the C++ runtime is built into Lathe and its helpers instead of coming from the
# Visual C++ Redistributable. Many PCs have none, or one too old for what Lathe uses (the
# render pool's semaphore needs MSVCP140_ATOMIC_WAIT.dll, from 2019 16.8 on), and Lathe
# then won't start. Set before JUCE so its targets (and the test plug-ins) match.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- JUCE ---------------------------------------------------------------------
# Downloaded on first configure (same pin as the plug-in projects). To use a
# local checkout instead:  cmake -B build -DLATHE_JUCE_DIR=/path/to/JUCE
set(LATHE_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout")

if(LATHE_JUCE_DIR)
    # A local checkout isn't patched: apply cmake/juce-8.0.9-coreaudio-listener.patch to it yourself.
    add_subdirectory(${LATHE_JUCE_DIR} JUCE)
else()
    include(FetchContent)
    # The patch fixes a macOS crash in JUCE 8.0.9's Core Audio code (separate input and output
    # devices, one of them changing): see the patch's comments. JUCE 9 rewrote that code.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.9
        GIT_SHALLOW    TRUE
        PATCH_COMMAND  ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/juce-8.0.9-coreaudio-listener.patch
                       -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- LAME (MP3 export) --------------------------------------------------------------
# JUCE reads MP3s but can't write them. LAME (LGPL) is fetched and built here as a small static
# library; cmake/lame/config.h stands in for the config.h LAME's autoconf build would make.
# To use your own copy: -DLATHE_LAME_DIR=/path/to/lame   (the folder that holds libmp3lame/)
set(LATHE_LAME_DIR "" CACHE PATH "Optional local LAME source")
if(NOT LATHE_LAME_DIR)
    include(FetchContent)
    FetchContent_Declare(lame
        GIT_REPOSITORY https://github.com/lameproject/lame.git
        GIT_TAG        1f5cc9487284d5950343aa5d4f70de433468070a   # 3.100.1
        SOURCE_SUBDIR  no-cmake-here)   # LAME has no CMakeLists; only fetch it
    FetchContent_MakeAvailable(lame)
    set(LATHE_LAME_DIR ${lame_SOURCE_DIR})
endif()
file(GLOB LATHE_LAME_SOURCES ${LATHE_LAME_DIR}/libmp3lame/*.c)
list(FILTER LATHE_LAME_SOURCES EXCLUDE REGEX "mpglib_interface\\.c$")
add_library(lathe_lame STATIC ${LATHE_LAME_SOURCES})
target_compile_definitions(lathe_lame PRIVATE HAVE_CONFIG_H)
target_include_directories(lathe_lame
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lame ${LATHE_LAME_DIR}/libmp3lame
    PUBLIC  ${LATHE_LAME_DIR}/include)
set_target_properties(lathe_lame PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(lathe_lame PRIVATE /W0)
    target_compile_definitions(lathe_lame PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(lathe_lame PRIVATE -w)
    if(NOT WIN32)
        target_link_libraries(lathe_lame PRIVATE m)
    endif()
endif()

# --- ASIO (Windows) -------------------------------------------------------------
# Most audio interfaces' low-latency Windows driver is ASIO. Steinberg's ASIO SDK is
# GPLv3 since Oct 2025; it is fetched automatically. To use your own copy:
#   -DLATHE_ASIO_SDK_DIR=C:/path/to/asiosdk   (the folder that holds common/)
# To build without ASIO (WASAPI / DirectSound only): -DLATHE_ASIO=OFF
if(WIN32)
    option(LATHE_ASIO "Build with ASIO support" ON)
    set(LATHE_ASIO_SDK_DIR "" CACHE PATH "Optional local ASIO SDK")
    if(LATHE_ASIO AND NOT LATHE_ASIO_SDK_DIR)
        include(FetchContent)
        FetchContent_Declare(asiosdk
            GIT_REPOSITORY https://github.com/jcelerier/asio-2.git
            GIT_TAG        1bc070d1c7b8f0bf92f9d3b7c72e5880a7dcffdb)
        FetchContent_MakeAvailable(asiosdk)
        set(LATHE_ASIO_SDK_DIR ${asiosdk_SOURCE_DIR})
    endif()
endif()

# --- App ----------------------------------------------------------------------
juce_add_gui_app(Lathe
    PRODUCT_NAME                 "Lathe"
    COMPANY_NAME                 "Taddei"
    BUNDLE_ID                    com.taddei.lathe
    VERSION                      ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT   "Lathe records audio from your audio interface."
    DOCUMENT_EXTENSIONS          lathe
    # Made from Resources/icon.svg (rsvg-convert -w 1024 icon.svg -o icon.png; -w 256 for icon-small.png)
    ICON_BIG                     "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL                   "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-small.png")

# Files built into the app: the song a new install opens with (Source/Demo/Welcome.h).
juce_add_binary_data(LatheData
    HEADER_NAME BinaryData.h
    NAMESPACE   BinaryData
    SOURCES     Resources/Welcome.lathe)

# Émilie Gillet's open-source DSP (MIT licence): the cores of Multi Synth, Resonator,
# Grains and Random Notes. Only the DSP, vendored as-is apart from the portability changes
# listed in ThirdParty/mutable/README.md. TEST selects its desktop (non-ARM) code.
file(GLOB_RECURSE LATHE_MUTABLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/mutable/*.cc")
add_library(LatheMutable STATIC ${LATHE_MUTABLE_SOURCES})
target_include_directories(LatheMutable SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/mutable")
target_compile_definitions(LatheMutable PRIVATE TEST=1)
target_compile_features(LatheMutable PRIVATE cxx_std_17)
set_target_properties(LatheMutable PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(LatheMutable PRIVATE /w /bigobj)
else()
    target_compile_options(LatheMutable PRIVATE -w)
endif()

# Lathe's built-in devices (instruments and effects), shared by the app and the tests.
set(LATHE_DEVICE_SOURCES
    Source/Devices/Device.cpp
    Source/Devices/Effects.cpp
    Source/Devices/Instruments.cpp
    Source/Devices/Reverb/Blocks.cpp
    Source/Devices/Reverb/Engine.cpp
    Source/Devices/Reverb/Nonlinear.cpp
    Source/Devices/Reverb/Plate.cpp
    Source/Devices/Reverb/Reflections.cpp
    Source/Devices/Reverb/Space.cpp
    Source/Devices/Reverb/Spring.cpp
    Source/Devices/Reverb/Tape.cpp
    Source/Devices/Reverb/Types.cpp
    Source/Devices/Delay/Engine.cpp
    Source/Devices/Delay/Types.cpp
    Source/Devices/Master/Master.cpp
    Source/Devices/Master/Loudness.cpp
    Source/Devices/Mutable.cpp)

target_sources(Lathe PRIVATE
    ${LATHE_DEVICE_SOURCES}
    Source/Main.cpp
    Source/Model/Edit.cpp
    Source/Model/ChannelEq.cpp
    Source/Cloud/CloudClient.cpp
    Source/Cloud/Packing.cpp
    Source/Cloud/ProjectDiff.cpp
    Source/Cloud/Merge.cpp
    Source/Cloud/Revisions.cpp
    Source/Engine/AudioPool.cpp
    Source/Engine/PluginHost.cpp
    Source/Engine/Engine.cpp
    Source/Engine/Mp3Writer.cpp
    Source/Engine/RenderPool.cpp
    Source/Engine/ClipJoin.cpp
    Source/UI/Theme.cpp
    Source/UI/Widgets.cpp
    Source/UI/GlobalKeys.cpp
    Source/UI/TransportBar.cpp
    Source/UI/Arrangement.cpp
    Source/UI/Mixer.cpp
    Source/UI/DeviceChain.cpp
    Source/UI/EqEditor.cpp
    Source/UI/CloudUI.cpp
    Source/UI/CloudLibrary.cpp
    Source/UI/CloudLinks.cpp
    Source/UI/Notes.cpp
    Source/UI/UndoHistory.cpp
    Source/UI/CompareWindow.cpp
    Source/UI/UpdateNotice.cpp
    Source/Patcher/Updater.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/Dialogs.cpp
    Source/UI/PluginBrowser.cpp
    Source/UI/AssistantDialog.cpp
    Source/Demo/DemoSong.cpp
    Source/Demo/Welcome.cpp
    Source/Bench/Benchmark.cpp
    Source/Assistant/AssistantShared.cpp
    Source/Assistant/AssistantServer.cpp
    Source/Assistant/AssistantTools.cpp
    Source/Assistant/MixAnalysis.cpp
    Source/Compare/Compare.cpp
    Source/Host/RemotePlugin.cpp
    Source/Assistant/SessionWatch.cpp
    Source/Assistant/AssistantService.cpp
    Source/UI/MainComponent.cpp)

target_include_directories(Lathe PRIVATE Source)

target_compile_definitions(Lathe PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Lathe,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Lathe,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1                    # Linux only: https for the plug-in browser
    JUCE_LOAD_CURL_SYMBOLS_LAZILY=1    # load libcurl at run time, no link dependency
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# Lathe talks only to Lathe Cloud (its address is in Source/Cloud/CloudClient.cpp).
# For development only: ON lets LATHE_API_URL etc. point Lathe at a local backend.
option(LATHE_CLOUD_DEV_OVERRIDES "Let LATHE_API_URL etc. override the Lathe Cloud address (development only)" OFF)
if(LATHE_CLOUD_DEV_OVERRIDES)
    target_compile_definitions(Lathe PRIVATE LATHE_CLOUD_DEV_OVERRIDES=1)
endif()

if(WIN32 AND LATHE_ASIO AND LATHE_ASIO_SDK_DIR)
    target_compile_definitions(Lathe PRIVATE JUCE_ASIO=1)
    target_include_directories(Lathe PRIVATE ${LATHE_ASIO_SDK_DIR}/common)
    message(STATUS "Lathe: ASIO enabled (${LATHE_ASIO_SDK_DIR})")
endif()

if(MSVC)
    target_compile_options(Lathe PRIVATE /utf-8 /MP)
endif()

# --- Plug-in scanner helper ------------------------------------------------------
# Each plug-in is scanned in this separate process, so one that crashes or hangs
# while loading can't take Lathe down. It's copied next to the Lathe executable
# (inside Lathe.app/Contents/MacOS on the Mac).
juce_add_console_app(LatheScanner PRODUCT_NAME "LatheScanner")
target_sources(LatheScanner PRIVATE Source/Scanner/ScannerMain.cpp)
target_compile_definitions(LatheScanner PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(LatheScanner PRIVATE
    juce::juce_audio_processors juce::juce_gui_extra
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(LatheScanner PRIVATE /utf-8)
endif()

add_dependencies(Lathe LatheScanner)
add_custom_command(TARGET Lathe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheScanner> $<TARGET_FILE_DIR:Lathe>/
    COMMENT "Copying LatheScanner next to Lathe")

# --- Plug-in host helper -------------------------------------------------------------
# A plug-in set to "Run separately" runs in this process, so if it crashes only this goes
# down and Lathe carries on. Copied next to Lathe like LatheScanner.
juce_add_console_app(LatheHost PRODUCT_NAME "LatheHost")
target_sources(LatheHost PRIVATE Source/Host/HostMain.cpp)
target_include_directories(LatheHost PRIVATE Source/Host)
target_compile_definitions(LatheHost PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(LatheHost PRIVATE
    juce::juce_audio_processors juce::juce_gui_extra
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(LatheHost PRIVATE /utf-8)
endif()

add_dependencies(Lathe LatheHost)
add_custom_command(TARGET Lathe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheHost> $<TARGET_FILE_DIR:Lathe>/
    COMMENT "Copying LatheHost next to Lathe")

# --- LatheMCP: the AI assistant bridge ------------------------------------------------
# What an AI app (Claude Desktop, Claude Code, any MCP client) runs to reach Lathe: MCP
# over stdin/stdout, passed on to the running Lathe on 127.0.0.1 with its key. Copied
# next to the Lathe executable (inside Lathe.app/Contents/MacOS on the Mac).
juce_add_console_app(LatheMCP PRODUCT_NAME "LatheMCP")
target_sources(LatheMCP PRIVATE
    Source/Assistant/BridgeMain.cpp
    Source/Assistant/AssistantShared.cpp)
target_compile_definitions(LatheMCP PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    LATHE_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(LatheMCP PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(LatheMCP PRIVATE /utf-8)
endif()

add_dependencies(Lathe LatheMCP)
add_custom_command(TARGET Lathe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheMCP> $<TARGET_FILE_DIR:Lathe>/
    COMMENT "Copying LatheMCP next to Lathe")

# --- Lathe Patcher -----------------------------------------------------------------
# Checks GitHub for a newer Lathe and installs it in place. Ships inside
# Lathe.app/Contents/Helpers on the Mac and next to Lathe(.exe) elsewhere;
# Options > Check for Updates opens it.
juce_add_gui_app(LathePatcher
    PRODUCT_NAME    "LathePatcher"
    COMPANY_NAME    "Taddei"
    BUNDLE_ID       com.taddei.lathe.patcher
    VERSION         ${PROJECT_VERSION}
    ICON_BIG        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL      "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-small.png")
target_sources(LathePatcher PRIVATE
    Source/Patcher/PatcherMain.cpp
    Source/Patcher/Updater.cpp)
target_include_directories(LathePatcher PRIVATE Source)
target_compile_definitions(LathePatcher PRIVATE
    LATHE_PATCHER_VERSION="${PROJECT_VERSION}"
    JUCE_APPLICATION_NAME_STRING="Lathe Patcher"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
    JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(LathePatcher PRIVATE
    juce::juce_gui_extra juce::juce_cryptography
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(LathePatcher PRIVATE /utf-8)
endif()

# Space for play from anywhere: on the Mac, an app-wide key monitor (Objective-C++).
if(APPLE)
    target_sources(Lathe PRIVATE Source/UI/GlobalKeys.mm)   # compiled as Objective-C++, like JUCE's own .mm files
endif()

add_dependencies(Lathe LathePatcher)
if(APPLE)
    add_custom_command(TARGET Lathe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_BUNDLE_DIR:Lathe>/Contents/Helpers/LathePatcher.app
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:LathePatcher> $<TARGET_BUNDLE_DIR:Lathe>/Contents/Helpers/LathePatcher.app
        COMMENT "Copying LathePatcher.app into Lathe.app/Contents/Helpers")
else()
    add_custom_command(TARGET Lathe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LathePatcher> $<TARGET_FILE_DIR:Lathe>/
        COMMENT "Copying LathePatcher next to Lathe")
endif()
if(APPLE)
    # Re-sign (ad-hoc) now that the bundle holds a second executable.
    add_custom_command(TARGET Lathe POST_BUILD
        COMMAND codesign --force --deep --sign - $<TARGET_BUNDLE_DIR:Lathe>
        COMMENT "Ad-hoc signing Lathe.app")
endif()

# --- Tests (headless engine checks with a fake audio device) --------------------
# cmake -B build -DLATHE_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
option(LATHE_BUILD_TESTS "Build the engine tests" OFF)
if(LATHE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(LatheTests PRODUCT_NAME "LatheTests")
    target_sources(LatheTests PRIVATE
        Tests/EngineTests.cpp
        Source/Bench/Benchmark.cpp
        Source/Assistant/AssistantShared.cpp
        Source/Assistant/AssistantServer.cpp
        Source/Assistant/AssistantTools.cpp
        Source/Assistant/AssistantService.cpp
        Source/Assistant/MixAnalysis.cpp
        Source/Compare/Compare.cpp
        Source/Host/RemotePlugin.cpp
        Source/Assistant/SessionWatch.cpp
        Source/Model/Edit.cpp
        Source/Model/ChannelEq.cpp
        Source/Cloud/CloudClient.cpp
        Source/Cloud/Packing.cpp
        Source/Cloud/ProjectDiff.cpp
        Source/Cloud/Merge.cpp
        Source/Cloud/Revisions.cpp
        Source/Engine/AudioPool.cpp
        Source/Engine/PluginHost.cpp
        ${LATHE_DEVICE_SOURCES}
        Source/Engine/Engine.cpp
        Source/Engine/Mp3Writer.cpp
        Source/Engine/RenderPool.cpp
        Source/Engine/ClipJoin.cpp
        Source/Patcher/Updater.cpp
        # The Cloud menu and windows, for the sign-in / sign-out tests.
        Source/UI/CloudUI.cpp
        Source/UI/CloudLibrary.cpp
        Source/UI/Theme.cpp
        Source/UI/Widgets.cpp
        Source/UI/GlobalKeys.cpp
        Source/UI/Dialogs.cpp
        Source/UI/CloudLinks.cpp
        Source/UI/UndoHistory.cpp
        # The welcome song (Resources/Welcome.lathe).
        Source/Demo/Welcome.cpp)
    target_include_directories(LatheTests PRIVATE Source)
    target_compile_definitions(LatheTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=1 JUCE_LOAD_CURL_SYMBOLS_LAZILY=1 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0
        JUCE_MODAL_LOOPS_PERMITTED=1 LATHE_CLOUD_DEV_OVERRIDES=1
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)
    target_link_libraries(LatheTests PRIVATE
        LatheData
        LatheMutable
        lathe_lame
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp
        juce::juce_recommended_config_flags)
    if(MSVC)
        target_compile_options(LatheTests PRIVATE /utf-8)
    endif()
    # Copied on every build, not only when LatheTests itself relinks: a change to just the
    # host or the scanner would otherwise leave the tests running the old one.
    add_custom_target(LatheTestsHelpers
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:LatheTests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheScanner> $<TARGET_FILE_DIR:LatheTests>/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheHost> $<TARGET_FILE_DIR:LatheTests>/
        COMMENT "Copying LatheScanner and LatheHost next to LatheTests")
    add_dependencies(LatheTestsHelpers LatheScanner LatheHost)
    add_dependencies(LatheTests LatheTestsHelpers)

    # Three tiny VST3s for the scanning tests: one that works, one that crashes
    # while loading and one that never finishes loading. No moduleinfo.json, so
    # the scanner really has to load them.
    # (Off for sanitizer builds: a plug-in can't link against the sanitizer's runtime, and the
    # scanning tests they serve run in separate processes anyway.)
    option(LATHE_TEST_FIXTURES "Build the test VST3s for the scanning tests" ON)
    set(LATHE_FIXTURE_DIR ${CMAKE_BINARY_DIR}/test-vst3)
    set(LATHE_FIXTURE_LIST Gain Crash Hang Flaky)
    if(NOT LATHE_TEST_FIXTURES)
        set(LATHE_FIXTURE_LIST "")
    endif()
    foreach(fixture ${LATHE_FIXTURE_LIST})
        set(target LatheFixture${fixture})
        string(SUBSTRING ${fixture} 0 1 letter)
        juce_add_plugin(${target}
            COMPANY_NAME             "Lathe Tests"
            PLUGIN_MANUFACTURER_CODE Ltst
            PLUGIN_CODE              Lf${letter}1
            PRODUCT_NAME             "${fixture}Fixture"
            FORMATS                  VST3
            VST3_AUTO_MANIFEST       FALSE
            COPY_PLUGIN_AFTER_BUILD  FALSE)
        target_sources(${target} PRIVATE Tests/FixturePlugin.cpp)
        string(TOUPPER ${fixture} upper)
        target_compile_definitions(${target} PUBLIC
            LATHE_FIXTURE_${upper}=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
        target_link_libraries(${target} PRIVATE juce::juce_audio_processors)
        add_custom_command(TARGET ${target}_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<GENEX_EVAL:$<TARGET_PROPERTY:${target}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
                    "${LATHE_FIXTURE_DIR}/${fixture}Fixture.vst3")
        add_dependencies(LatheTests ${target}_VST3)
    endforeach()

    add_test(NAME engine COMMAND LatheTests)
    # Multi-core (docs/multicore.md): every test again with each block shared out over 4
    # threads, and random sessions compared sample for sample with one thread.
    add_test(NAME engine-on-4-threads COMMAND LatheTests)
    set_tests_properties(engine-on-4-threads PROPERTIES ENVIRONMENT "LATHE_TEST_THREADS=4;LATHE_TEST_VST3_DIR=${LATHE_FIXTURE_DIR}")
    add_test(NAME random-routing-1-vs-4-threads COMMAND LatheTests --random 200 4)
    set_tests_properties(engine PROPERTIES ENVIRONMENT "LATHE_TEST_VST3_DIR=${LATHE_FIXTURE_DIR}")
endif()

target_link_libraries(Lathe
    PRIVATE
        LatheData
        LatheMutable
        lathe_lame
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_extra
        juce::juce_cryptography
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
