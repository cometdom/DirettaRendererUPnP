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
| `src/PrivilegeDrop.h` | Linux privilege drop with capability retention | No |
| `src/LogLevel.h` | Centralized log level system (ERROR/WARN/INFO/DEBUG) | No |
| `src/test_audio_memory.cpp` | 20 unit tests for DirettaRingBuffer | No |

## Diretta SDK Reference

**SDK Location:** `../DirettaHostSDK_147_19/` (v1.47.19)

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

Located in `../DirettaHostSDK_147_19/lib/`:

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
- [x] Privilege drop with Linux capabilities (`--user` option)
- [x] Systemd hardening (20+ security directives, dedicated `diretta` user)
- [x] Unit tests (20 tests covering PCM, DSD, ring buffer, integration)

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
2. **Linux only** - No Windows/macOS support
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
