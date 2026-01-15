# Changelog

## 2026-01-15

### FFmpeg ABI Compatibility Fix

**Problem:** Segmentation fault when running against FFmpeg 5.x libraries after compiling on a system with FFmpeg 7.x development headers. Crash occurred in `AudioDecoder::open()` when accessing `AVStream->codecpar`.

**Root cause:**
- Compile-time FFmpeg headers (libavformat 61.x from FFmpeg 7.x) have different `AVStream` structure layout than runtime libraries (libavformat 59.x from FFmpeg 5.x)
- The `codecpar` field offset differs between versions, causing garbage pointer dereference
- Debug output showed: `codecpar=0x5622000000001` (garbage) instead of valid pointer

**Solution - Multi-layered approach:**

1. **AudioEngine.cpp** - Safer stream detection:
   - Replaced manual stream iteration loop with `av_find_best_stream()` (FFmpeg's recommended API)
   - Added NULL checks for `audioStream` and `audioStream->codecpar` after retrieval
   - Handles edge cases in FFmpeg 5.x where codecpar may be invalid

2. **install.sh** - Automatic header management:
   - Added `download_ffmpeg_headers()` - downloads FFmpeg source for headers only
   - Added `check_ffmpeg_abi_compatibility()` - detects runtime vs compile-time version mismatch
   - Added `ensure_ffmpeg_headers()` - auto-downloads correct headers when needed
   - Added `detect_ffmpeg_runtime_version()` and `get_ffmpeg_target_version()` for auto-detection
   - `build_renderer()` now automatically uses `make FFMPEG_PATH=./ffmpeg-headers`
   - FFmpeg installation now offers both **5.1.2** and **7.1** (recommended) build options
   - Selected version saved to `.ffmpeg-version` for future builds
   - New config variables: `FFMPEG_HEADERS_DIR`, `FFMPEG_TARGET_VERSION`

3. **Makefile** - Auto-detection and warnings:
   - Auto-detects `./ffmpeg-headers/` directory (created by install.sh)
   - Shows clear warning box when using system headers
   - Supports explicit override: `make FFMPEG_PATH=/path/to/headers`

4. **New .gitignore** - Excludes downloaded headers from version control

**Usage:**
```bash
# Option A: Use install.sh (recommended - auto-downloads headers)
./install.sh --build

# Option B: Manual download
wget https://ffmpeg.org/releases/ffmpeg-5.1.2.tar.xz
tar xf ffmpeg-5.1.2.tar.xz
mv ffmpeg-5.1.2 ffmpeg-headers
make clean && make

# Option C: Explicit path
make clean && make FFMPEG_PATH=/path/to/ffmpeg-5.1.2
```

**Files:**
- `src/AudioEngine.cpp` (lines 139-171) - Stream detection rewrite
- `install.sh` (lines 512-656) - Header download functions
- `install.sh` (lines 721-734) - Build with correct headers
- `Makefile` (lines 181-229) - FFmpeg path auto-detection
- `.gitignore` - New file

---

### ARM64 Compilation Fix

**Problem:** Build failed on ARM64 (aarch64) with `fatal error: immintrin.h: No such file or directory`. The `immintrin.h` header is x86-only (AVX/SSE intrinsics) and doesn't exist on ARM64 systems.

**Solution:** Added architecture detection and conditional compilation throughout `DirettaRingBuffer.h`:

1. **Architecture detection macro:**
   ```cpp
   #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
       #define DIRETTA_HAS_AVX2 1
       #include <immintrin.h>
   #else
       #define DIRETTA_HAS_AVX2 0
   #endif
   ```

2. **Conditional AVX2 functions:**
   - `convert24BitPacked_AVX2()` - x86 AVX2 with scalar fallback
   - `convert24BitPackedShifted_AVX2()` - x86 AVX2 with scalar fallback
   - `convert16To32_AVX2()` - x86 AVX2 with scalar fallback
   - `convertDSDPlanar_AVX2()` - x86 AVX2 with scalar fallback (uses `convertDSDPlanar_Scalar()`)
   - `simd_bit_reverse()` - x86-only helper (guarded)

3. **Scalar fallbacks for ARM64:**
   - Pure C++ implementations for all conversion functions
   - Uses existing `convertDSDPlanar_Scalar()` for DSD processing
   - `memcpyfast_audio.h` already had ARM64 support (uses `std::memcpy`)

**Performance note:** ARM64 builds use scalar code paths which are still efficient due to:
- GCC/Clang NEON auto-vectorization for simple loops
- Standard library optimizations in `std::memcpy`

**Files:**
- `src/DirettaRingBuffer.h` - Architecture guards and scalar fallbacks

---

### Pre-Transition Silence for DSD Format Changes

**Problem:** Crackling noise when switching DSD rates or transitioning DSD→PCM, despite previous fixes (full close/reopen with delays). The issue reappeared after Zen3 stabilization buffer changes.

**Root cause analysis:**
- When `onSetURI` receives a new track, it calls `stopPlayback(true)` (immediate)
- With `immediate=true`, NO silence buffers are sent before stopping
- The Diretta target's internal buffers still contain old DSD audio
- Comment in code acknowledged this: "We can't send silence here because playback is already stopped"
- The Zen3 stabilization change (longer post-online warmup) gave more time for residual audio artifacts to manifest

**Solution:** Added `sendPreTransitionSilence()` method that sends rate-scaled silence BEFORE calling `stopPlayback()`:

| DSD Rate | Silence Buffers | Rationale |
|----------|-----------------|-----------|
| DSD64    | 100             | Base level |
| DSD128   | 200             | 2× data rate |
| DSD256   | 400             | 4× data rate |
| DSD512   | 800             | 8× data rate |
| PCM      | 30              | Lower throughput |

**Implementation:**
- New public method `DirettaSync::sendPreTransitionSilence()`
- Calculates silence buffers based on current DSD rate: `100 × (sampleRate / 2822400)`
- Waits for silence to be consumed by `getNewStream()` (timeout scales with buffer count)
- Called in two locations:
  1. `onSetURI` callback before `stopPlayback()` (normal track change)
  2. Audio callback format change detection (gapless transitions)

**Transition flow after fix:**
```
1. onSetURI receives new track
2. m_audioEngine->stop()
3. waitForCallbackComplete()
4. sendPreTransitionSilence()  ← NEW: Flushes Diretta pipeline
5. stopPlayback(true)
6. [New format open() proceeds with clean target state]
```

**Files:**
- `src/DirettaSync.h` (lines 244-251) - Method declaration
- `src/DirettaSync.cpp` (lines 1058-1103) - Implementation
- `src/DirettaRenderer.cpp` (lines 366-368, 226-228) - Call sites

**Status:** Significantly improved. If crackling persists in edge cases, consider:
- Increasing silence buffer multiplier
- Adjusting timeout scaling
- Adding post-silence delay before `stopPlayback()`

## 2026-01-14

### 1. DSD Buffer Optimization - Pre-allocated Buffers

- Eliminated per-call heap allocations in DSD hot path
- Replaced `std::vector<uint8_t>` with pre-allocated `AudioBuffer` members
- Added `m_dsdLeftBuffer`, `m_dsdRightBuffer`, `m_dsdBufferCapacity` to `AudioDecoder`
- All `.insert()` operations replaced with `memcpy()` + offset tracking
- Buffers only resize when capacity is insufficient (rare, typically once per session)
- **Files:** `src/AudioEngine.h` (lines 141-144), `src/AudioEngine.cpp` (lines 552-661, 534)

### 2. DSD Rate-Adaptive Chunk Sizing

- Added `DirettaBuffer::calculateDsdSamplesPerCall()` function
- DSD chunks now scale with sample rate to maintain ~12ms granularity
- Previously fixed at 32768 samples regardless of DSD rate
- Significantly reduces loop iterations for high-rate DSD (DSD256+)
- **Files:** `src/DirettaSync.h` (lines 109-132), `src/DirettaRenderer.cpp` (lines 567-575)

### Performance Impact

| DSD Rate | Before (fixed 32768) | After (rate-adaptive) | Improvement |
|----------|----------------------|-----------------------|-------------|
| DSD64    | ~11.6ms/chunk        | ~12.1ms/chunk         | Similar |
| DSD128   | ~5.8ms/chunk         | ~12.0ms/chunk         | 2x fewer iterations |
| DSD256   | ~2.9ms/chunk         | ~11.6ms/chunk         | 4x fewer iterations |
| DSD512   | ~1.45ms/chunk        | ~5.8ms/chunk          | 4x fewer iterations |
| DSD1024  | ~0.7ms/chunk         | ~2.9ms/chunk          | 4x fewer iterations |

| Metric | Before | After |
|--------|--------|-------|
| Heap allocations per DSD read | 2 (std::vector) | 0 (steady state) |
| Memory pattern | Alloc/free every call | Pre-allocated, reused |

### 3. DSD512 Startup Fix for Zen3 CPUs (MTU-Aware)

- Scaled post-online stabilization to achieve consistent **warmup TIME** regardless of MTU
- Fixes harsh sound at DSD512 startup on AMD Zen3 systems (works fine on Zen4)
- Root cause: Zen3's slower memory controller and different cache hierarchy need more warmup time at high data throughput
- Additional issue: With small MTU (1500), `getNewStream()` is called more frequently (shorter cycle time), so a fixed buffer count resulted in insufficient warmup time

**Target warmup time by DSD rate:**

| DSD Rate | Target Warmup |
|----------|---------------|
| DSD64    | 50ms          |
| DSD128   | 100ms         |
| DSD256   | 200ms         |
| DSD512   | 400ms         |

**Buffer count scales with MTU to achieve target time:**

| MTU | Cycle Time (DSD512) | Buffers for 400ms |
|-----|---------------------|-------------------|
| 1500 | 261 μs | ~1530 buffers |
| 9000 | 1,590 μs | ~252 buffers |
| 16128 | 2,853 μs | ~140 buffers |

**Formula:**
```
targetWarmupMs = 50ms × dsdMultiplier
cycleTimeUs = (MTU - 24) / bytesPerSecond × 1,000,000
buffersNeeded = targetWarmupMs × 1000 / cycleTimeUs
```

- **Files:** `src/DirettaSync.cpp` (lines 1201-1239)

### 4. DSD Rate Change Transition Noise Fix

- **All DSD rate changes** now use full close/reopen (not just downgrades)
- Includes clock domain changes: DSD512×44.1kHz ↔ DSD512×48kHz
- Previously used `reopenForFormatChange()` which tries to send silence buffers
- Problem: When user selects new track, playback stops before transition, so `getNewStream()` isn't called and silence buffers never get sent to target
- Target's internal buffers still contain old DSD data → causes noise on new format
- Solution: Same aggressive approach as DSD→PCM (full `DIRETTA::Sync::close()` + delay + fresh `open()`)

| Transition | Action | Delay |
|------------|--------|-------|
| DSD→PCM | Full close/reopen | 800ms |
| DSD→DSD (any rate change) | Full close/reopen | 400ms |
| PCM→DSD | reopenForFormatChange() | 800ms |
| PCM→PCM (rate change) | reopenForFormatChange() | 800ms |

- **Files:** `src/DirettaSync.cpp` (lines 401-482)

### 5. Install Script Restructuring

Complete rewrite of `install.sh` with modular architecture and improved FFmpeg handling.

**Structural improvements:**
- Modular function-based architecture with clear section headers
- CLI argument support: `--full`, `--deps`, `--build`, `--configure`, `--optimize`, `--help`
- Interactive menu system with numbered options
- `confirm()` helper for consistent yes/no prompts

**FFmpeg changes:**
- Removed FFmpeg 5.1.2 and 6.1.1 (both have DSD segfault issues with GCC 14+)
- FFmpeg 7.1 is now the only source build option
- Build flags: `--enable-lto` for link-time optimization
- Added `mjpeg` and `png` decoders for embedded album art in DSF/DFF files
- Options: Build from source (recommended), RPM Fusion (Fedora), System packages

**Network buffer optimization:**
- Added sysctl settings for high-resolution audio streaming:
  - `net.core.rmem_max=16777216` (16MB receive buffer)
  - `net.core.wmem_max=16777216` (16MB send buffer)
- Available in both normal network config and aggressive optimization
- Persistent via `/etc/sysctl.d/99-diretta.conf`

**Fedora aggressive optimization (option 5):**
- Integrated from `optimize_fedora_server.sh`
- Removes: firewalld, SELinux, polkit, gssproxy
- Disables: journald, oomd, homed, auditd
- Replaces sshd with dropbear (lightweight SSH)
- Double confirmation required (safety)
- Intended for dedicated audio servers only

- **Files:** `install.sh`

### 6. CPU Isolation and Thread Distribution Tuner Scripts

Added two tuner scripts for CPU core isolation and real-time scheduling optimization.

**Common features (both scripts):**
- CPU isolation via kernel parameters (`isolcpus`, `nohz_full`, `rcu_nocbs`)
- Systemd slice for CPU pinning
- Real-time FIFO scheduling (priority 90)
- IRQ affinity to housekeeping cores
- CPU governor set to performance
- Automatic thread distribution across cores (via `ExecStartPost`)
- Manual `redistribute` command for testing without service restart

**Option 1: `diretta-renderer-tuner.sh` (SMT enabled)**

For systems where SMT (Hyper-Threading) is acceptable:
- Housekeeping: cores 0,8 (1 physical core + SMT sibling)
- Renderer: cores 1-7,9-15 (14 logical CPUs)
- 11 threads distributed across 14 CPUs (~1 thread per CPU)

**Option 2: `diretta-renderer-tuner-nosmt.sh` (SMT disabled)**

For dedicated audio servers with low system load:
- Adds `nosmt` kernel parameter to disable Hyper-Threading
- Housekeeping: core 0 (1 physical core)
- Renderer: cores 1-7 (7 physical cores)
- 11 threads distributed across 7 cores (~1.5 threads per core)

**Recommendation:**
- For dedicated low-load audio servers: **no-SMT** provides more predictable latency
- For multi-purpose systems: **SMT** provides more parallelism

**Usage:**
```bash
# Apply configuration (requires reboot for kernel params)
sudo ./diretta-renderer-tuner.sh apply

# Test thread distribution immediately (no reboot)
sudo ./diretta-renderer-tuner.sh redistribute

# Check current status and thread layout
sudo ./diretta-renderer-tuner.sh status

# Revert all changes
sudo ./diretta-renderer-tuner.sh revert
```

- **Files:** `diretta-renderer-tuner.sh`, `diretta-renderer-tuner-nosmt.sh`

---

## 2026-01-13

### 1. Full Integration of @leeeanh Optimizations

- Integrated all ring buffer optimizations from @leeeanh
- Power-of-2 bitmask modulo for single-cycle operations
- Cache-line separation to eliminate false sharing
- Lock-free audio path with atomic operations
- **Files:** `src/DirettaRingBuffer.h`, `src/DirettaSync.cpp`, `src/DirettaSync.h`

### 2. FFmpeg Custom Build Configuration

- Following leeeanh recommendations
- Found optimal FFmpeg 7.1 configuration that works with DSD playback
- Minimal build with only audio codecs needed (FLAC, ALAC, DSD, AAC, Vorbis, MP3)
- Includes libsoxr for high-quality resampling
- Includes HDCD filter support
- Removed problematic `--disable-inline-asm` and `--disable-x86asm` flags
- **Files:** `install.sh`

### 3. Target Release Bug Fix

- Added `release()` function for proper disconnection when playlist ends
- Previously, target remained "connected" after playback stopped
- New `m_sdkOpen` flag tracks SDK-level connection state
- `open()` now automatically reopens SDK if it was released
- Ensures target can accept connections from other sources after playback
- **Files:** `src/DirettaSync.cpp`, `src/DirettaSync.h`, `src/DirettaRenderer.cpp`

### 4. Install Script Enhancements

- Updated with working FFmpeg 7.1 build configuration
- Added FFmpeg installation test suite:
  - Checks required decoders (FLAC, ALAC, DSD, PCM)
  - Checks required demuxers (FLAC, WAV, DSF, MOV)
  - Checks required protocols (HTTP, HTTPS, FILE)
  - Runs decode functionality test
- Fixed directory handling after FFmpeg build
- Installs to `/usr/local` (coexists with system FFmpeg)
- **Files:** `install.sh`

### 5. DSD→PCM Transition Fix for I2S Targets

- Added special handling in `DirettaSync::open()` for DSD→PCM format transitions
- I2S/LVDS targets are more timing-sensitive than USB and need cleaner transitions
- DSD→PCM now performs: full `DIRETTA::Sync::close()` + 800ms delay + fresh `open()`
- Other format transitions (PCM→DSD, PCM→PCM, DSD→DSD) unchanged
- **Files:** `src/DirettaSync.cpp` (lines 372-421)

### 6. UPnP Stop Signal Handling

- Diretta connection now properly closed when UPnP Stop action received
- Ensures clean handoff when switching renderers
- Pause action unchanged (keeps connection open)
- **Files:** `src/DirettaRenderer.cpp` (lines 419-431)

### 7. Enhanced Target Listing

- `--list-targets` now shows detailed target information:
  - Output name (e.g., "LVDS", "USB") - differentiates ports
  - Port numbers (IN/OUT) and multiport flag
  - SDK version
  - Product ID
- **Files:** `src/DirettaSync.cpp` (lines 269-325)

---

## 2026-01-12 (thanks to @leeeanh)

### 1. Power-of-2 Bitmask Modulo

- Added `roundUpPow2()` helper function (lines 33-44)
- Added `mask_` member variable (line 295)
- `resize()` now rounds up to power-of-2 and sets `mask_ = size_ - 1`
- Replaced all `% size_` with `& mask_` throughout:
  - `getAvailable()` - line 69
  - `getFreeSpace()` - line 73 (simplified)
  - `push()` - line 106
  - `push24BitPacked()` - lines 138, 141-142, 145
  - `push16To32()` - lines 168, 172-174, 177
  - `pushDSDPlanar()` - lines 214, 232-234, 237-239, 244
  - `pop()` - line 268

### 2. Cache-Line Separation

- Added `alignas(64)` to `writePos_` (line 298)
- Added `alignas(64)` to `readPos_` (line 299)

### Performance Impact

| Operation     | Before                              | After                           |
| ------------- | ----------------------------------- | ------------------------------- |
| Modulo        | `% size_` (10-20 cycles)            | `& mask_` (1 cycle)             |
| False sharing | Possible between writePos_/readPos_ | Eliminated (64-byte separation) |

### Note

The buffer size will now be rounded up to the next power of 2. For example:
- Request 3MB → allocate 4MB
- Request 1.5MB → allocate 2MB

This wastes some memory but the tradeoff is worth it for the consistent fast-path performance.

---

## 2026-01-11 (thanks to @leeeanh)

### DirettaSync.h

- Removed `m_pushMutex`
- Added `m_reconfiguring` and `m_ringUsers` atomics for lock-free access
- Converted 11 format parameters to `std::atomic<>` (`m_sampleRate`, `m_channels`, `m_bytesPerSample`, etc.)
- Added `ReconfigureGuard` RAII class
- Added `beginReconfigure()` / `endReconfigure()` method declarations

### DirettaSync.cpp

- Added `RingAccessGuard` class for lock-free ring buffer access
- Added `beginReconfigure()` / `endReconfigure()` implementations
- Updated `sendAudio()` to use `RingAccessGuard` instead of mutex (lock-free hot path)
- Updated `configureRingPCM()`, `configureRingDSD()`, `fullReset()` to use `ReconfigureGuard`
- Updated all format parameter accesses to use atomic load/store with proper memory ordering

### DirettaRingBuffer.h

- Added `S24PackMode` enum (`Unknown`, `LsbAligned`, `MsbAligned`)
- Added `detectS24PackMode()` method that checks first 32 samples
- Updated `push24BitPacked()` to auto-detect and handle both S24 formats
- S24 detection resets on `clear()` and `resize()`

---

## Key Benefits

1. **Lock-free audio path** - `sendAudio()` no longer takes any mutex
2. **Safe reconfiguration** - `ReconfigureGuard` waits for active readers to drain
3. **S24 format flexibility** - Handles both LSB-aligned (FFmpeg S24_LE) and MSB-aligned formats automatically