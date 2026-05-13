cmake_minimum_required(VERSION 3.13)
project(zeroSDR VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for ARM platforms
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a -mtune=cortex-a53")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=cortex-a53")
endif()

# Find required libraries
find_package(Threads REQUIRED)

# Try to find rtlsdr library
find_library(RTL_SDR_LIB NAMES rtlsdr librtlsdr PATHS /usr/lib /usr/local/lib /lib/aarch64-linux-gnu)
if(NOT RTL_SDR_LIB)
    message(FATAL_ERROR "librtlsdr not found. Please install librtlsdr-dev")
endif()

find_library(M_LIB m)

# Source files
set(SOURCES
    src/main.cpp
    src/framebuffer.cpp
    src/sdr_controller.cpp
    src/ui_renderer.cpp
    src/fft_processor.cpp
    src/font.cpp
    src/input_handler.cpp
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
)

# Install
install(TARGETS zerosdr DESTINATION bin)

# CPack configuration for DEB packaging
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "zerosdr")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Weiming Feng <bestedwin@gmail.com>")
set(CPACK_PACKAGEN_SUMMARY "Lightweight SDR interface for Raspberry Pi")
set(CPACK_PACKAGE_DESCRIPTION "A graphical SDR application optimized for smaSPI displays on Raspberry Pi")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "librtlsdr0, libc6")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

include(CPack)
