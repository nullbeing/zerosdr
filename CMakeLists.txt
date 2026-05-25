cmake_minimum_required(VERSION 3.13)
project(zeroSDR VERSION 0.2.0 LANGUAGES C CXX)

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
    src/main.cpp
    src/framebuffer.cpp
    src/sdr_controller.cpp
    src/ui_renderer.cpp
    src/fft_processor.cpp
    src/font.cpp
    src/input_handler.cpp
    src/audio_demodulator.cpp
    src/audio_output.cpp
)

# Executable
add_executable(zerosdr ${SOURCES})

# Include directories
target_include_directories(zerosdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(zerosdr PRIVATE
    ${RTL_SDR_LIB}
    ${M_LIB}
    Threads::Threads
    ALSA::ALSA
)

# Install
install(TARGETS zerosdr DESTINATION bin)
install(FILES share/applications/zerosdr.desktop DESTINATION share/applications)
install(FILES share/images/zerosdr.png DESTINATION share/zerosdr/images)

# CPack configuration for DEB packaging
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "zerosdr")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Weiming Feng <bestedwin@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight SDR interface for Raspberry Pi")
set(CPACK_PACKAGE_DESCRIPTION "A graphical SDR application optimized for small SPI displays on Raspberry Pi with audio demodulation")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "librtlsdr0, libasound2, libc6")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Weiming Feng <bestedwin@gmail.com>")

include(CPack)
