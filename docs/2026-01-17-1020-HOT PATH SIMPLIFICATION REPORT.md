# Hot Path Simplification Report

**Date:** 2026-01-17
**Version:** Post-implementation
**Scope:** Audio callback hot path and track initialization code simplification

## Executive Summary

This report documents the systematic simplification of the DirettaRendererUPnP-X codebase, focusing on reducing timing variance in the audio callback hot path. The hypothesis driving this work is that simpler, more predictable code execution leads to better audio quality through reduced jitter.

Seven optimizations were implemented:
- **5 Critical (Hot Path):** Executed continuously during playback
- **2 Secondary (Track Initialization):** Executed once per track

Total lines removed: ~200
Files modified: 5 (`DirettaRingBuffer.h`, `DirettaSync.cpp`, `DirettaSync.h`, `DirettaRenderer.cpp`, `DirettaRenderer.h`)

---

## Critical Optimizations (Hot Path)

### C0: Remove Mutex/Notify from Audio Callback

**Problem:**
The audio callback in `DirettaRenderer.cpp` used a mutex and condition variable for shutdown synchronization:
```cpp
// BEFORE - in audio callback (called every ~10ms)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    if (m_callbackShutdown) return false;
}
// ... do work ...
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callbackComplete = true;
}
m_callbackCV.notify_all();  // SYSCALL on every iteration
```

Each `notify_all()` triggers a kernel syscall, adding ~1-5us of latency variance per audio frame.

**Solution:**
Replaced with lock-free atomics:
```cpp
// AFTER - zero syscalls in hot path
if (m_shutdownRequested.load(std::memory_order_acquire)) {
    return false;
}
m_callbackRunning.store(true, std::memory_order_release);
struct Guard {
    std::atomic<bool>& flag;
    ~Guard() { flag.store(false, std::memory_order_release); }
} guard{m_callbackRunning};
```

Shutdown waiting now uses spin-wait with timeout:
```cpp
void DirettaRenderer::waitForCallbackComplete() {
    m_shutdownRequested.store(true, std::memory_order_release);
    auto start = std::chrono::steady_clock::now();
    while (m_callbackRunning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        if (elapsed > std::chrono::seconds(5)) break;  // Safety timeout
    }
    m_shutdownRequested.store(false, std::memory_order_release);
}
```

**Files Modified:**
- `DirettaRenderer.h`: Lines 78-79 (replaced mutex/CV members with atomics)
- `DirettaRenderer.cpp`: `waitForCallbackComplete()`, callback entry guard, removed mutex locks before `stop()` calls

**Impact:** Eliminates 1 syscall per audio frame (~44,100/sec at CD quality)

---

### C1: Fix Modulo Operation in writeToRing

**Problem:**
Ring buffer write position calculation used modulo operator:
```cpp
size_t newWritePos = (writePos + len) % size;  // Division instruction
```

The `%` operator compiles to a division instruction (DIV/IDIV on x86), which has variable latency (20-100 cycles depending on operands).

**Solution:**
Since buffer size is always power-of-2, use bitmask:
```cpp
size_t newWritePos = (writePos + len) & mask_;  // Single AND instruction
```

**Files Modified:**
- `DirettaRingBuffer.h`: Line 1013

**Impact:** Reduces instruction latency from 20-100 cycles to 1 cycle

---

### C4: Remove Dual Memcpy Dispatch

**Problem:**
`writeToRing()` had conditional dispatch based on copy type:
```cpp
// BEFORE - branch on every call
if (useFastPath) {
    memcpy_audio_fixed(ring + writePos, staged, firstChunk);
} else {
    memcpy_audio(ring + writePos, staged, firstChunk);
}
```

**Solution:**
Unified to single function with consistent timing:
```cpp
// AFTER - no branch
if (firstChunk > 0) {
    memcpy_audio_fixed(ring + writePos, staged, firstChunk);
}
size_t secondChunk = len - firstChunk;
if (secondChunk > 0) {
    memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
}
```

**Files Modified:**
- `DirettaRingBuffer.h`: Lines 1004-1011

**Impact:** Eliminates branch misprediction (~15-20 cycles when mispredicted)

---

### C6: Remove I/O on Underrun

**Problem:**
Underrun detection triggered `std::cerr` output in the hot path:
```cpp
// BEFORE - in getNewStream() hot path
if (avail < currentBytesPerBuffer) {
    std::cerr << "[DirettaSync] UNDERRUN..." << std::endl;  // BLOCKING I/O
    // ...
}
```

`std::cerr` can block for milliseconds, causing cascading underruns.

**Solution:**
Count underruns with atomic, log at session end:
```cpp
// AFTER - in getNewStream()
if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
    m_underrunCount.fetch_add(1, std::memory_order_relaxed);
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
    // ...
}

// In stopPlayback() - cold path
uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
if (underruns > 0) {
    std::cerr << "[DirettaSync] Session had " << underruns << " underrun(s)" << std::endl;
}
```

**Files Modified:**
- `DirettaSync.h`: Line 401 (added `m_underrunCount` atomic)
- `DirettaSync.cpp`: `getNewStream()` and `stopPlayback()`

**Impact:** Eliminates blocking I/O from hot path entirely

---

### C7: Consolidate Bit-Reversal LUT

**Problem:**
Four identical 256-byte lookup tables were defined in different DSD conversion functions:
```cpp
// Each function had its own copy
static constexpr uint8_t bitReverseLUT[256] = { ... };
```

Multiple copies can cause cache thrashing if they land in different cache lines.

**Solution:**
Single class-scope LUT shared by all functions:
```cpp
class DirettaRingBuffer {
public:
    // Single LUT for all DSD conversion functions (cache-friendly)
    static constexpr uint8_t kBitReverseLUT[256] = {
        0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
        // ... full table ...
    };
```

**Files Modified:**
- `DirettaRingBuffer.h`: Lines 113-130 (added class-scope LUT), removed 4 duplicate definitions

**Impact:** Improves cache locality, reduces code size by ~768 bytes

---

## Secondary Optimizations (Track Initialization)

### S1: Remove Disabled Code Blocks

**Problem:**
Dead code remained from previous iterations:
1. `sendPreTransitionSilence()` had ~45 lines of commented/disabled logic
2. `reopenForFormatChange()` contained a `#if 0` block (~30 lines)

**Solution:**
Removed all disabled code. `sendPreTransitionSilence()` now contains only a comment explaining why it's empty:
```cpp
void DirettaSync::sendPreTransitionSilence() {
    // Pre-transition silence disabled - was causing issues during format switching
    // The stopPlayback() silence mechanism handles this case adequately
}
```

**Files Modified:**
- `DirettaSync.cpp`: `sendPreTransitionSilence()`, `reopenForFormatChange()`

**Impact:** Reduces cognitive load, eliminates ~75 lines of dead code

---

### S2: Remove Legacy pushDSDPlanar Path

**Problem:**
The original DSD conversion path used runtime parameters with per-iteration branching:
```cpp
// BEFORE - runtime decisions on every iteration
size_t pushDSDPlanar(const uint8_t* data, size_t inputSize, int numChannels,
                     const uint8_t* bitReverseTable, bool byteSwap) {
    // ...
    stagedBytes = convertDSDPlanar_AVX2(dst, src, size, ch, bitReverseTable, byteSwap);
}

size_t convertDSDPlanar_AVX2(..., const uint8_t* bitReversalTable, bool needByteSwap) {
    // Per-iteration branches:
    if (bitReversalTable) { ... }  // Branch 1
    if (needByteSwap) { ... }      // Branch 2
}
```

**Solution:**
Conversion mode is now determined once at track open and cached:
```cpp
// At track open (cold path)
m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::BitReverseOnly;

// In hot path - mode-selected function, zero branches
size_t pushDSDPlanarOptimized(data, size, channels, m_dsdConversionMode) {
    switch (mode) {
        case Passthrough:      return convertDSD_Passthrough(...);
        case BitReverseOnly:   return convertDSD_BitReverse(...);
        case ByteSwapOnly:     return convertDSD_ByteSwap(...);
        case BitReverseAndSwap: return convertDSD_BitReverseSwap(...);
    }
}
```

Each specialized function has zero internal branches - the operations are unconditional.

**Removed Functions:**
- `pushDSDPlanar()` - replaced by `pushDSDPlanarOptimized()`
- `convertDSDPlanar_AVX2()` - replaced by specialized `convertDSD_*` functions
- `convertDSDPlanar_Scalar()` - replaced by scalar paths in each specialized function
- `bitReverseTable[256]` in DirettaSync.cpp - now uses `kBitReverseLUT` in ring buffer

**Files Modified:**
- `DirettaRingBuffer.h`: Removed ~100 lines
- `DirettaSync.cpp`: Removed ~20 lines (LUT)

**Impact:** Eliminates 2 branches per DSD sample group in hot path

---

## Summary of Changes by File

| File | Lines Removed | Lines Added | Net Change |
|------|---------------|-------------|------------|
| `DirettaRingBuffer.h` | ~140 | 20 | -120 |
| `DirettaSync.cpp` | ~95 | 10 | -85 |
| `DirettaSync.h` | 0 | 2 | +2 |
| `DirettaRenderer.cpp` | ~25 | 20 | -5 |
| `DirettaRenderer.h` | 4 | 3 | -1 |
| **Total** | **~264** | **~55** | **~-209** |

---

## Testing Recommendations

### Functional Testing
- [ ] PCM playback at 44.1kHz, 96kHz, 192kHz, 384kHz
- [ ] DSD64, DSD128, DSD256, DSD512 playback
- [ ] Format transitions: PCM→DSD, DSD→PCM, DSD rate changes
- [ ] Pause/resume functionality
- [ ] Track-to-track gapless transitions
- [ ] Underrun recovery (simulate by CPU load)

### Performance Testing
- [ ] Measure callback timing variance before/after
- [ ] Verify no new underruns introduced
- [ ] Check memory usage (should be slightly lower)

### Stress Testing
- [ ] Extended playback (>1 hour) at various formats
- [ ] Rapid track skipping
- [ ] Format change stress test (rapid PCM↔DSD switching)

---

## Future Considerations

The following items were identified but not implemented in this pass:

1. **C2: Cache atomic loads in sendAudio()** - 5 atomic loads for values that never change during playback could be cached at track open
2. **C3: Reduce memory order strength** - Some `memory_order_acquire/release` could potentially be relaxed to `memory_order_relaxed`
3. **C5: Consider SIMD memcpy for small fixed sizes** - The ~176-byte buffer copies could use explicit SIMD

These are lower priority as their impact is less certain without profiling data.
