cmake_minimum_required(VERSION 3.22)

# Release builds get a build number and the GitHub repo to update from (set by the
# release workflow). Local builds keep 0 / empty and never update themselves.
set(CORRECTIONHOST_BUILD_NUMBER "0" CACHE STRING "Build number baked into release builds (0 = local build)")
set(CORRECTIONHOST_UPDATE_REPO "" CACHE STRING "GitHub owner/repo whose releases the app updates from")

project(CorrectionHost VERSION 0.2.${CORRECTIONHOST_BUILD_NUMBER} LANGUAGES C CXX)

# Link the C++ runtime statically on Windows so the .exe runs on machines without
# the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

option(CORRECTIONHOST_BUILD_TESTS "Build the headless test programs" OFF)
set(CORRECTIONHOST_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE checkout (otherwise JUCE is downloaded)")

#==============================================================================
# JUCE
#==============================================================================
if(CORRECTIONHOST_JUCE_DIR)
    add_subdirectory("${CORRECTIONHOST_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    # Make JUCE's ASIO device follow the interface's sample rate after driver resets.
    set(CORRECTIONHOST_JUCE_SOURCE "${juce_SOURCE_DIR}")
    include(cmake/PatchJuceAsio.cmake)
endif()

#==============================================================================
# The app
#==============================================================================
# Logo artwork (sources in Resources/src). macOS icons carry the standard Big Sur padding.
if(APPLE)
    set(CORRECTIONHOST_ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_mac_1024.png")
else()
    set(CORRECTIONHOST_ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_windows_1024.png")
endif()

juce_add_gui_app(CorrectionHost
    PRODUCT_NAME                  "Correction Host"
    ICON_BIG                      "${CORRECTIONHOST_ICON_BIG}"
    ICON_SMALL                    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_small_256.png"
    COMPANY_NAME                  "CorrectionHost"
    BUNDLE_ID                     com.correctionhost.app
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Correction Host captures the audio sent to BlackHole so it can apply room correction to your second pair of speakers.")

target_sources(CorrectionHost PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/HostEngine.cpp
    Source/JuceDeviceCaptureSource.cpp
    Source/PlatformCapture.cpp
    Source/Updater.cpp)

if(WIN32)
    target_sources(CorrectionHost PRIVATE
        Source/LoopbackCaptureSource.cpp
        Source/WasapiLoopback.cpp)
    target_link_libraries(CorrectionHost PRIVATE ole32 avrt winmm)
endif()

target_compile_definitions(CorrectionHost PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:CorrectionHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:CorrectionHost,JUCE_VERSION>"
    CORRECTIONHOST_BUILD_NUMBER=${CORRECTIONHOST_BUILD_NUMBER}
    CORRECTIONHOST_UPDATE_REPO="${CORRECTIONHOST_UPDATE_REPO}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0)

if(APPLE)
    target_compile_definitions(CorrectionHost PRIVATE JUCE_PLUGINHOST_AU=1)
endif()

target_link_libraries(CorrectionHost
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# Headless tests (built on any platform; used to verify the code on Linux)
#==============================================================================
if(CORRECTIONHOST_BUILD_TESTS)
    # Simulates mismatched clocks, block sizes and dropouts to check the
    # drift-compensating FIFO (and measures its signal quality).
    juce_add_console_app(DriftFifoTest PRODUCT_NAME "DriftFifoTest")
    target_sources(DriftFifoTest PRIVATE Tests/DriftFifoTest.cpp)
    target_include_directories(DriftFifoTest PRIVATE Source)
    target_compile_definitions(DriftFifoTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(DriftFifoTest PRIVATE
        juce::juce_audio_basics
        juce::juce_recommended_config_flags)

    # Checks the release-JSON parsing and build-number logic used by the self-updater.
    juce_add_console_app(UpdaterTest PRODUCT_NAME "UpdaterTest")
    target_sources(UpdaterTest PRIVATE Tests/UpdaterTest.cpp Source/Updater.cpp)
    target_include_directories(UpdaterTest PRIVATE Source)
    target_compile_definitions(UpdaterTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(UpdaterTest PRIVATE juce::juce_events juce::juce_cryptography juce::juce_recommended_config_flags)

    # Renders the logo at tray sizes to PNG files for visual checking.
    juce_add_console_app(IconRenderTest PRODUCT_NAME "IconRenderTest")
    target_sources(IconRenderTest PRIVATE Tests/IconRenderTest.cpp)
    target_include_directories(IconRenderTest PRIVATE Source)
    target_compile_definitions(IconRenderTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(IconRenderTest PRIVATE juce::juce_graphics juce::juce_recommended_config_flags)

    # A tiny VST3 (fixed gain with saved state) and a test that hosts it through the
    # engine's real audio callback with a fake output device.
    juce_add_plugin(TestGainPlugin
        PRODUCT_NAME "TestGain"
        COMPANY_NAME "CorrectionHost"
        PLUGIN_MANUFACTURER_CODE Chst
        PLUGIN_CODE Tgai
        FORMATS VST3
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(TestGainPlugin PRIVATE Tests/TestGainPlugin.cpp)
    target_compile_definitions(TestGainPlugin PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(TestGainPlugin PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)

    juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
    target_sources(EngineTest PRIVATE
        Tests/EngineTest.cpp
        Source/HostEngine.cpp
        Source/JuceDeviceCaptureSource.cpp
        Source/PlatformCapture.cpp)
    if(WIN32)
        target_sources(EngineTest PRIVATE Source/LoopbackCaptureSource.cpp Source/WasapiLoopback.cpp)
        target_link_libraries(EngineTest PRIVATE ole32 avrt winmm)
    endif()
    target_include_directories(EngineTest PRIVATE Source)
    target_compile_definitions(EngineTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(EngineTest PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_recommended_config_flags)

    # Runs the Windows loopback adapter against a fake WASAPI backend so the
    # JUCE-side glue can be compiled and exercised on non-Windows machines.
    if(NOT WIN32)
        juce_add_console_app(LoopbackGlueTest PRODUCT_NAME "LoopbackGlueTest")
        target_sources(LoopbackGlueTest PRIVATE
            Source/LoopbackCaptureSource.cpp
            Tests/WasapiLoopbackStub.cpp
            Tests/LoopbackGlueTest.cpp)
        target_include_directories(LoopbackGlueTest PRIVATE Source)
        target_compile_definitions(LoopbackGlueTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(LoopbackGlueTest PRIVATE
            juce::juce_audio_basics
            juce::juce_events
            juce::juce_recommended_config_flags)
    endif()
endif()
