# DSD Conversion Function Specialization Design

**Date:** 2026-01-15
**Status:** Implemented
**Goal:** Eliminate per-iteration branch overhead in DSD hot path for improved performance at high DSD rates

## Implementation Summary

**Completed:** 2026-01-15

All phases implemented:
- Phase 1: `DSDConversionMode` enum added to DirettaRingBuffer.h (lines 104-110)
- Phase 2-3: Four specialized AVX2 conversion functions (lines 557-896)
- Phase 4: `pushDSDPlanarOptimized()` method with switch-case dispatch (lines 326-364)
- Phase 5: Scalar fallbacks embedded in each specialized function
- Phase 6: DirettaSync integration - mode set in `configureSinkDSD()`, used in `sendAudio()`

**Key changes:**
- `DirettaSync.cpp`: Each sink format path sets `m_dsdConversionMode`
- `DirettaSync::sendAudio()`: Uses `pushDSDPlanarOptimized()` with cached mode
- Hot loop now has zero per-iteration branches for conversion decisions

## Overview

This design implements optimizations to the DSD conversion path, analogous to the PCM bypass optimizations. The primary focus is eliminating conditional branches inside the innermost processing loop by pre-selecting specialized conversion functions at track open time.

## Current Implementation Analysis

### DSD Processing Chain

```
AudioDecoder::readSamples()     [Raw DSD packet reading]
    ↓ planar L/R separation
DirettaSync::sendAudio()        [Receives planar DSD]
    ↓
DirettaRingBuffer::pushDSDPlanar()
    ↓
convertDSDPlanar_AVX2()         [Hot path - bit reversal, byte swap, interleave]
    ↓
writeToRing()                   [Ring buffer write]
```

### Current Hot Loop (DirettaRingBuffer.h:513-537)

```cpp
for (; i + 32 <= bytesPerChannel; i += 32) {
    __m256i left = _mm256_loadu_si256(...);
    __m256i right = _mm256_loadu_si256(...);

    if (bitReversalTable) {           // Branch #1: checked every iteration
        left = simd_bit_reverse(left);
        right = simd_bit_reverse(right);
    }

    __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
    __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

    if (needByteSwap) {               // Branch #2: checked every iteration
        interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
        interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
    }

    // ... store results
}
```

### Problem Statement

1. **Per-iteration branches:** `if (bitReversalTable)` and `if (needByteSwap)` are evaluated on every 32-byte chunk
2. **Branch prediction overhead:** Even with good prediction, this adds cycles in the hottest loop
3. **DSD512/1024 impact:** At DSD512 (22.5 MHz), this loop runs ~700,000 times/second per channel
4. **Scalar tail:** Same pattern repeated in scalar fallback (lines 540-575)

### DSD Conversion Modes

| Source Format | Target Endianness | Bit Reversal | Byte Swap | Mode |
|---------------|-------------------|--------------|-----------|------|
| DSF (LSB) | LSB target | No | No | **Passthrough** |
| DSF (LSB) | MSB target | Yes | No | BitReverse |
| DFF (MSB) | LSB target | Yes | No | BitReverse |
| DFF (MSB) | MSB target | No | No | **Passthrough** |
| Any | Little-endian | Maybe | Yes | +ByteSwap |

**Key insight:** The conversion mode is determined at track open and never changes during playback.

## Optimizations

### 1. DSD Conversion Function Specialization

**Problem:** Runtime branch checks inside hot loop for operations that are constant per-track.

**Solution:** Create 4 specialized conversion functions with no internal branches:

```cpp
enum class DSDConversionMode {
    Passthrough,       // Just interleave (fastest path)
    BitReverseOnly,    // DSF→MSB or DFF→LSB
    ByteSwapOnly,      // Endianness conversion only
    BitReverseAndSwap  // Both operations
};

// Specialized functions - NO branches in hot path:
size_t convertDSD_Passthrough_AVX2(uint8_t* dst, const uint8_t* src,
                                    size_t totalBytes, int channels);
size_t convertDSD_BitReverse_AVX2(uint8_t* dst, const uint8_t* src,
                                   size_t totalBytes, int channels);
size_t convertDSD_ByteSwap_AVX2(uint8_t* dst, const uint8_t* src,
                                 size_t totalBytes, int channels);
size_t convertDSD_BitReverseSwap_AVX2(uint8_t* dst, const uint8_t* src,
                                       size_t totalBytes, int channels);
```

**Passthrough implementation (fastest):**

```cpp
size_t convertDSD_Passthrough_AVX2(uint8_t* dst, const uint8_t* src,
                                    size_t totalBytes, int channels) {
    size_t bytesPerChannel = totalBytes / channels;
    size_t outputBytes = 0;

    if (channels == 2) {
        const uint8_t* srcL = src;
        const uint8_t* srcR = src + bytesPerChannel;

        size_t i = 0;
        for (; i + 32 <= bytesPerChannel; i += 32) {
            __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
            __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

            // NO bit reversal check
            // NO byte swap check

            __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
            __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

            __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
            __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
            outputBytes += 32;
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
            outputBytes += 32;
        }

        // Scalar tail - also no branches
        for (; i + 4 <= bytesPerChannel; i += 4) {
            dst[outputBytes++] = srcL[i + 0];
            dst[outputBytes++] = srcL[i + 1];
            dst[outputBytes++] = srcL[i + 2];
            dst[outputBytes++] = srcL[i + 3];
            dst[outputBytes++] = srcR[i + 0];
            dst[outputBytes++] = srcR[i + 1];
            dst[outputBytes++] = srcR[i + 2];
            dst[outputBytes++] = srcR[i + 3];
        }
    }

    _mm256_zeroupper();
    return outputBytes;
}
```

**BitReverse implementation:**

```cpp
size_t convertDSD_BitReverse_AVX2(uint8_t* dst, const uint8_t* src,
                                   size_t totalBytes, int channels) {
    // Same structure but ALWAYS applies bit reversal - no check
    // ...
    for (; i + 32 <= bytesPerChannel; i += 32) {
        __m256i left = _mm256_loadu_si256(...);
        __m256i right = _mm256_loadu_si256(...);

        left = simd_bit_reverse(left);   // Always applied
        right = simd_bit_reverse(right); // Always applied

        // ... interleave and store
    }
    // ...
}
```

### 2. Conversion Mode Selection at Track Open

**Problem:** Conversion parameters are re-evaluated or passed on every buffer.

**Solution:** Determine mode once at track open, cache in DirettaSync:

```cpp
// In DirettaSync.h
class DirettaSync {
private:
    DSDConversionMode m_dsdConversionMode = DSDConversionMode::Passthrough;

    // Function pointer for current mode (optional optimization)
    using DSDConvertFunc = size_t(*)(uint8_t*, const uint8_t*, size_t, int);
    DSDConvertFunc m_dsdConvertFunc = nullptr;

public:
    void setDSDConversionMode(DSDConversionMode mode);
    DSDConversionMode getDSDConversionMode() const { return m_dsdConversionMode; }
};
```

**Mode selection in configureSinkDSD():**

```cpp
void DirettaSync::configureSinkDSD(uint32_t dsdBitRate, int channels,
                                    const AudioFormat& format) {
    // ... existing sink configuration ...

    // Determine conversion mode based on source format and target requirements
    bool sourceIsLSB = (format.dsdFormat == AudioFormat::DSDFormat::DSF);
    bool targetNeedsMSB = /* query from sink info */;
    bool targetNeedsSwap = /* query from sink info */;

    bool needBitReverse = (sourceIsLSB != targetNeedsMSB);

    if (!needBitReverse && !targetNeedsSwap) {
        m_dsdConversionMode = DSDConversionMode::Passthrough;
    } else if (needBitReverse && !targetNeedsSwap) {
        m_dsdConversionMode = DSDConversionMode::BitReverseOnly;
    } else if (!needBitReverse && targetNeedsSwap) {
        m_dsdConversionMode = DSDConversionMode::ByteSwapOnly;
    } else {
        m_dsdConversionMode = DSDConversionMode::BitReverseAndSwap;
    }

    DIRETTA_LOG("DSD conversion mode: " << static_cast<int>(m_dsdConversionMode));
}
```

### 3. Ring Buffer Mode-Aware Push

**Problem:** `pushDSDPlanar()` receives parameters it shouldn't need to evaluate.

**Solution:** New push method that uses pre-selected mode:

```cpp
// In DirettaRingBuffer.h
size_t pushDSDPlanarOptimized(const uint8_t* data, size_t inputSize,
                               int numChannels, DSDConversionMode mode) {
    if (size_ == 0 || numChannels == 0) return 0;

    size_t maxBytes = std::min(inputSize, STAGING_SIZE);
    size_t free = getFreeSpace();
    if (maxBytes > free) maxBytes = free;

    size_t bytesPerChannel = maxBytes / static_cast<size_t>(numChannels);
    size_t completeGroups = bytesPerChannel / 4;
    size_t usableInput = completeGroups * 4 * static_cast<size_t>(numChannels);
    if (usableInput == 0) return 0;

    prefetch_audio_buffer(data, usableInput);

    size_t stagedBytes;
    switch (mode) {
        case DSDConversionMode::Passthrough:
            stagedBytes = convertDSD_Passthrough_AVX2(m_stagingDSD, data,
                                                       usableInput, numChannels);
            break;
        case DSDConversionMode::BitReverseOnly:
            stagedBytes = convertDSD_BitReverse_AVX2(m_stagingDSD, data,
                                                      usableInput, numChannels);
            break;
        case DSDConversionMode::ByteSwapOnly:
            stagedBytes = convertDSD_ByteSwap_AVX2(m_stagingDSD, data,
                                                    usableInput, numChannels);
            break;
        case DSDConversionMode::BitReverseAndSwap:
            stagedBytes = convertDSD_BitReverseSwap_AVX2(m_stagingDSD, data,
                                                          usableInput, numChannels);
            break;
    }

    return writeToRing(m_stagingDSD, stagedBytes);
}
```

### 4. Scalar Fallback Specialization (ARM64)

**Problem:** ARM64 uses scalar path which also has per-iteration branches.

**Solution:** Same 4-function specialization for scalar code:

```cpp
// Scalar versions for ARM64 and other non-AVX2 architectures
size_t convertDSD_Passthrough_Scalar(uint8_t* dst, const uint8_t* src,
                                      size_t totalBytes, int channels);
size_t convertDSD_BitReverse_Scalar(uint8_t* dst, const uint8_t* src,
                                     size_t totalBytes, int channels);
size_t convertDSD_ByteSwap_Scalar(uint8_t* dst, const uint8_t* src,
                                   size_t totalBytes, int channels);
size_t convertDSD_BitReverseSwap_Scalar(uint8_t* dst, const uint8_t* src,
                                         size_t totalBytes, int channels);
```

**Passthrough scalar (no lookup table access):**

```cpp
size_t convertDSD_Passthrough_Scalar(uint8_t* dst, const uint8_t* src,
                                      size_t totalBytes, int channels) {
    size_t bytesPerChannel = totalBytes / channels;
    size_t outputBytes = 0;

    for (size_t i = 0; i < bytesPerChannel; i += 4) {
        for (int ch = 0; ch < channels; ch++) {
            // Direct copy - no bitReversalTable lookup
            dst[outputBytes++] = src[ch * bytesPerChannel + i + 0];
            dst[outputBytes++] = src[ch * bytesPerChannel + i + 1];
            dst[outputBytes++] = src[ch * bytesPerChannel + i + 2];
            dst[outputBytes++] = src[ch * bytesPerChannel + i + 3];
        }
    }

    return outputBytes;
}
```

## Integration

**Initialization flow:**

1. `DirettaSync::open()` called with `AudioFormat`
2. `configureSinkDSD()` queries sink capabilities
3. Mode determined: `m_dsdConversionMode` set based on source format + sink requirements
4. `sendAudio()` calls `pushDSDPlanarOptimized()` with cached mode
5. Ring buffer dispatches to specialized function - no per-iteration checks

**Call site in DirettaSync::sendAudio():**

```cpp
// Current:
written = m_ringBuffer.pushDSDPlanar(
    data, len, channels,
    m_needDsdBitReversal ? getBitReversalTable() : nullptr,
    m_needDsdByteSwap);

// Proposed:
written = m_ringBuffer.pushDSDPlanarOptimized(
    data, len, channels,
    m_dsdConversionMode);
```

## Files Modified

| File | Changes |
|------|---------|
| `src/DirettaRingBuffer.h` | Add `DSDConversionMode` enum; add 4 specialized AVX2 functions; add 4 specialized scalar functions; add `pushDSDPlanarOptimized()` method |
| `src/DirettaSync.h` | Add `m_dsdConversionMode` member; add `setDSDConversionMode()` / `getDSDConversionMode()` |
| `src/DirettaSync.cpp` | Set conversion mode in `configureSinkDSD()`; update `sendAudio()` to use optimized push |

**Lines of code estimate:**
- 4 AVX2 functions: ~200 lines (extracted from existing, branches removed)
- 4 scalar functions: ~120 lines (extracted from existing, branches removed)
- Mode selection: ~30 lines
- Integration: ~20 lines

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| DSF file → LSB target | Passthrough mode (fastest) |
| DSF file → MSB target | BitReverseOnly mode |
| DFF file → LSB target | BitReverseOnly mode |
| DFF file → MSB target | Passthrough mode (fastest) |
| Unknown source format | Default to BitReverseOnly (safe) |
| Mid-stream format change | Re-determine mode on next `open()` |
| Mono DSD (rare) | Scalar fallback handles any channel count |
| Gapless DSD transition (same format) | Mode unchanged, no overhead |
| Gapless DSD transition (DSF→DFF) | Mode re-evaluated at track boundary |

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Code duplication (4 functions) | Extract common setup/teardown; only loop body differs |
| Wrong mode selected | Validate against current behavior in tests |
| Regression in existing path | Keep original `pushDSDPlanar()` as fallback |
| ARM64 scalar performance | Measure before/after; scalar branches cheaper than x86 |
| Multi-channel DSD (>2ch) | Scalar fallback handles; AVX2 path for stereo only |

## Performance Estimate

**DSD512 (22.5 MHz bit rate):**
- Bytes per second: 22,579,200 / 8 = 2,822,400 bytes/channel
- Loop iterations (32 bytes/iter): ~88,200/second per channel
- Branch checks eliminated: 176,400/second (2 checks × 88,200)

**Expected improvement:**
- Branch misprediction: ~15 cycles each (modern x86)
- Best case (perfect prediction): ~1-2 cycles saved per check
- Worst case (occasional mispredict): 15-30 cycles saved
- Estimated: 2-5% CPU reduction in DSD hot path

## Testing

1. **Bit-perfect verification:** Compare output of Passthrough vs original with `bitReversalTable=nullptr, byteSwap=false`
2. **Mode selection:** Verify correct mode logged for DSF and DFF files
3. **DSD64 through DSD512:** Test all rates with new path
4. **DSF→MSB target:** Verify BitReverseOnly mode activates
5. **DFF→LSB target:** Verify BitReverseOnly mode activates
6. **Gapless DSF→DFF:** Verify mode changes at track boundary
7. **ARM64 scalar:** Test on Raspberry Pi to verify scalar specialization
8. **Performance measurement:** Profile CPU usage before/after on DSD512

## Implementation Order

1. **Phase 1:** Add `DSDConversionMode` enum and mode selection logic
2. **Phase 2:** Implement `convertDSD_Passthrough_AVX2()` (extract from existing)
3. **Phase 3:** Implement remaining 3 AVX2 specialized functions
4. **Phase 4:** Add `pushDSDPlanarOptimized()` and wire to DirettaSync
5. **Phase 5:** Implement 4 scalar specialized functions for ARM64
6. **Phase 6:** Performance testing and validation

## Appendix: Current Code Locations

| Component | File | Line |
|-----------|------|------|
| `convertDSDPlanar_AVX2` | DirettaRingBuffer.h | 491-590 |
| `convertDSDPlanar_Scalar` | DirettaRingBuffer.h | 665-700 |
| `pushDSDPlanar` | DirettaRingBuffer.h | 281-303 |
| `simd_bit_reverse` | DirettaRingBuffer.h | 703-719 |
| `configureSinkDSD` | DirettaSync.cpp | ~870-920 |
| `sendAudio` (DSD path) | DirettaSync.cpp | ~1180-1190 |
| DSD source format detection | AudioEngine.cpp | ~330-350 |
