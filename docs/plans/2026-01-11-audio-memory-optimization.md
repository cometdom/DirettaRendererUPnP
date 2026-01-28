# Audio Memory Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Optimize ring buffer and memcpy operations to minimize timing variance (jitter) for audio streaming on AMD Zen 4.

**Architecture:** Per-format staging buffers with SIMD conversions, consistent-timing memcpy using overlapping stores, and Zen 4 prefetch tuning. All conversions happen in linear staging buffers first, then single memcpy to ring buffer.

**Tech Stack:** C++17, AVX2 intrinsics, x86-64 (Zen 4 target)

---

## Task 1: Create Test Harness

**Files:**
- Create: `src/AudioMemoryTest.h`
- Create: `src/test_audio_memory.cpp`
- Modify: `Makefile`

**Step 1: Create test header with assertion macros**

Create `src/AudioMemoryTest.h`:

```cpp
#ifndef AUDIO_MEMORY_TEST_H
#define AUDIO_MEMORY_TEST_H

#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdint>

#define TEST_ASSERT(condition, msg) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b, msg) \
    TEST_ASSERT((a) == (b), msg << " (expected " << (b) << ", got " << (a) << ")")

#define RUN_TEST(test_func) \
    do { \
        std::cout << "Running " << #test_func << "... "; \
        if (test_func()) { \
            std::cout << "PASS" << std::endl; \
            passed++; \
        } else { \
            std::cout << "FAIL" << std::endl; \
            failed++; \
        } \
    } while(0)

// Timing measurement helper
struct TimingStats {
    double min_us = 1e9;
    double max_us = 0;
    double sum_us = 0;
    double sum_sq = 0;
    int count = 0;

    void record(double us) {
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        sum_us += us;
        sum_sq += us * us;
        count++;
    }

    double mean() const { return count > 0 ? sum_us / count : 0; }
    double variance() const {
        if (count < 2) return 0;
        double m = mean();
        return (sum_sq / count) - (m * m);
    }
    double stddev() const { return std::sqrt(variance()); }
    double cv() const { return mean() > 0 ? stddev() / mean() : 0; } // Coefficient of variation
};

#endif // AUDIO_MEMORY_TEST_H
```

**Step 2: Create test runner skeleton**

Create `src/test_audio_memory.cpp`:

```cpp
#include "AudioMemoryTest.h"
#include "memcpyfast_audio.h"
#include "DirettaRingBuffer.h"

// Forward declarations of test functions
bool test_memcpy_audio_fixed_correctness();
bool test_memcpy_audio_fixed_timing_variance();
bool test_staging_buffer_alignment();
bool test_24bit_packing_correctness();
bool test_24bit_packing_timing();
bool test_16to32_correctness();
bool test_dsd_stereo_correctness();
bool test_ring_buffer_wraparound();

int main() {
    std::cout << "=== Audio Memory Optimization Tests ===" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    // Tests will be added as we implement each component
    std::cout << "No tests implemented yet - add tests as components are built" << std::endl;

    std::cout << std::endl;
    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;

    return failed > 0 ? 1 : 0;
}

// Placeholder test implementations
bool test_memcpy_audio_fixed_correctness() { return true; }
bool test_memcpy_audio_fixed_timing_variance() { return true; }
bool test_staging_buffer_alignment() { return true; }
bool test_24bit_packing_correctness() { return true; }
bool test_24bit_packing_timing() { return true; }
bool test_16to32_correctness() { return true; }
bool test_dsd_stereo_correctness() { return true; }
bool test_ring_buffer_wraparound() { return true; }
```

**Step 3: Add test target to Makefile**

Add to `Makefile` before the `-include $(DEPENDS)` line:

```makefile
# ============================================
# Test Target
# ============================================

TEST_TARGET = $(BINDIR)/test_audio_memory
TEST_SOURCES = $(SRCDIR)/test_audio_memory.cpp
TEST_OBJECTS = $(TEST_SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

test: $(TEST_TARGET)
	@echo "Running tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS) | $(BINDIR)
	@echo "Linking $(TEST_TARGET)..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_OBJECTS) -o $(TEST_TARGET)
```

**Step 4: Verify test compiles and runs**

Run: `make test`
Expected: "No tests implemented yet" message, exit 0

**Step 5: Commit**

```bash
git add src/AudioMemoryTest.h src/test_audio_memory.cpp Makefile
git commit -m "feat: add test harness for audio memory optimization"
```

---

## Task 2: Implement memcpy_audio_fixed

**Files:**
- Modify: `src/memcpyfast_audio.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for correctness**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_memcpy_audio_fixed_correctness() {
    // Test various sizes in the 128-4096 byte range
    std::vector<size_t> test_sizes = {128, 180, 256, 512, 768, 1024, 1500, 2048, 4096};

    for (size_t size : test_sizes) {
        alignas(64) uint8_t src[8192];
        alignas(64) uint8_t dst[8192];
        alignas(64) uint8_t expected[8192];

        // Fill source with pattern
        for (size_t i = 0; i < size; i++) {
            src[i] = static_cast<uint8_t>(i & 0xFF);
        }
        memset(dst, 0xAA, size);
        memcpy(expected, src, size);

        // Test our function
        memcpy_audio_fixed(dst, src, size);

        // Verify
        TEST_ASSERT(memcmp(dst, expected, size) == 0,
            "memcpy_audio_fixed failed at size " << size);
    }

    return true;
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL - `memcpy_audio_fixed` not defined

**Step 3: Implement memcpy_audio_fixed**

Add to `src/memcpyfast_audio.h` after the existing includes:

```cpp
#include <immintrin.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

/**
 * Consistent-timing memcpy for audio buffers (128-4096 bytes)
 * Uses overlapping stores for tail handling to eliminate timing variance
 */
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Process 128-byte blocks (4 x 32-byte AVX2 registers)
    while (size >= 128) {
        __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 0));
        __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i r2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 64));
        __m256i r3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 96));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 0), r0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), r1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 64), r2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 96), r3);
        s += 128;
        d += 128;
        size -= 128;
    }

    // Tail handling with overlapping stores (fixed instruction count per size range)
    if (size >= 64) {
        __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 64));
        __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), a1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 64), b0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b1);
    } else if (size >= 32) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b);
    } else if (size >= 16) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + size - 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + size - 16), b);
    } else if (size >= 8) {
        uint64_t a, b;
        memcpy(&a, s, 8);
        memcpy(&b, s + size - 8, 8);
        memcpy(d, &a, 8);
        memcpy(d + size - 8, &b, 8);
    } else if (size >= 4) {
        uint32_t a, b;
        memcpy(&a, s, 4);
        memcpy(&b, s + size - 4, 4);
        memcpy(d, &a, 4);
        memcpy(d + size - 4, &b, 4);
    } else if (size > 0) {
        d[0] = s[0];
        if (size > 1) d[size - 1] = s[size - 1];
        if (size > 2) d[1] = s[1];
    }

    _mm256_zeroupper();
}
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS

**Step 5: Write timing variance test**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_memcpy_audio_fixed_timing_variance() {
    constexpr int ITERATIONS = 10000;
    std::vector<size_t> test_sizes = {180, 768, 1536};  // Typical audio buffer sizes

    for (size_t size : test_sizes) {
        alignas(64) uint8_t src[4096];
        alignas(64) uint8_t dst[4096];

        // Warm up
        for (int i = 0; i < 100; i++) {
            memcpy_audio_fixed(dst, src, size);
        }

        TimingStats stats;
        for (int i = 0; i < ITERATIONS; i++) {
            auto start = std::chrono::high_resolution_clock::now();
            memcpy_audio_fixed(dst, src, size);
            auto end = std::chrono::high_resolution_clock::now();

            double us = std::chrono::duration<double, std::micro>(end - start).count();
            stats.record(us);
        }

        // Coefficient of variation should be < 50% for consistent timing
        double cv = stats.cv();
        TEST_ASSERT(cv < 0.5,
            "Timing variance too high for size " << size <<
            " (CV=" << cv << ", mean=" << stats.mean() << "us)");

        std::cout << "[size=" << size << " mean=" << stats.mean()
                  << "us cv=" << cv << "] ";
    }

    return true;
}
```

**Step 6: Run timing test**

Run: `make test`
Expected: PASS with timing stats printed

**Step 7: Commit**

```bash
git add src/memcpyfast_audio.h src/test_audio_memory.cpp
git commit -m "feat: implement memcpy_audio_fixed with consistent timing"
```

---

## Task 3: Add Staging Buffers to DirettaRingBuffer

**Files:**
- Modify: `src/DirettaRingBuffer.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for staging buffer alignment**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_staging_buffer_alignment() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);  // 1MB ring

    // Get staging buffer addresses (we'll add accessor methods)
    const uint8_t* staging24 = ring.getStaging24BitPack();
    const uint8_t* staging16to32 = ring.getStaging16To32();
    const uint8_t* stagingDSD = ring.getStagingDSD();

    // Verify 64-byte alignment
    TEST_ASSERT((reinterpret_cast<uintptr_t>(staging24) % 64) == 0,
        "staging24BitPack not 64-byte aligned");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(staging16to32) % 64) == 0,
        "staging16To32 not 64-byte aligned");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(stagingDSD) % 64) == 0,
        "stagingDSD not 64-byte aligned");

    // Verify buffers don't overlap (64KB each)
    TEST_ASSERT(staging16to32 >= staging24 + 65536 || staging24 >= staging16to32 + 65536,
        "staging buffers overlap");
    TEST_ASSERT(stagingDSD >= staging24 + 65536 || staging24 >= stagingDSD + 65536,
        "staging buffers overlap");

    return true;
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL - accessor methods not defined

**Step 3: Add staging buffers to DirettaRingBuffer.h**

Add to `src/DirettaRingBuffer.h` in the private section of `DirettaRingBuffer` class:

```cpp
    // Staging buffers for SIMD conversions (64KB each, cache-line aligned)
    static constexpr size_t STAGING_SIZE = 65536;
    alignas(64) uint8_t m_staging24BitPack[STAGING_SIZE];
    alignas(64) uint8_t m_staging16To32[STAGING_SIZE];
    alignas(64) uint8_t m_stagingDSD[STAGING_SIZE];
```

Add accessor methods in public section:

```cpp
    // Staging buffer accessors (for testing)
    const uint8_t* getStaging24BitPack() const { return m_staging24BitPack; }
    const uint8_t* getStaging16To32() const { return m_staging16To32; }
    const uint8_t* getStagingDSD() const { return m_stagingDSD; }
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_audio_memory.cpp
git commit -m "feat: add per-format staging buffers to DirettaRingBuffer"
```

---

## Task 4: Implement writeToRing Helper

**Files:**
- Modify: `src/DirettaRingBuffer.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for wraparound handling**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_ring_buffer_wraparound() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);  // Small ring for wraparound testing

    // Fill ring to near end
    std::vector<uint8_t> data(900, 0xAA);
    ring.push(data.data(), data.size());

    // Pop most of it to move read pointer
    std::vector<uint8_t> tmp(800);
    ring.pop(tmp.data(), tmp.size());

    // Now write pointer is at ~900, read at ~800
    // Push data that wraps around (200 bytes, but only ~124 space before wrap)
    std::vector<uint8_t> wrapData(200);
    for (size_t i = 0; i < 200; i++) {
        wrapData[i] = static_cast<uint8_t>(i);
    }

    size_t written = ring.push(wrapData.data(), wrapData.size());
    TEST_ASSERT(written == 200, "Failed to write wraparound data");

    // Read it back and verify
    std::vector<uint8_t> readBack(200);
    size_t read = ring.pop(readBack.data(), readBack.size());
    TEST_ASSERT(read == 200, "Failed to read wraparound data");

    TEST_ASSERT(memcmp(wrapData.data(), readBack.data(), 200) == 0,
        "Wraparound data corrupted");

    return true;
}
```

**Step 2: Run test to verify current implementation works**

Run: `make test`
Expected: PASS (existing implementation should handle this)

**Step 3: Add writeToRing helper method**

Add to `src/DirettaRingBuffer.h` in private section:

```cpp
    /**
     * Write staged data to ring buffer with efficient wraparound handling
     * Uses memcpy_audio_fixed for consistent timing
     */
    size_t writeToRing(const uint8_t* staged, size_t len) {
        size_t size = buffer_.size();
        if (size == 0 || len == 0) return 0;

        size_t writePos = writePos_.load(std::memory_order_relaxed);
        size_t readPos = readPos_.load(std::memory_order_acquire);

        // Calculate available space
        size_t available = (readPos > writePos)
            ? (readPos - writePos - 1)
            : (size - writePos + readPos - 1);

        if (len > available) {
            len = available;
        }
        if (len == 0) return 0;

        uint8_t* ring = buffer_.data();
        size_t firstChunk = std::min(len, size - writePos);

        // First chunk (up to end of buffer)
        if (firstChunk >= 32) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        } else if (firstChunk > 0) {
            memcpy(ring + writePos, staged, firstChunk);
        }

        // Second chunk (wraparound)
        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            if (secondChunk >= 32) {
                memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
            } else {
                memcpy(ring, staged + firstChunk, secondChunk);
            }
        }

        // Update write position
        size_t newWritePos = (writePos + len) % size;
        writePos_.store(newWritePos, std::memory_order_release);

        return len;
    }
```

**Step 4: Run test to verify still passes**

Run: `make test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_audio_memory.cpp
git commit -m "feat: add writeToRing helper with consistent-timing memcpy"
```

---

## Task 5: Implement SIMD 24-bit Packing

**Files:**
- Modify: `src/DirettaRingBuffer.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for 24-bit packing correctness**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_24bit_packing_correctness() {
    // Test S24_P32 -> packed 24-bit conversion
    // Input: 4 bytes per sample (24-bit in 32-bit container, little-endian)
    // Output: 3 bytes per sample (packed 24-bit)

    constexpr size_t NUM_SAMPLES = 64;  // Must be multiple of 8 for SIMD
    alignas(64) uint8_t input[NUM_SAMPLES * 4];   // S24_P32
    alignas(64) uint8_t output[NUM_SAMPLES * 3];  // Packed 24-bit
    alignas(64) uint8_t expected[NUM_SAMPLES * 3];

    // Fill input with test pattern
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        // Sample value: 0x112233 for sample 0, 0x223344 for sample 1, etc.
        uint32_t sample = 0x112233 + (i * 0x010101);
        input[i * 4 + 0] = sample & 0xFF;         // LSB
        input[i * 4 + 1] = (sample >> 8) & 0xFF;
        input[i * 4 + 2] = (sample >> 16) & 0xFF; // MSB
        input[i * 4 + 3] = 0x00;                  // Padding byte (ignored)

        // Expected output: just the 3 valid bytes
        expected[i * 3 + 0] = sample & 0xFF;
        expected[i * 3 + 1] = (sample >> 8) & 0xFF;
        expected[i * 3 + 2] = (sample >> 16) & 0xFF;
    }

    // Create ring buffer and test conversion
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    // Use the new SIMD conversion (we'll add this method)
    size_t converted = ring.convert24BitPacked_AVX2(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 3, "Wrong output size");
    TEST_ASSERT(memcmp(output, expected, NUM_SAMPLES * 3) == 0,
        "24-bit packing produced incorrect output");

    return true;
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL - convert24BitPacked_AVX2 not defined

**Step 3: Implement AVX2 24-bit packing**

Add to `src/DirettaRingBuffer.h` in public section:

```cpp
    /**
     * Convert S24_P32 to packed 24-bit using AVX2
     * Input: 4 bytes per sample (24-bit in 32-bit container)
     * Output: 3 bytes per sample (packed)
     * Returns: number of output bytes written
     */
    size_t convert24BitPacked_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;

        // Process 8 samples at a time (32 bytes in -> 24 bytes out)
        // AVX2 shuffle mask to extract bytes 0,1,2 from each 32-bit element
        // and pack them together

        // Within each 128-bit lane:
        // Input:  [B0 B1 B2 X] [B4 B5 B6 X] [B8 B9 B10 X] [B12 B13 B14 X]
        // Output: [B0 B1 B2 B4 B5 B6 B8 B9 B10 B12 B13 B14 X X X X]
        static const __m256i shuffle_mask = _mm256_setr_epi8(
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1
        );

        size_t i = 0;
        for (; i + 8 <= numSamples; i += 8) {
            // Load 32 bytes (8 samples)
            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));

            // Shuffle within lanes to pack 24-bit values
            __m256i shuffled = _mm256_shuffle_epi8(in, shuffle_mask);

            // Extract the two 12-byte results from each lane
            // Low lane: bytes 0-11, High lane: bytes 16-27
            __m128i lo = _mm256_castsi256_si128(shuffled);
            __m128i hi = _mm256_extracti128_si256(shuffled, 1);

            // Store exactly 12 bytes from low lane (8 + 4 bytes)
            // Using _mm_storeu_si128 would write 16 bytes, corrupting next data
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), lo);  // 8 bytes
            uint32_t lo_tail;
            memcpy(&lo_tail, reinterpret_cast<const char*>(&lo) + 8, 4);
            memcpy(dst + outputBytes + 8, &lo_tail, 4);  // 4 bytes
            outputBytes += 12;

            // Store exactly 12 bytes from high lane (8 + 4 bytes)
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), hi);  // 8 bytes
            uint32_t hi_tail;
            memcpy(&hi_tail, reinterpret_cast<const char*>(&hi) + 8, 4);
            memcpy(dst + outputBytes + 8, &hi_tail, 4);  // 4 bytes
            outputBytes += 12;
        }

        // Scalar cleanup for remaining 0-7 samples
        for (; i < numSamples; i++) {
            dst[outputBytes + 0] = src[i * 4 + 0];
            dst[outputBytes + 1] = src[i * 4 + 1];
            dst[outputBytes + 2] = src[i * 4 + 2];
            outputBytes += 3;
        }

        _mm256_zeroupper();
        return outputBytes;
    }
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS

**Step 5: Write timing test for 24-bit packing**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_24bit_packing_timing() {
    constexpr int ITERATIONS = 10000;
    constexpr size_t NUM_SAMPLES = 192;  // ~768 bytes input, ~576 output (typical hi-res)

    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];

    // Fill with test data
    for (size_t i = 0; i < NUM_SAMPLES * 4; i++) {
        input[i] = static_cast<uint8_t>(i);
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    // Warm up
    for (int i = 0; i < 100; i++) {
        ring.convert24BitPacked_AVX2(output, input, NUM_SAMPLES);
    }

    TimingStats stats;
    for (int i = 0; i < ITERATIONS; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        ring.convert24BitPacked_AVX2(output, input, NUM_SAMPLES);
        auto end = std::chrono::high_resolution_clock::now();

        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.record(us);
    }

    std::cout << "[24bit mean=" << stats.mean() << "us cv=" << stats.cv() << "] ";

    // Should have low variance
    TEST_ASSERT(stats.cv() < 0.5, "24-bit packing timing variance too high");

    return true;
}
```

**Step 6: Run timing test**

Run: `make test`
Expected: PASS with timing stats

**Step 7: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_audio_memory.cpp
git commit -m "feat: implement AVX2 24-bit packing with SIMD"
```

---

## Task 6: Implement SIMD 16→32 Upsampling

**Files:**
- Modify: `src/DirettaRingBuffer.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for 16→32 correctness**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_16to32_correctness() {
    constexpr size_t NUM_SAMPLES = 64;  // Must be multiple of 16 for SIMD
    alignas(64) uint8_t input[NUM_SAMPLES * 2];   // 16-bit
    alignas(64) uint8_t output[NUM_SAMPLES * 4];  // 32-bit
    alignas(64) uint8_t expected[NUM_SAMPLES * 4];

    // Fill input with test pattern
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        int16_t sample = static_cast<int16_t>(i * 256 - 32768);
        input[i * 2 + 0] = sample & 0xFF;
        input[i * 2 + 1] = (sample >> 8) & 0xFF;

        // Expected: 16-bit value in upper 16 bits of 32-bit container
        // [00 00 LSB MSB] in memory (little-endian)
        expected[i * 4 + 0] = 0x00;
        expected[i * 4 + 1] = 0x00;
        expected[i * 4 + 2] = input[i * 2 + 0];
        expected[i * 4 + 3] = input[i * 2 + 1];
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert16To32_AVX2(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 4, "Wrong output size");
    TEST_ASSERT(memcmp(output, expected, NUM_SAMPLES * 4) == 0,
        "16->32 conversion produced incorrect output");

    return true;
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL - convert16To32_AVX2 not defined

**Step 3: Implement AVX2 16→32 upsampling**

Add to `src/DirettaRingBuffer.h` in public section:

```cpp
    /**
     * Convert 16-bit to 32-bit using AVX2
     * Input: 2 bytes per sample (16-bit)
     * Output: 4 bytes per sample (16-bit value in upper 16 bits)
     * Returns: number of output bytes written
     */
    size_t convert16To32_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;

        // Process 16 samples at a time (32 bytes in -> 64 bytes out)
        size_t i = 0;
        for (; i + 16 <= numSamples; i += 16) {
            // Load 32 bytes (16 x 16-bit samples)
            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 2));

            // Unpack to 32-bit with zero in lower 16 bits
            // We want [00 00 S0_L S0_H] for each sample
            __m256i zero = _mm256_setzero_si256();

            // Unpack low 8 samples from each lane
            __m256i lo = _mm256_unpacklo_epi16(zero, in);
            // Unpack high 8 samples from each lane
            __m256i hi = _mm256_unpackhi_epi16(zero, in);

            // Permute to get correct order across lanes
            // lo contains: [lane0_lo4, lane1_lo4] and hi contains: [lane0_hi4, lane1_hi4]
            // We want: [lane0_lo4, lane0_hi4] then [lane1_lo4, lane1_hi4]
            __m256i out0 = _mm256_permute2x128_si256(lo, hi, 0x20);  // lane0 from both
            __m256i out1 = _mm256_permute2x128_si256(lo, hi, 0x31);  // lane1 from both

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
            outputBytes += 32;
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
            outputBytes += 32;
        }

        // Scalar cleanup for remaining samples
        for (; i < numSamples; i++) {
            dst[outputBytes + 0] = 0x00;
            dst[outputBytes + 1] = 0x00;
            dst[outputBytes + 2] = src[i * 2 + 0];
            dst[outputBytes + 3] = src[i * 2 + 1];
            outputBytes += 4;
        }

        _mm256_zeroupper();
        return outputBytes;
    }
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_audio_memory.cpp
git commit -m "feat: implement AVX2 16-to-32 bit upsampling"
```

---

## Task 7: Implement DSD Stereo SIMD

**Files:**
- Modify: `src/DirettaRingBuffer.h`
- Modify: `src/test_audio_memory.cpp`

**Step 1: Write failing test for DSD stereo correctness**

Add to `src/test_audio_memory.cpp` (replace placeholder):

```cpp
bool test_dsd_stereo_correctness() {
    // Test planar DSD [LLLL...][RRRR...] -> interleaved [LR LR LR LR...]
    // 4-byte groups for each channel
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;  // L + R planar
    constexpr size_t TOTAL_OUTPUT = BYTES_PER_CHANNEL * 2; // Interleaved

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_OUTPUT];
    alignas(64) uint8_t expected[TOTAL_OUTPUT];

    // Fill L channel with 0xAA pattern, R channel with 0x55 pattern
    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = 0xAA;                           // L channel
        input[BYTES_PER_CHANNEL + i] = 0x55;       // R channel
    }

    // Expected interleaved output: [4 bytes L][4 bytes R][4 bytes L][4 bytes R]...
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        // L group
        expected[i * 8 + 0] = 0xAA;
        expected[i * 8 + 1] = 0xAA;
        expected[i * 8 + 2] = 0xAA;
        expected[i * 8 + 3] = 0xAA;
        // R group
        expected[i * 8 + 4] = 0x55;
        expected[i * 8 + 5] = 0x55;
        expected[i * 8 + 6] = 0x55;
        expected[i * 8 + 7] = 0x55;
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x69);

    size_t converted = ring.convertDSDPlanar_AVX2(
        output, input, TOTAL_INPUT, 2,  // 2 channels (stereo)
        nullptr,  // no bit reversal
        false     // no byte swap
    );

    TEST_ASSERT_EQ(converted, TOTAL_OUTPUT, "Wrong DSD output size");
    TEST_ASSERT(memcmp(output, expected, TOTAL_OUTPUT) == 0,
        "DSD stereo interleaving produced incorrect output");

    return true;
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL - convertDSDPlanar_AVX2 not defined

**Step 3: Implement AVX2 DSD stereo interleaving**

Add to `src/DirettaRingBuffer.h` in public section:

```cpp
    /**
     * Convert DSD planar to interleaved using AVX2 (stereo only)
     * Input: [L channel bytes][R channel bytes] planar
     * Output: [4B L][4B R][4B L][4B R]... interleaved
     * Falls back to scalar for non-stereo
     */
    size_t convertDSDPlanar_AVX2(
        uint8_t* dst,
        const uint8_t* src,
        size_t totalInputBytes,
        int numChannels,
        const uint8_t* bitReversalTable,
        bool needByteSwap
    ) {
        size_t bytesPerChannel = totalInputBytes / numChannels;
        size_t outputBytes = 0;

        // Stereo SIMD path
        if (numChannels == 2) {
            const uint8_t* srcL = src;
            const uint8_t* srcR = src + bytesPerChannel;

            // Byte swap mask for little-endian targets
            static const __m256i byteswap_mask = _mm256_setr_epi8(
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12
            );

            // Process 32 bytes at a time (8 x 4-byte groups per channel)
            size_t i = 0;
            for (; i + 32 <= bytesPerChannel; i += 32) {
                __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
                __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

                // Apply bit reversal if needed
                if (bitReversalTable) {
                    left = simd_bit_reverse(left);
                    right = simd_bit_reverse(right);
                }

                // Interleave 4-byte groups: [L0 R0 L1 R1 L2 R2 L3 R3]
                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                // Byte swap if needed
                if (needByteSwap) {
                    interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
                    interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
                }

                // Permute to get correct order across lanes
                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar cleanup for remaining bytes (process 4-byte groups)
            for (; i + 4 <= bytesPerChannel; i += 4) {
                // L group
                for (int j = 0; j < 4; j++) {
                    uint8_t b = srcL[i + j];
                    if (bitReversalTable) b = bitReversalTable[b];
                    dst[outputBytes++] = b;
                }
                // R group
                for (int j = 0; j < 4; j++) {
                    uint8_t b = srcR[i + j];
                    if (bitReversalTable) b = bitReversalTable[b];
                    dst[outputBytes++] = b;
                }
            }

            _mm256_zeroupper();
        } else {
            // Multi-channel fallback (scalar)
            outputBytes = convertDSDPlanar_Scalar(dst, src, totalInputBytes, numChannels,
                                                   bitReversalTable, needByteSwap);
        }

        return outputBytes;
    }

private:
    // Scalar fallback for multi-channel DSD
    size_t convertDSDPlanar_Scalar(
        uint8_t* dst,
        const uint8_t* src,
        size_t totalInputBytes,
        int numChannels,
        const uint8_t* bitReversalTable,
        bool needByteSwap
    ) {
        size_t bytesPerChannel = totalInputBytes / numChannels;
        size_t outputBytes = 0;

        // Process 4-byte groups
        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                // Collect 4 bytes for this channel
                uint8_t group[4];
                for (int j = 0; j < 4 && (i + j) < bytesPerChannel; j++) {
                    uint8_t b = src[ch * bytesPerChannel + i + j];
                    if (bitReversalTable) b = bitReversalTable[b];
                    group[j] = b;
                }

                // Apply byte swap if needed (reverse order within 4-byte group)
                if (needByteSwap) {
                    dst[outputBytes++] = group[3];
                    dst[outputBytes++] = group[2];
                    dst[outputBytes++] = group[1];
                    dst[outputBytes++] = group[0];
                } else {
                    dst[outputBytes++] = group[0];
                    dst[outputBytes++] = group[1];
                    dst[outputBytes++] = group[2];
                    dst[outputBytes++] = group[3];
                }
            }
        }

        return outputBytes;
    }

    // SIMD bit reversal helper
    static __m256i simd_bit_reverse(__m256i x) {
        static const __m256i nibble_reverse = _mm256_setr_epi8(
            0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
            0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF,
            0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
            0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
        );

        __m256i mask_0f = _mm256_set1_epi8(0x0F);
        __m256i lo_nibbles = _mm256_and_si256(x, mask_0f);
        __m256i hi_nibbles = _mm256_and_si256(_mm256_srli_epi16(x, 4), mask_0f);

        __m256i lo_reversed = _mm256_shuffle_epi8(nibble_reverse, lo_nibbles);
        __m256i hi_reversed = _mm256_shuffle_epi8(nibble_reverse, hi_nibbles);

        return _mm256_or_si256(_mm256_slli_epi16(lo_reversed, 4), hi_reversed);
    }
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_audio_memory.cpp
git commit -m "feat: implement AVX2 DSD stereo interleaving with bit reversal"
```

---

## Task 8: Add Prefetch Tuning

**Files:**
- Modify: `src/memcpyfast_audio.h`
- Modify: `src/DirettaRingBuffer.h`

**Step 1: Add prefetch helper function**

Add to `src/memcpyfast_audio.h`:

```cpp
/**
 * Prefetch audio buffer for upcoming memcpy
 * Tuned for 180-1500 byte buffers on Zen 4
 */
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    const char* p = static_cast<const char*>(src);

    // First cache line always
    _mm_prefetch(p, _MM_HINT_T0);

    if (size > 256) {
        _mm_prefetch(p + 64, _MM_HINT_T0);
    }
    if (size > 512) {
        // Prefetch end of buffer for overlapping tail copies
        _mm_prefetch(p + size - 64, _MM_HINT_T0);
    }
}
```

**Step 2: Add prefetch to 24-bit conversion loop**

Update `convert24BitPacked_AVX2` in `src/DirettaRingBuffer.h` to add prefetch:

```cpp
        for (; i + 8 <= numSamples; i += 8) {
            // Prefetch 2 iterations ahead
            if (i + 16 <= numSamples) {
                _mm_prefetch(reinterpret_cast<const char*>(src + (i + 16) * 4), _MM_HINT_T0);
            }

            // Load 32 bytes (8 samples)
            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
            // ... rest of loop
```

**Step 3: Commit**

```bash
git add src/memcpyfast_audio.h src/DirettaRingBuffer.h
git commit -m "feat: add Zen 4 prefetch tuning for audio buffers"
```

---

## Task 9: Integrate with push24BitPacked

**Files:**
- Modify: `src/DirettaRingBuffer.h`

**Step 1: Update push24BitPacked to use staging + SIMD**

Replace the existing `push24BitPacked` method with:

```cpp
    size_t push24BitPacked(const uint8_t* data, size_t inputSize) {
        // Input: S24_P32 format (4 bytes per sample)
        // Output: Packed 24-bit (3 bytes per sample)

        size_t numSamples = inputSize / 4;
        if (numSamples == 0) return 0;

        // Limit to staging buffer capacity
        size_t maxSamples = STAGING_SIZE / 3;  // Output is 3 bytes per sample
        if (numSamples > maxSamples) {
            numSamples = maxSamples;
        }

        // Prefetch source
        prefetch_audio_buffer(data, numSamples * 4);

        // Convert to staging buffer using SIMD
        size_t stagedBytes = convert24BitPacked_AVX2(m_staging24BitPack, data, numSamples);

        // Write to ring buffer with consistent-timing memcpy
        return writeToRing(m_staging24BitPack, stagedBytes);
    }
```

**Step 2: Run full test suite**

Run: `make test`
Expected: All PASS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "feat: integrate SIMD 24-bit packing with staging buffer"
```

---

## Task 10: Integrate with push16To32

**Files:**
- Modify: `src/DirettaRingBuffer.h`

**Step 1: Update push16To32 to use staging + SIMD**

Replace the existing `push16To32` method with:

```cpp
    size_t push16To32(const uint8_t* data, size_t inputSize) {
        // Input: 16-bit samples (2 bytes per sample)
        // Output: 32-bit samples (4 bytes per sample)

        size_t numSamples = inputSize / 2;
        if (numSamples == 0) return 0;

        // Limit to staging buffer capacity
        size_t maxSamples = STAGING_SIZE / 4;  // Output is 4 bytes per sample
        if (numSamples > maxSamples) {
            numSamples = maxSamples;
        }

        // Prefetch source
        prefetch_audio_buffer(data, numSamples * 2);

        // Convert to staging buffer using SIMD
        size_t stagedBytes = convert16To32_AVX2(m_staging16To32, data, numSamples);

        // Write to ring buffer with consistent-timing memcpy
        return writeToRing(m_staging16To32, stagedBytes);
    }
```

**Step 2: Run full test suite**

Run: `make test`
Expected: All PASS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "feat: integrate SIMD 16-to-32 upsampling with staging buffer"
```

---

## Task 11: Integrate with pushDSDPlanar

**Files:**
- Modify: `src/DirettaRingBuffer.h`

**Step 1: Update pushDSDPlanar to use staging + SIMD**

Replace the existing `pushDSDPlanar` method to use the new SIMD path:

```cpp
    size_t pushDSDPlanar(
        const uint8_t* data,
        size_t totalBytes,
        int numChannels,
        const uint8_t* bitReversalTable,
        bool needByteSwap
    ) {
        if (totalBytes == 0 || numChannels == 0) return 0;

        // Limit to staging buffer capacity
        if (totalBytes > STAGING_SIZE) {
            totalBytes = STAGING_SIZE;
        }

        // Prefetch source
        prefetch_audio_buffer(data, totalBytes);

        // Convert to staging buffer using SIMD (stereo) or scalar (multi-channel)
        size_t stagedBytes = convertDSDPlanar_AVX2(
            m_stagingDSD, data, totalBytes, numChannels,
            bitReversalTable, needByteSwap
        );

        // Write to ring buffer with consistent-timing memcpy
        return writeToRing(m_stagingDSD, stagedBytes);
    }
```

**Step 2: Run full test suite**

Run: `make test`
Expected: All PASS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "feat: integrate SIMD DSD interleaving with staging buffer"
```

---

## Task 12: Full Integration Test

**Files:**
- Modify: `src/test_audio_memory.cpp`

**Step 1: Add integration test**

Add to `src/test_audio_memory.cpp`:

```cpp
bool test_full_integration() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    // Test 24-bit path
    {
        alignas(64) uint8_t input[768];  // 192 samples @ S24_P32
        for (size_t i = 0; i < 768; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.push24BitPacked(input, 768);
        TEST_ASSERT(written > 0, "24-bit push failed");
        TEST_ASSERT(written == 192 * 3, "24-bit push wrong size");
    }

    // Test 16->32 path
    ring.clear();
    {
        alignas(64) uint8_t input[384];  // 192 samples @ 16-bit
        for (size_t i = 0; i < 384; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.push16To32(input, 384);
        TEST_ASSERT(written > 0, "16->32 push failed");
        TEST_ASSERT(written == 192 * 4, "16->32 push wrong size");
    }

    // Test DSD path
    ring.clear();
    {
        alignas(64) uint8_t input[128];  // 64 bytes per channel, stereo
        for (size_t i = 0; i < 128; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.pushDSDPlanar(input, 128, 2, nullptr, false);
        TEST_ASSERT(written > 0, "DSD push failed");
        TEST_ASSERT(written == 128, "DSD push wrong size");
    }

    return true;
}
```

**Step 2: Add to test runner**

Update main() to include:
```cpp
    RUN_TEST(test_full_integration);
```

**Step 3: Run full test suite**

Run: `make test`
Expected: All PASS

**Step 4: Commit**

```bash
git add src/test_audio_memory.cpp
git commit -m "test: add full integration test for all conversion paths"
```

---

## Task 13: Build and Verify Main Application

**Step 1: Build main application**

Run: `make clean && make`
Expected: Successful build with no warnings

**Step 2: Verify no regressions**

Run the application manually to verify audio playback still works correctly.

**Step 3: Final commit**

```bash
git add -A
git commit -m "chore: audio memory optimization complete"
```

---

## Summary

| Task | Component | Key Files |
|------|-----------|-----------|
| 1 | Test harness | `AudioMemoryTest.h`, `test_audio_memory.cpp` |
| 2 | memcpy_audio_fixed | `memcpyfast_audio.h` |
| 3 | Staging buffers | `DirettaRingBuffer.h` |
| 4 | writeToRing helper | `DirettaRingBuffer.h` |
| 5 | 24-bit SIMD packing | `DirettaRingBuffer.h` |
| 6 | 16→32 SIMD upsampling | `DirettaRingBuffer.h` |
| 7 | DSD stereo SIMD | `DirettaRingBuffer.h` |
| 8 | Prefetch tuning | `memcpyfast_audio.h` |
| 9-11 | Integration | `DirettaRingBuffer.h` |
| 12-13 | Testing | `test_audio_memory.cpp` |

**Total estimated steps:** 52 steps across 13 tasks
**Commit frequency:** Every task (13 commits)
