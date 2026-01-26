/**
 * @file DirettaRingBuffer.h
 * @brief Lock-free ring buffer for Diretta audio streaming
 *
 * Extracted from DirettaSyncAdapter for cleaner architecture.
 * Based on MPD Diretta Output Plugin v0.4.0
 */

#ifndef DIRETTA_RING_BUFFER_H
#define DIRETTA_RING_BUFFER_H

#include <vector>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <new>
#include <type_traits>

// Architecture detection for SIMD support
// Use compiler-defined __AVX2__ to detect actual AVX2 availability
// This respects -march= flags and works correctly on older CPUs (Sandy Bridge, Ivy Bridge)
#if defined(__AVX2__)
    #define DIRETTA_HAS_AVX2 1
    #include <immintrin.h>
#else
    #define DIRETTA_HAS_AVX2 0
#endif

#include "memcpyfast_audio.h"

template <typename T, size_t Alignment>
class AlignedAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using void_pointer = void*;
    using const_void_pointer = const void*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    template <typename U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() noexcept = default;
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    pointer allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        void* ptr = nullptr;
        std::size_t bytes = n * sizeof(T);
#if defined(_MSC_VER)
        ptr = _aligned_malloc(bytes, Alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
#else
        if (posix_memalign(&ptr, Alignment, bytes) != 0) {
            throw std::bad_alloc();
        }
#endif
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
#if defined(_MSC_VER)
        _aligned_free(p);
#else
        free(p);
#endif
    }
};

template <typename T, size_t Alignment>
bool operator==(const AlignedAllocator<T, Alignment>&, const AlignedAllocator<T, Alignment>&) {
    return true;
}

template <typename T, size_t Alignment>
bool operator!=(const AlignedAllocator<T, Alignment>&, const AlignedAllocator<T, Alignment>&) {
    return false;
}

/**
 * @brief Lock-free ring buffer for audio data
 *
 * Supports:
 * - Direct PCM copy
 * - 24-bit packing (4 bytes in -> 3 bytes out)
 * - 16-bit to 32-bit upsampling
 * - DSD planar-to-interleaved conversion with optional bit reversal
 */
class DirettaRingBuffer {
public:
    // DSD conversion mode - determined at track open, eliminates per-iteration branch checks
    // Declared early so it can be used in method signatures
    enum class DSDConversionMode {
        Passthrough,       // Just interleave (DSF→LSB target or DFF→MSB target) - fastest
        BitReverseOnly,    // DSF→MSB or DFF→LSB target
        ByteSwapOnly,      // Endianness conversion only
        BitReverseAndSwap  // Both operations needed
    };

    // Single bit-reversal LUT for all DSD conversion functions (cache-friendly)
    static constexpr uint8_t kBitReverseLUT[256] = {
        0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
        0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
        0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
        0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
        0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
        0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
        0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
        0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
        0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
        0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
        0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
        0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
        0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
        0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
        0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
        0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF
    };

    DirettaRingBuffer() = default;

    /**
     * @brief Resize buffer and set silence byte
     */
    void resize(size_t newSize, uint8_t silenceByte) {
        size_ = roundUpPow2(newSize);
        mask_ = size_ - 1;
        buffer_.resize(size_);
        silenceByte_.store(silenceByte, std::memory_order_release);
        clear();  // Resets all S24 state - hint will be set by caller via setS24PackModeHint()
        fillWithSilence();
    }

    size_t size() const { return size_; }
    uint8_t silenceByte() const { return silenceByte_.load(std::memory_order_acquire); }

    size_t getAvailable() const {
        if (size_ == 0) {
            return 0;
        }
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        return (wp - rp) & mask_;
    }

    size_t getFreeSpace() const {
        if (size_ == 0) {
            return 0;
        }
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        return (rp - wp - 1) & mask_;
    }

    void clear() {
        writePos_.store(0, std::memory_order_release);
        readPos_.store(0, std::memory_order_release);
        // Reset all S24 state to allow fresh detection for new tracks
        // New track will set hint via setS24PackModeHint() if available
        m_s24PackMode = S24PackMode::Unknown;
        m_s24Hint = S24PackMode::Unknown;
        m_s24DetectionConfirmed = false;
        m_deferredSampleCount = 0;
    }

    void fillWithSilence() {
        std::memset(buffer_.data(), silenceByte_.load(std::memory_order_relaxed), size_);
    }

    const uint8_t* getStaging24BitPack() const { return m_staging24BitPack; }
    const uint8_t* getStaging16To32() const { return m_staging16To32; }
    const uint8_t* getStagingDSD() const { return m_stagingDSD; }

    //=========================================================================
    // Direct Write API - eliminates memcpy for contiguous regions
    //=========================================================================

    /**
     * @brief Get direct write pointer for zero-copy writes
     *
     * Returns a pointer to contiguous writable space in the ring buffer.
     * Use this to write directly without staging buffer overhead.
     * Call commitDirectWrite() after writing to advance the write pointer.
     *
     * @param needed Minimum bytes needed
     * @param region Output: pointer to writable region (valid until commitDirectWrite)
     * @param available Output: contiguous bytes available (may be > needed)
     * @return true if contiguous space >= needed available, false if wraparound required
     *
     * Usage:
     *   uint8_t* ptr;
     *   size_t avail;
     *   if (ring.getDirectWriteRegion(size, ptr, avail)) {
     *       memcpy(ptr, data, size);  // or decode directly here
     *       ring.commitDirectWrite(size);
     *   } else {
     *       ring.push(data, size);  // fallback for wraparound case
     *   }
     */
    bool getDirectWriteRegion(size_t needed, uint8_t*& region, size_t& available) {
        if (size_ == 0 || needed == 0) {
            region = nullptr;
            available = 0;
            return false;
        }

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);

        // Calculate free space
        size_t free = (rp > wp) ? (rp - wp - 1) : (size_ - wp + rp - 1);
        if (free < needed) {
            region = nullptr;
            available = 0;
            return false;
        }

        // Calculate contiguous space from write position to end of buffer
        size_t contiguous = size_ - wp;

        // Also consider the read position for wrap case
        if (rp <= wp) {
            // Write position is ahead of or equal to read position
            // Contiguous space is to end of buffer (we can't wrap past read)
            contiguous = size_ - wp;
        } else {
            // Read position is ahead - contiguous space is to read position
            contiguous = rp - wp - 1;
        }

        if (contiguous >= needed) {
            region = buffer_.data() + wp;
            available = contiguous;
            return true;
        }

        // Not enough contiguous space - caller should use fallback
        region = nullptr;
        available = 0;
        return false;
    }

    /**
     * @brief Commit a direct write, advancing the write pointer
     * @param written Number of bytes actually written (must be <= available from getDirectWriteRegion)
     */
    void commitDirectWrite(size_t written) {
        if (written == 0 || size_ == 0) return;
        size_t wp = writePos_.load(std::memory_order_relaxed);
        writePos_.store((wp + written) & mask_, std::memory_order_release);
    }

    /**
     * @brief Get staging buffer for format conversion with direct commit
     *
     * For format conversions that need a staging area, this provides the staging
     * buffer and allows direct commit to ring buffer afterward.
     *
     * @param stagingType Which staging buffer to use (0=24bit, 1=16to32, 2=DSD)
     * @return Pointer to staging buffer (STAGING_SIZE bytes available)
     */
    uint8_t* getStagingForConversion(int stagingType) {
        switch (stagingType) {
            case 0: return m_staging24BitPack;
            case 1: return m_staging16To32;
            case 2: return m_stagingDSD;
            default: return m_staging24BitPack;
        }
    }

    // Expose staging buffer size for callers
    static constexpr size_t getStagingBufferSize() { return STAGING_SIZE; }

    //=========================================================================
    // Push methods (write to buffer)
    //=========================================================================

    /**
     * @brief Push PCM data directly (no conversion)
     *
     * Optimized path: uses direct write when contiguous space available,
     * avoiding the check-then-copy overhead of the wraparound case.
     */
    size_t push(const uint8_t* data, size_t len) {
        if (size_ == 0) return 0;
        size_t free = getFreeSpace();
        if (len > free) len = free;
        if (len == 0) return 0;

        // Fast path: try direct write (no wraparound)
        uint8_t* region;
        size_t available;
        if (getDirectWriteRegion(len, region, available)) {
            memcpy_audio(region, data, len);
            commitDirectWrite(len);
            return len;
        }

        // Slow path: handle wraparound
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - wp);

        memcpy_audio(buffer_.data() + wp, data, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(buffer_.data(), data + firstChunk, len - firstChunk);
        }

        writePos_.store((wp + len) & mask_, std::memory_order_release);
        return len;
    }

    /**
     * @brief Push with 24-bit packing (4 bytes in -> 3 bytes out, S24_P32 format)
     * @return Input bytes consumed
     *
     * S24 mode selection:
     * 1. FFmpeg hint (from codec detection) takes priority - FLAC, ALAC, PCM_S24 are always LSB
     * 2. Sample-based detection only runs when hint is Unknown
     * 3. Timeout defaults to LSB after ~1 second of silence
     *
     * v2.0.0 fix: Trust FFmpeg hint completely - don't let sample detection override it.
     * Sample detection can incorrectly detect MsbAligned when byte 0 happens to be zero
     * (quiet passages) and byte 3 has garbage/sign-extension bits.
     */
    size_t push24BitPacked(const uint8_t* data, size_t inputSize) {
        if (size_ == 0) return 0;
        size_t numSamples = inputSize / 4;
        if (numSamples == 0) return 0;

        size_t maxSamples = STAGING_SIZE / 3;
        size_t free = getFreeSpace();
        size_t maxSamplesByFree = free / 3;

        if (numSamples > maxSamples) numSamples = maxSamples;
        if (numSamples > maxSamplesByFree) numSamples = maxSamplesByFree;
        if (numSamples == 0) return 0;

        prefetch_audio_buffer(data, numSamples * 4);

        // DEBUG: Log first samples to diagnose ARM distortion (remove after debugging)
        static int s_debugLogCount = 0;
        if (s_debugLogCount < 3 && numSamples >= 4) {
            fprintf(stderr, "[S24 DEBUG] First 4 samples (hex): ");
            for (int s = 0; s < 4; s++) {
                fprintf(stderr, "[%02X %02X %02X %02X] ",
                    data[s*4+0], data[s*4+1], data[s*4+2], data[s*4+3]);
            }
            fprintf(stderr, "\n");
            s_debugLogCount++;
        }

        // S24 mode selection - trust FFmpeg hint when available
        // Only run sample detection when hint is Unknown (rare/exotic formats)
        if (m_s24PackMode == S24PackMode::Unknown || m_s24PackMode == S24PackMode::Deferred) {
            // No hint from FFmpeg - try sample-based detection
            S24PackMode detected = detectS24PackMode(data, numSamples);
            if (detected != S24PackMode::Deferred) {
                // Sample detection found result - use it
                m_s24PackMode = detected;
                m_s24DetectionConfirmed = true;
                m_deferredSampleCount = 0;
            } else {
                // Still silence - accumulate count for timeout
                m_deferredSampleCount += numSamples;
                // Timeout: if still silent after threshold, default to LSB (most common)
                if (m_deferredSampleCount > DEFERRED_TIMEOUT_SAMPLES) {
                    m_s24PackMode = S24PackMode::LsbAligned;
                    m_s24DetectionConfirmed = true;
                }
            }
        }
        // When hint is set (LsbAligned/MsbAligned from FFmpeg), trust it completely

        // Use effective mode for conversion (Deferred/Unknown use LSB as fallback)
        S24PackMode effectiveMode = m_s24PackMode;
        if (effectiveMode == S24PackMode::Deferred || effectiveMode == S24PackMode::Unknown) {
            effectiveMode = S24PackMode::LsbAligned;  // Safe default for standard formats
        }

        // ARM64 fix: FFmpeg on ARM produces MSB-aligned S24 data (byte 0 = padding)
        // while x86 FFmpeg produces LSB-aligned (byte 3 = padding)
        #if defined(__aarch64__) || defined(_M_ARM64)
        effectiveMode = S24PackMode::MsbAligned;  // Force MSB for ARM
        #endif

        size_t stagedBytes = (effectiveMode == S24PackMode::MsbAligned)
            ? convert24BitPackedShifted_AVX2(m_staging24BitPack, data, numSamples)
            : convert24BitPacked_AVX2(m_staging24BitPack, data, numSamples);
        size_t written = writeToRing(m_staging24BitPack, stagedBytes);
        size_t samplesWritten = written / 3;

        return samplesWritten * 4;
    }

    /**
     * @brief Push with 16-to-32 bit upsampling
     * @return Input bytes consumed
     */
    size_t push16To32(const uint8_t* data, size_t inputSize) {
        if (size_ == 0) return 0;
        size_t numSamples = inputSize / 2;
        if (numSamples == 0) return 0;

        size_t maxSamples = STAGING_SIZE / 4;
        size_t free = getFreeSpace();
        size_t maxSamplesByFree = free / 4;

        if (numSamples > maxSamples) numSamples = maxSamples;
        if (numSamples > maxSamplesByFree) numSamples = maxSamplesByFree;
        if (numSamples == 0) return 0;

        prefetch_audio_buffer(data, numSamples * 2);

        size_t stagedBytes = convert16To32_AVX2(m_staging16To32, data, numSamples);
        size_t written = writeToRing(m_staging16To32, stagedBytes);
        size_t samplesWritten = written / 4;

        return samplesWritten * 2;
    }

    /**
     * @brief Push with 16-to-24 bit upsampling
     * @return Input bytes consumed
     *
     * Converts 16-bit samples to packed 24-bit format.
     * Used when sink only supports 24-bit (not 32-bit).
     */
    size_t push16To24(const uint8_t* data, size_t inputSize) {
        if (size_ == 0) return 0;
        size_t numSamples = inputSize / 2;
        if (numSamples == 0) return 0;

        size_t maxSamples = STAGING_SIZE / 3;
        size_t free = getFreeSpace();
        size_t maxSamplesByFree = free / 3;

        if (numSamples > maxSamples) numSamples = maxSamples;
        if (numSamples > maxSamplesByFree) numSamples = maxSamplesByFree;
        if (numSamples == 0) return 0;

        prefetch_audio_buffer(data, numSamples * 2);

        size_t stagedBytes = convert16To24(m_staging16To32, data, numSamples);
        size_t written = writeToRing(m_staging16To32, stagedBytes);
        size_t samplesWritten = written / 3;

        return samplesWritten * 2;
    }

    /**
     * @brief Optimized DSD planar push using pre-selected conversion mode
     *
     * Uses specialized conversion functions with no per-iteration branch checks.
     * Mode should be determined at track open and cached in DirettaSync.
     *
     * @param data Planar DSD data
     * @param inputSize Total input size in bytes
     * @param numChannels Number of audio channels
     * @param mode Pre-selected conversion mode (eliminates runtime checks)
     * @return Input bytes consumed
     */
    size_t pushDSDPlanarOptimized(const uint8_t* data, size_t inputSize,
                                   int numChannels, DSDConversionMode mode) {
        if (size_ == 0) return 0;
        if (numChannels == 0) return 0;

        size_t maxBytes = inputSize;
        if (maxBytes > STAGING_SIZE) maxBytes = STAGING_SIZE;
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
                stagedBytes = convertDSD_Passthrough(m_stagingDSD, data, usableInput, numChannels);
                break;
            case DSDConversionMode::BitReverseOnly:
                stagedBytes = convertDSD_BitReverse(m_stagingDSD, data, usableInput, numChannels);
                break;
            case DSDConversionMode::ByteSwapOnly:
                stagedBytes = convertDSD_ByteSwap(m_stagingDSD, data, usableInput, numChannels);
                break;
            case DSDConversionMode::BitReverseAndSwap:
                stagedBytes = convertDSD_BitReverseSwap(m_stagingDSD, data, usableInput, numChannels);
                break;
            default:
                // Fallback to passthrough if unknown mode
                stagedBytes = convertDSD_Passthrough(m_stagingDSD, data, usableInput, numChannels);
                break;
        }

        return writeToRing(m_stagingDSD, stagedBytes);
    }

    //=========================================================================
    // Format conversion functions - with AVX2 optimization on x86
    //=========================================================================

#if DIRETTA_HAS_AVX2
    /**
     * Convert S24_P32 to packed 24-bit using AVX2
     * Input: 4 bytes per sample (24-bit in 32-bit container)
     * Output: 3 bytes per sample (packed)
     * Returns: number of output bytes written
     */
    size_t convert24BitPacked_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;

        static const __m256i shuffle_mask = _mm256_setr_epi8(
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1
        );

        size_t i = 0;
        for (; i + 8 <= numSamples; i += 8) {
            if (i + 16 <= numSamples) {
                _mm_prefetch(reinterpret_cast<const char*>(src + (i + 16) * 4), _MM_HINT_T0);
            }

            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
            __m256i shuffled = _mm256_shuffle_epi8(in, shuffle_mask);

            __m128i lo = _mm256_castsi256_si128(shuffled);
            __m128i hi = _mm256_extracti128_si256(shuffled, 1);

            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), lo);
            uint32_t lo_tail;
            std::memcpy(&lo_tail, reinterpret_cast<const char*>(&lo) + 8, 4);
            std::memcpy(dst + outputBytes + 8, &lo_tail, 4);
            outputBytes += 12;

            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), hi);
            uint32_t hi_tail;
            std::memcpy(&hi_tail, reinterpret_cast<const char*>(&hi) + 8, 4);
            std::memcpy(dst + outputBytes + 8, &hi_tail, 4);
            outputBytes += 12;
        }

        for (; i < numSamples; i++) {
            dst[outputBytes + 0] = src[i * 4 + 0];
            dst[outputBytes + 1] = src[i * 4 + 1];
            dst[outputBytes + 2] = src[i * 4 + 2];
            outputBytes += 3;
        }

        _mm256_zeroupper();
        return outputBytes;
    }

    size_t convert24BitPackedShifted_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;

        static const __m256i shuffle_mask = _mm256_setr_epi8(
            1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, -1, -1, -1, -1,
            1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, -1, -1, -1, -1
        );

        size_t i = 0;
        for (; i + 8 <= numSamples; i += 8) {
            if (i + 16 <= numSamples) {
                _mm_prefetch(reinterpret_cast<const char*>(src + (i + 16) * 4), _MM_HINT_T0);
            }

            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
            __m256i shuffled = _mm256_shuffle_epi8(in, shuffle_mask);

            __m128i lo = _mm256_castsi256_si128(shuffled);
            __m128i hi = _mm256_extracti128_si256(shuffled, 1);

            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), lo);
            uint32_t lo_tail;
            std::memcpy(&lo_tail, reinterpret_cast<const char*>(&lo) + 8, 4);
            std::memcpy(dst + outputBytes + 8, &lo_tail, 4);
            outputBytes += 12;

            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + outputBytes), hi);
            uint32_t hi_tail;
            std::memcpy(&hi_tail, reinterpret_cast<const char*>(&hi) + 8, 4);
            std::memcpy(dst + outputBytes + 8, &hi_tail, 4);
            outputBytes += 12;
        }

        for (; i < numSamples; i++) {
            dst[outputBytes + 0] = src[i * 4 + 1];
            dst[outputBytes + 1] = src[i * 4 + 2];
            dst[outputBytes + 2] = src[i * 4 + 3];
            outputBytes += 3;
        }

        _mm256_zeroupper();
        return outputBytes;
    }

    /**
     * Convert 16-bit to 32-bit using AVX2
     * Input: 2 bytes per sample (16-bit)
     * Output: 4 bytes per sample (16-bit value in upper 16 bits)
     * Returns: number of output bytes written
     */
    size_t convert16To32_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;

        size_t i = 0;
        for (; i + 16 <= numSamples; i += 16) {
            __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 2));
            __m256i zero = _mm256_setzero_si256();

            __m256i lo = _mm256_unpacklo_epi16(zero, in);
            __m256i hi = _mm256_unpackhi_epi16(zero, in);

            __m256i out0 = _mm256_permute2x128_si256(lo, hi, 0x20);
            __m256i out1 = _mm256_permute2x128_si256(lo, hi, 0x31);

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
            outputBytes += 32;
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
            outputBytes += 32;
        }

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

    /**
     * Convert 16-bit to packed 24-bit (scalar - AVX2 shuffle for 3-byte output is complex)
     * Input: 2 bytes per sample (16-bit)
     * Output: 3 bytes per sample (16-bit value in upper bits, LSB padded with 0)
     * Returns: number of output bytes written
     *
     * Note: Scalar is efficient enough for this relatively rare case
     * (16-bit input to 24-bit-only sink)
     */
    size_t convert16To24(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;
        for (size_t i = 0; i < numSamples; i++) {
            dst[outputBytes + 0] = 0x00;              // padding (LSB)
            dst[outputBytes + 1] = src[i * 2 + 0];    // 16-bit LSB
            dst[outputBytes + 2] = src[i * 2 + 1];    // 16-bit MSB
            outputBytes += 3;
        }
        return outputBytes;
    }

#else // !DIRETTA_HAS_AVX2 - Scalar implementations for ARM64 and other architectures

    /**
     * Convert S24_P32 to packed 24-bit (scalar version)
     */
    size_t convert24BitPacked_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;
        for (size_t i = 0; i < numSamples; i++) {
            dst[outputBytes + 0] = src[i * 4 + 0];
            dst[outputBytes + 1] = src[i * 4 + 1];
            dst[outputBytes + 2] = src[i * 4 + 2];
            outputBytes += 3;
        }
        return outputBytes;
    }

    size_t convert24BitPackedShifted_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;
        for (size_t i = 0; i < numSamples; i++) {
            dst[outputBytes + 0] = src[i * 4 + 1];
            dst[outputBytes + 1] = src[i * 4 + 2];
            dst[outputBytes + 2] = src[i * 4 + 3];
            outputBytes += 3;
        }
        return outputBytes;
    }

    /**
     * Convert 16-bit to 32-bit (scalar version)
     */
    size_t convert16To32_AVX2(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;
        for (size_t i = 0; i < numSamples; i++) {
            dst[outputBytes + 0] = 0x00;
            dst[outputBytes + 1] = 0x00;
            dst[outputBytes + 2] = src[i * 2 + 0];
            dst[outputBytes + 3] = src[i * 2 + 1];
            outputBytes += 4;
        }
        return outputBytes;
    }

    /**
     * Convert 16-bit to packed 24-bit (scalar version)
     */
    size_t convert16To24(uint8_t* dst, const uint8_t* src, size_t numSamples) {
        size_t outputBytes = 0;
        for (size_t i = 0; i < numSamples; i++) {
            dst[outputBytes + 0] = 0x00;              // padding (LSB)
            dst[outputBytes + 1] = src[i * 2 + 0];    // 16-bit LSB
            dst[outputBytes + 2] = src[i * 2 + 1];    // 16-bit MSB
            outputBytes += 3;
        }
        return outputBytes;
    }

#endif // DIRETTA_HAS_AVX2

    //=========================================================================
    // Specialized DSD conversion functions - no per-iteration branch checks
    // Mode is determined at track open, eliminating runtime conditionals
    //=========================================================================

    /**
     * DSD Passthrough: Just interleave channels (fastest path)
     * Used when source bit ordering matches target (DSF→LSB or DFF→MSB)
     * NO bit reversal, NO byte swap
     */
    size_t convertDSD_Passthrough(uint8_t* dst, const uint8_t* src,
                                   size_t totalInputBytes, int numChannels) {
        size_t bytesPerChannel = totalInputBytes / static_cast<size_t>(numChannels);
        size_t outputBytes = 0;

#if DIRETTA_HAS_AVX2
        if (numChannels == 2) {
            const uint8_t* srcL = src;
            const uint8_t* srcR = src + bytesPerChannel;

            size_t i = 0;
            for (; i + 32 <= bytesPerChannel; i += 32) {
                __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
                __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

                // NO bit reversal - passthrough
                // NO byte swap - passthrough

                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar tail - no branches
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

            _mm256_zeroupper();
            return outputBytes;
        }
#endif
        // Scalar fallback for non-AVX2 or non-stereo
        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                size_t chOffset = static_cast<size_t>(ch) * bytesPerChannel;
                dst[outputBytes++] = src[chOffset + i + 0];
                dst[outputBytes++] = src[chOffset + i + 1];
                dst[outputBytes++] = src[chOffset + i + 2];
                dst[outputBytes++] = src[chOffset + i + 3];
            }
        }
        return outputBytes;
    }

    /**
     * DSD BitReverse: Apply bit reversal only (no byte swap)
     * Used for DSF→MSB or DFF→LSB target conversions
     */
    size_t convertDSD_BitReverse(uint8_t* dst, const uint8_t* src,
                                  size_t totalInputBytes, int numChannels) {
        size_t bytesPerChannel = totalInputBytes / static_cast<size_t>(numChannels);
        size_t outputBytes = 0;

#if DIRETTA_HAS_AVX2
        if (numChannels == 2) {
            const uint8_t* srcL = src;
            const uint8_t* srcR = src + bytesPerChannel;

            size_t i = 0;
            for (; i + 32 <= bytesPerChannel; i += 32) {
                __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
                __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

                // ALWAYS apply bit reversal
                left = simd_bit_reverse(left);
                right = simd_bit_reverse(right);

                // NO byte swap

                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar tail with bit reversal lookup (using class-scope LUT)
            for (; i + 4 <= bytesPerChannel; i += 4) {
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 0]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 1]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 2]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 3]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 0]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 1]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 2]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 3]];
            }

            _mm256_zeroupper();
            return outputBytes;
        }
#endif
        // Scalar fallback with bit reversal (using class-scope LUT)
        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                size_t chOffset = static_cast<size_t>(ch) * bytesPerChannel;
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 0]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 1]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 2]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 3]];
            }
        }
        return outputBytes;
    }

    /**
     * DSD ByteSwap: Apply byte swap only (no bit reversal)
     * Used for endianness conversion
     */
    size_t convertDSD_ByteSwap(uint8_t* dst, const uint8_t* src,
                                size_t totalInputBytes, int numChannels) {
        size_t bytesPerChannel = totalInputBytes / static_cast<size_t>(numChannels);
        size_t outputBytes = 0;

#if DIRETTA_HAS_AVX2
        if (numChannels == 2) {
            const uint8_t* srcL = src;
            const uint8_t* srcR = src + bytesPerChannel;

            static const __m256i byteswap_mask = _mm256_setr_epi8(
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12
            );

            size_t i = 0;
            for (; i + 32 <= bytesPerChannel; i += 32) {
                __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
                __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

                // NO bit reversal

                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                // ALWAYS apply byte swap
                interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
                interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);

                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar tail with byte swap
            for (; i + 4 <= bytesPerChannel; i += 4) {
                dst[outputBytes++] = srcL[i + 3];
                dst[outputBytes++] = srcL[i + 2];
                dst[outputBytes++] = srcL[i + 1];
                dst[outputBytes++] = srcL[i + 0];
                dst[outputBytes++] = srcR[i + 3];
                dst[outputBytes++] = srcR[i + 2];
                dst[outputBytes++] = srcR[i + 1];
                dst[outputBytes++] = srcR[i + 0];
            }

            _mm256_zeroupper();
            return outputBytes;
        }
#endif
        // Scalar fallback with byte swap
        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                size_t chOffset = static_cast<size_t>(ch) * bytesPerChannel;
                dst[outputBytes++] = src[chOffset + i + 3];
                dst[outputBytes++] = src[chOffset + i + 2];
                dst[outputBytes++] = src[chOffset + i + 1];
                dst[outputBytes++] = src[chOffset + i + 0];
            }
        }
        return outputBytes;
    }

    /**
     * DSD BitReverse + ByteSwap: Apply both operations
     * Used when both bit reversal and endianness conversion are needed
     */
    size_t convertDSD_BitReverseSwap(uint8_t* dst, const uint8_t* src,
                                      size_t totalInputBytes, int numChannels) {
        size_t bytesPerChannel = totalInputBytes / static_cast<size_t>(numChannels);
        size_t outputBytes = 0;

#if DIRETTA_HAS_AVX2
        if (numChannels == 2) {
            const uint8_t* srcL = src;
            const uint8_t* srcR = src + bytesPerChannel;

            static const __m256i byteswap_mask = _mm256_setr_epi8(
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12
            );

            size_t i = 0;
            for (; i + 32 <= bytesPerChannel; i += 32) {
                __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + i));
                __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + i));

                // ALWAYS apply bit reversal
                left = simd_bit_reverse(left);
                right = simd_bit_reverse(right);

                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                // ALWAYS apply byte swap
                interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
                interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);

                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar tail with bit reversal + byte swap (using class-scope LUT)
            for (; i + 4 <= bytesPerChannel; i += 4) {
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 3]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 2]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 1]];
                dst[outputBytes++] = kBitReverseLUT[srcL[i + 0]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 3]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 2]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 1]];
                dst[outputBytes++] = kBitReverseLUT[srcR[i + 0]];
            }

            _mm256_zeroupper();
            return outputBytes;
        }
#endif
        // Scalar fallback with bit reversal + byte swap (using class-scope LUT)
        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                size_t chOffset = static_cast<size_t>(ch) * bytesPerChannel;
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 3]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 2]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 1]];
                dst[outputBytes++] = kBitReverseLUT[src[chOffset + i + 0]];
            }
        }
        return outputBytes;
    }

    //=========================================================================
    // Pop method (read from buffer)
    //=========================================================================

    /**
     * @brief Pop data from buffer
     */
    size_t pop(uint8_t* dest, size_t len) {
        if (size_ == 0) return 0;
        size_t avail = getAvailable();
        if (len > avail) len = avail;
        if (len == 0) return 0;

        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - rp);

        memcpy_audio(dest, buffer_.data() + rp, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(dest + firstChunk, buffer_.data(), len - firstChunk);
        }

        readPos_.store((rp + len) & mask_, std::memory_order_release);
        return len;
    }

    uint8_t* data() { return buffer_.data(); }
    const uint8_t* data() const { return buffer_.data(); }

private:
    /**
     * Write staged data to ring buffer with efficient wraparound handling
     * Uses memcpy_audio_fixed for consistent timing
     */
    size_t writeToRing(const uint8_t* staged, size_t len) {
        size_t size = buffer_.size();
        if (size == 0 || len == 0) return 0;

        size_t writePos = writePos_.load(std::memory_order_relaxed);
        size_t readPos = readPos_.load(std::memory_order_acquire);

        size_t available = (readPos > writePos)
            ? (readPos - writePos - 1)
            : (size - writePos + readPos - 1);

        if (len > available) {
            len = available;
        }
        if (len == 0) return 0;

        uint8_t* ring = buffer_.data();
        size_t firstChunk = std::min(len, size - writePos);

        if (firstChunk > 0) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        }

        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
        }

        size_t newWritePos = (writePos + len) & mask_;
        writePos_.store(newWritePos, std::memory_order_release);

        return len;
    }

#if DIRETTA_HAS_AVX2
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
#endif // DIRETTA_HAS_AVX2

    static size_t roundUpPow2(size_t value) {
        if (value < 2) {
            return 2;
        }
        size_t result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    static constexpr size_t STAGING_SIZE = 65536;
    alignas(64) uint8_t m_staging24BitPack[STAGING_SIZE];
    alignas(64) uint8_t m_staging16To32[STAGING_SIZE];
    alignas(64) uint8_t m_stagingDSD[STAGING_SIZE];

    static constexpr size_t kRingAlignment = 64;

    std::vector<uint8_t, AlignedAllocator<uint8_t, kRingAlignment>> buffer_;
    size_t size_ = 0;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
    std::atomic<uint8_t> silenceByte_{0};

public:
    // S24 pack mode detection - determines byte alignment of 24-bit samples in 32-bit containers
    enum class S24PackMode { Unknown, LsbAligned, MsbAligned, Deferred };

    /**
     * @brief Set S24 pack mode hint from FFmpeg metadata
     *
     * Call this when track info indicates 24-bit content. The hint from FFmpeg
     * codec detection (FLAC, ALAC, PCM_S24) is authoritative - we trust it completely.
     *
     * v2.0.0 fix: Hint now takes full priority. Sample-based detection is only used
     * when FFmpeg cannot determine the format (hint == Unknown).
     */
    void setS24PackModeHint(S24PackMode hint) {
        m_s24Hint = hint;
        if (hint != S24PackMode::Unknown) {
            // FFmpeg provided a definitive hint - use it directly, skip detection
            m_s24PackMode = hint;
            m_s24DetectionConfirmed = true;  // Mark as confirmed so detection won't run
        }
    }

    S24PackMode getS24PackMode() const { return m_s24PackMode; }
    S24PackMode getS24Hint() const { return m_s24Hint; }

private:
    /**
     * Detect S24 pack mode by examining sample data
     *
     * Checks both LSB (byte 0) and MSB (byte 3) positions:
     * - LSB-aligned: data in bytes 0-2, byte 3 is zero (standard S24_LE)
     * - MSB-aligned: data in bytes 1-3, byte 0 is zero (left-justified)
     * - Deferred: all samples are zero (silence) - cannot determine
     */
    S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) const {
        size_t checkSamples = std::min<size_t>(numSamples, 64);
        bool allZeroLSB = true;
        bool allZeroMSB = true;

        for (size_t i = 0; i < checkSamples; i++) {
            uint8_t b0 = data[i * 4];       // LSB position
            uint8_t b3 = data[i * 4 + 3];   // MSB position
            if (b0 != 0x00) allZeroLSB = false;
            if (b3 != 0x00) allZeroMSB = false;
        }

        if (!allZeroLSB && allZeroMSB) {
            return S24PackMode::LsbAligned;  // Data in LSB, MSB is padding
        } else if (allZeroLSB && !allZeroMSB) {
            return S24PackMode::MsbAligned;  // Data in MSB, LSB is padding
        } else if (allZeroLSB && allZeroMSB) {
            return S24PackMode::Deferred;    // Silence - can't determine yet
        }
        // Both non-zero - ambiguous, default to LSB (more common)
        return S24PackMode::LsbAligned;
    }

    S24PackMode m_s24PackMode = S24PackMode::Unknown;
    S24PackMode m_s24Hint = S24PackMode::Unknown;
    bool m_s24DetectionConfirmed = false;
    size_t m_deferredSampleCount = 0;
    static constexpr size_t DEFERRED_TIMEOUT_SAMPLES = 48000;  // ~1 second at 48kHz
};

#endif // DIRETTA_RING_BUFFER_H
