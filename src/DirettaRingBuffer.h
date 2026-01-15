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
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
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
    DirettaRingBuffer() = default;

    /**
     * @brief Resize buffer and set silence byte
     */
    void resize(size_t newSize, uint8_t silenceByte) {
        size_ = roundUpPow2(newSize);
        mask_ = size_ - 1;
        buffer_.resize(size_);
        silenceByte_.store(silenceByte, std::memory_order_release);
        clear();
        fillWithSilence();
        m_s24PackMode = S24PackMode::Unknown;
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
        m_s24PackMode = S24PackMode::Unknown;
    }

    void fillWithSilence() {
        std::memset(buffer_.data(), silenceByte_.load(std::memory_order_relaxed), size_);
    }

    const uint8_t* getStaging24BitPack() const { return m_staging24BitPack; }
    const uint8_t* getStaging16To32() const { return m_staging16To32; }
    const uint8_t* getStagingDSD() const { return m_stagingDSD; }

    //=========================================================================
    // Push methods (write to buffer)
    //=========================================================================

    /**
     * @brief Push PCM data directly (no conversion)
     */
    size_t push(const uint8_t* data, size_t len) {
        if (size_ == 0) return 0;
        size_t free = getFreeSpace();
        if (len > free) len = free;
        if (len == 0) return 0;

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

        if (m_s24PackMode == S24PackMode::Unknown) {
            m_s24PackMode = detectS24PackMode(data, numSamples);
        }

        size_t stagedBytes = (m_s24PackMode == S24PackMode::MsbAligned)
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
     * @brief Push DSD data from PLANAR input (FFmpeg format)
     *
     * Input format: [L0 L1 L2 L3...][R0 R1 R2 R3...] (planar, per-channel blocks)
     * Output format: 4-byte groups per channel, interleaved
     *
     * @param data Planar DSD data
     * @param inputSize Total input size in bytes
     * @param numChannels Number of audio channels
     * @param bitReverseTable Lookup table for MSB<->LSB conversion (nullptr if not needed)
     * @param byteSwap If true, swap byte order within 4-byte groups (for LITTLE endian targets)
     * @return Input bytes consumed
     */
    size_t pushDSDPlanar(const uint8_t* data, size_t inputSize, int numChannels,
                         const uint8_t* bitReverseTable, bool byteSwap = false) {
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

        size_t stagedBytes = convertDSDPlanar_AVX2(
            m_stagingDSD, data, usableInput, numChannels, bitReverseTable, byteSwap);
        size_t written = writeToRing(m_stagingDSD, stagedBytes);

        return written;
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

#endif // DIRETTA_HAS_AVX2

    /**
     * Convert DSD planar to interleaved
     * Input: [L channel bytes][R channel bytes] planar
     * Output: [4B L][4B R][4B L][4B R]... interleaved
     */
    size_t convertDSDPlanar_AVX2(
        uint8_t* dst,
        const uint8_t* src,
        size_t totalInputBytes,
        int numChannels,
        const uint8_t* bitReversalTable,
        bool needByteSwap
    ) {
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

                if (bitReversalTable) {
                    left = simd_bit_reverse(left);
                    right = simd_bit_reverse(right);
                }

                __m256i interleaved_lo = _mm256_unpacklo_epi32(left, right);
                __m256i interleaved_hi = _mm256_unpackhi_epi32(left, right);

                if (needByteSwap) {
                    interleaved_lo = _mm256_shuffle_epi8(interleaved_lo, byteswap_mask);
                    interleaved_hi = _mm256_shuffle_epi8(interleaved_hi, byteswap_mask);
                }

                __m256i out0 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x20);
                __m256i out1 = _mm256_permute2x128_si256(interleaved_lo, interleaved_hi, 0x31);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out0);
                outputBytes += 32;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + outputBytes), out1);
                outputBytes += 32;
            }

            // Scalar tail for remaining bytes
            for (; i + 4 <= bytesPerChannel; i += 4) {
                uint8_t groupL[4];
                for (int j = 0; j < 4; j++) {
                    uint8_t b = srcL[i + j];
                    if (bitReversalTable) b = bitReversalTable[b];
                    groupL[j] = b;
                }
                if (needByteSwap) {
                    dst[outputBytes++] = groupL[3];
                    dst[outputBytes++] = groupL[2];
                    dst[outputBytes++] = groupL[1];
                    dst[outputBytes++] = groupL[0];
                } else {
                    dst[outputBytes++] = groupL[0];
                    dst[outputBytes++] = groupL[1];
                    dst[outputBytes++] = groupL[2];
                    dst[outputBytes++] = groupL[3];
                }

                uint8_t groupR[4];
                for (int j = 0; j < 4; j++) {
                    uint8_t b = srcR[i + j];
                    if (bitReversalTable) b = bitReversalTable[b];
                    groupR[j] = b;
                }
                if (needByteSwap) {
                    dst[outputBytes++] = groupR[3];
                    dst[outputBytes++] = groupR[2];
                    dst[outputBytes++] = groupR[1];
                    dst[outputBytes++] = groupR[0];
                } else {
                    dst[outputBytes++] = groupR[0];
                    dst[outputBytes++] = groupR[1];
                    dst[outputBytes++] = groupR[2];
                    dst[outputBytes++] = groupR[3];
                }
            }

            _mm256_zeroupper();
        } else {
            outputBytes = convertDSDPlanar_Scalar(dst, src, totalInputBytes, numChannels,
                                                  bitReversalTable, needByteSwap);
        }
#else
        // Pure scalar implementation for ARM64 and other architectures
        outputBytes = convertDSDPlanar_Scalar(dst, src, totalInputBytes, numChannels,
                                              bitReversalTable, needByteSwap);
#endif

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

        if (firstChunk >= 32) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        } else if (firstChunk > 0) {
            std::memcpy(ring + writePos, staged, firstChunk);
        }

        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            if (secondChunk >= 32) {
                memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
            } else {
                std::memcpy(ring, staged + firstChunk, secondChunk);
            }
        }

        size_t newWritePos = (writePos + len) % size;
        writePos_.store(newWritePos, std::memory_order_release);

        return len;
    }

    size_t convertDSDPlanar_Scalar(
        uint8_t* dst,
        const uint8_t* src,
        size_t totalInputBytes,
        int numChannels,
        const uint8_t* bitReversalTable,
        bool needByteSwap
    ) {
        size_t bytesPerChannel = totalInputBytes / static_cast<size_t>(numChannels);
        size_t outputBytes = 0;

        for (size_t i = 0; i < bytesPerChannel; i += 4) {
            for (int ch = 0; ch < numChannels; ch++) {
                uint8_t group[4] = {0, 0, 0, 0};
                for (int j = 0; j < 4 && (i + static_cast<size_t>(j)) < bytesPerChannel; j++) {
                    uint8_t b = src[static_cast<size_t>(ch) * bytesPerChannel + i + static_cast<size_t>(j)];
                    if (bitReversalTable) b = bitReversalTable[b];
                    group[j] = b;
                }

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

    enum class S24PackMode { Unknown, LsbAligned, MsbAligned };
    S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) const {
        size_t checkSamples = std::min<size_t>(numSamples, 32);
        for (size_t i = 0; i < checkSamples; i++) {
            if (data[i * 4] != 0x00) {
                return S24PackMode::LsbAligned;
            }
        }
        return S24PackMode::MsbAligned;
    }
    S24PackMode m_s24PackMode = S24PackMode::Unknown;
};

#endif // DIRETTA_RING_BUFFER_H
