cmake_minimum_required(VERSION 3.22)

# macOS 11+. Builds for the machine you're on by default; for a universal
# binary pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64". Set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Lathe VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- JUCE ---------------------------------------------------------------------
# Downloaded on first configure (same pin as the plug-in projects). To use a
# local checkout instead:  cmake -B build -DLATHE_JUCE_DIR=/path/to/JUCE
set(LATHE_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout")

if(LATHE_JUCE_DIR)
    add_subdirectory(${LATHE_JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- ASIO (Windows) -------------------------------------------------------------
# The UMC1820's low-latency Windows driver is ASIO. Steinberg's ASIO SDK is
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
    DOCUMENT_EXTENSIONS          lathe)

target_sources(Lathe PRIVATE
    Source/Main.cpp
    Source/Model/Edit.cpp
    Source/Model/ChannelEq.cpp
    Source/Engine/AudioPool.cpp
    Source/Engine/PluginHost.cpp
    Source/Engine/PluginStore.cpp
    Source/Engine/Engine.cpp
    Source/UI/Theme.cpp
    Source/UI/Widgets.cpp
    Source/UI/TransportBar.cpp
    Source/UI/Arrangement.cpp
    Source/UI/Mixer.cpp
    Source/UI/DeviceChain.cpp
    Source/UI/EqEditor.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/Dialogs.cpp
    Source/UI/PluginBrowser.cpp
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
        Source/Model/Edit.cpp
        Source/Model/ChannelEq.cpp
        Source/Engine/AudioPool.cpp
        Source/Engine/PluginHost.cpp
        Source/Engine/PluginStore.cpp
        Source/Engine/Engine.cpp)
    target_include_directories(LatheTests PRIVATE Source)
    target_compile_definitions(LatheTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(LatheTests PRIVATE
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    if(MSVC)
        target_compile_options(LatheTests PRIVATE /utf-8)
    endif()
    add_dependencies(LatheTests LatheScanner)
    add_custom_command(TARGET LatheTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LatheScanner> $<TARGET_FILE_DIR:LatheTests>/)

    # Three tiny VST3s for the scanning tests: one that works, one that crashes
    # while loading and one that never finishes loading. No moduleinfo.json, so
    # the scanner really has to load them.
    set(LATHE_FIXTURE_DIR ${CMAKE_BINARY_DIR}/test-vst3)
    foreach(fixture Gain Crash Hang)
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
    set_tests_properties(engine PROPERTIES ENVIRONMENT "LATHE_TEST_VST3_DIR=${LATHE_FIXTURE_DIR}")
endif()

target_link_libraries(Lathe
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
