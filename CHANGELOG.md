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
## [1.0.8] - 2022-12-XX

### Fixed

- **CRITICAL: Race condition in AudioDecoder** - Converted 9 static variables to instance 
  members to prevent data races during gapless playback when multiple decoder instances 
  run concurrently
  - Fixed: callCount, packetCount, and 7 logging flags now per-instance
  - Impact: Eliminates crashes, data corruption, and incorrect behavior in gapless mode
  - Each AudioDecoder instance now maintains its own independent state
  - Thread-safe for concurrent preloading and playback

- **CRITICAL: SEEK deadlock causing control points to freeze** - Implemented asynchronous 
  seek mechanism using atomic flags to avoid mutex deadlock
  - The UPnP thread no longer blocks waiting for the audio thread's mutex
  - Seek requests are processed in the audio thread during the next process() cycle
  - Removed unnecessary DirettaOutput seek during playback (AudioEngine seek is sufficient)
  - Latency: ~170-185ms (imperceptible, standard for real-time audio)
  - Fixes: mConnect, BubbleUPnP, and other control points no longer freeze on seek
  - Impact: Seek now works reliably across all UPnP control points

- **Refactor Makefile**
  - proper ARCH_NAME/VARIANT handling, unified auto-detection (Thanks to udosan174)

### Technical Details

- Added atomic members `m_seekRequested` and `m_seekTarget` to AudioEngine
- Modified `AudioEngine::seek()` to be lock-free and non-blocking
- Seek processing moved to audio thread in `process()` for thread safety
- All static debug counters in AudioDecoder converted to instance members




## [1.0.7]

### Add
 - DSD512 and DSD1024 support added thanks to norman-arch
### 🎵 Audirvana Compatibility Fix (In progress not perfect)

**Problem:**
- Audirvana pre-decodes audio and wraps it in a strange format
- Standard DSD native mode failed with Audirvana
- DSD playback resulted in incorrect format or no audio

**Solution:**
- Detect Audirvana streams via URL pattern (`/audirvana/`)
- For Audirvana DSD: Use FFmpeg decoding instead of raw mode
- For other sources: Continue using DSD native mode

**Result:**
- ✅ Audirvana DSD (DoP) works as follow→ Decoded to PCM Hi-Res (352.8k/705.6k DirettaHostSDK doesn't support DoP)
- ✅ BubbleUPnP/JPLAY DSD works → Native DSD bitstream to DAC
- ✅ No regression on PCM formats

### 🔧 Float Buffer Support (Sub-second Precision)

**Problem:**
- Buffer stored as `int`, causing truncation for values < 1 second
- Example: 0.8s buffer → cast to int → 0 seconds!
- Result: Buffer underruns, audio artifacts (pink noise)

**Tested:**
- ✅ DSD with 0.8s buffer works correctly
- ✅ PCM with 1.2s buffer works correctly
- ✅ No regression on integer buffer values

**Files Modified:**
- `DirettaOutput.h` - Changed member type
- `DirettaOutput.cpp` - Updated calculations
- `DirettaRenderer.h` - Updated config
- `main.cpp` - Updated default value

**Feature:**
- Intelligent buffer sizing based on audio format complexity
- Sub-second precision using float instead of int

**Buffer Strategy:**

| Format Type | Buffer Size | Rationale |
|-------------|-------------|-----------|
| DSD (DSF/DFF) | 0.8s | Raw bitstream, instant read |
| PCM Standard (≤48kHz/16-bit) | 1.0s | Low data rate, minimal latency |
| PCM Hi-Res (≥88.2kHz/24-bit) | 1.2-1.5s | High data rate, DAC stabilization |
| Compressed (FLAC/ALAC) | 2.0s | Decode overhead required |

**Benefits:**
- No more 10-second or more startup delays but a delay betwwen 3 to 8 seconds still occurs
- Optimal latency for each format type
- Maintains stability for Hi-Res and compressed formats

**Files Modified:**
- `DirettaOutput.h` - Changed buffer type to float
- `DirettaOutput.cpp` - Adaptive buffer logic
- `DirettaRenderer.h` - Updated config structure
- `main.cpp` - Default buffer reduced from 10s to 2s

### 🔴 CRITICAL FIX: Stop/Play Cycle (Audirvana Exclusive Mode)

**Problem:**
- After STOP command (especially with Audirvana exclusive mode unlock), DirettaOutput is closed
- Next PLAY command incorrectly attempted to resume from a closed connection
- `isPaused()` flag remained true even after connection was closed
- Result: Crash, freeze, or undefined behavior requiring double PLAY

**Tested:**
- ✅ PAUSE → PLAY works correctly (true resume)
- ✅ STOP → PLAY works on first attempt
- ✅ Multiple STOP/PLAY cycles are stable
- ✅ Audirvana exclusive mode fully functional

**Files Modified:**
- `DirettaRenderer.cpp` - onPlay callback logic

### Fixed
- **Dynamic sample rate switching**: 

Fixed DAC not changing sample rate when transitioning between tracks with different formats (e.g., 96kHz → 44.1kHz)
  - Root cause: After `close()`, the callback would immediately `open()` with the new format without giving the Diretta Target time to reinitialize
  - Solution: Added persistent format tracking using static variables to detect format changes even after `close()`, and implemented a 500ms pause before reopening to allow the Target's clock generator and PLL to properly reset
  - The Diretta Target now correctly reconfigures the DAC for each format change
  - Format changes are properly logged with detailed transition information

### Changed
- **Improved audio thread logging**: 

Reduced log spam when `process()` returns false
  - Now logs only every 100 consecutive failures instead of every occurrence
  - Prevents multi-gigabyte log files during idle states
  - Added consecutive failure counter for better debugging visibility
- **Improved Roon + Squeeze2UPnP compatibility** Thanks to herisson-88
### Technical Details

- Format change detection now works in two scenarios:
  1. When `isConnected() == true`: Compares current format with `getFormat()`
  2. When `isConnected() == false`: Compares with last known format stored in static variable (critical for JPLAY's AUTO-STOP behavior)
- Total format change duration: ~1 second (500ms Target reset + 290ms SyncBuffer setup + 200ms DAC stabilization)
- Validated transitions: PCM ↔ PCM (different sample rates), PCM ↔ DSD

### Notes

- Format changes between tracks are expected to have a brief pause (~1s) for proper DAC reconfiguration
- Gapless playback for tracks with the same format remains unaffected and works seamlessly
- This fix enables proper bit-perfect playback across entire multi-format playlists

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

**Version Display**
- Added version display type:
   
  ./DirettaRenderer --version or ./DirettaRendererUPnP -V
  
### Fixed
- Removed clicks or pink noise bursts at the end of the album or during the transition between tracks in a playlist
 * Buffers weren't correctly cleared at the en of an album or during a format change.
- Disable gapless playback on format changes (Many thanks to herisson88)
  * When the next track has a different format (sample rate, bit depth,
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
