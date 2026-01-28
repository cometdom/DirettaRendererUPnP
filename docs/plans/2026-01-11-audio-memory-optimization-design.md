# Audio Memory Optimization Design

**Date:** 2026-01-11
**Goal:** Optimize memory operations in ring buffer and fast memcpy to minimize timing variance (jitter) for audio streaming.
**Target:** AMD Zen 4 with AVX2/AVX-512
**Primary Use Case:** High-res PCM (24-bit/32-bit) and standard PCM (16-bit)

---

## 1. Architecture Overview

**Core Principle:** Eliminate variable-cost operations from the hot path. Every push operation executes the same code path regardless of buffer position or data alignment.

```
AudioEngine
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  DirettaRingBuffer (modified)                       │
│  ┌───────────────────────────────────────────────┐  │
│  │  Staging Buffers (per-format, 64KB each)      │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐         │  │
│  │  │ PCM 24  │ │ PCM 16  │ │   DSD   │         │  │
│  │  │  Pack   │ │  →32    │ │ Planar  │         │  │
│  │  └────┬────┘ └────┬────┘ └────┬────┘         │  │
│  │       │           │           │               │  │
│  │       ▼           ▼           ▼               │  │
│  │  ┌─────────────────────────────────────────┐  │  │
│  │  │  SIMD Conversion (AVX2, fixed blocks)   │  │  │
│  │  └─────────────────────────────────────────┘  │  │
│  │                     │                         │  │
│  │                     ▼                         │  │
│  │  ┌─────────────────────────────────────────┐  │  │
│  │  │  Ring Buffer (64-byte aligned)          │  │  │
│  │  │  Single memcpy_audio per push           │  │  │
│  │  └─────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**Key changes:**
- All conversions happen in linear staging buffers first
- Single `memcpy_audio` call transfers converted data to ring
- Wraparound handling moves from per-byte to per-transfer level

---

## 2. Staging Buffer Design

**Structure:** Three dedicated staging buffers, each 64KB, cache-line aligned.

```cpp
// New members in DirettaRingBuffer class
static constexpr size_t STAGING_SIZE = 65536;  // 64KB
static constexpr size_t STAGING_ALIGN = 64;    // Cache line

alignas(64) uint8_t m_staging24BitPack[STAGING_SIZE];
alignas(64) uint8_t m_staging16To32[STAGING_SIZE];
alignas(64) uint8_t m_stagingDSD[STAGING_SIZE];
```

**Why 64KB per buffer:**
- Fits in Zen 4 L2 cache (1MB per core)
- Covers ~57ms of 192kHz/24-bit stereo audio (1.15MB/s)
- Typical push operations are 180 bytes - 1.5KB (~1ms), so 64KB provides 40-350× headroom
- Power of 2 for efficient indexing

**Memory layout benefits:**
- Each buffer on separate cache lines - no false sharing
- Predictable L1/L2 cache residency during conversion
- Source data → staging → ring buffer flows linearly through cache hierarchy

**Conversion flow (all formats):**
1. Convert input → staging buffer (SIMD, linear memory)
2. Calculate ring buffer write position and space
3. Single or dual `memcpy_audio` to ring (handles wraparound)
4. Update write position atomically

**Wraparound handling changes from:**
```cpp
// OLD: Per-byte wraparound check in conversion loop
for (each sample) {
    *dst++ = converted_byte;
    if (dst == end) dst = buffer_.data();  // Branch per byte!
}
```

**To:**
```cpp
// NEW: Single check after conversion
size_t firstChunk = min(convertedSize, size_ - writePos);
memcpy_audio(ring + writePos, staging, firstChunk);
if (firstChunk < convertedSize) {
    memcpy_audio(ring, staging + firstChunk, convertedSize - firstChunk);
}
```

---

## 3. SIMD 24-bit Packing

**The problem:** Converting S24_P32 (24-bit samples in 32-bit containers) to packed 24-bit (3 bytes per sample).

**Input:** `[S0:4B][S1:4B][S2:4B][S3:4B]...` (32 bytes = 8 samples)
**Output:** `[S0:3B][S1:3B][S2:3B][S3:3B]...` (24 bytes = 8 samples)

**AVX2 approach:** Process 8 samples (32 bytes → 24 bytes) per iteration using shuffle.

```cpp
// Load 32 bytes (8 samples × 4 bytes)
__m256i src = _mm256_loadu_si256(input);

// Shuffle to extract bytes 0,1,2 from each 32-bit sample
// Input:  [B0 B1 B2 X][B0 B1 B2 X][B0 B1 B2 X][B0 B1 B2 X]...
// Output: [B0 B1 B2 B0 B1 B2 B0 B1 B2 B0 B1 B2]...

// Two-step shuffle: within lanes, then cross-lane permute
__m256i shuffled = _mm256_shuffle_epi8(src, pack_mask);
// Pack and store 24 bytes
```

**Timing consistency:**
- Fixed iteration count: `numSamples / 8` full SIMD iterations
- Scalar cleanup only for final 0-7 samples (rare, predictable)
- No data-dependent branches in hot loop
- Prefetch next cache line during current iteration

**Performance estimate:**
- Current: ~3 cycles per byte (byte-by-byte with branch)
- SIMD: ~0.2 cycles per byte (8 samples per iteration)
- **~15x throughput improvement** for 24-bit path

**Fallback:** Scalar loop for:
- Remainders (0-7 samples after SIMD loop)
- Builds without AVX2 support

---

## 4. Consistent-Timing memcpy

**The problem:** Current `memcpy_audio` has multiple code paths with different timings:
- `size <= 256`: Jump table (`memcpy_tiny`) - variable timing per size
- `size > 256`: Loop-based SIMD - more predictable

**Audio buffer sizes:** Mostly 180-1500 bytes - right at the boundary.

**Solution:** Replace branchy paths with fixed-block processing for audio-sized buffers.

```cpp
// New: Audio-optimized path for 128-4096 byte range
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    // Process 128-byte blocks (4 × 32-byte AVX2 registers)
    // Fixed unrolled loop - same timing regardless of size
    while (size >= 128) {
        __m256i r0 = _mm256_loadu_si256((const __m256i*)(s + 0));
        __m256i r1 = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i r2 = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i r3 = _mm256_loadu_si256((const __m256i*)(s + 96));
        _mm256_storeu_si256((__m256i*)(d + 0), r0);
        _mm256_storeu_si256((__m256i*)(d + 32), r1);
        _mm256_storeu_si256((__m256i*)(d + 64), r2);
        _mm256_storeu_si256((__m256i*)(d + 96), r3);
        s += 128; d += 128; size -= 128;
    }

    // Fixed-size tail handling (0-127 bytes)
    // Use overlapping stores to avoid branches
    if (size >= 64) {
        // Copy first 64 and last 64 bytes (may overlap)
        __m256i a0 = _mm256_loadu_si256((const __m256i*)s);
        __m256i a1 = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i b0 = _mm256_loadu_si256((const __m256i*)(s + size - 64));
        __m256i b1 = _mm256_loadu_si256((const __m256i*)(s + size - 32));
        _mm256_storeu_si256((__m256i*)d, a0);
        _mm256_storeu_si256((__m256i*)(d + 32), a1);
        _mm256_storeu_si256((__m256i*)(d + size - 64), b0);
        _mm256_storeu_si256((__m256i*)(d + size - 32), b1);
    } else if (size >= 32) {
        // Overlapping 32-byte copies
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + size - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + size - 32), b);
    } else if (size >= 16) {
        __m128i a = _mm_loadu_si128((const __m128i*)s);
        __m128i b = _mm_loadu_si128((const __m128i*)(s + size - 16));
        _mm_storeu_si128((__m128i*)d, a);
        _mm_storeu_si128((__m128i*)(d + size - 16), b);
    } else if (size >= 8) {
        uint64_t a = *(const uint64_t*)s;
        uint64_t b = *(const uint64_t*)(s + size - 8);
        *(uint64_t*)d = a;
        *(uint64_t*)(d + size - 8) = b;
    } else if (size >= 4) {
        uint32_t a = *(const uint32_t*)s;
        uint32_t b = *(const uint32_t*)(s + size - 4);
        *(uint32_t*)d = a;
        *(uint32_t*)(d + size - 4) = b;
    } else if (size > 0) {
        d[0] = s[0];
        if (size > 1) d[size-1] = s[size-1];
        if (size > 2) d[1] = s[1];
    }

    _mm256_zeroupper();
}
```

**Key principle:** Overlapping stores for tail handling - always execute the same number of stores regardless of exact size. Eliminates the 256-case jump table.

---

## 5. Zen 4 Prefetch Tuning

**Zen 4 cache characteristics:**
- L1D: 32KB per core, 8-way, 64-byte lines
- L2: 1MB per core, 8-way, 64-byte lines
- L1 latency: ~4 cycles
- L2 latency: ~12 cycles
- L3 latency: ~40-50 cycles

**Optimized prefetch strategy for audio buffers:**

```cpp
// Tuned for 180-1500 byte audio buffers on Zen 4
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    const char* p = (const char*)src;

    // For small buffers (<512 bytes): single prefetch at start
    // Data will fit in L1, no need for aggressive prefetching
    _mm_prefetch(p, _MM_HINT_T0);

    if (size > 256) {
        // Second cache line
        _mm_prefetch(p + 64, _MM_HINT_T0);
    }
    if (size > 512) {
        // Prefetch end of buffer (for overlapping tail copies)
        _mm_prefetch(p + size - 64, _MM_HINT_T0);
    }
}
```

**In-loop prefetch for staging buffer conversions:**

```cpp
// During 24-bit packing loop, prefetch next iteration's source
for (size_t i = 0; i < numBlocks; i++) {
    // Prefetch 2 iterations ahead (256 bytes = 4 cache lines)
    if (i + 2 < numBlocks) {
        _mm_prefetch(src + (i + 2) * 32, _MM_HINT_T0);
    }

    // Process current block (unaligned load - input buffers have no alignment guarantee)
    __m256i data = _mm256_loadu_si256((const __m256i*)(src + i * 32));
    // ... conversion ...
}
```

**Ring buffer destination prefetch:**

```cpp
// Before memcpy to ring buffer, prefetch destination
_mm_prefetch(ring_dst, _MM_HINT_T0);
```

---

## 6. 16→32 Upsampling & DSD Optimization

**16-bit to 32-bit upsampling:**

```cpp
// Input:  [S0:2B][S1:2B][S2:2B][S3:2B]... (16 samples = 32 bytes)
// Output: [00 00 S0:2B][00 00 S1:2B]...   (16 samples = 64 bytes)

// AVX2: Process 16 samples per iteration
__m256i src = _mm256_loadu_si256(input);  // 16 × 16-bit samples

// Unpack to 32-bit with zero extension
__m256i lo = _mm256_unpacklo_epi16(_mm256_setzero_si256(), src);
__m256i hi = _mm256_unpackhi_epi16(_mm256_setzero_si256(), src);

// Permute to correct order and store
__m256i out0 = _mm256_permute2x128_si256(lo, hi, 0x20);
__m256i out1 = _mm256_permute2x128_si256(lo, hi, 0x31);
_mm256_storeu_si256(dst + 0, out0);
_mm256_storeu_si256(dst + 32, out1);
```

**DSD planar-to-interleaved optimization:**

Note: SIMD path is for **stereo only** (2 channels). Multi-channel DSD (4/6 channels) uses scalar fallback to preserve correct channel ordering.

```cpp
// STEREO SIMD PATH: Process 32 bytes (8 × 4-byte groups) per iteration
if (numChannels == 2) {
    __m256i left  = _mm256_loadu_si256(src_L);
    __m256i right = _mm256_loadu_si256(src_R);

    // Interleave 4-byte groups: [L0 R0 L1 R1 L2 R2 L3 R3]
    __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
    __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

    // Apply bit reversal if needed (vectorized table lookup)
    if (needBitReversal) {
        interleaved_lo = simd_bit_reverse(interleaved_lo);
        interleaved_hi = simd_bit_reverse(interleaved_hi);
    }

    // Byte swap for little-endian targets
    if (needByteSwap) {
        interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
        interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
    }
} else {
    // MULTI-CHANNEL FALLBACK: Use existing scalar loop for 4/6+ channels
    // Maintains correct channel ordering for surround DSD
    convertDSDPlanar_Scalar(dst, src, numChannels, ...);
}
```

**Vectorized bit reversal:**

```cpp
static const __m256i nibble_reverse = _mm256_setr_epi8(
    0x0,0x8,0x4,0xC,0x2,0xA,0x6,0xE,0x1,0x9,0x5,0xD,0x3,0xB,0x7,0xF,
    0x0,0x8,0x4,0xC,0x2,0xA,0x6,0xE,0x1,0x9,0x5,0xD,0x3,0xB,0x7,0xF
);

__m256i simd_bit_reverse(__m256i x) {
    __m256i lo_nibbles = _mm256_and_si256(x, _mm256_set1_epi8(0x0F));
    __m256i hi_nibbles = _mm256_and_si256(_mm256_srli_epi16(x, 4), _mm256_set1_epi8(0x0F));

    __m256i lo_reversed = _mm256_shuffle_epi8(nibble_reverse, lo_nibbles);
    __m256i hi_reversed = _mm256_shuffle_epi8(nibble_reverse, hi_nibbles);

    return _mm256_or_si256(_mm256_slli_epi16(lo_reversed, 4), hi_reversed);
}
```

---

## 7. Implementation Plan

### Files to Modify

| File | Changes |
|------|---------|
| `DirettaRingBuffer.h` | Add staging buffers, SIMD conversion methods |
| `memcpyfast_audio.h` | Add `memcpy_audio_fixed()` for consistent timing |
| `FastMemcpy_Audio.h` | Update dispatcher, add prefetch tuning |
| `DirettaSync.cpp` | Minor: ensure staging buffers used in sendAudio path |

### New Code Structure

```cpp
class DirettaRingBuffer {
public:
    // Existing public interface unchanged
    size_t push(const uint8_t* data, size_t len);
    size_t push24BitPacked(const uint8_t* data, size_t inputSize);
    size_t push16To32(const uint8_t* data, size_t inputSize);
    size_t pushDSDPlanar(...);
    size_t pop(uint8_t* dest, size_t len);

private:
    // Ring buffer (existing)
    std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> buffer_;

    // NEW: Per-format staging buffers
    static constexpr size_t STAGING_SIZE = 65536;
    alignas(64) uint8_t m_staging24BitPack[STAGING_SIZE];
    alignas(64) uint8_t m_staging16To32[STAGING_SIZE];
    alignas(64) uint8_t m_stagingDSD[STAGING_SIZE];

    // NEW: SIMD conversion helpers
    size_t convert24BitPacked_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples);
    size_t convert16To32_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples);
    size_t convertDSDPlanar_AVX2(uint8_t* dst, const uint8_t* src, ...);

    // NEW: Consistent-timing ring write
    void writeToRing(const uint8_t* staged, size_t len);
};
```

### Implementation Order

1. **memcpy_audio_fixed()** - consistent timing memcpy
2. **DirettaRingBuffer staging buffers** and `writeToRing()`
3. **convert24BitPacked_AVX2()** - main hi-res path
4. **convert16To32_AVX2()** - CD quality path
5. **convertDSDPlanar_AVX2()** - DSD path
6. **Prefetch tuning** integration
7. **Testing and benchmarking**

### Expected Improvements

| Metric | Before | After |
|--------|--------|-------|
| 24-bit packing throughput | ~3 cycles/byte | ~0.2 cycles/byte |
| Timing variance (jitter) | High (branchy) | Low (fixed paths) |
| Cache efficiency | Variable | Predictable |
| Wraparound overhead | Per-byte branch | Per-transfer check |

---

## 8. Testing Strategy

### Unit Tests
- Verify bit-exact output for all conversion paths
- Test boundary conditions (wraparound at various positions)
- Test all sample counts (including 0-7 sample remainders)

### Timing Tests
- Measure variance across 10,000 push operations
- Compare before/after histograms
- Verify no outliers exceed 2x median

### Integration Tests
- Full playback test with hi-res content
- Gapless transition test
- Format change test (16-bit → 24-bit → 16-bit)

---

## Appendix: Design Decisions

**Q: Why per-format staging buffers instead of shared?**
A: Eliminates contention and ensures each format path has predictable cache residency.

**Q: Why 64KB staging size?**
A: Covers ~57ms of 192kHz/24-bit audio (1.15MB/s), which is 40-350× larger than typical push sizes (180B-1.5KB). Fits L2 cache, power of 2 for alignment.

**Q: Why overlapping stores for tail handling?**
A: Executes same instruction count regardless of exact size, eliminating timing variance from the 256-case jump table.

**Q: Why prefetch 2 iterations ahead?**
A: Matches Zen 4 L2 latency (~12 cycles) with conversion loop timing, ensuring data arrives before needed.

**Q: Why use unaligned loads (`_mm256_loadu_si256`) for input?**
A: Input buffers from AudioEngine have no alignment guarantee. Using aligned loads would cause SIGBUS/GPF on unaligned data. On modern CPUs (Haswell+, Zen+), unaligned loads have no penalty when data is naturally aligned, so there's no performance cost.

**Q: Why is DSD SIMD stereo-only?**
A: The SIMD interleave pattern ([L0 R0 L1 R1...]) is specific to 2-channel layout. Multi-channel DSD (4/6 channels) requires different interleaving that would need per-channel-count SIMD variants. Since multi-channel DSD is rare and stereo covers 99%+ of use cases, we use scalar fallback for multi-channel to maintain correct channel ordering without over-engineering.
