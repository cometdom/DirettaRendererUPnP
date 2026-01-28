# DSD Buffer Optimization

This document describes the DSD buffer optimizations implemented on 2026-01-14, addressing two performance issues in the DSD audio processing path.

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Technical Background](#technical-background)
3. [Solution Overview](#solution-overview)
4. [Implementation Details](#implementation-details)
5. [Performance Analysis](#performance-analysis)
6. [Files Modified](#files-modified)
7. [Testing Considerations](#testing-considerations)

---

## Problem Statement

### Issue 1: Heap Allocations in Hot Path

The DSD reading path in `AudioEngine::readSamples()` created two `std::vector<uint8_t>` objects on every call:

```cpp
// Previous implementation (problematic)
std::vector<uint8_t> leftData;
std::vector<uint8_t> rightData;
leftData.reserve(bytesPerChannelNeeded);
rightData.reserve(bytesPerChannelNeeded);
```

This violated the "zero heap allocations in hot path" design principle established for PCM processing, where reusable buffers (`m_packet`, `m_frame`, `m_resampleBuffer`) are used instead.

**Impact:**
- Memory allocator pressure on every audio chunk
- Potential heap fragmentation over long playback sessions
- Unpredictable latency spikes from allocator contention
- Cache pollution from newly allocated memory

### Issue 2: Fixed Chunk Size Regardless of DSD Rate

The `samplesPerCall` value was hardcoded at 32768 for all DSD rates:

```cpp
// Previous implementation (problematic)
size_t samplesPerCall = isDSD ? 32768 : 2048;
```

DSD sample rates vary dramatically:
- DSD64: 2,822,400 Hz
- DSD128: 5,644,800 Hz
- DSD256: 11,289,600 Hz
- DSD512: 22,579,200 Hz
- DSD1024: 45,158,400 Hz

A fixed 32768 samples produces wildly different chunk durations:

| DSD Rate | Chunk Duration |
|----------|----------------|
| DSD64    | ~11.6 ms       |
| DSD128   | ~5.8 ms        |
| DSD256   | ~2.9 ms        |
| DSD512   | ~1.45 ms       |
| DSD1024  | ~0.73 ms       |

**Impact:**
- At DSD512/1024, the audio thread loops 8-16x more frequently than necessary
- Increased context switching and CPU wake-ups
- Higher power consumption
- More frequent buffer level checks causing potential oscillation

---

## Technical Background

### DSD Data Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ AudioDecoder::readSamples() [DSD native mode]                   │
│                                                                 │
│   1. Calculate bytes needed: (numSamples * channels) / 8        │
│   2. Read FFmpeg packets (DSF: [L_block][R_block] per packet)   │
│   3. Accumulate L/R channel data separately                     │
│   4. Output planar format: [all_L_bytes][all_R_bytes]           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ DirettaSync::sendAudio()                                        │
│                                                                 │
│   1. Convert samples to bytes                                   │
│   2. Call m_ringBuffer.pushDSDPlanar()                          │
│   3. Interleave channels, apply bit reversal/byte swap          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ DirettaRingBuffer → getNewStream() → Diretta Target             │
└─────────────────────────────────────────────────────────────────┘
```

### DSD Sample Rate Calculation

DSD is a 1-bit format. The "sample rate" represents the bit rate per channel:
- DSD64 = 64 × 44,100 = 2,822,400 bits/second/channel
- DSD128 = 128 × 44,100 = 5,644,800 bits/second/channel
- etc.

For stereo, bytes per second = (sample_rate × 2 channels) / 8 bits

### Why PCM Didn't Have This Problem

PCM already used pre-allocated buffers:
- `m_packet` - Reusable AVPacket for reading
- `m_frame` - Reusable AVFrame for decoding
- `m_resampleBuffer` - Reusable buffer for resampling output

PCM's `samplesPerCall` (2048) was tuned for ~46ms at 44.1kHz, providing consistent scheduling granularity across common sample rates (44.1-192kHz vary only 4x, not 16x like DSD).

---

## Solution Overview

### Part A: Pre-allocated DSD Buffers

Replace per-call `std::vector` allocation with persistent `AudioBuffer` members that resize only when capacity is insufficient.

**Before:**
```cpp
std::vector<uint8_t> leftData;      // Allocated every call
std::vector<uint8_t> rightData;     // Allocated every call
leftData.insert(...);               // Potential reallocation
```

**After:**
```cpp
// Members persist across calls
AudioBuffer m_dsdLeftBuffer;
AudioBuffer m_dsdRightBuffer;
size_t m_dsdBufferCapacity = 0;

// In readSamples():
if (m_dsdBufferCapacity < bytesPerChannelNeeded) {
    m_dsdLeftBuffer.resize(bytesPerChannelNeeded);   // Rare
    m_dsdRightBuffer.resize(bytesPerChannelNeeded);  // Rare
    m_dsdBufferCapacity = bytesPerChannelNeeded;
}
uint8_t* leftData = m_dsdLeftBuffer.data();          // Zero allocation
memcpy(leftData + leftOffset, source, size);         // Direct copy
```

### Part B: Rate-Adaptive Chunk Sizing

Calculate `samplesPerCall` dynamically to maintain consistent ~12ms chunk duration regardless of DSD rate.

**Before:**
```cpp
size_t samplesPerCall = isDSD ? 32768 : 2048;  // Fixed
```

**After:**
```cpp
size_t samplesPerCall;
if (isDSD) {
    samplesPerCall = DirettaBuffer::calculateDsdSamplesPerCall(sampleRate);
} else {
    samplesPerCall = 2048;
}
```

The calculation targets 12ms chunks with alignment and clamping:

```cpp
inline size_t calculateDsdSamplesPerCall(uint32_t dsdSampleRate) {
    constexpr double TARGET_CHUNK_MS = 12.0;
    constexpr size_t MIN_DSD_SAMPLES = 8192;    // Floor: ~3ms at DSD64
    constexpr size_t MAX_DSD_SAMPLES = 131072;  // Ceiling: prevents huge buffers

    size_t samplesPerCall = static_cast<size_t>(dsdSampleRate * TARGET_CHUNK_MS / 1000.0);
    samplesPerCall = ((samplesPerCall + 255) / 256) * 256;  // Align to 256
    samplesPerCall = std::max(samplesPerCall, MIN_DSD_SAMPLES);
    samplesPerCall = std::min(samplesPerCall, MAX_DSD_SAMPLES);

    return samplesPerCall;
}
```

---

## Implementation Details

### AudioEngine.h Changes

Added three new members to `AudioDecoder` class:

```cpp
// Pre-allocated DSD channel buffers (eliminates per-call std::vector allocation)
AudioBuffer m_dsdLeftBuffer;
AudioBuffer m_dsdRightBuffer;
size_t m_dsdBufferCapacity = 0;
```

These follow the same pattern as the existing `m_resampleBuffer` / `m_resampleBufferCapacity`.

### AudioEngine.cpp Changes

#### Buffer Initialization (lines 552-563)

```cpp
// Ensure pre-allocated DSD buffers are large enough (resize only if capacity insufficient)
if (m_dsdBufferCapacity < bytesPerChannelNeeded) {
    m_dsdLeftBuffer.resize(bytesPerChannelNeeded);
    m_dsdRightBuffer.resize(bytesPerChannelNeeded);
    m_dsdBufferCapacity = bytesPerChannelNeeded;
}

// Use offset tracking instead of vector operations (zero allocations)
size_t leftOffset = 0;
size_t rightOffset = 0;
uint8_t* leftData = m_dsdLeftBuffer.data();
uint8_t* rightData = m_dsdRightBuffer.data();
```

#### Data Accumulation (lines 575-578, 621-624)

Replaced `vector.insert()` with `memcpy()` + offset increment:

```cpp
// Before: leftData.insert(leftData.end(), source, source + size);
// After:
memcpy(leftData + leftOffset, source, size);
leftOffset += size;
```

#### Loop Condition (line 596)

```cpp
// Before: while (leftData.size() < bytesPerChannelNeeded && !m_eof)
// After:
while (leftOffset < bytesPerChannelNeeded && !m_eof)
```

#### Output Assembly (lines 655, 659-660)

```cpp
// Before: size_t actualPerCh = std::min(leftData.size(), rightData.size());
// After:
size_t actualPerCh = std::min(leftOffset, rightOffset);

// Before: memcpy_audio(buffer.data(), leftData.data(), actualPerCh);
// After:
memcpy_audio(buffer.data(), leftData, actualPerCh);  // leftData is now uint8_t*
```

#### Cleanup in close() (line 534)

```cpp
m_dsdBufferCapacity = 0;  // Reset DSD buffer capacity tracking
```

### DirettaSync.h Changes

Added `calculateDsdSamplesPerCall()` to the `DirettaBuffer` namespace (lines 109-132):

```cpp
// Calculate DSD samples per call based on rate
// Target: ~10-12ms chunks for consistent scheduling granularity
// Returns DSD samples (1-bit), which convert to bytes via: bytes = samples * channels / 8
inline size_t calculateDsdSamplesPerCall(uint32_t dsdSampleRate) {
    constexpr double TARGET_CHUNK_MS = 12.0;
    constexpr size_t MIN_DSD_SAMPLES = 8192;
    constexpr size_t MAX_DSD_SAMPLES = 131072;

    size_t samplesPerCall = static_cast<size_t>(dsdSampleRate * TARGET_CHUNK_MS / 1000.0);
    samplesPerCall = ((samplesPerCall + 255) / 256) * 256;
    samplesPerCall = std::max(samplesPerCall, MIN_DSD_SAMPLES);
    samplesPerCall = std::min(samplesPerCall, MAX_DSD_SAMPLES);

    return samplesPerCall;
}
```

### DirettaRenderer.cpp Changes

Updated audio thread loop (lines 567-575):

```cpp
// Adjust samples per call based on format
// PCM: 2048 samples = ~46ms at 44.1kHz
// DSD: Rate-adaptive for consistent ~12ms chunks
size_t samplesPerCall;
if (isDSD) {
    samplesPerCall = DirettaBuffer::calculateDsdSamplesPerCall(sampleRate);
} else {
    samplesPerCall = 2048;
}
```

---

## Performance Analysis

### Chunk Timing Comparison

| DSD Rate | Sample Rate (Hz) | Before (32768) | After (calculated) | Improvement |
|----------|------------------|----------------|--------------------|-------------|
| DSD64    | 2,822,400        | 11.6 ms        | 12.1 ms (34,048)   | Similar |
| DSD128   | 5,644,800        | 5.8 ms         | 12.0 ms (67,840)   | 2x fewer loops |
| DSD256   | 11,289,600       | 2.9 ms         | 11.6 ms (131,072)  | 4x fewer loops |
| DSD512   | 22,579,200       | 1.45 ms        | 5.8 ms (131,072)   | 4x fewer loops |
| DSD1024  | 45,158,400       | 0.73 ms        | 2.9 ms (131,072)   | 4x fewer loops |

Note: DSD256+ are clamped at MAX_DSD_SAMPLES (131,072) to prevent excessive buffer sizes.

### Memory Allocation Comparison

| Metric | Before | After |
|--------|--------|-------|
| Allocations per readSamples() | 2 (std::vector constructor) | 0 (steady state) |
| Potential reallocations | Multiple (vector::insert) | 0 (pre-sized) |
| Memory pattern | Alloc → use → free (every call) | Alloc once → reuse |
| Allocator pressure | High (thousands/second at DSD512) | Near zero |

### CPU Impact

| Metric | Before | After |
|--------|--------|-------|
| Loop iterations at DSD512 | ~690/sec | ~172/sec |
| Loop iterations at DSD1024 | ~1370/sec | ~345/sec |
| Buffer level checks | Proportional to iterations | 4x fewer |
| Context switch opportunities | More frequent | Reduced |

### Calculated Values by Rate

| DSD Rate | Raw Calculation | After Alignment | After Clamping | Bytes (stereo) |
|----------|-----------------|-----------------|----------------|----------------|
| DSD64    | 33,869          | 34,048          | 34,048         | 8,512          |
| DSD128   | 67,738          | 67,840          | 67,840         | 16,960         |
| DSD256   | 135,475         | 135,680         | 131,072        | 32,768         |
| DSD512   | 270,950         | 271,104         | 131,072        | 32,768         |
| DSD1024  | 541,901         | 542,208         | 131,072        | 32,768         |

---

## Files Modified

| File | Lines Changed | Description |
|------|---------------|-------------|
| `src/AudioEngine.h` | 141-144 | Added `m_dsdLeftBuffer`, `m_dsdRightBuffer`, `m_dsdBufferCapacity` |
| `src/AudioEngine.cpp` | 552-661 | Replaced vectors with pre-allocated buffers and offset tracking |
| `src/AudioEngine.cpp` | 534 | Added capacity reset in `close()` |
| `src/DirettaSync.h` | 109-132 | Added `calculateDsdSamplesPerCall()` function |
| `src/DirettaRenderer.cpp` | 567-575 | Updated to use rate-adaptive calculation |

---

## Testing Considerations

### Functional Testing

1. **DSD64 playback** - Baseline, should behave similarly to before
2. **DSD128 playback** - Verify 2x larger chunks work correctly
3. **DSD256 playback** - Verify clamped chunk size works
4. **DSD512/1024 playback** - If available, verify high-rate handling
5. **Gapless DSD transitions** - Verify buffer reuse across tracks
6. **DSD→PCM transitions** - Verify buffer capacity reset works
7. **Long playback sessions** - Verify no memory growth

### Performance Testing

1. **Memory monitoring** - Verify no per-call allocations (use `valgrind` or similar)
2. **CPU usage comparison** - Compare before/after at DSD256+
3. **Latency measurement** - Verify no regression in audio latency

### Edge Cases

1. **Mono DSD files** - Rare but should work (different bytesPerChannel calculation)
2. **Very short DSD files** - Verify buffer sizing doesn't over-allocate
3. **Rapid track changes** - Verify capacity tracking handles format switches

### Build Verification

```bash
# Clean build
make clean && make ARCH_NAME=x64-linux-15v3

# Run with verbose logging
sudo ./bin/DirettaRendererUPnP --target 1 --verbose

# Observe log output for samplesPerCall values
# DSD64: ~34,048
# DSD128: ~67,840
# DSD256+: ~131,072
```

---

## Design Rationale

### Why 12ms Target?

- PCM uses ~46ms at 44.1kHz (2048 samples)
- DSD64's original 32768 samples ≈ 11.6ms
- 12ms provides similar granularity to the original DSD64 behavior
- Not too short (excessive looping) or too long (poor responsiveness)

### Why Clamp at 131,072?

- 131,072 samples = 32,768 bytes per channel (stereo)
- Matches the original fixed buffer size in bytes
- Prevents excessive memory usage at extreme rates
- Still provides 4x improvement over fixed 32768 samples at DSD512+

### Why 256-Sample Alignment?

- 256 samples = 32 bytes per channel
- Aligns with common SIMD vector widths (AVX2 = 32 bytes)
- Ensures clean boundaries for potential future SIMD optimization
- Matches existing alignment patterns in the ring buffer

### Why Not Dynamic Buffer Allocation?

Pre-allocated buffers with capacity tracking provide:
- Predictable memory usage
- Zero allocations in steady state
- Simple implementation matching existing patterns
- No risk of allocation failure during playback

---

## Conclusion

These optimizations address two long-standing inefficiencies in the DSD processing path:

1. **Memory**: Eliminated per-call heap allocations, reducing allocator pressure and improving cache behavior
2. **CPU**: Reduced loop iterations by up to 4x for high-rate DSD formats

The changes follow established patterns in the codebase (pre-allocated buffers, rate-adaptive calculations) and maintain backward compatibility with existing behavior at DSD64.
