# DSD Conversion Function Specialization

**Date:** 2026-01-15
**Design:** `docs/plans/2026-01-15-dsd-conversion-optimization-design.md`
**Goal:** Eliminate per-iteration branch overhead in DSD hot path

## Overview

This optimization eliminates runtime branch checks inside the DSD conversion hot loop by pre-selecting specialized conversion functions at track open time. At DSD512 (22.5 MHz), this removes ~176,000 branch predictions per second from the critical path.

## Problem

The original `convertDSDPlanar_AVX2()` function checked two conditions on every 32-byte chunk:

```cpp
for (; i + 32 <= bytesPerChannel; i += 32) {
    __m256i left = _mm256_loadu_si256(...);
    __m256i right = _mm256_loadu_si256(...);

    if (bitReversalTable) {           // Branch #1: checked every iteration
        left = simd_bit_reverse(left);
        right = simd_bit_reverse(right);
    }

    // ... interleave ...

    if (needByteSwap) {               // Branch #2: checked every iteration
        interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
        interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
    }

    // ... store results
}
```

**Issues:**
- These values never change during playback of a track
- Branch prediction overhead on every iteration
- Scalar tail had same conditional pattern

---

## Solution

### DSD Conversion Modes

Pre-determine conversion mode at track open based on source format and target requirements:

| Mode | Bit Reversal | Byte Swap | Use Case |
|------|--------------|-----------|----------|
| `Passthrough` | No | No | DSF→LSB target, DFF→MSB target (fastest) |
| `BitReverseOnly` | Yes | No | DSF→MSB target, DFF→LSB target |
| `ByteSwapOnly` | No | Yes | Little-endian targets |
| `BitReverseAndSwap` | Yes | Yes | Little-endian + bit order mismatch |

### Mode Selection Matrix

| Source | Target Format | Mode |
|--------|---------------|------|
| DSF (LSB) | LSB \| BIG | Passthrough |
| DSF (LSB) | MSB \| BIG | BitReverseOnly |
| DSF (LSB) | LSB \| LITTLE | ByteSwapOnly |
| DSF (LSB) | MSB \| LITTLE | BitReverseAndSwap |
| DFF (MSB) | LSB \| BIG | BitReverseOnly |
| DFF (MSB) | MSB \| BIG | Passthrough |
| DFF (MSB) | LSB \| LITTLE | BitReverseAndSwap |
| DFF (MSB) | MSB \| LITTLE | ByteSwapOnly |

---

## Implementation

### 1. Conversion Mode Enum

```cpp
// DirettaRingBuffer.h
enum class DSDConversionMode {
    Passthrough,       // Just interleave (fastest)
    BitReverseOnly,    // Apply bit reversal only
    ByteSwapOnly,      // Apply byte swap only
    BitReverseAndSwap  // Both operations
};
```

### 2. Specialized Conversion Functions

Four functions with **no internal branches** in the hot loop:

**Passthrough (fastest):**
```cpp
size_t convertDSD_Passthrough(uint8_t* dst, const uint8_t* src,
                               size_t totalInputBytes, int numChannels) {
    // ... setup ...
    for (; i + 32 <= bytesPerChannel; i += 32) {
        __m256i left = _mm256_loadu_si256(...);
        __m256i right = _mm256_loadu_si256(...);

        // NO bit reversal - unconditional
        // NO byte swap - unconditional

        __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
        __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

        // ... store ...
    }
    // Scalar tail - also no branches
}
```

**BitReverse:**
```cpp
size_t convertDSD_BitReverse(uint8_t* dst, const uint8_t* src,
                              size_t totalInputBytes, int numChannels) {
    for (; i + 32 <= bytesPerChannel; i += 32) {
        __m256i left = _mm256_loadu_si256(...);
        __m256i right = _mm256_loadu_si256(...);

        // ALWAYS apply - no check
        left = simd_bit_reverse(left);
        right = simd_bit_reverse(right);

        // NO byte swap - unconditional
        // ... interleave and store ...
    }
    // Scalar tail with embedded LUT (no pointer check)
}
```

**ByteSwap:**
```cpp
size_t convertDSD_ByteSwap(uint8_t* dst, const uint8_t* src,
                            size_t totalInputBytes, int numChannels) {
    for (; i + 32 <= bytesPerChannel; i += 32) {
        // NO bit reversal - unconditional
        // ...interleave...

        // ALWAYS apply - no check
        interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
        interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
        // ...store...
    }
}
```

**BitReverseAndSwap:**
```cpp
size_t convertDSD_BitReverseSwap(uint8_t* dst, const uint8_t* src,
                                  size_t totalInputBytes, int numChannels) {
    for (; i + 32 <= bytesPerChannel; i += 32) {
        // ALWAYS apply both - no checks
        left = simd_bit_reverse(left);
        right = simd_bit_reverse(right);
        // ...interleave...
        interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
        interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
        // ...store...
    }
}
```

### 3. Optimized Push Method

```cpp
// DirettaRingBuffer.h
size_t pushDSDPlanarOptimized(const uint8_t* data, size_t inputSize,
                               int numChannels, DSDConversionMode mode) {
    // ... setup ...
    switch (mode) {
        case DSDConversionMode::Passthrough:
            stagedBytes = convertDSD_Passthrough(...);
            break;
        case DSDConversionMode::BitReverseOnly:
            stagedBytes = convertDSD_BitReverse(...);
            break;
        case DSDConversionMode::ByteSwapOnly:
            stagedBytes = convertDSD_ByteSwap(...);
            break;
        case DSDConversionMode::BitReverseAndSwap:
            stagedBytes = convertDSD_BitReverseSwap(...);
            break;
    }
    return writeToRing(m_stagingDSD, stagedBytes);
}
```

### 4. Mode Selection at Track Open

```cpp
// DirettaSync.cpp - configureSinkDSD()
void DirettaSync::configureSinkDSD(uint32_t dsdBitRate, int channels,
                                    const AudioFormat& format) {
    bool sourceIsLSB = (format.dsdFormat == AudioFormat::DSDFormat::DSF);

    // Try formats in order of preference...
    if (checkSinkSupport(LSB | BIG)) {
        m_needDsdBitReversal = !sourceIsLSB;
        m_needDsdByteSwap = false;
        // Set cached conversion mode
        m_dsdConversionMode = m_needDsdBitReversal
            ? DSDConversionMode::BitReverseOnly
            : DSDConversionMode::Passthrough;
    }
    // ... other format paths ...
}
```

### 5. Usage in sendAudio()

```cpp
// DirettaSync.cpp
if (dsdMode) {
    // Use optimized path with cached mode (no per-iteration branching)
    written = m_ringBuffer.pushDSDPlanarOptimized(
        data, totalBytes, numChannels, m_dsdConversionMode);
}
```

---

## Scalar Fallbacks (ARM64)

Each specialized function includes both AVX2 SIMD (for x86 stereo) and scalar fallbacks:

```cpp
size_t convertDSD_BitReverse(...) {
#if DIRETTA_HAS_AVX2
    if (numChannels == 2) {
        // AVX2 path with embedded bit reversal
        // ...
    }
#endif
    // Scalar fallback with embedded LUT
    static const uint8_t bitReverseLUT[256] = { ... };
    for (size_t i = 0; i < bytesPerChannel; i += 4) {
        for (int ch = 0; ch < numChannels; ch++) {
            dst[outputBytes++] = bitReverseLUT[src[chOffset + i + 0]];
            // ...
        }
    }
}
```

---

## Performance Analysis

### Branch Elimination

| DSD Rate | Loop Iterations/sec | Branches Eliminated/sec |
|----------|---------------------|-------------------------|
| DSD64 | ~11,000 | ~22,000 |
| DSD128 | ~22,000 | ~44,000 |
| DSD256 | ~44,000 | ~88,000 |
| DSD512 | ~88,000 | ~176,000 |
| DSD1024 | ~176,000 | ~352,000 |

### Expected Improvement

- Branch misprediction: ~15 cycles each (modern x86)
- Best case (perfect prediction): 1-2 cycles saved per check
- Worst case (occasional mispredict): 15-30 cycles saved
- **Estimated: 2-5% CPU reduction in DSD hot path**

---

## Files Modified

| File | Lines | Description |
|------|-------|-------------|
| `src/DirettaRingBuffer.h` | ~800 | Enum, 4 specialized functions, optimized push |
| `src/DirettaSync.h` | ~5 | `m_dsdConversionMode` member |
| `src/DirettaSync.cpp` | ~100 | Mode selection in all sink paths, usage in sendAudio |

---

## Testing Recommendations

1. **DSF → LSB target**: Verify Passthrough mode (log shows `mode=0`)
2. **DSF → MSB target**: Verify BitReverseOnly mode (log shows `mode=1`)
3. **DFF → LSB target**: Verify BitReverseOnly mode
4. **DFF → MSB target**: Verify Passthrough mode
5. **All DSD rates**: Test DSD64/128/256/512 with each mode
6. **Gapless DSF→DFF**: Verify mode changes at track boundary
7. **ARM64**: Verify scalar fallback works correctly

---

## Related Documents

- **Design Document:** `docs/plans/2026-01-15-dsd-conversion-optimization-design.md`
- **DSD Buffer Optimization:** `docs/DSD_BUFFER_OPTIMIZATION.md`
- **PCM Optimization:** `docs/PCM_FIFO_BYPASS_OPTIMIZATION.md`
