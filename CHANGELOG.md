# Changelog

## [2.0.0] - 2026-01-28

### 🚀 Complete Architecture Rewrite

Version 2.0.0 is a **complete rewrite** of DirettaRendererUPnP focused on low-latency and jitter reduction. It uses the Diretta SDK (by **Yu Harada**) at a lower level (`DIRETTA::Sync` instead of `DIRETTA::SyncBuffer`) for finer timing control, with core Diretta integration code contributed by **SwissMountainsBear** (ported from his MPD Diretta Output Plugin), and incorporating advanced optimizations from **leeeanh**.

**SDK Changes:**
- Inherits `DIRETTA::Sync` directly (pull model with `getNewStream()` callback)
- Requires SDK version 148 with application-managed memory
- Full control over buffer timing and format transitions

### ⚡ Performance Improvements

| Metric | v1.x | v2.0 | Improvement |
|--------|------|------|-------------|
| PCM buffer latency | ~1000ms | ~300ms | **70% reduction** |
| Time to first audio | ~50ms | ~30ms | **40% faster** |
| Jitter (DSD flow control) | ±2.5ms | ±50µs | **50× reduction** |
| Ring buffer operations | 10-20 cycles | 1 cycle | **10-20× faster** |
| 24-bit conversion | ~1 sample/cycle | ~8 samples/cycle | **8× faster** |
| DSD interleave | ~1 byte/cycle | ~32 bytes/cycle | **32× faster** |

**Key Optimizations:**
- Lock-free SPSC ring buffer with power-of-2 bitmask modulo
- Cache-line separated atomics (`alignas(64)`) to eliminate false sharing
- AVX2 SIMD format conversions (24-bit pack, 16→32 upsample, DSD interleave)
- Zero heap allocations in audio hot path (pre-allocated buffers)
- Condition variable flow control (500µs timeout vs 5ms blocking sleep)
- Worker thread SCHED_FIFO priority 50 for reduced scheduling jitter
- Generation counter caching (1 atomic load vs 5-6 per call)

### ✨ New Features

**PCM Bypass Mode:**
- Direct path for bit-perfect playback when formats match exactly
- Skips SwrContext for zero-processing audio path
- Log message: `[AudioDecoder] PCM BYPASS enabled - bit-perfect path`

**DSD Conversion Specialization:**
- 4 specialized functions selected at track open (no per-iteration branches):
  - `Passthrough` - Just interleave (fastest)
  - `BitReverseOnly` - Apply bit reversal
  - `ByteSwapOnly` - Endianness conversion
  - `BitReverseAndSwap` - Both operations

**Timestamped Logging:**
- All console output now includes `[HH:MM:SS.mmm]` timestamps
- Easier log analysis for diagnosing timing issues

**Enhanced Target Listing:**
- `--list-targets` shows output name, port numbers, SDK version, product ID

**Production Build:**
- `make NOLOG=1` completely removes all logging code for zero overhead

### 🐛 Bug Fixes

**High Sample Rate Stuttering Fix:**
- Fixed stuttering at >96kHz (192kHz, 352.8kHz, 384kHz)
- Root cause: `bytesPerBuffer` vs SDK cycle time mismatch (~4% data deficit)
- Solution: Synchronized buffer sizing with `DirettaCycleCalculator`

**MTU Overhead Fix (thanks to Hoorna):**
- Fixed stuttering on networks with MTU 1500 (standard Ethernet)
- Root cause: SDK's `m_effectiveMTU` already accounts for IP/UDP headers
- Original OVERHEAD=24 was too high, causing unnecessarily small packets
- Solution: Changed OVERHEAD from 24 to 3 (Diretta protocol overhead only)
- Tested: OVERHEAD=3 works at MTU 1500, OVERHEAD=2 causes stuttering

**16-bit Audio Segfault Fix (thanks to SwissMountainsBear):**
- Fixed crash when playing 16-bit audio on 24-bit-only sinks
- Root cause: Missing conversion path for 16-bit input to 24-bit sink
- Code calculated bytesPerFrame using sink's 3 bytes but input only had 2 bytes
- Solution: Added `push16To24()` and `convert16To24()` conversion functions

**AVX2 Detection Fix:**
- Fixed crash on older CPUs without AVX2 (Sandy Bridge, Ivy Bridge)
- Root cause: Code assumed all x86/x64 CPUs have AVX2
- Solution: Use compiler-defined `__AVX2__` macro for proper detection
- CPUs without AVX2 now correctly use scalar implementations

**S24 Detection Fix (ARM64 distortion):**
- Fixed audio distortion on 24-bit playback on ARM64 platforms (RPi4, RPi5, etc.)
- Root cause: FFmpeg on ARM64 outputs S24 samples in MSB-aligned format (byte 0 = padding)
- x86 FFmpeg outputs LSB-aligned format (byte 3 = padding)
- Solution: Force MSB-aligned extraction on ARM64 platforms
- Diagnostic: `[00 XX XX XX]` pattern = MSB (ARM), `[XX XX XX 00]` = LSB (x86)

**SDK 148 Track Change Fix:**
- Application-managed memory pattern for `getNewStream(diretta_stream&)`
- Persistent buffer with direct C structure field assignment
- Fixes segmentation faults during track changes

**DSD→PCM Transition Noise:**
- Full `close()` + 800ms delay + fresh `open()` for clean I2S target transitions
- Pre-transition silence buffers (rate-scaled) flush Diretta pipeline

**DSD Rate Change Noise:**
- All DSD rate changes now use full close/reopen (not just downgrades)
- Includes clock domain changes (44.1kHz ↔ 48kHz families)

**PCM Rate Change Noise:**
- PCM rate changes now use full close/reopen approach (200ms delay)
- Previously tried to send silence but playback was already stopped

**PCM 8fs Runtime Format Fix:**
- Runtime verification of frame format in bypass path
- Auto-fallback to resampler if format mismatch detected mid-stream

**FLAC Bypass Bug:**
- Compressed formats correctly excluded from bypass mode
- FLAC always decodes to planar format requiring SwrContext

**44.1kHz Family Drift Fix:**
- Bresenham-style accumulator for fractional frame tracking
- Eliminates gradual underruns from rounding errors

**DSD512 Zen3 Warmup:**
- MTU-aware stabilization buffer scaling
- Consistent warmup TIME regardless of MTU (400ms for DSD512)

**Playlist End Target Release:**
- `release()` function properly disconnects target when playlist ends
- Target can accept connections from other sources

**UPnP Stop Handling:**
- Diretta connection properly closed on UPnP Stop action

### 🔧 Tools & Scripts

**CPU Tuner Auto-Detection:**
- Tuner scripts now auto-detect CPU topology (AMD and Intel)
- Support for any number of cores with/without SMT
- New `detect` command to preview configuration before applying
- Dynamic allocation of housekeeping and renderer CPUs
- Tested with Ryzen 5/7/9 and Intel Core processors
- Clean handoff when switching renderers

### 📦 Installation

**New unified `install.sh` script:**
```bash
chmod +x install.sh
./install.sh
```

**Interactive menu options:**
1. Full installation (dependencies, FFmpeg, build, systemd)
2. Install dependencies only
3. Build only
4. Install systemd service only
5. Configure network only
6. Aggressive Fedora optimization (dedicated servers only)

**Command-line options:**
- `--full` - Full installation
- `--deps` - Dependencies only
- `--build` - Build only
- `--service, -s` - Install systemd service
- `--network, -n` - Configure network

### 🔧 Build System

**FFmpeg Version Detection:**
- Automatic header/library version mismatch detection
- Clear error if compile-time vs runtime versions differ
- Options: `FFMPEG_PATH`, `FFMPEG_LIB_PATH`, `FFMPEG_IGNORE_MISMATCH`

**Architecture Auto-Detection:**
- Automatically selects optimal SDK library variant
- x64: v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4
- ARM64: Standard (4KB pages), k16 (16KB pages for Pi 5)

### 📚 Documentation

- Comprehensive `README.md` for v2.0
- `CLAUDE.md` project brief for contributors
- Technical documentation in `docs/`:
  - `PCM_FIFO_BYPASS_OPTIMIZATION.md`
  - `DSD_CONVERSION_OPTIMIZATION.md`
  - `DSD_BUFFER_OPTIMIZATION.md`
  - `SIMD_OPTIMIZATION_CHANGES.md`
  - `Timing_Variance_Optimization_Report.md`

### 🙏 Credits

- **Yu Harada** - Diretta SDK guidance and `DIRETTA::Sync` API recommendations

#### Key Contributors

- **SwissMountainsBear** - Ported and adapted the core Diretta integration code from his [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin). The `DIRETTA::Sync` architecture, `getNewStream()` callback implementation, same-format fast path, and buffer management patterns were directly contributed from his plugin. This project would not exist in its current form without his code contribution.

- **leeeanh** - Brilliant optimization strategies that made v2.0 a true low-latency solution:
  - Lock-free SPSC ring buffer design with atomic operations
  - Power-of-2 buffer sizing with bitmask modulo (10-20× faster than division)
  - Cache-line separation (`alignas(64)`) eliminating false sharing
  - Consumer hot path analysis leading to zero heap allocations
  - AVX2 SIMD batch conversion strategy (8-32× throughput improvement)
  - Condition variable flow control replacing blocking sleeps

---

## [1.3.3]

### 🐛 Bug Fixes

**Fixed:** Random playback failure when skipping tracks ("zapping")

Some users experienced an issue where skipping from one track to another would result in no audio playback, even though the progress bar in the UPnP control app continued to advance. Stopping and restarting playback would fix the issue.

**Root causes identified and fixed:**

1. **Play state notification without verification**
   - The UPnP controller was notified "PLAYING" even when the decoder failed to open
   - Now properly checks `AudioEngine::play()` return value before notifying
   - If playback fails, controller is notified "STOPPED" instead

2. **DAC stabilization delay skipped after Auto-STOP**
   - When changing tracks during playback, an "Auto-STOP" is triggered for JPlay iOS compatibility
   - The DAC stabilization delay timer (`lastStopTime`) was not updated during Auto-STOP
   - This could cause the next playback to start before the DAC was ready
   - Now properly records stop time in both manual Stop and Auto-STOP scenarios

**Impact:** More reliable track skipping, especially with rapid navigation through playlists.

---

## [1.3.2]

### 🐛 Bug Fixes

**Fixed:** DSD gapless playback on standard networks (MTU 1500)

If you experienced glitches between DSD tracks, this fixes it!
Works on any network equipment, no configuration needed.

---

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

---

## [1.3.0] - 2026-01-11

### 🚀 NEW FEATURES

**Same-Format Fast Path (Thanks to SwissMountainsBear)**

Track transitions within the same audio format are now dramatically faster.

| Before | After | Improvement |
|--------|-------|-------------|
| 600-1200ms | <50ms | **24× faster** |

How it works:
- Connection kept alive between same-format tracks
- Smart buffer management (DSD: silence clearing, PCM: seek_front)
- Format changes still trigger full reconnection (safe behavior)

**Dynamic Cycle Time Calculation**

Network timing now adapts automatically to audio format characteristics:
- DSD64: ~23ms optimal cycle time (was 10ms fixed)
- PCM 44.1k: ~50ms optimal cycle time (was 10ms fixed)
- DSD512: ~5ms optimal cycle time (high throughput)

**Transfer Mode Option**

Added `--transfer-mode` option:
- **VarMax (default)**: Adaptive cycle timing
- **Fix**: Fixed cycle timing for precise control

```bash
# Fixed timing at 528 Hz
sudo ./DirettaRendererUPnP --target 1 --transfer-mode fix --cycle-time 1893
```

---

## [1.2.1] and earlier

See git history for previous versions.
