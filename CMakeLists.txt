cmake_minimum_required(VERSION 3.13)
project(ZeroSDR VERSION 0.5.2 LANGUAGES C CXX)

# Default to an optimized build. Without this, a plain `cmake ..` produces
# an unoptimized binary that cannot keep up with real-time audio on the
# Cortex-A53 in the Cardputer Zero.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for ARM platforms
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        # 64-bit ARM (arm64)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a -mtune=cortex-a53")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=cortex-a53")
    else()
        # 32-bit ARM (armhf)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
    endif()
endif()

# Find required libraries
find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)

# PulseAudio client library: used to read and write the default sink volume,
# which is the system volume on the Cardputer Zero (PipeWire's pulse server).
if(CMAKE_CROSSCOMPILING)
    find_library(PULSE_LIB NAMES pulse
                 PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
                 NO_DEFAULT_PATH)
else()
    find_library(PULSE_LIB NAMES pulse
                 PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE})
endif()

if(NOT PULSE_LIB)
    message(FATAL_ERROR "libpulse not found. Please install libpulse-dev")
endif()

# Try to find rtlsdr library
if(CMAKE_CROSSCOMPILING)
    # Cross-compiling: search in cross-compile sysroot
    find_library(RTL_SDR_LIB NAMES rtlsdr librtlsdr
                 PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
                 NO_DEFAULT_PATH)
else()
    # Native build
    find_library(RTL_SDR_LIB NAMES rtlsdr librtlsdr
                 PATHS /usr/lib /usr/local/lib /lib/${CMAKE_LIBRARY_ARCHITECTURE})
endif()

if(NOT RTL_SDR_LIB)
    message(FATAL_ERROR "librtlsdr not found. Please install librtlsdr-dev")
endif()

# FreeType for the anti-aliased radar callsign
find_package(Freetype REQUIRED)

# Find math library
if(CMAKE_CROSSCOMPILING)
    find_library(M_LIB m PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} NO_DEFAULT_PATH)
else()
    find_library(M_LIB m PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib)
endif()
if(NOT M_LIB)
    set(M_LIB "m")
endif()

# Source files
set(SOURCES
    # Main
    src/main.cpp

    # Core framework
    src/core/page_manager.cpp
    src/core/beep_player.cpp
    src/core/gps_locator.cpp
    src/core/first_run_guide.cpp

    # Pages
    src/pages/sdr_page.cpp
    src/pages/adsb_page.cpp
    src/pages/spectrum_page.cpp
    src/pages/rfi_page.cpp
    src/pages/ais_page.cpp
    src/pages/aprs_page.cpp
    src/pages/acars_page.cpp

    # AIS
    src/ais/ais_decoder.cpp
    src/ais/ais_message.cpp
    src/ais/ais_store.cpp
    src/ais/nmea_encoder.cpp

    # APRS
    src/aprs/aprs_decoder.cpp
    src/aprs/aprs_store.cpp
    src/aprs/aprs_log.cpp

    # ACARS integration (external acarsdec subprocess)
    src/acars/acars_controller.cpp
    src/acars/acars_labels.cpp
    src/acars/acars_log.cpp

    # ADSB integration
    src/adsb/readsb_controller.cpp
    src/adsb/readsb_json_parser.cpp
    src/adsb/adsb_config.cpp
    src/adsb/map_background.cpp

    # UI rendering
    src/ui/base_ui_renderer.cpp
    src/ui/sdr_ui_renderer.cpp
    src/ui/adsb_ui_renderer.cpp
    src/ui/radar_ui_renderer.cpp
    src/ui/ais_ui_renderer.cpp
    src/ui/ais_portrait.cpp
    src/ui/aprs_ui_renderer.cpp
    src/ui/acars_ui_renderer.cpp
    src/ui/ttf_text.cpp

    # Hardware
    src/framebuffer.cpp
    src/sdr_controller.cpp
    src/audio_output.cpp
    src/system_volume.cpp
    src/imu_compass.cpp

    # Processing
    src/fft_processor.cpp
    src/audio_demodulator.cpp

    # Utils
    src/font.cpp
    src/font8x16.cpp
    src/input_handler.cpp
    src/screenshot.cpp
    src/app_config.cpp
)

# Executable
add_executable(zerosdr ${SOURCES})

# Include directories
target_include_directories(zerosdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Offline AIS replay tool. It shares the native decoder with the application
# but is a development/validation helper, so it is not installed.
add_executable(ais_replay
    tools/ais_replay.cpp
    src/ais/ais_decoder.cpp
    src/ais/ais_message.cpp
    src/ais/nmea_encoder.cpp
)
target_include_directories(ais_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ais_replay PRIVATE
    ${M_LIB}
    Threads::Threads
)

# Offline APRS replay tool. Shares the native decoder with the application
# but is a development/validation helper, so it is not installed.
add_executable(aprs_replay
    tools/aprs_replay.cpp
    src/aprs/aprs_decoder.cpp
)
target_include_directories(aprs_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aprs_replay PRIVATE ${M_LIB})

# Audio-only APRS replay, for validating the AFSK/HDLC chain on receiver
# recordings (48000 Hz raw float32 mono).
add_executable(aprs_audio_replay
    tools/aprs_audio_replay.cpp
    src/aprs/aprs_decoder.cpp
)
target_include_directories(aprs_audio_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aprs_audio_replay PRIVATE ${M_LIB})

# AIS name padding regression test (development helper, not installed).
add_executable(ais_message_test
    tests/ais_message_test.cpp
    src/ais/ais_message.cpp
)
target_include_directories(ais_message_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ais_message_test PRIVATE ${M_LIB})

# AIS NMEA encoder regression test (development helper, not installed).
add_executable(nmea_encoder_test
    tests/nmea_encoder_test.cpp
    src/ais/nmea_encoder.cpp
)
target_include_directories(nmea_encoder_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(nmea_encoder_test PRIVATE ${M_LIB})

# APRS AFSK/AX.25 decoder regression test (development helper, not installed).
add_executable(aprs_decoder_test
    tests/aprs_decoder_test.cpp
    src/aprs/aprs_decoder.cpp
)
target_include_directories(aprs_decoder_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aprs_decoder_test PRIVATE ${M_LIB})

# APRS station/track store regression test (development helper, not installed).
add_executable(aprs_store_test
    tests/aprs_store_test.cpp
    src/aprs/aprs_store.cpp
)
target_include_directories(aprs_store_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aprs_store_test PRIVATE ${M_LIB})

# APRS daily log persistence/retention test (development helper, not installed).
add_executable(aprs_log_test
    tests/aprs_log_test.cpp
    src/aprs/aprs_log.cpp
)
target_include_directories(aprs_log_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(aprs_log_test PRIVATE ${M_LIB})

# Link libraries
target_link_libraries(zerosdr PRIVATE
    ${RTL_SDR_LIB}
    ${M_LIB}
    Threads::Threads
    ALSA::ALSA
    ${PULSE_LIB}
    Freetype::Freetype
)

# Install
# 1) APPLaunch layout (used by the CardputerZero AppStore package)
install(TARGETS zerosdr DESTINATION share/APPLaunch/bin)
install(PROGRAMS share/bin/run-zerosdr DESTINATION share/APPLaunch/bin)
install(FILES share/APPLaunch/applications/zerosdr.desktop
        DESTINATION share/APPLaunch/applications)
install(FILES share/images/zerosdr.png
        DESTINATION share/APPLaunch/share/images)
# ADSB decoder bundled with the RTL-SDR backend (Debian's readsb lacks it).
install(PROGRAMS vendor/readsb/readsb DESTINATION lib/zerosdr)
# ACARS decoder, GPLv2-only: bundled and executed as a separate process.
install(PROGRAMS vendor/acarsdec/acarsdec DESTINATION lib/zerosdr)
install(FILES vendor/acarsdec/LICENSE.md DESTINATION share/doc/zerosdr
        RENAME acarsdec-LICENSE.md)
install(FILES vendor/acarsdec/VERSION DESTINATION share/doc/zerosdr
        RENAME acarsdec-VERSION.txt)

# 2) CLI install. The system-wide /usr/share/applications entry is NOT
#    installed: the AppStore only allows usr/share/APPLaunch/**,
#    usr/share/<package-name>/** and a fixed list of shared dirs, so an
#    entry directly under /usr/share/applications would be rejected.
install(TARGETS zerosdr DESTINATION bin)

# CPack configuration for DEB packaging
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "zerosdr")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Weiming Feng <bestedwin@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight SDR interface for Raspberry Pi")
set(CPACK_PACKAGE_DESCRIPTION "A graphical SDR application optimized for small SPI displays on Raspberry Pi with audio demodulation")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "librtlsdr0, libasound2, libpulse0, libfreetype6, libc6")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Weiming Feng <bestedwin@gmail.com>")

include(CPack)
