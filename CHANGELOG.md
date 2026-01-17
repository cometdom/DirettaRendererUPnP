# Changelog

## [1.3.2]
### 🐛 Bug Fixes
Fixed: DSD gapless playback on standard networks (MTU 1500)

If you experienced glitches between DSD tracks, this fixes it!
Works on any network equipment, no configuration needed.


## [1.3.1]
### 🐛 Bug Fixes
 **Critical:** Fixed freeze after pause >10-20 seconds
  - Root cause: Drainage state machine not reset on resume
  - Solution: Reset m_isDraining and m_silenceCount flags
  - Affects: GentooPlayer and other distributions

### ✨ New Features
 **Timestamps:** Automatic [HH:MM:SS.mmm] on all log output
  - Enables precise timing analysis
  - Helps identify timeouts and race conditions
  - Useful for debugging network issues


## [1.3.0] - 2026-01-11
### 🚀 NEW FEATURES
 **Same-Format Fast Path (Thanks to SwissMountainsBear)**
 Track transitions within the same audio format are now dramatically faster.

BEFORE: 600-1200ms (full reconnection, configuration, DAC lock)
AFTER:  <50ms (instant resume)

Performance Gain: 24× faster transitions

How it works:
- Connection kept alive between same-format tracks
- Smart buffer management (DSD: silence clearing, PCM: seek_front)
- Format changes still trigger full reconnection (safe behavior)

Impact:
- Seamless album playback (DSD64, DSD128, DSD256, DSD512)
- Better user experience with control points (JPLAY, Bubble UPnP, etc.)
- Especially beneficial for high DSD rates where reconnection is expensive

Technical details:
- Implemented in DirettaOutput::open() with format comparison
- Format change detection enhanced for reliability
- Connection persistence logic in DirettaRenderer callbacks


📡 Dynamic Cycle Time Calculation

**Network timing now adapts automatically to audio format characteristics**

 Implementation:
- New DirettaCycleCalculator class analyzes format parameters
- Calculates optimal cycle time based on sample rate, bit depth, channels
- Considers MTU size and network overhead (24 bytes)
- Range: 100µs to 50ms (dynamically calculated per format)

Results:
- DSD64 (2.8MHz):  ~23ms optimal cycle time (was 10ms fixed)
- PCM 44.1k:       ~50ms optimal cycle time (was 10ms fixed)
- DSD512:          ~5ms optimal cycle time (high throughput)

Performance Impact:
- PCM 44.1k: Network packets reduced from 100/sec to 20/sec (5× reduction)
- Better MTU utilization: PCM now uses 55% of 16K jumbo frames vs 11% before
- Significantly reduced audio dropouts
- Lower CPU overhead for network operations

Technical details:
- Formula: cycleTime = (effectiveMTU / bytesPerSecond) × 1,000,000 µs
- Effective MTU = configured MTU - 24 bytes overhead
- Applied in DirettaOutput::optimizeNetworkConfig()

**Added `--transfer-mode` option for precise timing control**

Users can now choose between two transfer timing modes:

- **VarMax (default)**: Adaptive cycle timing for optimal bandwidth usage
  - Cycle time varies dynamically between min and max values
  - Best for most users and use cases
  
- **Fix**: Fixed cycle timing for precise timing control
  - Cycle time remains constant at user-specified value
  - Enables experimentation with specific frequencies
  - Requested by audiophile users who report sonic differences with certain fixed frequencies

**Usage examples:**

```bash
# Default adaptive mode (VarMax)
sudo ./DirettaRendererUPnP --target 1

# Fixed timing mode at 528 Hz (1893 µs)
sudo ./DirettaRendererUPnP --target 1 --transfer-mode fix --cycle-time 1893

# Fixed timing mode at 500 Hz (2000 µs)
sudo ./DirettaRendererUPnP --target 1 --transfer-mode fix --cycle-time 2000
```

**Popular cycle time values for Fix mode:**
- 1893 µs = 528 Hz (reported as "musical" by some audiophiles)
- 2000 µs = 500 Hz
- 1000 µs = 1000 Hz


### Technical Details

- **VarMax mode**: Uses Diretta SDK `configTransferVarMax()` 
  - Adaptive cycle timing between min (333 µs) and max (default 10000 µs)
  - Optimal for bandwidth efficiency
  
- **Fix mode**: Uses Diretta SDK `configTransferFix()`
  - Fixed cycle time at user-specified value
  - Requires explicit `--cycle-time` parameter
  - Provides precise timing control for audio experimentation

- **Cycle time parameter behavior:**
  - In VarMax mode: Sets maximum cycle time (optional)
  - In Fix mode: Sets fixed cycle time (required)


### Requirements

- Fix mode requires explicit `--cycle-time` specification
- If `--transfer-mode fix` is used without `--cycle-time`, the renderer will exit with a clear error message and usage examples


### Breaking Changes

None. VarMax mode is the default, so existing configurations and scripts continue to work unchanged.


### 🐛 CRITICAL BUGFIXES (Thanks to SwissMountainsBear)
 🔴 Shadow Variable in Audio Thread (DirettaRenderer.cpp)
───────────────────────────────────────────────────────────────────────────
Problem: 
- Two separate `static int failCount` variables in if/else branches
- Reset logic never worked (wrong variable scope)
- Consecutive failure counter didn't accumulate properly

Impact:
- Inaccurate error reporting
- Misleading debug logs

Fix:
- Moved static declaration outside if/else scope
- Single shared variable for both success and failure paths
- Proper counter reset on success

Files: src/DirettaRenderer.cpp


🟡 Duplicate DEBUG_LOG (AudioEngine.cpp)
───────────────────────────────────────────────────────────────────────────
Problem:
- PCM format logged twice in verbose mode
- First log statement missing semicolon (potential compilation issue)

Impact:
- Cluttered logs in verbose mode
- Risk of compilation errors on strict compilers

Fix:
- Removed duplicate log statement
- Ensured proper semicolon on remaining log

Files: src/AudioEngine.cpp


🔴 AudioBuffer Rule of Three Violation (AudioEngine.h)
───────────────────────────────────────────────────────────────────────────
Problem:
- AudioBuffer class manages raw memory (new[]/delete[])
- No copy constructor or copy assignment operator
- Risk of double-delete crash if buffer accidentally copied

Impact:
- Potential crashes (double-delete)
- Undefined behavior with buffer copies
- Memory safety issue

Fix:
- Added copy prevention: Copy constructor/assignment = delete
- Implemented move semantics for safe ownership transfer
- Move constructor and move assignment operator added

### Fixed
- No more crashes when Diretta target unavailable - service waits indefinitely and auto-connects (no reboot needed).

### ⚠️  BEHAVIOR CHANGES
 **DSD Seek Disabled**
 Issue: 
DSD seek causes audio distortion and desynchronization due to buffer 
alignment issues and SDK synchronization problems.

Implementation:
- DSD seek commands are accepted (return success) but not executed
- Prevents crashes in poorly-implemented UPnP clients (e.g., JPLAY iOS)
- Audio continues playing without interruption
- Position tracking may be approximate

Behavior:
- PCM: Seek works perfectly with exact positioning
- DSD: Seek command ignored (no-op), playback continues

Workaround:
For precise DSD positioning: Use Stop → Seek → Play sequence

Technical details:
- Blocked in AudioEngine::process() before calling AudioDecoder::seek()
- DirettaOutput::seek() commented out (unused code)
- Resume without seek for DSD (position approximate)

Files: src/AudioEngine.cpp, src/DirettaOutput.cpp

## 👥 CREDITS
═══════════════════════════════════════════════════════════════════════════

SwissMountainsBear:
  - Same-format fast path implementation
  - Critical bug identification and fixes (shadow variable, Rule of Three)
  - DSD512 testing and validation
  - Collaborative development

Dominique COMET:
  - Dynamic cycle time implementation
  - Integration and testing
  - DSD/PCM validation
  - Project maintenance


## 🔄 MIGRATION FROM v1.2.x
═══════════════════════════════════════════════════════════════════════════

Configutaion change, please remove diretta-renderer.conf and and start-renderer.sh files in /opt/diretta-renderer-upnp/ before install sytemd.


Optional Recommendations:
────────────────────────────────────────────────────────────────────────────
For DSD256/512 users: Consider increasing buffer parameter if minor scratches 
occur during fast path transitions:

  --buffer 1.0   (for DSD256)
  --buffer 1.2   (for DSD512)

This provides more headroom for same-format transitions.

## [1.2.2] - 2026-01-09

--no-gapless option removed.
The --no-gapless option is no longer supported.
Gapless works perfectly with all standard UPnP control points.

For Audirvana users, simply setting Universal Gapless in Audirvana might work, though with some limitations. If you want Audirvana to work with the Diretta Host SDK, please reach out to the Audirvana Team.

No functional changes - gapless continues to work perfectly.

## [1.2.1] - 2026-01-06

No functional changes - gapless continues to work perfectly.
## [1.2.1] - 2026-01-06

### 🎵 DSD Format Enhancement Thanks to @SwissMontainsBear
**Improved DSD File Detection**
- **Smart DSF vs DFF detection**: Automatic detection of DSD source format based on file extension (`.dsf` or `.dff`)
- **Bit order handling**: Proper bit reversal flag (`m_needDsdBitReversal`) to handle LSB-first (DSF) vs MSB-first (DFF) formats
- **Format propagation**: DSD source format information flows from AudioEngine to DirettaRenderer for accurate playback configuration

**Technical Implementation:**
- `TrackInfo::DSDSourceFormat` enum to track DSF vs DFF files
- File extension parsing in AudioEngine to detect format type
- Fallback to codec string parsing if file detection fails
- Integration with DirettaOutput for correct bit order processing

### 🔧 Seeking Improvements

**DSD Raw Seek Enhancement**
- **File repositioning for DSD**: Precise seeking in raw DSD streams using byte-level positioning
- **Accurate calculation**: Bit-accurate positioning based on sample rate and channel count
- **Better logging**: Enhanced debug output showing target bytes, bits, and format information

**Benefits:**
- More accurate seek operations in DSD files
- Proper file pointer management during playback
- Improved user experience when scrubbing through DSD tracks

## [1.2.0] - 2025-12-27

### 🎵 Major Features

#### Gapless Pro (SDK Native)
- **Seamless track transitions** using Diretta SDK native gapless methods
- Implemented `writeStreamStart()`, `addStream()`, and `checkStreamStart()` for zero-gap playback
- Pre-buffering of next track (1 second) for instant transitions
- Support for format changes with minimal interruption (~50-200ms for DAC resync)
- Fully automatic - works with any UPnP control point supporting `SetNextAVTransportURI`
- Enable/disable via `--no-gapless` command-line option

**User Experience:**
- Live albums play without interruption
- Conceptual albums (Pink Floyd, etc.) maintain artistic flow
- DJ mixes and crossfades preserved
- Perfect for audiophile listening sessions

### 🛡️ Stability Improvements

#### Critical Format Change Fixes
- **Buffer draining** before format changes to prevent pink noise and crashes
- **Double close protection** prevents crashes from concurrent close() calls
- **Anti-deadlock callback system** eliminates 5-second timeouts during format transitions
- **Exception handling** in SyncBuffer disconnect operations

**Impact:** Estimated 70-90% reduction in format change related crashes

#### Network Optimization
- **Adaptive network configuration** based on audio format:
  - **DSD**: VarMax mode for maximum throughput
  - **Hi-Res (≥192kHz or ≥88.2kHz/24bit)**: Adaptive variable timing
  - **Standard (44.1/48kHz)**: Fixed timing for stability
- Automatic optimization on format changes
- Better performance for high-resolution audio streams

### 🔧 Technical Improvements

#### AudioEngine
- Optimized `prepareNextTrackForGapless()` to reuse pre-loaded decoder
- Eliminates redundant file opens and I/O operations
- Better CPU and memory efficiency during gapless transitions

#### DirettaOutput
- New `isBufferEmpty()` method for clean buffer management
- New `optimizeNetworkConfig()` for format-specific network tuning
- Enhanced `close()` with early state marking to prevent re-entrance
- Try-catch protection around SDK disconnect operations

#### DirettaRenderer
- CallbackGuard supports manual early release
- Callback flag released before long operations to prevent deadlocks
- Explicit buffer draining with timeout in format change sequences

### 📊 Performance

- **Gapless transitions:** 0ms gap for same format, ~50-200ms for format changes
- **Format change stability:** +70-90% improvement
- **Network throughput:** Optimized per format (DSD/Hi-Res/Standard)
- **CPU usage:** Reduced redundant decoder operations

# Changelog

All notable changes to DirettaRendererUPnP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.2] - 2025-12-30

### Fixed
Some bugs fixes

DSD files playback after a delay of 1 nminute 40.

**Simplified structure - mutex held throughout callback:**

## [1.1.1] - 2025-12-25

### Fixed

#### **CRITICAL: Deadlock causing no audio output on slower systems (RPi4, etc.)**
- **Issue**: Renderer would freeze after "Waiting for DAC stabilization" message, no audio output
- **Affected systems**: Primarily Raspberry Pi 4 and other ARM-based systems, occasional issues on slower x86 systems
- **Root cause**: Mutex (`m_mutex`) held during `waitForCallbackComplete()` in `onSetURI` callback
  - Added in v1.0.8 for JPlay iOS compatibility (Auto-STOP feature)
  - On slow CPUs (RPi4), the audio callback thread would attempt to acquire the same mutex
  - Result: Deadlock - each thread waiting for the other
- **Solution**: Release `m_mutex` before calling `waitForCallbackComplete()`
  - Lock mutex → Read state → Unlock
  - Perform Auto-STOP without mutex held
  - Re-lock mutex → Update URI → Unlock
- **Impact**: Fixes freeze on all systems, maintains JPlay iOS compatibility
- **Technical details**:
  ```cpp
  // BEFORE (v1.1.0 - DEADLOCK):
  std::lock_guard<std::mutex> lock(m_mutex);  // Held entire time
  auto currentState = m_audioEngine->getState();
  // ... Auto-STOP ...
  waitForCallbackComplete();  // DEADLOCK: waiting while mutex locked
  
  // AFTER (v1.1.1 - FIXED):
  {
      std::lock_guard<std::mutex> lock(m_mutex);
      currentState = m_audioEngine->getState();
  }  // Mutex released here
  // ... Auto-STOP ...
  waitForCallbackComplete();  // SAFE: no mutex held
  ```

#### **Multi-interface support not working**
- **Issue**: `--interface` option parsed but ignored, UPnP always bound to default interface
- **Symptom**: 
  ```
  ✓ Will bind to interface: enp1s0u1u1  ← Recognized
  🌐 Using default interface for UPnP (auto-detect)  ← Ignored!
  ✓ UPnP initialized on 172.20.0.1:4005  ← Wrong interface
  ```
- **Root cause**: `networkInterface` parameter not passed from `DirettaRenderer::Config` to `UPnPDevice::Config`
- **Solution**: Added missing parameter propagation in `DirettaRenderer.cpp`:
  ```cpp
  upnpConfig.networkInterface = m_config.networkInterface;
  ```
- **Impact**: Multi-interface support now works correctly for 3-tier architectures

#### **Raspberry Pi variant detection (Makefile)**
- **Issue**: RPi3/4 incorrectly detected as k16 variant (16KB pages), causing link errors
- **Symptom**:
  ```
  Architecture:  ARM64 (aarch64) - Kernel 6.12 (using k16 variant)  ← WRONG for RPi4
  Variant:       aarch64-linux-15k16  ← RPi4 doesn't support k16
  ```
- **Root cause**: Detection based on kernel version (>= 4.16) instead of actual page size
  - RPi3/4: 4KB pages, need `aarch64-linux-15`
  - RPi5: 16KB pages, need `aarch64-linux-15k16`
  - Kernel 6.12 on RPi4 triggered wrong detection
- **Solution**: Use `getconf PAGESIZE` and `/proc/device-tree/model` for accurate detection
  ```makefile
  PAGE_SIZE := $(shell getconf PAGESIZE)
  IS_RPI5 := $(shell grep -q "Raspberry Pi 5" /proc/device-tree/model)
  
  # Use k16 only if explicitly RPi5 or 16KB pages detected
  ```
- **Impact**: Correct library selection on all Raspberry Pi models

### Changed
- Improved mutex handling in UPnP callbacks for better thread safety
- Enhanced Makefile architecture detection logic for ARM systems

### Technical Notes

#### Deadlock Debug Information
The deadlock manifested differently based on system performance:
- **Fast x86 systems**: Rare or no issues (callback completes before conflict)
- **Slow ARM systems (RPi4)**: Consistent freeze (timing window for deadlock much larger)
- **Trigger**: User changing tracks/albums while audio playing
- **Timing window**: 400ms sleep in callback provided ample opportunity for deadlock on slow systems

#### Multi-Interface Fix
Affects users with:
- 3-tier architecture (control points on one network, DAC on another)
- VPN + local network configurations
- Multiple Ethernet adapters
- Any scenario requiring explicit interface binding

Without this fix, the `--interface` parameter was completely ignored, making 3-tier setups impossible.

#### Raspberry Pi Detection
Previous kernel-based detection failed because:
- Kernel version indicates OS capability, not hardware configuration
- RPi4 can run kernel 6.12+ but hardware still uses 4KB pages
- RPi5 introduced 16KB pages and requires different SDK library variant
- Using wrong variant causes immediate segfault on startup

### Compatibility
- ✅ Backward compatible with v1.1.0 configurations
- ✅ No changes to command-line options
- ✅ No changes to systemd configuration files
- ✅ All v1.1.0 features preserved (multi-interface, format change fix)

### Tested Configurations
- ✅ Raspberry Pi 3 (aarch64-linux-15)
- ✅ Raspberry Pi 4 (aarch64-linux-15)
- ✅ Raspberry Pi 5 (aarch64-linux-15k16)
- ✅ x86_64 systems (all variants: v2, v3, v4, zen4)
- ✅ GentooPlayer distribution
- ✅ AudioLinux distribution
- ✅ 3-tier network architectures
- ✅ JPlay iOS (Auto-STOP functionality)

### Migration from v1.1.0

No special migration steps required. Simply:

```bash
cd DirettarendererUPnP
# Pull latest code
git pull

# Rebuild
make clean
make

sudo systemctl stop diretta-renderer

# Reinstall (if using systemd)
cd Systemd
chmod +x install-systemd.sh
sudo ./install-systemd.sh

# Restart service
sudo systemctl restart diretta-renderer
```

### Known Issues
None

### Credits
- Deadlock issue reported and tested by RPi4 users (Alfred and Nico)
- Multi-interface issue reported by dsnyder (3-tier architecture pioneer) and kiran kumar reddy kasa
- Raspberry Pi detection issue reported by Filippo GentooPlayer developer
- Special thanks to Yu Harada for Diretta SDK support

---

## Summary of Critical Fixes in v1.1.1

| Issue | Severity | Affected Systems | Status |
|-------|----------|------------------|--------|
| Deadlock (no audio) | **CRITICAL** | RPi4, slower systems | ✅ **FIXED** |
| Multi-interface ignored | **HIGH** | 3-tier setups | ✅ **FIXED** |
| Wrong RPi variant | **CRITICAL** | RPi3/4 | ✅ **FIXED** |

**All critical issues resolved. v1.1.1 is recommended for all users, especially those on Raspberry Pi systems.**


## [1.1.0] - 2025-12-24

### Added
- 🌐 **Multi-interface support** for multi-homed systems
  - New command-line option: `--interface <name>` to bind to specific network interface (e.g., eth0, eno1, enp6s0)
  - New command-line option: `--bind-ip <address>` to bind to specific IP address (e.g., 192.168.1.10)
  - Essential for 3-tier architecture configurations with separate control and audio networks
  - Fixes SSDP discovery issues on systems with multiple network interfaces (VPN, multiple NICs, bridged networks)
  - Auto-detection remains default behavior for single-interface systems (backward compatible)
  
- **Advanced Configuration with command-line Parameters**
  - ### Basic Options

```bash
--name, -n <name>       Renderer name (default: Diretta Renderer)
--port, -p <port>       UPnP port (default: auto)
--buffer, -b <seconds>  Buffer size in seconds (default: 2.0)
--target, -t <index>    Select Diretta target by index (1, 2, 3...)
--no-gapless            Disable gapless playback
--verbose               Enable verbose debug output
```
 - ### Advanced Diretta SDK Options
Fine-tune the Diretta protocol behavior for optimal performance such as Thread-mode, transfer timing....

### Fixed
- **Critical**: Fixed format change freeze when transitioning between bit depths
  - **Issue**: Playlist playback would freeze for 10 seconds when switching between 24-bit and 16-bit tracks
  - **Root cause**: 4 residual samples in Diretta SDK buffer never drained, causing timeout
  - **Solution**: Implemented force flush with silence padding to push incomplete frames through pipeline
  - **Result**: Format changes now complete in ~200-300ms instead of 10-second timeout
  - **Impact**: Smooth playlist playback with mixed formats (16-bit/24-bit/32-bit)
- Improved error recovery during format transitions
- Better handling of incomplete audio frames at track boundaries

### Changed
- **UPnP Initialization**: Now uses `UpnpInit2()` with interface parameter for precise network binding
- **Format Change Timeout**: Reduced from 10s to 3s for faster error recovery
- **Buffer Drain Logic**: Added tolerance for ≤4 residual samples (considered "empty enough")
- **Hardware Stabilization**: Increased from 200ms to 300ms for better reliability during format changes
- **Logging**: Enhanced debug output during format change sequence with flush detection

### Configuration
- **Systemd**: New `NETWORK_INTERFACE` parameter in `/opt/diretta-renderer-upnp/diretta-renderer.conf`
  ```bash
  # For 3-tier architecture
  NETWORK_INTERFACE="eth0"      # Interface connected to control points
  
  # Or by IP address
  NETWORK_INTERFACE="192.168.1.10"
  ```
- **Wrapper Script**: Automatically detects whether parameter is IP address or interface name

### Use Cases

#### Multi-Interface Scenarios
1. **3-tier Architecture** (recommended by dsnyder):
   - Control Points (JPlay, Roon) on 192.168.1.x via eth0
   - Diretta DAC on 192.168.2.x via eth1
   ```bash
   sudo ./bin/DirettaRendererUPnP --interface eth0 --target 1
   ```

2. **VPN + Local Network**:
   - Local network on 192.168.1.x via eth0
   - VPN on 10.0.0.x via tun0
   ```bash
   sudo ./bin/DirettaRendererUPnP --bind-ip 192.168.1.10 --target 1
   ```

3. **Multiple Ethernet Adapters**:
   - Specify which adapter handles UPnP discovery
   ```bash
   sudo ./bin/DirettaRendererUPnP --interface eno1 --target 1
   ```

#### Format Change Improvements
- **Mixed Format Playlists**: Seamless transitions between 16-bit, 24-bit, and different sample rates
- **Streaming Services**: Better compatibility with services like Qobuz that mix bit depths
- **Gapless Playback**: Maintains gapless behavior even during format changes

### Documentation
- Added comprehensive **Multi-Homed Systems** section in README
- Added troubleshooting guide for network interface selection
- Added examples for common multi-interface configurations
- Updated systemd configuration guide
- Added FORMAT_CHANGE_FIX.md technical documentation

### Technical Details

#### Multi-Interface Implementation
- Modified `UPnPDevice.cpp`: `UpnpInit2(interfaceName, port)` instead of `UpnpInit2(nullptr, port)`
- Added `networkInterface` parameter to `UPnPDevice::Config` structure
- Propagated interface selection from command-line → DirettaRenderer → UPnPDevice
- Enhanced error messages when binding fails (suggests `ip link show`, permissions check)

#### Format Change Fix Implementation
- Added **Step 1.5** in `changeFormat()`: Force flush with 128 samples of silence padding
  - Pushes incomplete frames through Diretta SDK pipeline
  - Only triggered when residual < 64 samples detected
- Modified drain logic to accept small residual (≤4 samples) as successful
- Implemented `sendAudio()` function for unified audio data transmission
- Better synchronization between AudioEngine and DirettaOutput during transitions

### Breaking Changes
None - all changes are backward compatible

### Migration Guide
No migration needed. Existing configurations continue to work:
- Systems with single network interface: No changes required
- Multi-interface systems: Add `--interface` or configure `NETWORK_INTERFACE` in systemd

### Known Issues
- None reported

### Tested Configurations
- ✅ Fedora 39/40 (x64)
- ✅ Ubuntu 22.04/24.04 (x64)
- ✅ AudioLinux (x64)
- ✅ Raspberry Pi 4 (aarch64)
- ✅ 3-tier architecture with Intel i225 + RTL8125 NICs
- ✅ Mixed format playlists (16/24-bit, 44.1/96/192kHz)
- ✅ Qobuz streaming (16/24-bit)
- ✅ Local FLAC/WAV files
- ✅ DSD64/128/256 playback

### Performance
- Format change latency: ~200-300ms (down from 10s)
- Network discovery: Immediate on specified interface
- Memory usage: Unchanged
- CPU usage: Unchanged

### Credits
- Multi-interface support requested and tested by community members
- Format change fix developed in collaboration with Yu Harada (Diretta protocol creator)
- Testing and validation by early adopters on AudioPhile Style forum

---

## [1.0.8] - 2025-12-23

### Fixed
- Fixed SEEK functionality deadlock issue
  - Replaced mutex-based synchronization with atomic flag
  - Implemented asynchronous seek mechanism
  - Seek now completes in <100ms without blocking

### Changed
- Improved seek reliability and responsiveness
- Better error handling during seek operations

## [1.0.7] - 2025-12-22

### Added
- Advanced Diretta SDK configuration options:
  - `--thread-mode <value>`: Configure thread priority and behavior (bitmask)
  - `--cycle-time <µs>`: Transfer packet cycle maximum time (default: 10000)
  - `--cycle-min-time <µs>`: Transfer packet cycle minimum time (default: 333)
  - `--info-cycle <µs>`: Information packet cycle time (default: 5000)
  - `--mtu <bytes>`: Override MTU for network packets (default: auto-detect)

### Changed
- Buffer size parameter changed from integer to float for finer control
  - Now accepts values like `--buffer 2.5` for 2.5 seconds
- Improved buffer adaptation logic based on audio format complexity
- Better MTU detection and configuration

### Documentation
- Added comprehensive documentation for advanced Diretta SDK parameters
- Added thread mode bitmask reference
- Added MTU optimization guide

## [1.0.6] - 2025-12-21

### Fixed
- Audirvana Studio streaming compatibility issues
  - Fixed pink noise after 6-7 seconds when streaming 24-bit content from Qobuz
  - Issue was related to HTTP streaming implementation vs Diretta SDK buffer handling
  - Workaround: Use 16-bit or 20-bit streaming settings in Audirvana

### Changed
- Improved buffer handling for HTTP streaming sources
- Better error detection and recovery for streaming issues

## [1.0.5] - 2025-12-20

### Fixed
- Format change handling improvements
  - Fixed clicking sounds during 24-bit audio playback
  - Removed artificial silence generation that caused artifacts
  - Proper buffer draining using Diretta SDK's `buffer_empty()` methods

### Changed
- Improved audio playback behavior during track transitions
- Better handling of sample rate changes

## [1.0.4] - 2025-12-19

### Added
- Jumbo frame support with 16k MTU optimization
- Configurable MTU settings for network optimization

### Fixed
- Network configuration issues with Intel i225 cards (limited to 9k MTU)
- Buffer handling improvements

## [1.0.3] - 2025-12-18

### Added
- Gapless playback support
- Improved track transition handling

### Changed
- Better buffer management during track changes
- Improved format detection and handling

## [1.0.2] - 2025-12-17

### Fixed
- DSD playback improvements
- Sample rate detection accuracy

## [1.0.1] - 2025-12-16

### Added
- Support for multiple Diretta DAC targets
- Interactive target selection
- Command-line target specification

### Fixed
- Target discovery reliability
- Connection stability improvements

## [1.0.0] - 2025-12-15

### Added
- Initial release
- UPnP MediaRenderer implementation
- Diretta protocol integration
- Support for PCM audio (16/24/32-bit, up to 768kHz)
- Support for DSD audio (DSD64/128/256/512/1024)
- AVTransport service (Play, Pause, Stop, Seek, Next, Previous)
- RenderingControl service (Volume, Mute)
- ConnectionManager service
- Automatic SSDP discovery
- Format-specific buffer optimization
- Systemd service integration

### Supported Formats
- PCM: 16/24/32-bit, 44.1kHz to 768kHz
- DSD: DSD64, DSD128, DSD256, DSD512, DSD1024
- Containers: FLAC, WAV, AIFF, ALAC, APE, DSF, DFF

### Supported Control Points
- JPlay
- Roon
- BubbleUPnP
- Any UPnP/DLNA control point

---

## Version Numbering

- **Major.Minor.Patch** (e.g., 1.1.0)
- **Major**: Breaking changes or complete rewrites
- **Minor**: New features, significant improvements, backward compatible
- **Patch**: Bug fixes, minor improvements, backward compatible

## Unreleased

### Planned Features
- Web UI for configuration
- Docker container support
- Automatic format detection improvement
- Multi-room synchronization
- Volume normalization
- Equalizer support
