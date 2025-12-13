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
## [1.0.6]
### Add
**Verbose Logging Mode**
- Added optional --verbose mode to reduce console output verbosity
By default, the renderer now displays only essential user-facing messages. Technical debug information can be enabled using the --verbose flag.
- Example:
sudo ./DirettaRendererUPnP --port 4005 --target 1 --buffer 0.9 --verbose
Command Line Options

- Added --verbose / -v flag to enable detailed debug output
- Added --help documentation for the new verbose mode

### Fixed
- Removed clicks or pink noise bursts at the end of the album or during the transition between tracks in a playlist
 * Buffers weren't correctly cleared at the en of an album or during a format change.
- Disable gapless playback on format changes (Many thanks to herisson88)
When the next track has a different format (sample rate, bit depth,
channels, or DSD vs PCM), disable gapless transition and force a
clean stop/start sequence to prevent audio artifacts.
- Optimize FFmpeg settings for faster streaming startup (Many thanks to herisson88)
  * Add probesize (1MB) and analyzeduration (1.5s) for faster initial buffering
  * Increase network buffer from 32KB to 512KB for better stability
Fix thread-safety issues in AudioEngine (Many thanks to herisson88)
- Replace unsafe detached preload thread with joinable thread
  * Add m_preloadThread member and m_preloadRunning atomic flag
  * Add waitForPreloadThread() helper called in stop() and destructor
  * Prevents use-after-free when AudioEngine is destroyed during preload
- Fix race condition in setNextURI()
  * std::string is not thread-safe for concurrent read/write
  * Add pending mechanism with m_pendingMutex protection
  * UPnP thread queues URI, audio thread applies it safely in process()
  * Use memory_order_acquire/release for proper synchronization
- Update stop() to clear pending state and wait for preload thread
- Update process() to apply pending next URI before processing
Serialize UPnP action callbacks to prevent race conditions
- When multiple UPnP commands arrive in quick succession (e.g., rapid track
skipping), they can execute concurrently and cause race conditions leading
to silent playback or segmentation faults.
- Add std::lock_guard<std::mutex> at the entry of each UPnP
callback to ensure only one action executes at a time:
 * onSetURI
 * onSetNextURI
 * onPlay
 * onPause
 * onStop
 * onSeek

## [1.0.5]
### Fixed
**Audio Player**
  - No more delays when starting playback
  Now you can set --buffer from 0.5 to 4.0 to ensure gapless playback on your setup.
  Buffer from 0.5 to 1.0 no delay.
  Buffer from 1.0 to 4.0 increase delay but can guarantee stability and gapless on some equipment.


## [1.0.4]
 ### Fixed
  **Audio**
  - Correct PCM data size calculation and convert S32 to S24 format
  - Improving the detection of actual bit depth from the audio source
  - Fixed detection of actual bit depth from audio source
  - Detect real bit depth from source instead of relying on FFmpeg's internal format
  - Adjust sync buffer thread mode to improve performance (thanks to herisson88)
 **Diretta Target**
  - Increase retry delay for target availability verification from 2 to 5 seconds
  - Update target information display to use targetName instead of Device
  - Correct iteration over targets and target selection by index
 **Systemd**
  - Update service management instructions in installation script
  - Improve binary detection and installation instructions in the systemd script
 ### Add
  **Diretta Target**
  - Implement retry mechanism for target discovery with detailed feedback
  **Systemd**
  - Update service management instructions in installation script
  - Add a uninstall script for diretta-renderer.service
  - Add systemd service and configuration for Diretta UPnP Renderer

## [1.0.3] - 2025-12-07
 ### Added
  **Display more device-friendly information when outputting device list**
  - Added display of target device name
  - Display device output name and configuration information
  - Output product ID in hexadecimal format
  - Display protocol version number
  - Indicate whether multi-port support is enabled
  - Display synchronization status and its detailed parameters




## [1.0.2] - 2025-12-06

### Added
  **restructuring of the Makefile to:**
  - automatically manage library variants depending on the CPU architecture,
  - ensure multi‑platform compatibility (x64, ARM64),
  - prevent errors on unsupported architectures,
  - simplify maintenance by centralizing the suffix into a reusable variable for all libraries
  **Diretta target device selection and validation features**
  - Implemented the findAndSelectTarget function to support automatic or interactive target selection.
  - Added the verifyTargetAvailable function to check whether valid targets exist on the network.
  - Implemented the listAvailableTargets function to list all detected Diretta targets.
  - Added a new m_targetIndex member and related setter functions to the DirettaOutput class.
  - Ensured that DirettaRenderer verifies target availability before startup, preventing launch without a valid device.
  - Extended main.cpp with new command‑line arguments:
          --target to set the target index
          --list-targets to display available targets
  - Enhanced the command‑line help messages, adding usage instructions for target selection.
  - Adjusted the default buffer size warning messages to improve user experience.
  **Documentation (Installation): Diretta Target Listing & Selection**
  - Added “List Diretta Targets” and “Select Diretta Target” sections to README.md, guiding users on how to scan the network for  Diretta devices
  - Expanded INSTALLATION.md with detailed instructions on listing and selecting targets via command line
  - Introduced generate_service.sh script to automatically create systemd service files, enabling configuration of target index and parameters
  - Updated system service configuration examples to support customization of target index, port, and buffer size via environment variables
  - Documented behavior where, if no target is explicitly specified and only one device is detected, the renderer will automatically use that target

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
