# CLAUDE.md - DirettaRendererUPnP-L Project Brief

## Overview

DirettaRendererUPnP-L is the **low-latency optimized** fork of DirettaRendererUPnP - a native UPnP/DLNA audio renderer that streams high-resolution audio (up to DSD1024/PCM 1536kHz) using the Diretta protocol for bit-perfect playback.

**Key differentiation from upstream (v1.2.1):**
- Inherits `DIRETTA::Sync` directly (vs `DIRETTA::SyncBuffer`) for finer timing control
- `getNewStream()` callback (pull model) vs SDK-managed push model
- Extracted `DirettaRingBuffer` class for lock-free SPSC operations
- Lock-free audio hot path with `RingAccessGuard` pattern
- Full format transition control with silence buffers and reopening
- DSD byte swap support for little-endian targets

**Low-latency optimizations (L variant):**
- Reduced PCM buffer from ~1s to ~300ms (70% latency reduction)
- 500µs micro-sleeps vs 10ms blocking sleeps (96% jitter reduction)
- AVX2/AVX-512 SIMD format conversions (8-32x throughput)
- Zero heap allocations in audio hot path
- Power-of-2 ring buffer with bitmask modulo (1 cycle vs 10-20)

## Architecture

```
┌─────────────────────────────┐
│  UPnP Control Point         │  (JPlay, BubbleUPnP, Roon, etc.)
└─────────────┬───────────────┘
              │ UPnP/DLNA Protocol (HTTP/SOAP/SSDP)
              ▼
┌───────────────────────────────────────────────────────────────┐
│  DirettaRendererUPnP-X                                        │
│  ┌─────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │   UPnPDevice    │─▶│ DirettaRenderer │─▶│  AudioEngine  │  │
│  │ (discovery,     │  │ (orchestrator,  │  │ (FFmpeg       │  │
│  │  transport)     │  │  threading)     │  │  decode)      │  │
│  └─────────────────┘  └────────┬────────┘  └───────┬───────┘  │
│                                │                   │          │
│                                ▼                   ▼          │
│                  ┌─────────────────────────────────────────┐  │
│                  │           DirettaSync                   │  │
│                  │  ┌───────────────────────────────────┐  │  │
│                  │  │       DirettaRingBuffer           │  │  │
│                  │  │  (lock-free SPSC, format conv.)   │  │  │
│                  │  └───────────────────────────────────┘  │  │
│                  │              │                          │  │
│                  │              ▼ getNewStream() callback  │  │
│                  │  ┌───────────────────────────────────┐  │  │
│                  │  │      DIRETTA::Sync (SDK)          │  │  │
│                  │  └───────────────────────────────────┘  │  │
│                  └─────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
              │ Diretta Protocol (UDP/Ethernet)
              ▼
┌─────────────────────────────┐
│      Diretta TARGET         │  (Memory Play, GentooPlayer, etc.)
└─────────────┬───────────────┘
              ▼
┌─────────────────────────────┐
│            DAC              │
└─────────────────────────────┘
```

## Key Files

| File | Purpose | Hot Path? |
|------|---------|-----------|
| `src/DirettaSync.cpp/h` | Inherits `DIRETTA::Sync`, manages ring buffer, format config | Yes |
| `src/DirettaRingBuffer.h` | Lock-free SPSC ring buffer with AVX2 format conversion | **Critical** |
| `src/DirettaRenderer.cpp/h` | Orchestrates playback, UPnP callbacks, threading | Partial |
| `src/AudioEngine.cpp/h` | FFmpeg decode, format detection, sample reading | No |
| `src/PrefetchReader.cpp/h` | Dedicated HTTP read-ahead thread (own byte ring, not `DirettaRingBuffer`) | No |
| `src/UPnPDevice.cpp/hpp` | UPnP/DLNA protocol, SSDP discovery, HTTP server | No |
| `src/ProtocolInfoBuilder.h` | UPnP protocol info generation | No |
| `src/main.cpp` | CLI parsing, initialization, signal handling | No |
| `src/memcpyfast_audio.h` | AVX2/AVX-512 optimized memcpy dispatcher | **Critical** |
| `src/fastmemcpy-avx.c` | C AVX implementation (x86 only) | **Critical** |
| `src/LogLevel.h` | Centralized log level system (ERROR/WARN/INFO/DEBUG) | No |
| `src/TimestampedLogger.h` | `std::cout`/`std::cerr` timestamp prefixing + journald-safe flush on `std::endl` | No |
| `src/test_audio_memory.cpp` | Unit tests for DirettaRingBuffer (PCM, DSD, DoP, S24 lifecycle) | No |
| `src/test_decode.cpp` | Standalone `AudioDecoder` test harness (`make test-decode`, no SDK/target needed) | No |

## Diretta SDK Reference

**SDK Location:** auto-detected by the Makefile from `$HOME / . / .. / /opt` (latest `DirettaHostSDK_<version>/` directory wins, via `sort -V | tail -1`). Override with `DIRETTA_SDK_PATH=...`. Current SDK at the time of writing: v1.50.x — v1.49.x also still builds and runs fine (verified: `DIRETTA_SDK_PATH` override to a v149 tree compiles and links cleanly), since the auto-detect simply picks whichever version is actually installed rather than requiring the latest.

### Key SDK Headers

| Header | Purpose |
|--------|---------|
| `Host/Sync.hpp` | Base class `DIRETTA::Sync` - stream transmission, thread modes |
| `Host/Format.hpp` | `FormatID`, `FormatConfigure` - 64-bit format bitmasks |
| `Host/Find.hpp` | Target discovery |
| `Host/Stream.hpp` | `DIRETTA::Stream` data structure |
| `Host/Profile.hpp` | Transmission profiles |
| `Host/Connection.hpp` | Base connection class |

### SDK Thread Modes (`DIRETTA::Sync::THRED_MODE`)

```cpp
CRITICAL = 1       // High priority sending thread
NOSHORTSLEEP = 2   // Busy loop for short waits
NOSLEEP4CORE = 4   // Disable busy loop if <4 cores
OCCUPIED = 16      // Pin thread to CPU
NOSLEEPFORCE = 2048// Force busy loop
NOJUMBOFRAME = 8192// Disable jumbo frames
```

### SDK Format Bitmasks (from `Format.hpp`)

```cpp
// Channels
CHA_2 = 0x02  // Stereo

// PCM formats
FMT_PCM_SIGNED_16 = 0x0200
FMT_PCM_SIGNED_24 = 0x0400
FMT_PCM_SIGNED_32 = 0x0800

// DSD formats
FMT_DSD1 = 0x010000      // DSD 1-bit
FMT_DSD_LSB = 0x100000   // DSF (LSB first)
FMT_DSD_MSB = 0x200000   // DFF (MSB first)
FMT_DSD_LITTLE = 0x400000
FMT_DSD_BIG = 0x800000
FMT_DSD_SIZ_32 = 0x02000000  // 32-bit grouping

// Sample rates (multipliers of 44.1k/48k base)
RAT_44100 = 0x0200_00000000
RAT_48000 = 0x0400_00000000
RAT_MP2 = 0x1000_00000000    // 2x (88.2/96k)
RAT_MP4 = 0x2000_00000000    // 4x (176.4/192k)
// ... up to RAT_MP4096 for DSD1024
```

## Bit Depth Handling

`configureSinkPCM()` negotiates the PCM format with the Diretta sink based on the source bit depth (`inputBits`) and returns `bool` (an unsupported format fails the track, never throws through `open()`):
- **16-bit sources**: 24 → 16 → 32 (the historical 24 → 16, plus 32 as a last resort).
- **24-bit sources**: 24 → 32 → 16. 24 first keeps the v2.4.4 behaviour on DACs that report 32-bit support at the Diretta target level but are physically 24-bit (TEAC UD-701N); 32 is only reached on a sink that refuses 24-bit, where it is lossless (the ring already holds S24 in an S32 container, the 32-bit sink path is a plain copy) and beats truncating to 16.
- **32-bit sources**: 32 → 24 → 16.
- 32-bit is never offered first to a 16/24-bit source.
- A 24/32-bit source on a 16-bit-only sink goes through `push32To16()` (explicit MSB truncation, logged as a warning). DoP requires an exact 24-bit sink.

`AudioEngine.cpp` detects the real bit depth via FFmpeg's `bits_per_raw_sample` (authoritative when set) or the `sample_fmt` fallback. The detected `bitDepth` is passed through `TrackInfo` → `AudioFormat` → `configureSinkPCM()`.

## Audio Hot Path

The following functions are in the critical audio path:

```
AudioEngine::readSamples()
    └─▶ DirettaSync::sendAudio()
            └─▶ RingAccessGuard (atomic increment)
            └─▶ DirettaRingBuffer::push*() or pushDSDPlanarOptimized()
                    └─▶ AVX2 format conversion (staging buffer)
                    └─▶ For DSD: switch(m_dsdConversionMode) - no per-iteration branches
                    └─▶ memcpy_audio_fixed() to ring

DirettaSync::getNewStream()  [SDK callback, runs in SDK thread]
    └─▶ DirettaRingBuffer::pop()
            └─▶ memcpy_audio() to DIRETTA::Stream
```

### DSD Conversion Mode Selection

Mode is determined once at track open in `configureSinkDSD()`:

| Mode | Bit Reverse | Byte Swap | Use Case |
|------|-------------|-----------|----------|
| `Passthrough` | No | No | DSF→LSB target, DFF→MSB target |
| `BitReverseOnly` | Yes | No | DSF→MSB target, DFF→LSB target |
| `ByteSwapOnly` | No | Yes | Little-endian targets |
| `BitReverseAndSwap` | Yes | Yes | Little-endian + bit mismatch |

**Rules for hot path code:**
- No heap allocations (reuse `m_packet`, `m_frame`, staging buffers)
- No mutex locks (use atomics via `RingAccessGuard`)
- Bitmask modulo (power-of-2 buffer size: `pos & mask_`)
- Predictable branch patterns
- Use `memcpy_audio()` instead of `std::memcpy`
- 64-byte alignment for SIMD buffers (`alignas(64)`)

## Lock-Free Patterns

### Ring Buffer Access (readers - `sendAudio()`)
```cpp
// From DirettaSync.cpp
class RingAccessGuard {
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) return;
        users_.fetch_add(1, std::memory_order_acq_rel);  // acq_rel: visible to beginReconfigure + see reconfiguring
        if (reconfiguring.load(std::memory_order_acquire)) {
            users_.fetch_sub(1, std::memory_order_relaxed);  // bail-out: never entered guarded section
            return;
        }
        active_ = true;
    }
    ~RingAccessGuard() {
        if (active_) users_.fetch_sub(1, std::memory_order_acq_rel);
    }
    bool active() const { return active_; }
};
```

### Reconfiguration (writer - format changes)
```cpp
class ReconfigureGuard {
    explicit ReconfigureGuard(DirettaSync& sync) : sync_(sync) {
        sync_.beginReconfigure();  // Sets m_reconfiguring = true, waits for m_ringUsers == 0
    }
    ~ReconfigureGuard() { sync_.endReconfigure(); }
};
```

### Lifecycle Mutex (v2.1.1 - thread-safe format transitions)
```cpp
// m_lifecycleMutex (std::recursive_mutex) protects open/close/stop/release
// Prevents concurrent access when UPnP thread calls stopPlayback() while
// audio callback is inside open() doing format transition
// Recursive because release() → close(), open() → reopenForFormatChange()
std::lock_guard<std::recursive_mutex> lifecycleLock(m_lifecycleMutex);

// m_openAbortRequested: signals open() to abort early when stop is requested
// stopPlayback()/close() set this + wake m_transitionCv before acquiring lock
// open() checks at strategic points and returns false if set
```

## Format Support

| Format | Bit Depth | Sample Rates | Ring Buffer Method | SIMD |
|--------|-----------|--------------|-------------------|------|
| PCM | 16-bit | 44.1kHz - 384kHz | `push16To32()` | AVX2 16x |
| PCM | 24-bit | 44.1kHz - 384kHz | `push24BitPacked()` | AVX2 8x |
| PCM | 32-bit | 44.1kHz - 384kHz | `push()` | memcpy |
| DSD | 1-bit | DSD64 - DSD512 | `pushDSDPlanarOptimized()` | AVX2 32x |

### S24 Alignment

FFmpeg has no S24 sample format: 24-bit content always reaches the ring as S32 with the audio in the upper 24 bits (MSB-aligned, low byte zero), whatever the codec. `AudioEngine.cpp` therefore sets the `MsbAligned` hint for every 24-bit track and `configureRingPCM()` sets it whenever a 32-bit container is packed to 24 bits (a 32-bit source on a sink that refuses 32-bit), and the ring's sample-sniffing detection only runs when the hint is missing; its timeout and fallback default to `MsbAligned` (the former LSB default, reached after pause → resume during a quiet passage because `clear()` also forgot the hint, turned the music into full-scale white noise on x86). `clear()` keeps the hint (pause → resume never re-sets it); only `resize()` forgets it, and its caller re-sets it. Same code on x86 and ARM (no `#if __aarch64__`).
- **MSB-aligned**: bytes 1-3 contain data → `convert24BitPackedShifted_AVX2()` (the normal path)
- **LSB-aligned**: bytes 0-2 contain data → `convert24BitPacked_AVX2()` (only via an explicit hint)

### SIMD Format Conversions

All format conversions use 64-byte aligned staging buffers before writing to the ring. Both AVX2 (x86-64) and NEON (ARM64) are supported with automatic detection via `DIRETTA_HAS_AVX2` / `DIRETTA_HAS_NEON` macros:

| Conversion | Function | AVX2 Throughput | NEON Throughput |
|------------|----------|----------------|-----------------|
| 24-bit pack (LSB) | `convert24BitPacked_AVX2()` | 8 samples/iter | 4 samples/iter |
| 24-bit pack (MSB) | `convert24BitPackedShifted_AVX2()` | 8 samples/iter | 4 samples/iter |
| 16→32 upsample | `convert16To32_AVX2()` | 16 samples/iter | 8 samples/iter |
| DSD planar→interleaved | `convertDSD_Passthrough()` | 32 bytes/iter | 16 bytes/iter |
| DSD bit reversal | `convertDSD_BitReverse()` | 32 bytes/iter | 16 bytes/iter |
| DSD byte swap | `convertDSD_ByteSwap()` | 32 bytes/iter | 16 bytes/iter |
| DSD bit reverse + swap | `convertDSD_BitReverseSwap()` | 32 bytes/iter | 16 bytes/iter |

## Buffer Configuration

From `DirettaSync.h` (low-latency tuned):

```cpp
namespace DirettaBuffer {
    constexpr float DSD_BUFFER_SECONDS = 0.8f;
    constexpr float PCM_BUFFER_SECONDS = 0.5f;          // Local playback
    constexpr float PCM_REMOTE_BUFFER_SECONDS = 1.0f;   // Remote streaming (Tidal/Qobuz)

    constexpr size_t DSD_PREFILL_MS = 200;
    constexpr size_t PCM_PREFILL_MS = 80;
    constexpr size_t PCM_REMOTE_PREFILL_MS = 150;        // Remote - larger prefill
    constexpr size_t PCM_LOWRATE_PREFILL_MS = 100;

    constexpr float REBUFFER_THRESHOLD_PCT = 0.20f;      // Resume after 20% buffer refill

    constexpr unsigned int DAC_STABILIZATION_MS = 100;
    constexpr unsigned int ONLINE_WAIT_MS = 2000;
    constexpr unsigned int FORMAT_SWITCH_DELAY_MS = 800;
    constexpr unsigned int POST_ONLINE_SILENCE_BUFFERS = 20;

    constexpr size_t MIN_BUFFER_BYTES = 65536;
    constexpr size_t MAX_BUFFER_BYTES = 16777216; // 16MB
}
```

### Flow Control Constants

From `DirettaRenderer.cpp`:

```cpp
namespace FlowControl {
    constexpr int MICROSLEEP_US = 500;           // Was 10,000µs (10ms)
    constexpr int MAX_WAIT_MS = 20;              // Was 500ms
    constexpr float CRITICAL_BUFFER_LEVEL = 0.10f; // Early-return below 10%
}
```

## Performance Summary

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| PCM buffer latency | ~1000ms | ~300ms | 70% reduction |
| Time to first audio | ~50ms prefill | ~30ms prefill | 40% faster |
| Backpressure max stall | 500ms | 20ms | 96% reduction |
| Heap allocs per decode | 3-4 | 0 (steady state) | Eliminated |
| Scheduling granularity | 186ms @44.1k | 46ms @44.1k | 4x finer |
| Ring buffer modulo | 10-20 cycles | 1 cycle | 10-20x faster |
| 24-bit conversion | ~1 sample/cycle | ~8 samples/cycle | 8x faster |
| DSD interleave | ~1 byte/cycle | ~32 bytes/cycle | 32x faster |

## Coding Conventions

- **Language:** C++17
- **Member prefix:** `m_` for instance members
- **Constants:** `constexpr` in namespace or `static constexpr` in class
- **Atomics:** Use `std::memory_order_acquire`/`release` appropriately
- **Alignment:** `alignas(64)` for cache-line separation on atomics
- **Indentation:** 4 spaces
- **Line length:** max 120 characters
- **Logging format:** `[ComponentName] Message`

### Commit Messages

```
type: short description

Longer explanation if needed.

Co-Authored-By: Claude <noreply@anthropic.com>
```

Types: `feat`, `fix`, `perf`, `refactor`, `test`, `chore`, `docs`

## Build & Run

```bash
# Build (auto-detects architecture)
make

# Build with specific variant
make ARCH_NAME=x64-linux-15zen4   # AMD Zen 4
make ARCH_NAME=x64-linux-15v3     # x64 with AVX2 (most common)
make ARCH_NAME=aarch64-linux-15   # Raspberry Pi 4 (4KB pages)
make ARCH_NAME=aarch64-linux-15k16 # Raspberry Pi 5 (16KB pages)

# SDK v149+ also ships GCC16-built variants (x64-linux-16v3/16v4/16zen4,
# aarch64-linux-16k4/16k16, riscv64-linux-16) alongside the GCC15 ones
# auto-detected by default. Reported to measurably improve sound quality
# on some setups (slim2diretta issue #10) but untested against an older
# host toolchain — the Makefile warns if the system gcc is older than
# the selected variant's GCC major version. Opt in explicitly:
make ARCH_NAME=x64-linux-16v3     # x64 AVX2, GCC16-built lib

# Same override via install.sh (ARCH_NAME env var, mirrors the LLVM=1 convention):
env ARCH_NAME=x64-linux-16v3 ./install.sh -b

# Production build (disables SDK logging)
make NOLOG=1

# Clean and rebuild
make clean && make

# Show build info
make info

# Run with target selection
sudo ./bin/DirettaRendererUPnP --list-targets
sudo ./bin/DirettaRendererUPnP --target 1 --verbose
```

**Note:** Building requires Linux. macOS builds are not supported due to missing FFmpeg/libupnp compatibility.

## SDK Library Variants

Located in `<SDK>/lib/` (SDK path auto-detected, see above):

| Pattern | Description |
|---------|-------------|
| `x64-linux-15v2` | x86-64 baseline |
| `x64-linux-15v3` | x86-64 with AVX2 |
| `x64-linux-15v4` | x86-64 with AVX-512 |
| `x64-linux-15zen4` | AMD Zen 4 optimized |
| `aarch64-linux-15` | ARM64 (4KB pages) |
| `aarch64-linux-15k16` | ARM64 (16KB pages, Pi 5) |
| `riscv64-linux-15` | RISC-V 64-bit |
| `x64-linux-16v3`/`16v4`/`16zen4` | x86-64 variants above, GCC16-built (SDK v149+; opt-in via `ARCH_NAME=`, see Build & Run above) |
| `aarch64-linux-16k4`/`16k16` | ARM64 (4KB/16KB pages), GCC16-built (SDK v149+) — note the naming shift from the GCC15 scheme: the 4KB variant has an explicit `k4` suffix instead of none |
| `riscv64-linux-16` | RISC-V 64-bit, GCC16-built (SDK v149+) |
| `*-musl*` | musl libc variants |
| `*-nolog` | Logging disabled |
| `*-win` (`x64-win`, `w32-win`, `arm64-win`) | Windows binaries — **present in the SDK but of unclear official status**: the SDK's own `memo_host.txt` explicitly states *« It only runs on Linux. It does not run on other operating systems. »* Ask Yu Harada before relying on the Windows libs for a real port. |

## Dependencies

- **Diretta Host SDK v1.47** - Proprietary (personal use only)
- **FFmpeg** - libavformat, libavcodec, libavutil, libswresample
- **libupnp** - UPnP/DLNA implementation
- **pthread** - Threading

Install on Fedora:
```bash
sudo dnf install gcc-c++ make ffmpeg-free-devel libupnp-devel
```

Install on Ubuntu/Debian:
```bash
sudo apt install build-essential libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libupnp-dev
```

## Current Work & Plans

### Completed
- [x] Lock-free audio path with `RingAccessGuard`
- [x] Power-of-2 bitmask modulo in ring buffer (`mask_ = size_ - 1`)
- [x] Cache-line separated atomics (`alignas(64)`)
- [x] S24 pack mode auto-detection with hint propagation
- [x] DSD byte swap for little-endian targets
- [x] Full format transition with `reopenForFormatChange()`
- [x] DSD→PCM transition fix (800ms settling for I2S targets)
- [x] DSD rate change transition fix (full close/reopen)
- [x] PCM rate change transition fix (full close/reopen with 200ms delay)
- [x] AVX2 SIMD format conversions (24-bit pack, 16→32, DSD interleave)
- [x] Low-latency buffer mode (~300ms PCM)
- [x] 500µs micro-sleep flow control
- [x] Zero heap allocations in hot path (reusable `m_packet`, `m_frame`)
- [x] ARM64 compatibility (auto-vectorized `std::memcpy`)
- [x] Playlist end target release fix
- [x] UPnP Stop closes Diretta connection properly
- [x] PCM FIFO with AVAudioFifo (O(1) circular buffer)
- [x] PCM bypass mode for bit-perfect playback
- [x] FLAC bypass bug fix (compressed formats never bypass)
- [x] DSD conversion function specialization (4 modes, no per-iteration branches)
- [x] Pre-transition silence for DSD format changes
- [x] DSD512 Zen3 warmup fix (MTU-aware buffer scaling)
- [x] ARM NEON hand-optimized format conversions (PCM + DSD 4 modes)
- [x] Systemd hardening (20+ security directives)
- [x] Unit tests (20 tests covering PCM, DSD, ring buffer, integration)
- [x] UAPP SOAP response compatibility (`u:` namespace prefix on action responses)
- [x] Lifecycle mutex for thread-safe format transitions (`m_lifecycleMutex`)
- [x] Timed worker thread join (`joinWorkerWithTimeout`) — prevents deadlock on SDK hang
- [x] Interruptible `open()` via `m_openAbortRequested` abort flag
- [x] High sample rate adaptive buffers (>192kHz: 2.0s buffer, 1000ms prefill, 32MB max)
- [x] Build capabilities logging at startup (architecture + SIMD detection)
- [x] Resilient target discovery (retry indefinitely at startup instead of exiting)
- [x] Fix: removed `verifyTargetAvailable()` pre-check in `DirettaRenderer::start()` that bypassed retry loop
- [x] RENDERER_NAME configuration option
- [x] Bit depth negotiation fix — only offer 32-bit when source is 32-bit — refined: never *first* for a 16/24-bit source; 32-bit is a last resort on a sink that refuses 24-bit (see Bit Depth Handling)
- [x] Audirvana preload probe fix — limit `probesize` to 32KB for local servers (herisson-88, PR #61)
- [x] First-play pre-connect — eliminates cold connect silence on first track
- [x] UAPP milliseconds fix — `HH:MM:SS` without fractional seconds in GetPositionInfo
- [x] UAPP async Play — `onPlay` callback launched asynchronously for fast HTTP 200 response
- [x] UAPP SCPD fix — added missing AbsTime/RelCount/AbsCount to GetPositionInfo SCPD declaration
- [x] Minimal UPnP mode (`--minimal-upnp`) — disables position thread and event notifications
- [x] Track restart fix — removed same-URI shortcut that prevented restarting track from beginning
- [x] RENDERER_NAME configuration option
- [x] Config variable alignment — `NAME`, `INTERFACE`, `MTU` (old names as fallback)
- [x] AIFF support — added `aiff` demuxer + `pcm_s16be/s24be/s32be` decoders to FFmpeg build config
- [x] CPU affinity (`--cpu-audio`, `--cpu-other`) — pin threads to dedicated cores via config/CLI/web UI
- [x] Multi-core CPU affinity — `--cpu-audio`/`--cpu-other` accept comma-separated lists (e.g. `3,4`)
- [x] Configurable buffer settings — PCM/DSD buffer seconds + prefill ms via config/CLI/web UI
- [x] Clang + LTO build support (PR #64 by sheviks) — `env LLVM=1 ./install.sh` or `make LLVM=1`
- [x] EIO→EOF fix — treat EIO after successful reads as normal EOF (fixes 32-bit 768kHz playlist advancement)
- [x] Audirvana internet radio fix — detect `/audirvana/*.pcm` URL pattern, open HTTP manually and wrap in a custom `AVIOContext` (no `mime_type` in its AVClass tree) so FFmpeg's `s16be` demuxer skips the strict RFC 2586 MIME check, then force 44100Hz + `ch_layout=stereo` (RFC 3551 fallback). FFmpeg minimal build also gets `pcm_s16be` demuxer added (was missing). Fixes "Invalid sample_rate found in mime_type 'audio/L16'" failure when Audirvana relays radio with non-conformant `audio/L16` headers.
- [x] Target network link tuning (PR #67 by Daniel/Koala887, v2.4.0) — `TARGET_INTERFACE` / `TARGET_SPEED` / `TARGET_DUPLEX` in the web UI force the host NIC speed/duplex via `ethtool` for audiophile users who perceive a sound-quality difference when constraining the link. Hardened with shell quoting, graceful skip when `ethtool` is missing, base-deps install, and bandwidth-vs-format warnings (10/100 Mbit not safe for hi-res PCM or DSD512+).
- [x] IRQ affinity for the target NIC(s) (v2.4.0) — `IRQ_INTERFACE` (single name or comma-separated list, e.g. `"enp1s0,enp2s0"`) / `IRQ_CPUS` config keys pin all hardware IRQs (incl. MSI-X queues) of the listed NICs to the specified CPU list at service start, walking `/proc/interrupts` and writing each `/proc/irq/N/smp_affinity_list`. Multi-interface support covers hosts with separate NICs for the upstream source (LMS/Roon) and the Diretta target. Kernel-managed IRQs that refuse reassignment are reported as "skipped". Documented alongside expanded `isolcpus=` kernel cmdline tuning in `docs/CONFIGURATION.md`.
- [x] SMT (Hyper-Threading) toggle (v2.4.0) — new `SMT` config key (`on` / `off` / `forceoff` / empty) writes `/sys/devices/system/cpu/smt/control` before DRUP launches, so subsequent `CPU_AUDIO` / `CPU_OTHER` pinning sees the right topology. System-wide setting; non-persistent across reboots (wrapper re-applies on each service start). BIOS lock or kernel-restricted control is detected and reported as a warning rather than a failure.
- [x] `--cpu-decode` option (PR #68 by Daniel/Koala887, v2.4.2) — third CPU-affinity granularity that pins the renderer audio thread (HTTP receive + FFmpeg decode) to its own dedicated core, separate from `--cpu-audio` (Diretta SDK worker) and `--cpu-other` (UPnP/position/main). When set, the audio thread is also raised to `SCHED_FIFO` priority (using `RT_PRIORITY`), since the dedicated core makes that safe. Falls back to `--cpu-other` when empty (preserves earlier behaviour). Same PR also fixed a regression introduced in v2.4.0: `ProtectKernelTunables=true` in the systemd unit was blocking `start-renderer.sh` from writing to `/proc/irq/N/smp_affinity_list`, silently breaking the IRQ affinity feature shipped in v2.4.0. The directive is now commented out; other systemd hardening directives remain in place.
- [x] Install script stop-before-replace (PR #69 by Daniel/Koala887, v2.4.2) — `install.sh` now detects whether `diretta-renderer.service` is running, stops it before copying the new binary into `/opt/diretta-renderer-upnp/`, and restarts it after install completes. Fixes a silent reinstall failure (`cp` cannot overwrite a file held open by systemd) that left users running the old binary until the next reboot. Same logic mirrored to `slim2Diretta/install.sh` for `slim2diretta.service` (commit c2f828f, v1.3.3).
- [x] FFmpeg 8 minimal build: drop `--enable-small`, add `--enable-lto` (Issue #70 reported by sheviks, v2.4.3) — the minimal FFmpeg 8.x configure flags in `install.sh` previously included `--enable-small`, which silently downgrades compiler optimization from `-O3` to `-Os` (GCC) / `-Oz` (Clang). With `--disable-everything` + selective `--enable-*` already trimming the build, that flag offered negligible size benefit while measurably hurting performance in the audio hot path (FLAC/AAC/PCM decoders, format conversions). Replaced with `--enable-lto` to align with the legacy/full FFmpeg build configuration. The pre-existing FFmpeg 5.x GCC-14 LTO workaround does not apply to FFmpeg 8.x.
- [x] Lossy-codec bit-depth cap (v2.4.4, reported by Dominique for a TEAC UD-701N on AudioLinux) — FFmpeg decodes lossy codecs (AAC/MP3/Vorbis/Opus/AC-3/WMA) into float (`FLT`/`FLTP`); `AudioEngine.cpp` bit-depth detection mapped that to 32-bit. The bogus 32-bit made `configureSinkPCM()` negotiate `FMT_PCM_SIGNED_32`, so DACs that advertise 32-bit at the Diretta target level but are physically 24-bit (TEAC UD-701N) played silence on AAC/MP3 web radio. Fix caps lossy codecs at 24-bit, identifying them via the FFmpeg codec descriptor (`AV_CODEC_PROP_LOSSY` set, `AV_CODEC_PROP_LOSSLESS` clear) so FLAC/ALAC/PCM keep negotiating their real depth. Single control point: `realBitDepth` just before `m_trackInfo.bitDepth` assignment in `AudioEngine.cpp`.
- [x] Corrupt PCM packet zombie-state fix (v2.4.5, PR #72 by hoorna/Alfred) — a corrupt packet mid-stream caused `avcodec_receive_frame()` to return an error after some samples were already decoded in the same `readSamples()` call. The `samplesRead == 0` guard silently skipped the error check, leaving the renderer producing silence and ignoring all UPnP commands. Fix: decode-error check moved before the `samplesRead == 0` guard; on detection the preload thread is joined, next-track state (`m_nextDecoder`, `m_nextURI`, `m_nextMetadata`, `m_formatChangePending`) is cleared, `m_state` set to `STOPPED` before firing `m_trackEndCallback()` — mirrors the normal EOF teardown's state-then-callback ordering (without the end-of-track drain delay, intentionally, since a corrupt packet is not a clean end).
- [x] Lossy-codec S24 alignment hint (v2.4.5, reported by Laurent for AAC web radio on TEAC UD-701N via JPLAY iOS) — companion to the v2.4.4 sink-cap fix. v2.4.4 made `configureSinkPCM()` correctly negotiate 24-bit for lossy codecs, but the `s24Alignment` detection block in `AudioEngine.cpp` still left the hint as `Unknown` for AAC/MP3/Vorbis/Opus/AC-3/WMA: their `m_codecContext->sample_fmt` is `AV_SAMPLE_FMT_FLTP` (float), which matches none of the three pre-existing branches (`PCM_S24LE/BE`, `FLAC/ALAC`, `S32/S32P`). With the hint missing, `DirettaRingBuffer` auto-detected alignment on first push and could pick `LsbAligned` on dynamic/silent content → white noise on 24-bit-only DACs. Added a 4th branch reusing the same `AV_CODEC_PROP_LOSSY && !AV_CODEC_PROP_LOSSLESS` codec-descriptor check as the v2.4.4 cap: lossy codecs go through the resampler with output `AV_SAMPLE_FMT_S32` (data in upper 24 bits = MSB-aligned), so we set `s24Alignment = MsbAligned` explicitly. The two v2.4.5 fixes (this + PR #72) are independent and orthogonal.
- [x] `mlockall` at startup (v2.5.0) — `main()` calls `mlockall(MCL_CURRENT | MCL_FUTURE)` after the CPU-affinity validation block and before the main thread is pinned / any worker thread is created. All current and future process pages are pinned in RAM for the lifetime of the binary — closes the last non-deterministic stall source (page fault from swap, cache eviction, or zero-fill on first write) that SCHED_FIFO + CPU pinning + isolcpus cannot prevent on their own. Same discipline as JACK / PipeWire in RT mode. Requires `CAP_IPC_LOCK` and `LimitMEMLOCK=infinity` — both shipped with `systemd/diretta-renderer.service` in the same release (`CAP_IPC_LOCK` added to `AmbientCapabilities` + `CapabilityBoundingSet`; `LimitMEMLOCK=infinity` added under a new `# --- Resource limits ---` section). On `EPERM` a `LOG_WARN` is emitted and the binary continues. `LOG_INFO("Memory locked in RAM (mlockall MCL_CURRENT|MCL_FUTURE)")` confirms success in the journal on every boot.
- [x] Live radio stream stall fix via `AVIOInterruptCB` (v2.5.1, PR #73 by hoorna/Alfred) — when a live stream is proxied via a local HTTP server (Roon, Audirvana, etc.) and the upstream stops sending audio, the TCP connection stays alive (keepalives) so `av_read_frame()` blocks indefinitely without ever returning. The renderer hung permanently. Fix registers an `AVIOInterruptCB` on the `AVFormatContext` before `avformat_open_input()`. In each `readSamples()` call, a `now + READ_STALL_TIMEOUT_S` (20s) deadline is written to `std::atomic<int64_t> m_readDeadlineNs` before `av_read_frame()` and cleared to 0 after. The callback (a `static int(*)(void*)` cast back to `AudioDecoder*`) checks the deadline on each FFmpeg I/O poll; when `now >= deadline` it returns 1 and FFmpeg aborts with `AVERROR_EXIT`. A new `m_readTimeout` flag (parallel to v2.4.5's `m_decodeError`) is set, accessor `hasReadTimeout()` added, and `process()` detects it before the `samplesRead == 0` guard to trigger an immediate clean stop. A `triggerFatalStop` lambda in `process()` factors the 14-line teardown shared between `hasDecodeError()` and `hasReadTimeout()` cases. Covers both DSD-raw and PCM paths. Zero-cost during normal playback (deadline == 0 in the callback's hot branch → immediate return). `memory_order_relaxed` is sufficient because the callback runs synchronously on the same thread that did the `store`.
- [x] `install.sh` UX: `--allowerasing` on FFmpeg `dnf install` + low-RAM warning before LTO compile (v2.5.2). No change to the DRUP binary itself, two small improvements that make `install.sh` rerun-safe and less prone to silent OOM failures: (1) `install_ffmpeg_rpm_fusion` and `install_ffmpeg_system` pass `--allowerasing` so dnf can swap between `ffmpeg-free*` (Fedora default) and `ffmpeg*` (RPM Fusion) cleanly on a re-run instead of failing on "conflicting requests"; (2) new `check_compile_ram()` helper called at the start of both `build_ffmpeg_8_minimal` and `build_ffmpeg_from_source` reads `MemTotal` from `/proc/meminfo` and, when total RAM is below 8 GB, prints a non-blocking warning plus copy-paste-ready commands to create a temporary 4 GB swap file before the build and remove it afterwards. We deliberately don't create one automatically — the matching `09-swap-disable` module in `fedora-audiophile-setup` actively turns swap off for audio determinism, so swap management is left to the user.
- [x] Web UI: comma-list normalization at save time (v2.5.3, reported by Dominique while wiring `IRQ_INTERFACE` through the fedora-audiophile-setup stable-naming refactor on 2026-06-02). Setting JSON declarations now carry an optional `"normalize"` field; `save_settings()` in `webui/diretta_webui.py` builds a `{key: rule}` map from the active profile once and canonicalises each matching value before splitting between CLI-opts (`CliOptsConfig.save`) and shell-vars (`ShellVarConfig.save`) paths. Only `comma_list` is implemented today (`re.sub(r'\s*,\s*', ',', value.strip())`, idempotent, safe for empty strings and non-string values), but the structure is in place for future per-field rules. Marked the five known comma-list fields: `IRQ_INTERFACE`, `IRQ_CPUS`, `CPU_AUDIO`, `CPU_DECODE`, `CPU_OTHER` (full profile); `CPU_AUDIO`, `CPU_DECODE`, `CPU_OTHER` (minimal — IRQ tuning belongs to the downstream distro on that flavour). Functionally a no-op (the shell wrappers `start-renderer.sh` and `install.sh` already trim per-element via `tr -d ' '`); the value is purely cosmetic divergence between input and on-disk storage. Symmetric fix shipped in slim2Diretta v1.4.2 since the two webuis share an identical Python codebase.
- [x] Boot warmup: defensive Target reset to escape stale idle-mode (v2.5.6, PR #79 by hoorna/Alfred). When a Diretta Target is powered on several minutes before DRUP starts, it can enter a stuck idle-mode: accepts the SDK connection but never becomes stream-ready (LEDs blink fast, no renderer can claim it). Root cause is a Target firmware state-machine bug — confirmed independent of OS, SD card, and predecessor client. Previous warmup (`open()` → `stopPlayback()`) left the connection open, trapping DRUP inside the stuck state. Fix: `open()` → `stopPlayback()` → `sleep(6s)` → `release()` — the hold gives Target time to exit stuck mode, the clean `release()` lets it reset. First real play does a fresh cold connect (~500ms–2.5s). Symmetric fix ported to slim2Diretta v1.4.8 (where there was no warmup at all).
- [x] DoP (DSD over PCM) output mode (v2.5.8, `--dop` flag, issue #80 by yama3kzh). Transmits DSD streams over Diretta PCM as standard 24-bit DoP v1.1 frames (alternating 0x05/0xFA marker in MSB byte, 2 DSD bytes per channel in bits 15:0). DSD64→176.4kHz, DSD128→352.8kHz, DSD256→705.6kHz, DSD512→1.4MHz PCM. Ring buffer in PCM mode (24-bit packed, 3 bps); `pushDSDToDoP()` in `DirettaRingBuffer` replaces `pushDSDPlanarOptimized()` when `--dop` is active. `m_isDoPMode` atomic (set by `configureRingPCM(isDoPMode=true)`) gates the new path in `sendAudio()`. DoP marker state resets in `clear()`. PCM and native DSD paths unchanged; `--dop` is a no-op for PCM content.
- [x] `--dop-msb` flag (v2.5.8, issue #80). Bit-reverses each DSD byte before DoP packing for DACs that expect MSB-first DSD payload. 256-entry lookup table, zero allocs in hot path. `DOP=msb` in config, third option in web UI.
- [x] DoP even-frame invariant — ring pops and drift compensation (v2.5.8, issue #80). Two defensive changes ensure ring read position stays at 0x05-aligned DoP frames: (1) `pushDSDToDoP` even-frame guard (`if (pcmFrames % 2 != 0) pcmFrames--;`, commit `209365f`) — every push writes even frames, `m_dopMarkerState` stays at `false` (0x05) after each push; (2) 44.1k drift corrector in DoP mode: threshold 1000→2000, add 1→2 frames (commit `8d74693`) — fires every ~5 calls adding 178 frames (even) instead of 177 (odd); average rate unchanged. After any silence period, first real audio frame always resumes at a clean 0x05 marker. Unit test `test_pushDSD_dop_marker_phase_invariant` covers the push-side invariant.
- [x] DoP silence: plain PCM 0x00 for all modes (v2.5.8, issue #80). `fillSilence()` in `getNewStream()` uses plain `memset(0x00)` in all modes including DoP, matching the MinimServer/Asset UPnP pre-encoded DoP reference path. An earlier revision generated DoP-marked silence frames (0x69 + alternating markers, tracked by `m_doPSilenceMarkerState`); reverted and `m_doPSilenceMarkerState` removed. Verbose mode logs first 5 ring pops with hex-decoded marker+DSD bytes (`[DoP POP #N]`) for field diagnosis.
- [x] DoP stabilisation count fix (v2.5.8, issue #80). `getNewStream()` computed cycle time as `efficientMTU / bytesPerSecond` → 1.4 µs at 176.4kHz DoP → 3000 buffers (3 s) instead of 100 ms. Fixed (commit `912a177`) to use `currentBytesPerBuffer`. Also fixes DSD branch and stores missing `m_sampleRate`/`m_bytesPerSample` in `configureRingDSD()`.
- [x] DoP byte pair order fix — root cause of "music + constant hiss"
- [x] Online-timeout deadlock fix (v2.5.10) — `sendAudio()` returned 0 after `waitForOnline` timeout → ring empty → silence loop → never online. New `m_onlineTimeoutOccurred` flag bypasses `!is_online()` guard in recovery mode so the ring can fill and the target can go online. (v2.5.8, commit `233b520`, issue #80, confirmed by Dominique on HOLO Audio Spring 3 / DDC-0 and by yama3kzh on SFORZATO DAC-01). `pushDSDToDoP()` was writing each DoP 24-bit word as `[DSD_N, DSD_N+1, marker]` instead of the correct `[DSD_N+1, DSD_N, marker]` (DoP v1.1 spec: bits[23:16]=marker, bits[15:8]=DSD_N, bits[7:0]=DSD_N+1 in little-endian memory). The two DSD bytes within every frame were transposed, breaking PDM correlation at byte boundaries while keeping DoP framing intact — the DAC locked and displayed DSD2.8MHz but produced music + constant HF hiss. Fix: two-line swap in the `pushDSDToDoP()` output loop. Both `--dop` (LSB-first) and `--dop-msb` (bit-reversed) now produce correct output.
- [x] CPU affinity validation fix (v2.5.8, reported by Kiran on Ryzen 7730U / AudioLinux with HT off) — `sysconf(_SC_NPROCESSORS_ONLN)` returns CPU *count* (8), not max ID; with SMT off on AMD Ryzen, online CPUs are 0,2,4,...,14, so valid cores ≥ 8 were silently rejected and thread pinning disabled. New `getOnlineCpus()` reads `/sys/devices/system/cpu/online` and checks actual set membership. Same fix in slim2Diretta v1.4.10.
- [x] `install.sh` check fixes (v2.5.8): (1) `[MISSING] mov` false positive — FFmpeg 8.x lists aliases as `mov,mp4,m4a,...`; grep now matches `" ${dem}[ ,]"`; (2) decode test false failure — `lavfi`/`sine` absent in minimal builds, replaced with silent `s16le` pipe; (3) `build_renderer()` auto-refreshes WebUI profiles if already installed, so `--build` alone keeps the UI in sync.
- [x] Live stream proxy stalls: reconnect instead of hard stop (v2.5.11, PR #84 by hoorna/Alfred). When an internet radio station resets its live stream, a local proxy (Roon, Audirvana) can stall or deliver a truncated packet mid-stream. Three complementary changes: (1) a single transient `avcodec_receive_frame()` error on a live stream (`duration == 0`) now flushes the codec and continues instead of setting `m_decodeError`, capped at 1 consecutive error via `m_liveStreamDecodeErrors`; (2) on a genuine read stall (the `AVIOInterruptCB` deadline from v2.5.1 firing), `process()` reopens the same URL up to 3 times (`m_liveStreamReconnects`) instead of calling `triggerFatalStop`, with the counter reset on any successful read, on `play()`, and on `stop()`; (3) `READ_STALL_TIMEOUT_S` reduced from 20s to 5s, cutting the audible gap on a real stall from ~21s to ~5s. The reconnect itself uses the same capture-validate-commit pattern as `preloadNextTrack()`: the new `AudioDecoder` is opened into a local `unique_ptr` with `m_mutex` released (avoiding a ~90s freeze of all UPnP transport commands during the blocking `avformat_open_input()`), then committed to `m_currentDecoder`/`m_currentTrackInfo` under lock only after re-validating `m_state == PLAYING && m_currentURI == capturedURI` — closes a data race on `m_currentDecoder` (found in code review of an earlier iteration where `openCurrentTrack()` wrote to shared members unguarded during the unlocked window; a concurrent `Stop`/`SetURI` could race a `unique_ptr` write against `m_currentDecoder.reset()`).
- [x] Post-reconnect buffer oscillation fix (v2.5.11, PR #85 by hoorna/Alfred, depends on #84). Immediately after a live-stream reconnect, the ring buffer typically holds only ~20-22% — just enough to clear the normal `REBUFFER_THRESHOLD_PCT` (20%) instantly, before the decoder has caught up, causing rapid underrun/rebuffering oscillation for several minutes. New `DirettaSync::requestPostReconnectRebuffering()` raises the threshold to `REBUFFER_THRESHOLD_REMOTE_PCT` (50%) for the single rebuffering cycle following a reconnect via a `m_postReconnectRebuffering` atomic (checked in `getNewStream()`, cleared once the 50% threshold is met or on `open()`/`close()`/`fullReset()` to prevent bleed-through to an unrelated later stream). `AudioEngine::consumeReconnectFlag()` signals `DirettaRenderer::audioThreadFunc()` after each `process()` call (including the low-buffer catch-up call) that a reconnect just occurred. Tested on `pcm_s24le`/48kHz internet radio via Roon: underruns per session dropped from ~63 to ~6 over a comparable 4h45m run.
- [x] Raw packet bypass decoder (v2.5.11, PR #86 by hoorna/Alfred). For native PCM streams where the raw FFmpeg packet already matches the output frame size exactly (`block_align == outBytesPerFrame`, little-endian containers only — e.g. `pcm_s16le`), `AudioDecoder::readSamples()` copies demuxed packet bytes directly into the audio path via `m_rawPacketBypass`, skipping `avcodec_send_packet()`/`avcodec_receive_frame()` entirely for zero decode overhead. Detected once in `initResampler()`; falls back to the normal decode path for anything else (big-endian, compressed, or 24-bit where `block_align` (`3×channels`) can never match the S32-container `outBytesPerFrame` (`4×channels`) — deliberately listed but inert for that case).

- [x] `start-renderer.sh`: non-fatal `ethtool` link tuning (v2.5.12, reported by Daniel via TuneOS/fedora-audiophile-setup). `TARGET_INTERFACE`/`TARGET_SPEED`/`TARGET_DUPLEX` (v2.4.0) run `ethtool -s` before the renderer starts, under a script-wide `set -e`. The call was unguarded: a `TARGET_INTERFACE` naming an interface that doesn't exist yet (e.g. a stable-naming rename via `.link` files that only takes effect at the next reboot — confirmed root cause on Daniel's TuneOS box) makes `ethtool` fail, `set -e` aborts the whole script, and the DRUP binary is never `exec`'d at all. Systemd only ever saw a bare wrapper exit code (75, ethtool's own `EX_TEMPFAIL`) with `Restart=on-failure` looping forever — no indication anything audio-related was even reached. Fix: the `ethtool` call is now wrapped in `if ! ethtool ...; then <warning>; fi`, so a failure prints an actionable message (checked with a simulated failing `ethtool`) and the script falls through to `exec` the renderer regardless — this link-tuning step is cosmetic, never a requirement for playback. `DirettaRenderer::start()` (`DirettaRenderer.cpp:758-789`) already retries `UpnpInit2()` indefinitely with a "Network not ready, retrying UPnP init..." log every 5s when `stopSignal` is set (always true in normal usage, `main.cpp` passes `&g_running`), so if the real `--interface` binding is also not ready yet, the renderer now degrades to a clean retry loop instead of a crash loop, and self-heals once the network comes up.

- [x] Signal-handling race: second SIGINT/SIGTERM during shutdown crashed the renderer (v2.5.13, PR #88 by hoorna/Alfred). `signalHandler()` does blocking work directly (renderer `stop()`, thread joins, SDK release), but default `signal()` semantics only block re-delivery of the same signal on the thread already running the handler — every other thread (UPnP, audio, position, Diretta SDK worker) stayed eligible to receive a second SIGINT/SIGTERM while the first was still being handled (shutdown can take a few seconds: SDK release, buffer drain, several thread joins). If the kernel delivered the second signal to a worker thread instead of main, `signalHandler()` re-entered concurrently and called `stop()` a second time while the first call was still in progress, joining the same `std::thread` objects from two threads at once — crashed with `terminate called without an active exception`. Fix: each worker thread now calls `blockShutdownSignalsOnThisThread()` (`pthread_sigmask(SIG_BLOCK, ...)` on SIGINT/SIGTERM) as the very first thing in its own entry function (`upnpThreadFunc`/`audioThreadFunc`/`positionThreadFunc` in `DirettaRenderer.cpp`, `g_logDrainThread` in `main.cpp`) — a process-directed signal only delivers to a thread that isn't blocking it, so a second signal mid-shutdown can now never land anywhere but the main thread. An earlier iteration instead blocked the signals on the *main* thread across the whole `start()` call (new worker threads inherit their creator's mask) — caught in code review: this made the process briefly un-interruptible whenever `start()`'s own indefinite network/target retry loops were active, since no thread existed with the signal unblocked during that window (verified by starting the renderer with no reachable Diretta target and confirming Ctrl-C was silently ignored mid-retry). Corrected before merge to the current per-worker-thread-blocks-itself design, which keeps main permanently interruptible while preserving the original fix (re-verified: Ctrl-C during a "Target not found, retrying..." loop now exits cleanly and immediately).

- [x] GCC16-built SDK variant support + toolchain compatibility check (v2.5.14, slim2diretta issue #10, sheviks). SDK v149 ships each arch variant built with both GCC15 and GCC16 static libraries (e.g. `libDirettaHost_x64-linux-16v3.a` alongside the existing `x64-linux-15v3`); sheviks reported the GCC16 build measurably improved sound quality on his setup and asked whether a system-GCC-version check would be worth adding given the untested risk of mixing a GCC16-built static lib with an older host toolchain. `make ARCH_NAME=x64-linux-16v3` (and the aarch64/riscv64 GCC16 equivalents) already worked with no code change — variant selection was already generic. Added the safety check (ported from slim2diretta v1.4.18): the Makefile parses the GCC major version embedded in the selected variant name and compares it against the system's actual `gcc -dumpversion`, warning (non-fatal) when the system toolchain is older than a GCC16+ variant — a static lib built with a newer GCC can reference `libstdc++` symbol versions the installed runtime doesn't have, a failure that surfaces at runtime (`GLIBCXX_3.4.xx not found`) rather than at build time. Silent for the proven GCC15-vs-older-system combination; only the new GCC16 territory is unverified. Auto-detection still defaults to GCC15. `install.sh`'s `build_renderer()` also gained an `ARCH_NAME` env-var passthrough mirroring `LLVM=1`.

- [x] SDK 150.x ~50s connection stall / outright sink-set failures (v2.5.15, PR #89). Two distinct bugs, both root-caused by Yu Harada while diagnosing the report:
  1. **`DirettaSync::statusUpdate() override` was an empty stub** (`{}`), silently swallowing the base class's notification that `Sync::connectWait()` waits on. Confirmed via a live packet capture during a stall: the real UDP negotiation with the target completed and streamed normally within ~1s the entire time — `connectWait()` was simply never woken up despite the connection already being good, sitting out its full internal timeout every time. This was the actual cause. Fixed by chaining to the base class: `void statusUpdate() override { DIRETTA::Sync::statusUpdate(); }`.
  2. **`configureSinkPCM()`/`configureSinkDSD()` called `setSinkConfigure()` before `setSink()`** — the wrong order per Yu's explicit guidance ("if you call `Sync::setSink`, you must also call `Sync::setSinkConfigure`" — after, not before). SDK 149's own uninitialized-flag bug apparently tolerated the wrong order; SDK 150 fixed that flag, exposing it. Didn't turn out to affect the reported stall (tested in isolation, no effect), but a real correctness bug per the SDK author, fixed regardless: the format `checkSinkSupport()` determines via trial-and-error is now stashed in a new `m_pendingSinkFormat` member (`DirettaSync.h`) and applied via `setSinkConfigure()` once, right after `setSink()` succeeds in `open()` (`DirettaSync.cpp`).

  Confirmed on real hardware (SDK 150_4, DDC-0 target firmware 150_1): `OPEN` → `OPEN COMPLETE` completes in well under a second on both boot warmup and real playback — matching SDK 149.x — across repeated restarts, with `Stop` during playback transitioning cleanly instead of appearing ignored. Also added `DIRETTA_SDK_SYSLOG_DEBUG=1` (env var, off by default): wires up the SDK's own internal syslog (`DIRETTA::SysLogDiretta`, `Host/SysLog.hpp`) into the renderer's log, added at Yu Harada's request while investigating this stall — kept as diagnostic tooling for future SDK-level issues.

- [x] Seek reliability (v2.5.16, PR #91, herisson-88). Three independent bugs behind occasional "seek does nothing" / "seek lands seconds late" reports: (1) the Diretta ring was never flushed after the decoder seeks, so whatever was already buffered from the old position (up to 0.5s local / 3s remote) played first — a new `AudioEngine::setSeekCallback()` now calls `DirettaSync::flushForSeek()` right when the seek lands; (2) the async seek flag was read-target-then-clear-flag, a window in which a second seek arriving from a scrubbing control point (several per second) could be silently dropped — now consumed atomically with `m_seekRequested.exchange(false, ...)` before the target is read; (3) a seek while `PAUSED` was flatly rejected ("Cannot seek when not playing") instead of queued for resume, since scrubbing while paused is a common control-point pattern. `Seek` with a non-time unit (`TRACK_NR`) is now ignored instead of misapplied as seconds. Verified with the new `test_decode --seeks` harness (below): 5-seek sequences and 30 random rapid seeks on FLAC 16/44, FLAC 24/192, ALAC and MP3, identical output with and without the prefetch thread.

- [x] Tuner/systemd priority fix + doc corrections (v2.5.16, PR #92, herisson-88). The CPU tuner drop-in scripts (`diretta-renderer-tuner*.sh`) had been setting a process-wide `CPUSchedulingPolicy=fifo`/`Priority=90` in the systemd unit, which put every control thread (UPnP, position, main) above the audio worker's own `RT_PRIORITY` (default 50) and made `NICE_LEVEL` meaningless (nice is ignored under SCHED_FIFO). The renderer already applies SCHED_FIFO per-thread itself (Diretta worker always; decode thread only when `--cpu-decode`/`CPU_DECODE` is set), so the blanket unit-level override was redundant and actively harmful. Removed; re-run the tuner to apply on an existing host. Also fixed several stale claims in `docs/CONFIGURATION.md`/README: `--thread-mode` bit 8 is not `SOCKETNOBLOCK` (that flag is commented out in the SDK header, no documented effect), `FEEDBACKOFFSET` is a 3-bit field (32/64/128) not three separate flags, `--cycle-time`'s real default is one MTU of audio (not a fixed 2620µs base), and the documented remote buffer defaults had drifted from the actual `PCM_REMOTE_BUFFER_SECONDS`/`PCM_REMOTE_PREFILL_MS` constants (3.0s/500ms, not 1.0s/150ms).

- [x] `--port-strict` — opt-in fixed-port wait after a hot restart (v2.5.16, PR #93, herisson-88). libupnp is built without `SO_REUSEADDR`; on a hot restart, if the previous instance's control-point connections are still in `TIME_WAIT` on the configured port (up to 60s on Linux, not tunable), `bind()` fails and libupnp silently falls back to port+1, announcing the new LOCATION over SSDP. Most control points follow that; JPLAY keeps its cached address and stops reaching the renderer entirely. `--port-strict`/`PORT_STRICT=1` makes `UPnPDevice::start()` retry `UpnpInit2()` every 2s for up to 75s until the configured port is actually available, instead of accepting the SSDP-announced port+1 — off by default since it costs up to a minute without a renderer after every hot restart. The retry wait sleeps in 100ms increments checking `m_stopSignal` each time (not one long blocking sleep), so it stays interruptible per the v2.5.13 invariant — verified: Ctrl-C during the wait aborts within ~100ms rather than sitting out the full window.

- [x] SDK usage improvements: `auto-sdk` transfer mode, `--sink-buffer-ms`, `--rapid-start`, FIX-profile buffer alignment (v2.5.16, PR #94, herisson-88; rebuilt and confirmed compiling against both SDK 149 and 150). `--transfer-mode auto-sdk` exposes `Sync::configTransferAuto()`, the mode the SDK's own sample host uses (lets the SDK pick fixed vs. variable cycles itself, `--cycle-min-time` as the floor). `--sink-buffer-ms` exposes `setSink()`'s documented sink buffer time parameter (v2.5.15 always passed the computed cycle time there; now overridable, `0` = sink's own default). `--rapid-start` exposes SDK 150's `connect(cpu, rapidStart=true)` — resolved at compile time via SFINAE (`SdkHasRapidStart<S>`, `if constexpr`) rather than pinning to one SDK version, so SDK 149 (`connect(int)` only) keeps building. In `auto-sdk` mode, when the SDK negotiates a fixed cycle (observed on a Holo Red: 2ms) it sends exactly `getCycleSize()` bytes per cycle and expects that much back from `getNewStream()`; the renderer's callback buffers were still 1ms, leaving the target silent. Fixed via `alignBufferToNegotiatedCycle()`, called after `connectWait()`, with the 44.1kHz drift accumulator disabled in that mode — verified on 16/44, 24/44.1 and 24/96 (cycle 2000µs measured 1997µs mean, 0 underruns). The negotiated profile (cycle, min cycle, cycle size/packets, mode, MS mode, latency, `SinkInfo`) is now logged at every `OPEN` for diagnosis.

- [x] EOF-drain heap-overflow fix + bit-depth negotiation refinement + RT-thread logging + journald flush (v2.5.16, PR #95, herisson-88; the EOF-drain and bit-depth items independently verified — the first by compiling and reading the underflow mechanics directly, the second by re-tracing the fallback order against the CLAUDE.md invariant below). `readSamples()`'s drain loop deliberately keeps pulling decoder frames past `totalSamplesRead == numSamples` to flush codec lookahead (ALAC/AAC/MP3/Vorbis/WavPack); once that crossed, `size_t samplesNeeded = numSamples - totalSamplesRead` underflowed to a huge value, defeating the `std::min()` clamps at the copy sites and letting `memcpy_audio()` write past the end of the exactly-sized output buffer. Fixed by clamping `samplesNeeded` to 0 once the caller's buffer is already full; excess decoded samples still route to the FIFO. `configureSinkPCM()` now returns `bool` instead of throwing `std::runtime_error` through `open()` on total negotiation failure (a track fails cleanly now instead of crashing the process), and negotiates in source-depth-aware order (see Bit Depth Handling above, which this PR is the source of) instead of the old flat 32→24→16. Also: `getNewStream()` (the SDK's real-time worker thread) no longer touches iostreams for underrun/rebuffering-complete logging — it raises bits in a new atomic mailbox (`m_rtEvents`, `alignas(64)`, cache-line separated from the fields the worker writes every call) that the decode thread drains via `logRtEvents()`; PCM ring pushes are now whole-frame only (closes a latent `getFreeSpace()` floor-division edge case, unreachable in practice behind the 50% flow-control throttle); the S24 alignment hint now survives `clear()` (only `resize()` forgets it), with the timeout/fallback default flipped from LSB to MSB (see S24 Alignment above); `TimestampedStreambuf` now flushes to the stdio buffer on `std::endl` (`sync()` override) — under systemd stdout is a fully-buffered socket, so log lines previously surfaced only once 4KB had piled up, or never at all for a quiet renderer.

- [x] HTTP prefetch thread, quieter host, FLAC bypass, decode test harness (v2.5.16, PR #96, herisson-88; the prefetch thread's shutdown-safety specifically verified by tracing the interrupt-callback wiring end to end, not just confirming the callback exists). New `PrefetchReader` (`src/PrefetchReader.cpp/h`, own mutex+condvar byte ring — not the lock-free `DirettaRingBuffer`, since it isn't on the hot path): every HTTP source except the Audirvana raw-PCM wrapper and the built-in DFF parser is now read ahead by a dedicated `SCHED_OTHER` thread on the `--cpu-other` cores via `avio_read_partial()`, up to 4MB ahead of the demuxer through a custom `AVIOContext` — the decode thread no longer blocks on the network. Its `stop()` does a bare untimed `m_thread.join()`, yet is still bounded: the inner HTTP context's own interrupt callback checks an unconditional `m_stopRequested` flag first (FFmpeg polls its interrupt callback roughly every 100ms while blocked), before chaining to the decode thread's existing stall-deadline callback — and that deadline stays armed for the whole `av_read_frame()` call even when the call is blocked indirectly inside the prefetch ring's own wait, so a genuine network stall while the prefetch thread holds the blocking read is still caught by the existing 5s stall timeout and v2.5.11 reconnect path. `--no-prefetch`/`NO_PREFETCH=1` restores synchronous decode-thread reads for A/B. Also in this PR: decode-thread flow control switched from a 10ms poll to hysteresis (fill to 70%, sleep until 30%, sleep duration sized from the measured drain rate — about 20 wake-ups and 5 decode bursts/s on a 0.5s ring instead of ~100 wake-ups); `--quiet` now genuinely discards stdout (previously still wrote ~250 raw `std::cout` per track to journald even in quiet mode); stable heap via `mallopt(M_MMAP_THRESHOLD=32MB, M_TRIM_THRESHOLD=-1, M_TOP_PAD=1MB)` ahead of `mlockall`; the empty 1Hz "UPnP Thread" removed (it never did anything but sleep — a leftover placeholder, not load-bearing); FLAC now takes the bit-perfect bypass path (decided by the decoder's actual output format, not the container — ALAC/WavPack stay on the resampler, planar); a SIGTERM-during-shutdown fix (the old handler called `stop()` and `exit()` from inside the handler itself, and `exit()` destroyed objects the interrupted main thread was still using — could hang until systemd's 45s timeout); the dead, unbuilt `src/sync/` copy of `DirettaSync`/`DirettaRingBuffer` (drifted from `src/`) removed. New `make test-decode` / `tests/decode_suite.sh` / `tests/bitexact_suite.sh` harness decodes any URL through `AudioDecoder` standalone (no SDK, no Diretta target needed) for regression testing — used to verify the seek fixes above.

- [x] `install.sh` FFmpeg self-test now catches the `_planar` DSD decoder gap (v2.5.17). `test_ffmpeg_installation()` only checked `dsd_lsbf`/`dsd_msbf`, not the `_planar` variants FFmpeg's `dsf` demuxer actually requests for a real `.dsf` file (DSF is always planar) — a build with only the base decoders (e.g. Fedora's `ffmpeg-free-devel`) passed the check while failing every real DSD file with "Codec not found". Root-caused live: Dominique's own from-source FFmpeg (full DSD support, same `install.sh`) had been silently overwritten by the distro package after running `slim2Diretta/install.sh`'s codec menu instead (that script only ever installed the distro package — see the slim2Diretta CHANGELOG v1.4.23 for the matching fix there). Now checks all four decoders.

- [x] `is_MSmode()` SDK-version guard (v2.5.18, reported by Didier/ds21 building v2.5.17 fresh on Fedora). `logNegotiatedProfile()` (new in v2.5.16/PR #94) called `Sync::is_MSmode()` unconditionally; present in the SDK 149 tree this project's own dev/test machine has, absent from the specific SDK 149 sub-revision `setup.sh` fetched for Didier — "SDK 149" is not one fixed snapshot, and this getter apparently isn't in every sub-revision of it. Same class of problem `connect()` already had between SDK 149/150 (fixed in v2.5.16 via `SdkHasRapidStart<S>`), just not covered by a guard at the time this newer call was added. Fixed identically: `SdkHasMSmode<S>`/`sdkMsMode()` resolve at compile time via SFINAE/`if constexpr`, logging `msMode=-1` instead of failing the build when the SDK doesn't expose it. Verified by compiling against both local SDK 149 and 150, plus an isolated standalone test reproducing the exact "SDK without `is_MSmode()`" case.

- [x] `install.sh` `get_libdir()` / Makefile FFmpeg-version-detection fix (v2.5.19, reported by simonhiggs on Fedora 44 aarch64/Raspberry Pi). Two compounding bugs, both in build tooling, not the renderer itself: (1) `get_libdir()` routed to `/usr/lib64` only when `uname -m = x86_64`, but Fedora/RHEL use `/usr/lib64` on every 64-bit arch they ship (aarch64, ppc64le too) — on aarch64 the freshly-built FFmpeg installed to `/usr/lib`, and `pkg-config` (which only searches `/usr/lib64/pkgconfig` on Fedora) silently failed to find `libavformat.pc`; (2) the Makefile's `FFMPEG_LIB_VERSION` detection used `||` between piped shell commands, which only tests `cut`'s own exit status (0 even on empty input from a failed `pkg-config`), so the `ldconfig` fallback never actually fired and the result was an empty string instead of `"unknown"` — which the mismatch guard didn't treat the same way, so "couldn't detect the version" was reported as a genuine version disagreement and aborted the build, even though the FFmpeg that had just been built and verified (all four DSD decoders present) was correct. Fixed at both layers: `get_libdir()` now checks only for `/usr/lib64`'s existence (Debian/Ubuntu, without a real `/usr/lib64` on any arch, fall through to `/usr/lib` unchanged); the Makefile explicitly tests for empty output instead of relying on pipe exit-status propagation, and the mismatch guard treats empty the same as `unknown` on both the header and library side. `make FFMPEG_IGNORE_MISMATCH=1` was already available as an immediate workaround and remains so. Same `get_libdir()` fix ported to slim2Diretta's `install.sh` (no Makefile-equivalent guard there to also fix — its CMake build doesn't have this heuristic).

### Potential Future Work
- [ ] AVX-512 format conversions (currently only memcpy uses AVX-512)
- [ ] Multi-producer ring buffer for multiple audio sources
- [ ] Adaptive prefetch tuning based on cache behavior

## Format Transition Handling

| From | To | Handling | Delay |
|------|-----|----------|-------|
| PCM | Same PCM (same rate) | Quick resume (buffer clear) | None |
| PCM | PCM (rate change) | Full `close()` + fresh `open()` | 200ms |
| PCM | DSD | `reopenForFormatChange()` | 800ms |
| DSD | Same DSD (same rate) | Quick resume (buffer clear) | None |
| DSD | DSD (rate change) | Full `close()` + fresh `open()` | 400ms |
| **DSD** | **PCM** | **Full `close()` + fresh `open()`** | **800ms** |

**Pre-transition silence:** Before stopping DSD playback, `sendPreTransitionSilence()` sends rate-scaled silence buffers (100 × rate_multiplier) to flush the Diretta pipeline.

## Troubleshooting

| Symptom | Likely Cause | Check |
|---------|--------------|-------|
| No audio | Target not running | `--list-targets` |
| Dropouts | Buffer underrun | Increase buffer, check network |
| Pink noise (DSD) | Bit reversal wrong | Check DSF vs DFF detection |
| Gapless gaps | Format change | Expected for sample rate changes |
| DSD→PCM clicks | I2S target sensitivity | See `PLAN-DSD-PCM-TRANSITION.md` |
| Target stuck after playlist | Old bug (fixed) | `trackEndCallback` now closes connection |

## Key Constraints

1. **No commercial use** - Diretta SDK is personal use only
2. **Linux first-class** — DRUP / slim2Diretta currently only build and run on Linux. The SDK *ships* Windows libraries (`x64-win`, `w32-win`, `arm64-win`, see SDK Library Variants table) but the SDK's own `memo_host.txt` explicitly states *« It only runs on Linux »* — so their official status is unclear. A native Windows port of DRUP/S2D is technically feasible (same C++ API, FFmpeg/libupnp are portable, AVX2 intrinsics portable, `pthread`/`SCHED_FIFO`/`mlockall`/`systemd` to be replaced with their Windows equivalents — MMCSS, `VirtualLock`, Windows Service) but should be **preceded by clarification with Yu Harada** on whether the Windows libs are supported. macOS is not in scope (no SDK).
3. **Root required** - Network operations need elevated privileges
4. **Jumbo frames recommended** - 9000+ MTU for hi-res audio

## Working with This Codebase

When modifying this codebase:

1. **Check if hot path** - `DirettaRingBuffer`, `sendAudio()`, `getNewStream()` need extra scrutiny
2. **Test with DSD** - DSD is more timing-sensitive than PCM
3. **Verify lock-free** - No mutex in audio path
4. **Check alignment** - New buffers should be `alignas(64)` if atomics are involved
5. **Test format transitions** - PCM↔DSD transitions are most problematic

## Reference Documents

| Document | Purpose |
|----------|---------|
| `docs/PCM_FIFO_BYPASS_OPTIMIZATION.md` | PCM FIFO, bypass mode, S24 detection |
| `docs/DSD_CONVERSION_OPTIMIZATION.md` | DSD conversion specialization (4 modes) |
| `docs/DSD_BUFFER_OPTIMIZATION.md` | DSD buffer pre-allocation, rate-adaptive chunks |
| `docs/PCM_OPTIMIZATION_CHANGES.md` | Low-latency PCM optimizations, buffer tuning |
| `docs/SIMD_OPTIMIZATION_CHANGES.md` | AVX2/AVX-512 SIMD, lock-free patterns |
| `docs/FORK_CHANGES.md` | Detailed diff from original v1.2.1 |
| `docs/plans/` | Design documents for each optimization |
| `CHANGELOG.md` | Chronological change history |
| `README.md` | User documentation |
| `docs/TROUBLESHOOTING.md` | User troubleshooting guide |
| `docs/CONFIGURATION.md` | Configuration reference |

## Credits

- Original DirettaRendererUPnP by Dominique COMET (cometdom)
- Diretta Host SDK by Yu Harada

### Key Contributors

- **SwissMountainsBear** - Ported and adapted the core Diretta integration code from his [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin). The `DIRETTA::Sync` architecture, `getNewStream()` callback, same-format fast path, and buffer management were directly contributed from his plugin.

- **leeeanh** - Brilliant optimization strategies that made v2.0 a true low-latency solution:
  - Lock-free SPSC ring buffer with atomic operations
  - Power-of-2 bitmask modulo (10-20× faster)
  - Cache-line separation (`alignas(64)`)
  - Zero heap allocation hot path
  - AVX2 SIMD batch conversions

- Claude Code for refactoring assistance
