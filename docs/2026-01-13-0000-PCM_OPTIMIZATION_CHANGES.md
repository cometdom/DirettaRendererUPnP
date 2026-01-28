# PCM Latency and Jitter Optimization Changes

**Date:** 2026-01-12
**Version:** Post-v1.2.1 (DirettaRendererUPnP-L)
**Goal:** Minimize latency and jitter in PCM decode/playback path

---

## Summary

This update implements a comprehensive PCM optimization targeting three areas:
1. **Buffer tuning** - Reduced from ~1s to ~300ms for lower latency
2. **Flow control** - Replaced 10ms blocking sleeps with 500µs micro-sleeps
3. **Allocation elimination** - Removed per-call heap allocations in audio hot path

Additionally includes Makefile enhancements for Zen4 CPU optimization.

---

## Changes by File

### 1. `src/DirettaSync.h` - Buffer Constants

**Lines 76-91**

| Constant | Before | After | Impact |
|----------|--------|-------|--------|
| `PCM_BUFFER_SECONDS` | 1.0f | 0.3f | ~700ms latency reduction |
| `PCM_PREFILL_MS` | 50 | 30 | Faster playback start |
| `MIN_BUFFER_BYTES` | 3,072,000 | 65,536 | Allows 300ms buffer at all rates |

**Rationale:** 300ms buffer provides sufficient headroom for LAN jitter while significantly reducing end-to-end latency. The 64KB floor ensures minimum safety margin at low sample rates.

---

### 2. `src/DirettaSync.cpp` - DSD→PCM Transition Fix

**Lines 363-427** (inside `open()` method)

Added special handling for DSD→PCM format transitions:

```cpp
if (wasDSD && nowPCM) {
    // Full close/reopen for clean I2S transition
    stop();
    disconnect(true);
    DIRETTA::Sync::close();
    // ... worker thread shutdown ...
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    DIRETTA::Sync::open(...);
}
```

**Rationale:** I2S targets are more timing-sensitive than USB. DSD→PCM transitions require the target to switch from DSD clock to PCM clock, which needs a clean break and 800ms settling time.

---

### 3. `src/DirettaSync.cpp` - Enhanced Target Listing

**Lines 265-318** (`listTargets()` method)

Enhanced output now shows:
- Output name (differentiates I2S vs USB)
- Port numbers (IN/OUT) with multiport flag
- Configuration URL
- SDK version
- Product ID

**Before:**
```
[1] MyTarget
```

**After:**
```
[1] MyTarget
    Output: USB Audio
    Port: IN=1 OUT=2
    Config: http://192.168.1.100/config
    Version: 1.47.19
    ProductID: 0x12345678
```

---

### 4. `src/DirettaRenderer.cpp` - Hybrid Flow Control

**Lines 27-32** - Added FlowControl namespace:

```cpp
namespace FlowControl {
    constexpr int MICROSLEEP_US = 500;           // Was 10,000µs (10ms)
    constexpr int MAX_WAIT_MS = 20;              // Was 500ms (50 × 10ms)
    constexpr int MAX_RETRIES = 40;              // 40 × 500µs = 20ms max
    constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;
}
```

**Lines 258-290** - Replaced PCM send loop:

| Scenario | Before | After |
|----------|--------|-------|
| Backpressure sleep | 10ms fixed | 500µs micro-sleep |
| Max stall time | 500ms | 20ms |
| Low buffer (<10%) | Same sleep | Immediate early-return |

**Rationale:**
- Micro-sleeps reduce jitter from coarse timing
- Critical mode ensures buffer refill is prioritized when running low
- 20ms max stall prevents long audio gaps

---

### 5. `src/DirettaRenderer.cpp` - Chunk Size Reduction

**Line 543-544**

```cpp
// Before
size_t samplesPerCall = isDSD ? 32768 : 8192;

// After
size_t samplesPerCall = isDSD ? 32768 : 2048;
```

| Sample Rate | Before (8192) | After (2048) |
|-------------|---------------|--------------|
| 44.1kHz | ~186ms | ~46ms |
| 96kHz | ~85ms | ~21ms |
| 192kHz | ~43ms | ~11ms |
| 352.8kHz | ~23ms | ~5.8ms |

**Rationale:** Smaller chunks enable more responsive buffer-level flow control.

---

### 6. `src/DirettaRenderer.cpp` - UPnP Stop Handling

**Lines 419-431** (inside `onStop` callback)

```cpp
// Before: Keep connection open
m_direttaSync->stopPlayback(true);
// m_direttaSync->close();  // Was commented out

// After: Close on Stop for clean resource release
m_direttaSync->stopPlayback(true);
m_direttaSync->close();
```

**Rationale:** Closing on Stop (not Pause) ensures:
- Clean handoff when switching renderers
- Proper resource release on Diretta target
- Expected behavior for control points

---

### 7. `src/AudioEngine.h` - Reusable Members

**Lines 128-139**

Added member variables for allocation elimination:

```cpp
AVPacket* m_packet;      // Reusable for raw packet reading (DSD and PCM)
AVFrame* m_frame;        // Reusable for decoded frames (PCM)

AudioBuffer m_resampleBuffer;
size_t m_resampleBufferCapacity = 0;
```

---

### 8. `src/AudioEngine.cpp` - Allocation Elimination

**Constructor (lines 73-85):**
- Initialize `m_frame = nullptr`
- Initialize `m_resampleBufferCapacity = 0`

**close() method (lines 514-534):**
- Free `m_frame` with `av_frame_free()`
- Reset `m_resampleBufferCapacity = 0`

**readSamples() method:**

| Location | Before | After |
|----------|--------|-------|
| Lines 752-762 | `av_packet_alloc()` every call | Lazy init once, reuse |
| Lines 867-875 | `AudioBuffer tempBuffer(size)` every call | Capacity-tracked member |
| Lines 947-949 | `av_packet_free()`, `av_frame_free()` | `av_*_unref()` (no dealloc) |

**Impact:** Eliminates 3-4 heap allocations per `readSamples()` call during steady-state playback.

---

### 9. `Makefile` - Zen4 Microarchitecture Optimization

**Lines 41-48** - Improved Zen4 detection:
- Matches Ryzen 5/7/9 7000/8000/9000 series
- Matches EPYC 9004 (Genoa)
- Matches Threadripper 7000 series
- Fallback: detects `avx512vbmi2` + `vaes` features

**Lines 82-114** - Architecture-specific compiler flags:

| Variant | Compiler Flags |
|---------|---------------|
| `zen4` | `-march=znver4 -mtune=znver4` |
| `v4` | `-march=x86-64-v4 -mavx512f/bw/vl/dq` |
| `v3` | `-march=x86-64-v3 -mavx2 -mfma` |
| `v2` | `-march=x86-64-v2` |
| `aarch64` | `-mcpu=native` |

**Lines 150-165** - Updated SDK search paths:
- Added `DirettaHostSDK_147_19` (newest version first)

**Lines 299-334** - New helper targets:
- `make show-arch` - Display detected architecture and flags
- `make list-variants` - List available SDK library variants

**Lines 225-230** - Conditional C sources:
- `fastmemcpy-avx.c` only compiled on x86 (not ARM64)

---

### 10. `src/memcpyfast_audio.h` - ARM64 Compatibility

**Lines 13-17** - Architecture detection:

```cpp
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define MEMCPY_AUDIO_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define MEMCPY_AUDIO_ARM64 1
#endif
```

**Lines 19-162** - x86 implementation (wrapped in `#ifdef MEMCPY_AUDIO_X86`):
- AVX2 optimized `memcpy_audio_fixed()` with overlapping stores
- `prefetch_audio_buffer()` with cache hints
- Runtime AVX-512 detection and dispatch
- `memcpy_audio()` main dispatcher

**Lines 164-203** - ARM64/Fallback implementation:

```cpp
// Prefetch - no-op on ARM64 (compiler may auto-prefetch)
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    (void)src;
    (void)size;
}

// Uses standard memcpy - GCC/Clang auto-vectorize with NEON
static inline void* memcpy_audio(void *dst, const void *src, size_t len) {
    return std::memcpy(dst, src, len);
}

static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    std::memcpy(dst, src, size);
}
```

**Makefile integration** (lines 225-230):

```makefile
# C sources (AVX optimized memcpy - x86 only)
ifeq ($(BASE_ARCH),x64)
    C_SOURCES = $(SRCDIR)/fastmemcpy-avx.c
else
    C_SOURCES =
endif
```

| Platform | memcpy_audio() | fastmemcpy-avx.c |
|----------|----------------|------------------|
| x86_64 | AVX2/AVX-512 SIMD | Compiled |
| ARM64 | std::memcpy (NEON auto-vectorized) | Skipped |
| RISC-V | std::memcpy | Skipped |

**Rationale:** ARM64 compilers (GCC/Clang) auto-vectorize `std::memcpy` with NEON instructions, providing good performance without hand-written intrinsics. The x86-specific `fastmemcpy-avx.c` is excluded from ARM64 builds to avoid compilation errors.

---

## Performance Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| PCM buffer latency | ~1000ms | ~300ms | 70% reduction |
| Time to first audio | ~50ms prefill | ~30ms prefill | 40% faster |
| Backpressure max stall | 500ms | 20ms | 96% reduction |
| Heap allocs per decode | 3-4 | 0 (steady state) | Eliminated |
| Scheduling granularity | 186ms @44.1k | 46ms @44.1k | 4x finer |

---

## Testing Checklist

### x86_64 Testing
- [ ] PCM playback at 44.1kHz/16-bit
- [ ] PCM playback at 96kHz/24-bit
- [ ] PCM playback at 192kHz/24-bit
- [ ] PCM playback at 352.8kHz/24-bit (DXD)
- [ ] DSD64 playback
- [ ] DSD128 playback
- [ ] DSD→PCM transition (no clicks)
- [ ] PCM→DSD transition
- [ ] Gapless playback (same format)
- [ ] Track skip/seek
- [ ] Stop and restart
- [ ] Network stress test (if possible)
- [ ] Zen4 build verification (`make show-arch`)

### ARM64 Testing (Raspberry Pi 4/5, etc.)
- [ ] Build completes without errors (`make`)
- [ ] Architecture detection correct (`make show-arch`)
- [ ] PCM playback at 44.1kHz/16-bit
- [ ] PCM playback at 96kHz/24-bit
- [ ] PCM playback at 192kHz/24-bit
- [ ] DSD64 playback
- [ ] DSD→PCM transition (no clicks)
- [ ] Stop and restart

---

## Rollback

To revert to previous behavior, restore these constants in `src/DirettaSync.h`:

```cpp
constexpr float PCM_BUFFER_SECONDS = 1.0f;
constexpr size_t PCM_PREFILL_MS = 50;
constexpr size_t MIN_BUFFER_BYTES = 3072000;
```

---

## Bug Fixes

### 11. `src/DirettaRenderer.cpp` - Playlist End Target Release

**Date:** 2026-01-13

**Lines 319-335** (`trackEndCallback`)

**Bug:** When a playlist ended naturally (last track finished), the Diretta target was not released. The control point received the STOPPED notification but the target remained held, making the system appear "stuck".

**Before:**
```cpp
m_audioEngine->setTrackEndCallback([this]() {
    std::cout << "[DirettaRenderer] Track ended naturally" << std::endl;
    m_upnp->notifyStateChange("STOPPED");
});
```

**After:**
```cpp
m_audioEngine->setTrackEndCallback([this]() {
    std::cout << "[DirettaRenderer] Track ended naturally" << std::endl;

    // Close Diretta connection to release the target
    if (m_direttaSync) {
        m_direttaSync->stopPlayback(true);
        m_direttaSync->close();
    }

    m_upnp->notifyStateChange("STOPPED");
});
```

**Rationale:** The explicit `onStop` callback (from control point Stop command) properly closed the Diretta connection, but the natural track end callback did not. This caused the target to remain held indefinitely after playlist completion.

---

## Credits

- PCM optimization design and implementation with Claude Code assistance
- Based on patterns from MPD Diretta Output Plugin v0.4.0
- Diretta Host SDK by Yu Harada
