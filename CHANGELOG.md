# Changelog

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