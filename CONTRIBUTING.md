# Contributing to zeroSDR

Thank you for your interest in contributing to zeroSDR! This document provides guidelines for contributing to the project.

## 🐛 Reporting Issues

When reporting bugs or issues, please include:

1. **Hardware Information**:
   - RTL-SDR dongle model and version (e.g., "RTL-SDR Blog V4")
   - Raspberry Pi model (e.g., "Raspberry Pi 5", "Pi Zero 2W")
   - Display specifications (resolution, connection type)

2. **Software Environment**:
   - Operating system and version
   - rtl-sdr driver version (run `rtl_test` and include the output)
   - CMake version
   - Compiler version

3. **Issue Description**:
   - Clear description of the problem
   - Steps to reproduce
   - Expected behavior vs actual behavior
   - Any error messages or logs

### Known Issues to Check First

Before reporting, check if your issue is related to:
- **Frequency offset**: Update to the latest rtl-sdr drivers
- **Device not detected**: Check USB connection and udev rules
- **Permission denied**: Add user to `video` group

## 🔧 Development Setup

### Prerequisites

```bash
sudo apt-get install -y build-essential cmake librtlsdr-dev git
```

### Building

```bash
git clone https://github.com/<your-username>/zeroSDR.git
cd zeroSDR
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 📝 Code Style

### C++ Guidelines

- **Standard**: C++17
- **Formatting**: 
  - 4 spaces for indentation (no tabs)
  - Opening braces on the same line for functions and classes
  - Descriptive variable names (avoid single-letter names except for loop counters)
  
- **Naming Conventions**:
  - Classes: `PascalCase` (e.g., `SdrController`)
  - Functions/methods: `snake_case` (e.g., `process_samples`)
  - Member variables: `snake_case` with trailing underscore (e.g., `sample_rate_`)
  - Constants: `UPPER_SNAKE_CASE`

### CMake

- Maintain compatibility with CMake 3.13+
- Keep ARM optimization flags for embedded targets
- Document any new dependencies

## 🚀 Submitting Pull Requests

1. **Fork the repository** and create a new branch from `main`
   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes**:
   - Write clear, concise commit messages
   - Keep commits focused (one logical change per commit)
   - Test your changes on actual hardware if possible

3. **Test thoroughly**:
   - Build on both x86_64 and ARM if possible
   - Test with RTL-SDR Blog V4 (or document testing with other hardware)
   - Verify no memory leaks or crashes during extended operation

4. **Update documentation**:
   - Update README.md if adding features or changing usage
   - Update BUILD.md if changing build process
   - Add comments for complex code sections

5. **Submit the PR**:
   - Provide a clear description of what the PR does
   - Reference any related issues
   - Include testing details (hardware used, duration tested, etc.)

## 🧪 Testing Requirements

Since this is embedded hardware software, testing requirements are flexible:

- **Minimum**: Code must compile without warnings
- **Recommended**: Test on Raspberry Pi with RTL-SDR dongle
- **Ideal**: Test on both Pi 5 and Pi Zero 2W with extended runtime

If you don't have access to the hardware, mention this in your PR and we'll help with testing.

## 🎯 Areas for Contribution

We welcome contributions in these areas:

- **Hardware Support**: Testing and fixes for different RTL-SDR models
- **Display Support**: Support for different framebuffer resolutions
- **Performance**: Optimization for Pi Zero 2W
- **Features**: Waterfall improvements, frequency memory, signal recording
- **Documentation**: Tutorials, troubleshooting guides, translations
- **M5Stack Cardputer Zero**: Porting and optimization when hardware is available

## 📜 License

By contributing to zeroSDR, you agree that your contributions will be licensed under the GNU General Public License v3.0.

## 💬 Questions?

Feel free to open an issue for questions or discussion before starting major work. This helps ensure your contribution aligns with the project's direction.

## 🙏 Thank You

Every contribution, whether it's code, documentation, bug reports, or testing, helps make zeroSDR better for everyone!
