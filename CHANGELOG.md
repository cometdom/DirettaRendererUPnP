# Changelog

All notable changes to the Diretta UPnP Renderer project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned
- Web UI for configuration
- Volume control via UPnP RenderingControl
- Configuration file support
- Docker container
- Systemd service generator script

---
## [1.0.1] - 2025-12-05

### Fixed
- **Gapless playback bug**: Fixed issue where tracks from previous album would play after Stop
  - Clear next URI queue on Stop action
  - Clear next URI queue when setting new transport URI
- **UPnP gapless detection**: Added SetNextAVTransportURI action declaration in SCPD
  - BubbleUPnP and other control points now properly detect gapless support

### Added
- Multi-architecture support in Makefile (x64: v2/v3/v4/zen4, ARM64, RISC-V)
- Automatic CPU capability detection for optimal library selection
- `make list-variants` command to show available library variants
- `make arch-info` command for architecture detection details

### Changed
- Makefile now auto-detects architecture and uses appropriate Diretta library (.a)
- Improved SCPD completeness (added Seek, GetMediaInfo, Next, Previous actions)

## [1.0.0] - 2025-12-03

### Added
- Initial release
- Native UPnP/DLNA renderer with Diretta protocol support
- Support for high-resolution audio up to DSD1024 and PCM 1536kHz
- Gapless playback implementation
- Adaptive packet sizing based on audio format
- Full transport control (Play, Stop, Pause, Resume, Seek)

## [1.0.0] - 20225-12-04

### 🎉 Initial Release

The world's first native UPnP/DLNA renderer with Diretta protocol support!

### Added

#### Core Features
- **UPnP/DLNA MediaRenderer** implementation
  - AVTransport service (Play, Stop, Pause, Seek)
  - ConnectionManager service
  - Basic RenderingControl service
- **Diretta Protocol Integration**
  - Native bit-perfect streaming
  - Bypass OS audio stack
  - Direct DAC communication
- **Audio Format Support**
  - PCM: Up to 1536kHz/32bit
  - DSD: DSD64, DSD128, DSD256, DSD512, DSD1024
  - Compressed: FLAC, ALAC (via FFmpeg decoding)
- **Transport Controls**
  - Play/Stop/Pause/Resume
  - Seek with time-based positioning
  - Position tracking and reporting
- **Gapless Playback**
  - Seamless track transitions
  - Next track preloading
  - Buffer management

#### Network Optimization
- **Adaptive Packet Sizing**
  - Small packets (~1-3k) for 16bit/44.1-48kHz (prevents fragmentation)
  - Jumbo frames (~16k) for Hi-Res formats (maximum performance)
  - Automatic detection based on audio format
- **Jumbo Frame Support**
  - MTU up to 16128 bytes
  - Configurable MTU
  - Fallback to standard MTU

#### Audio Engine
- **FFmpeg Integration**
  - On-the-fly decoding of compressed formats
  - Multiple codec support
  - Stream handling
- **Format Detection**
  - Automatic format identification
  - Sample rate and bit depth detection
  - DSD vs PCM differentiation
- **Buffer Management**
  - Configurable buffer size (1-10 seconds)
  - Underrun prevention
  - Smooth playback

#### User Interface
- **Command-Line Interface**
  - `--port`: Configurable UPnP port
  - `--buffer`: Adjustable buffer size
  - `--name`: Custom device name
  - `--uuid`: Custom device UUID
  - `--no-gapless`: Option to disable gapless
- **Logging**
  - Detailed console output
  - Format information
  - Transport state changes
  - Error reporting

#### Documentation
- Comprehensive README with quick start
- Detailed installation guide
- Configuration guide with optimization tips
- Troubleshooting guide
- Contributing guidelines
- Complete LICENSE with SDK notices

### Technical Implementation

#### Architecture
- Modular design with clear separation of concerns
- Thread-safe implementation
- Non-blocking audio processing
- Efficient resource management

#### Performance
- Low CPU usage (<5% for CD quality)
- Minimal latency (2s default buffer)
- Bit-perfect audio delivery
- Stable under load

#### Compatibility
- Tested on Fedora 38
- Tested on AudioLinux
- Compatible with Ubuntu/Debian
- Works with JPlay, BubbleUPnP, mConnect

### Known Issues
- Seek operations may receive multiple commands from some control points (normal behavior)
- Some control points may not support all UPnP features
- Requires root privileges for network access

### Dependencies
- Diretta Host SDK v1.47+
- FFmpeg 4.4+
- libupnp 1.14+
- C++17 compiler (GCC 8+ or Clang 7+)

---

## Version History

### Version Numbering

We use [Semantic Versioning](https://semver.org/):
- **MAJOR**: Incompatible API changes
- **MINOR**: New functionality (backwards compatible)
- **PATCH**: Bug fixes (backwards compatible)

### Release Cadence

- **Major releases**: When significant features are added
- **Minor releases**: For new features and improvements
- **Patch releases**: For bug fixes

---

## Upgrading

### From Source

```bash
cd DirettaUPnPRenderer
git pull
make clean
make
sudo systemctl restart diretta-renderer  # If using systemd
```

### Configuration Changes

Check documentation for any configuration changes between versions.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to contribute changes.

All notable changes should be documented here before release.

---

## Links

- **Repository**: https://github.com/YOUR_USERNAME/DirettaUPnPRenderer
- **Issues**: https://github.com/YOUR_USERNAME/DirettaUPnPRenderer/issues
- **Diretta Website**: https://www.diretta.link

---

*For older versions, see the [releases page](https://github.com/YOUR_USERNAME/DirettaUPnPRenderer/releases).*
