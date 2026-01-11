# Changelog

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

### 🎵 DSD Format Enhancement Thanks to [SwissMountainBear ](https://github.com/[SwissMontainsBear](https://github.com/SwissMontainsBear))

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

### 🎛️ Command Line

**New Options:**
- `--no-gapless`: Disable gapless playback (enabled by default)

**Existing Options (reminder):**
- `--buffer <seconds>`: Buffer size (default: 2.0, can use 0.5-1.0 for lower latency)
- `--thread-mode <value>`: Advanced Diretta SDK threading
- Network options: `--interface`, `--bind-ip` for multi-homed systems

### 📝 Configuration

**Default Settings (optimal for most users):**
```bash
Buffer: 2.0 seconds
Gapless: Enabled
Network: Auto-adaptive by format
```

**Low Latency Setup:**
```bash
./DirettaRendererUPnP --target 1 --buffer 0.5
# ~1.5s total latency with gapless still working perfectly
```

### 🐛 Bug Fixes

- Fixed race condition in `prepareNextTrackForGapless()`
- Fixed potential deadlock during format changes
- Fixed double close() causing crashes
- Fixed pink noise on format changes due to improper buffer handling
- Fixed memory leak in error paths (proper cleanup with `.reset()`)

### 🔍 Debugging

**Enhanced Logging:**
- Gapless preparation logs: "♻️ Reusing pre-loaded decoder"
- Network config logs: "Mode: VarMax/Var/Fix" based on format
- Buffer draining logs: "Buffer drained in Xms"
- Format change sequence logs with step-by-step progress

**Verbose Mode:**
```bash
sudo ./DirettaRendererUPnP --target 1 --verbose
# Shows detailed gapless, network, and buffer operations
```

### 📚 Documentation

- Updated installation guide
- Added gapless troubleshooting section
- Network optimization explained
- Buffer sizing recommendations

### ⚙️ Compatibility

- **Backward Compatible:** All v1.1.x features preserved
- **Control Points Tested:**
  - ✅ Roon (excellent gapless support)
  - ✅ BubbleUPnP (good support)
  - ✅ mConnect (basic support)
  - ⚠️ JPlay iOS (limited - no SetNextURI support)

### 🎯 Known Limitations

- Format changes require DAC resynchronization (~50-200ms gap) - hardware limitation
- Control points must support `SetNextAVTransportURI` for gapless
- Some control points (JPlay iOS) have limited gapless capabilities

### 🔄 Migration from v1.1.1

**No configuration changes needed!** Simply update binaries.

**Recommendations:**
1. Test with `--buffer 1.0` for lower latency (gapless still works)
2. Enable verbose mode to see gapless in action: `--verbose`
3. For format-heavy playlists, ensure stable network (Ethernet recommended)

### 👥 Credits

- **Diretta SDK v1.4.7** by Yu Harada - Native gapless implementation
- **Community testers** for format change crash reports
- **Audiophile users** for demanding seamless playback

---

## [1.1.1] - 2024-12-XX

### Bug Fixes
- Fixed mutex deadlock in SEEK operations
- Improved multi-interface network support
- Various stability improvements

### Features
- Advanced Diretta SDK configuration options
- Network interface selection
- Jumbo frame support (MTU 16k)

---

## [1.0.0] - 2024-XX-XX

### Initial Release
- First UPnP renderer for Diretta protocol
- Support for all PCM formats up to 1536kHz
- DSD support (DSD64-DSD1024)
- Bit-perfect audio streaming
- Basic gapless support via AudioEngine
