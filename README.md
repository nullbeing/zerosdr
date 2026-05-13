# zeroSDR

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%20%7C%20M5Stack-orange.svg)]()

A lightweight SDR (Software Defined Radio) spectrum viewer designed for embedded systems with small SPI displays.

## 🎯 Project Background

zeroSDR was originally developed for the upcoming **M5Stack Cardputer Zero**, a compact Raspberry Pi Zero device. The current development environment uses a **Raspberry Pi 5** with a 320x170 SPI display to prototype and test the interface.

## ⚠️ Important Hardware Compatibility Notes

- **Tested Hardware**: This software has **only been tested with RTL-SDR Blog V4** dongles
- **Driver Version Critical**: You **MUST use the latest rtl-sdr drivers**. Older driver versions cause frequency offset issues where the displayed spectrum does not match the actual tuned frequency
- **Display**: Optimized for 320x170 pixel SPI displays connected to `/dev/fb0` (Linux framebuffer)

## ✨ Features

- Real-time spectrum and waterfall visualization
- RTL-SDR device support
- Optimized for small embedded displays
- Low resource footprint suitable for Raspberry Pi Zero 2W
- Direct framebuffer rendering for minimal overhead

## 🔧 Hardware Requirements

- **Raspberry Pi 5** or **Raspberry Pi Zero 2W** (or M5Stack Cardputer Zero when available)
- **RTL-SDR Blog V4** USB dongle (other RTL-SDR devices may work but are untested)
- 320x170 SPI display connected to `/dev/fb0`

## 📦 Software Requirements

- **Latest rtl-sdr library and drivers** (critical for correct frequency tuning)
- Linux framebuffer support (`/dev/fb0`)
- CMake 3.13+
- C++17 compatible compiler

## 🛠️ Building from Source

### Install Dependencies

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake librtlsdr-dev git
```

**Important**: Ensure you have the latest `librtlsdr-dev` package. On older distributions, you may need to build rtl-sdr from source:

```bash
git clone https://github.com/osmocom/rtl-sdr.git
cd rtl-sdr
mkdir build && cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make
sudo make install
sudo ldconfig
```

### Build for Raspberry Pi 5

On Raspberry Pi 5, build directly on the device:

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

### Build for M5Stack Cardputer Zero (Raspberry Pi Zero 2W)

The Cardputer Zero is based on Raspberry Pi Zero 2W. Due to limited resources, you have two options:

**Option 1: Build directly on the device** (slower but simpler)

```bash
mkdir build
cd build
cmake ..
make -j4  # Pi Zero 2W has 4 cores, but build will be slow
```

**Option 2: Cross-compile on a faster machine** (recommended)

On your development machine (e.g., Raspberry Pi 5 or x86_64 Linux):

```bash
# Install cross-compilation toolchain
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Create build directory
mkdir build-zero2w
cd build-zero2w

# Configure for ARM64 (Pi Zero 2W architecture)
cmake .. \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++

# Build
make -j$(nproc)

# Copy the binary to your Cardputer Zero
scp zerosdr pi@cardputer-zero.local:/home/pi/
```

### Install

```bash
sudo make install
```

Or build and install a DEB package:

```bash
# Install packaging tools
sudo apt-get install -y debhelper devscripts

# Build the package
dpkg-buildpackage -us -uc -b

# Install (from parent directory)
sudo dpkg -i ../zerosdr_*.deb
```

## 🚀 Usage

```bash
z the displayed spectrum doesn't match the actual frequency you're tuned to:
- **Update your rtl-sdr drivers to the latest version** (this is the most common cause)
- Check for PPM correction needs with `rtl_test`

### RTL-SDR Not Detec
```bash
# Verify framebuffer device exists
ls -l /dev/fb0

# Check current framebuffer resolution
fbset
```

### Permission Denied

```bash
# Add user to video group
sudo usermod -a -G video $USER

# Log out and back in, or reboot
```

## 📖 Documentation

- [BUILD.md](BUILD.md) - Detailed build instructions including cross-compilation
- [CONTRIBUTING.md](CONTRIBUTING.md) - Contribution guidelines

## 🤝 Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

When reporting issues, please include:
- RTL-SDR dongle model and version
- rtl-sdr driver version (`rtl_test` output)
- Raspberry Pi model
- Display specNSE) file for details.

## 👤 Author

**Weiming Feng**
- Email: bestedwin@gmail.com

## 🙏 Acknowledgments

- Built with [rtl-sdr](https://github.com/osmocom/rtl-sdr) library
- Designed for M5Stack Cardputer Zero
