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
| `src/UPnPDevice.cpp/hpp` | UPnP/DLNA protocol, SSDP discovery, HTTP server | No |
| `src/ProtocolInfoBuilder.h` | UPnP protocol info generation | No |
| `src/main.cpp` | CLI parsing, initialization, signal handling | No |
| `src/memcpyfast_audio.h` | AVX2/AVX-512 optimized memcpy dispatcher | **Critical** |
| `src/fastmemcpy-avx.c` | C AVX implementation (x86 only) | **Critical** |
| `src/LogLevel.h` | Centralized log level system (ERROR/WARN/INFO/DEBUG) | No |
| `src/test_audio_memory.cpp` | 20 unit tests for DirettaRingBuffer | No |

## Diretta SDK Reference

**SDK Location:** auto-detected by the Makefile from `$HOME / . / .. / /opt` (latest `DirettaHostSDK_<version>/` directory wins, via `sort -V | tail -1`). Override with `DIRETTA_SDK_PATH=...`. Current SDK at the time of writing: v1.49.x.

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

`configureSinkPCM()` negotiates the PCM format with the Diretta sink based on the source bit depth (`inputBits`):
- **16-bit and 24-bit sources**: Only negotiate up to 24-bit. Prevents silence/noise on DACs that report 32-bit support at the Diretta target level but are physically limited to 24-bit.
- **32-bit sources**: Try 32-bit first, fall back to 24-bit if the sink doesn't support it.

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

### S24 Format Auto-Detection

The ring buffer auto-detects 24-bit sample alignment on first push:
- **LSB-aligned**: bytes 0-2 contain data (standard S24_LE) → `convert24BitPacked_AVX2()`
- **MSB-aligned**: bytes 1-3 contain data (S24_32BE-style) → `convert24BitPackedShifted_AVX2()`

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
- [x] Bit depth negotiation fix — only offer 32-bit when source is 32-bit
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
