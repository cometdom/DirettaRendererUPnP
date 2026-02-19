# Changelog

## [2.0.4] - Unreleased

### ✨ New Features

**Centralized Log Level System:**
- New `LogLevel.h` header with 4 levels: ERROR, WARN, INFO, DEBUG
- `--quiet` (`-q`) option: show only warnings and errors (WARN level)
- `--verbose` continues to work as before (DEBUG level)
- Default level (INFO) produces the same output as v2.0.3
- All source files migrated from per-file `DEBUG_LOG` macros to unified `LOG_DEBUG`/`LOG_INFO`/`LOG_WARN`/`LOG_ERROR`
- In `NOLOG` builds, all logging macros compile to no-ops

**Runtime Statistics via SIGUSR1:**
- Send `kill -USR1 <pid>` to dump live statistics to stdout
- Shows: playback state, current format, buffer fill level, MTU, stream/push/underrun counters
- Useful for monitoring production systems via systemd journal

**MS Mode Negotiation Logging (feature request by Alfred):**
- Verbose log now shows the MS mode negotiated with the Diretta Target
- From second track onwards: supported modes, requested mode, and negotiated mode
- First track: clear message that MS info becomes available after first connection
- Uses "negotiated" wording to clarify the mode is inferred from AUTO algorithm + target capabilities

**Rebuffering on Underrun (streaming resilience):**
- When the ring buffer empties during a network stall (e.g., Tidal/Qobuz streaming), small data bursts were immediately consumed, creating a rapid silence/audio alternation ("CD skip" effect)
- Now enters rebuffering mode on underrun: holds silence until the buffer refills to 20%
- Result: clean silence gap followed by smooth playback resumption instead of stuttering
- Logged as `Buffer underrun — entering rebuffering mode` / `Rebuffering complete — resuming playback`

### ⚡ Performance

**Zero-Allocation Streaming Detection:**
- Replaced `std::string` + `std::transform` with POSIX `strcasestr()` for Qobuz/Tidal URL detection
- Eliminates heap allocation on every `openSource()` call

### 🐛 Bug Fixes

**FFmpeg DSD Streaming Error Handling:**
- Added handling for `AVERROR(ETIMEDOUT)`, `AVERROR(ECONNRESET)`, and `AVERROR_EXIT` in DSD read loop
- Generic fallback with `av_strerror()` for unexpected error codes
- Prevents silent hangs on network interruptions during DSD streaming

**Atomic Ordering Fix in RingAccessGuard:**
- Changed `fetch_add` from `memory_order_acquire` to `memory_order_acq_rel`
- Ensures the increment is visible to the reconfiguration thread on all architectures (ARM64)

**Stop Action Log Noise Reduction:**
- Redundant stop requests from control points now log at DEBUG level instead of INFO
- Actual stop actions still show a clear banner at INFO level
- Reduces log clutter when control points send multiple Stop actions (normal UPnP behavior)

### 🔧 Build & Configuration

**Production Build in install.sh:**
- `install.sh` now builds with `NOLOG=1` by default (disables SDK internal logging)
- Application-level logging (`--verbose`/`--quiet`) remains fully functional

**Updated systemd Configuration:**
- `diretta-renderer.conf`: documented `--quiet` option alongside `--verbose`
- `start-renderer.sh`: updated comments for log verbosity options

**Privilege Drop (`--user`):**
- New `--user, -u <name>` option to drop root privileges after network initialization
- Uses Linux-native `prctl(PR_SET_KEEPCAPS)` + `capset()` syscall — no libcap dependency
- Retains `CAP_NET_RAW`, `CAP_NET_ADMIN`, `CAP_SYS_NICE` capabilities after dropping to unprivileged user
- Non-fatal fallback: if `capset()` fails, logs a warning and continues with reduced capabilities

**Systemd Hardening:**
- 20+ security directives added to `diretta-renderer.service`
- Filesystem isolation: `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`
- Kernel protection: `ProtectKernelTunables`, `ProtectKernelModules`, `ProtectKernelLogs`
- Syscall filtering: blocks `@mount`, `@keyring`, `@debug`, `@module`, `@swap`, `@reboot`, `@obsolete`
- Dedicated `diretta` system user created by `install-systemd.sh`
- `CapabilityBoundingSet` limits to `CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE`

### 🏗️ ARM Architecture

**ARM NEON SIMD Format Conversions:**
- Hand-optimized NEON intrinsics for all PCM and DSD format conversions on ARM64
- PCM: `convert24BitPacked` (LSB/MSB), `convert16To32` using `vzip`/`vshrn`/`vmovn` intrinsics
- DSD: all 4 conversion modes (Passthrough, BitReverse, ByteSwap, BitReverseSwap) using `vzip1q_u32`/`vzip2q_u32` interleaving
- Bit reversal via `vqtbl1q_u8` LUT-based nibble swap, byte swap via `vrev32q_u8`
- Automatic detection via `DIRETTA_HAS_NEON` macro (`__aarch64__` + `__ARM_NEON`)
- Fallback to scalar code when NEON is not available

### 🧪 Testing

**Unit Test Suite (20 tests):**
- Comprehensive test suite for `DirettaRingBuffer` format conversions
- 3 memory infrastructure tests (memcpy correctness, timing variance, buffer alignment)
- 6 PCM conversion tests (24-bit pack LSB/MSB, 16→32, 16→24, single-sample edge cases)
- 5 DSD conversion tests (all 4 modes + small input scalar path)
- 4 ring buffer tests (wraparound, power-of-2 sizing, full buffer, empty pop)
- 2 integration tests (push24BitPacked→pop, pushDSDPlanarOptimized→pop)
- Run with `make test` — zero external dependencies

---

## [2.0.3] - 2026-02-15

### 🐛 Bug Fixes

**UPnP Event Deduplication (Audirvana compatibility):**
- Removed duplicate GENA events that caused progress bar hiccups on Audirvana
- Each UPnP action (Play, Pause, Stop, SetNextAVTransportURI) now sends exactly one `LastChange` event
- Root cause: action handlers called `sendAVTransportEvent()` redundantly — the DirettaRenderer callbacks already send the event via `notifyStateChange()`
- **Play**: Fixed by **herisson-88** ([PR #53](https://github.com/cometdom/DirettaRendererUPnP/pull/53))
- **SetAVTransportURI**: Removed duplicate `STOPPED` event during track-to-track transitions (auto-stop callback already sends it)
- **SetNextAVTransportURI**: Removed spurious event that triggered re-synchronization during gapless queueing
- **Pause**: Removed duplicate `PAUSED_PLAYBACK` event
- **Stop**: Removed duplicate `STOPPED` event
- Fixes progress bar stuttering/jumping in Audirvana and other control points that react to duplicate state notifications

**Format Change Preload Guard (squeeze2UPnP/LMS fix):**
- Prevented repeated preloading of the same next track during format changes (e.g., bit depth or sample rate change between tracks)
- The anticipated preload (from `SetNextAVTransportURI`) and EOF preload were not coordinated: when a format change was detected, the EOF path would re-open the same URL 2-3 additional times
- Added `m_formatChangePending` flag to skip redundant preloads once a format change transition is already scheduled
- Reduces unnecessary HTTP connections, especially beneficial for squeeze2UPnP/LMS setups that use ephemeral ports per track

**Adaptive Buffer for Remote Streaming (Qobuz/Tidal):**
- Ring buffer now adapts based on source type: 1.0s for internet streaming vs 0.5s for local playback
- Prefill increased to 150ms for remote sources (vs 80ms local) to absorb network latency
- Source type detection (local/remote) propagated from AudioEngine to DirettaSync via `isRemoteStream` flag
- Reduces underruns caused by CDN reconnections (e.g., Akamai dropping HTTP connections mid-stream)

**libupnp Callback Compatibility:**
- Fixed compilation error with libupnp <= 1.14.25 where `Upnp_FunPtr` Event parameter is `void*` instead of `const void*` (changed in 1.14.26)
- Compile-time detection of the callback signature using C++17 template type deduction
- Builds correctly with all libupnp 1.14.x versions without manual configuration

**Crash on Startup Failure (verbose mode):**
- Fixed `std::terminate()` crash when renderer fails to start with `--verbose` enabled
- Root cause: async log drain thread was not joined before `return 1`, causing `std::thread::~thread()` to call `std::terminate()` on a joinable thread
- Extracted cleanup into `shutdownAsyncLogging()` called at all exit paths (start failure, exception, signal handler, normal exit)
- Observed in the wild: `UpnpInit2 failed: -203` at boot → crash → systemd auto-restart

### ✅ Compatibility

**Audirvana (macOS/Windows):**
- Full compatibility confirmed with gapless PCM and DSD playback
- The "Universal Gapless" option in Audirvana is **no longer needed** and should be disabled
- DirettaRendererUPnP handles gapless transitions natively via `SetNextAVTransportURI`

---

## [2.0.2] - 2026-02-09

### ✨ New Features

**DSDIFF/DFF Native Playback (Audirvana DSD support):**
- Built-in DSDIFF container parser - FFmpeg has no DSDIFF demuxer, so DFF files were completely unplayable
- Uses FFmpeg's `avio` for HTTP I/O while parsing the DSDIFF container manually
- Parses FRM8 header, PROP chunk (sample rate, channels, compression type), and DSD data chunk
- Supports uncompressed DSD (rejects DST-compressed DSDIFF)
- Byte de-interleaves DFF data (L R L R...) to planar format ([all L][all R]) expected by the ring buffer
- DFF data stays MSB-first; DirettaRingBuffer handles bit conversion for the target
- Enables native DSD playback from Audirvana, which converts DSF files to DFF when streaming via UPnP
- Tested with DSD64 from Audirvana

**UPnP Event Notifications (progress bar fix):**
- Implemented proper UPnP GENA eventing with `UpnpNotify()` and `LastChange` XML
- Control points (Audirvana, BubbleUPnP, mConnect) now receive real-time notifications for:
  - Transport state changes (PLAYING, STOPPED, PAUSED)
  - Track changes (URI, duration, metadata)
  - Position updates
- Implemented `UpnpAcceptSubscription()` to send initial state on new subscriptions
- XML-escaped metadata values to prevent malformed events
- Fixes progress bar not updating on track transitions in control points

### ⚡ Performance

**Anticipated Gapless Preload (3-tier architecture fix):**
- HTTP connection for the next track now opens immediately when `SetNextAVTransportURI` is received
- Previously, the connection opened at EOF, causing buffer underruns (~0.2%) during the HTTP handshake
- Preload runs in a background thread while the current track plays, giving several seconds of headroom
- Fixes audio glitches during gapless transitions, especially on 3-tier Diretta setups (Host + Target on separate devices)
- Thanks to the user who reported this issue with Pi-5 Host/Target configuration

**Atomic Gapless Transition (Audirvana UI fix attempt):**
- Added `notifyGaplessTransition()` for atomic track data update + event sending
- Epoch counter prevents position thread from overwriting fresh track data with stale values
- Addresses race condition between 1-second position polling and track change callback

### 🐛 Bug Fixes

**Local vs Remote Server HTTP Options:**
- Detect local servers (192.168.x, 10.x, 172.x, localhost) and use simplified HTTP options
- Remote servers (Qobuz, Tidal) keep full reconnection/persistent options
- Fixes connection issues with local UPnP servers (Audirvana, JRiver) that don't support advanced HTTP features

**Streaming Proxy Detection (Qobuz/Tidal via local UPnP servers):**
- When a control point (e.g. Audirvana) proxies Qobuz/Tidal streams, the URL has a local IP (192.168.x.x) but the content comes from a remote streaming service
- The local/remote server detection now checks for streaming service names in the URL (case-insensitive)
- Proxied streams correctly use robust HTTP options (reconnect, http_persistent, ignore_eof) instead of simple local options
- Fixes potential connection drops when streaming Qobuz/Tidal through Audirvana or similar proxying control points
- Contributed by **herisson-88** ([PR #51](https://github.com/cometdom/DirettaRendererUPnP/pull/51))

**Premature Track Stop During Playlist Transitions:**
- Position reported to control points was ahead of DAC output by ~300ms (decoded vs played samples)
- Integer truncation caused `RelTime >= TrackDuration` before the track actually finished
- Some control points (mConnect) sent STOP prematurely, cutting audio on ~20% of transitions
- Fix: cap reported position to `duration - 1` while PLAYING; track end is signaled via `TransportState=STOPPED`
- Contributed by **herisson-88** ([PR #52](https://github.com/cometdom/DirettaRendererUPnP/pull/52))

**Ring Buffer Drain on Natural Track End:**
- `stopPlayback(true)` discarded ~75-150ms of buffered audio at end of track
- Now waits for ring buffer to drain below 1% before stopping (poll every 5ms, 2s timeout)
- Uses `stopPlayback(false)` for clean silence tail to DAC
- Contributed by **herisson-88** ([PR #52](https://github.com/cometdom/DirettaRendererUPnP/pull/52))

**Streaming Buffer Size for Remote Servers:**
- Increased FFmpeg HTTP buffer from 32KB to 512KB for remote streams (Qobuz, Tidal)
- Absorbs network jitter that caused intermittent micro-dropouts during streaming
- Local server buffer unchanged at 32KB (LAN is reliable)

---

## [2.0.1] - 2026-01-28

### 🐛 Bug Fixes

**24-bit Audio White Noise Fix (TEAC UD-701N and similar DACs):**
- Fixed white noise when playing 24-bit audio on DACs that only support 24-bit (not 32-bit)
- Root cause: S24 alignment hint was incorrectly set to LSB-aligned instead of MSB-aligned
- FFmpeg decodes 24-bit content into S32 format where audio data is in the upper 24 bits
- The v2.0.0 ring buffer extracted the wrong bytes (lower 24 bits including padding)
- Now correctly extracts bytes 1-3 (MSB-aligned) instead of bytes 0-2 (LSB-aligned)
- Affected: x86/x64 platforms with 24-bit-only DACs (ARM64 had a workaround)
- Thanks to the user who reported this issue with the TEAC UD-701N

---

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
