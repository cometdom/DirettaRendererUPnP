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
#include <cstdint>
#include <cstring>
#include <algorithm>

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
private:
    // S24 format auto-detection (declared early for use in public methods)
    enum class S24PackMode { Unknown, LsbAligned, MsbAligned };
    S24PackMode m_s24PackMode = S24PackMode::Unknown;

public:
    DirettaRingBuffer() = default;

    /**
     * @brief Resize buffer and set silence byte
     */
    void resize(size_t newSize, uint8_t silenceByte) {
        buffer_.resize(newSize);
        size_ = newSize;
        silenceByte_ = silenceByte;
        clear();
        fillWithSilence();
        m_s24PackMode = S24PackMode::Unknown;  // Reset S24 detection on resize
    }

    size_t size() const { return size_; }
    uint8_t silenceByte() const { return silenceByte_; }

    size_t getAvailable() const {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        return (wp >= rp) ? (wp - rp) : (size_ - rp + wp);
    }

    size_t getFreeSpace() const {
        return size_ - getAvailable() - 1;
    }

    void clear() {
        writePos_.store(0, std::memory_order_release);
        readPos_.store(0, std::memory_order_release);
        m_s24PackMode = S24PackMode::Unknown;  // Reset S24 detection on clear
    }

    void fillWithSilence() {
        std::memset(buffer_.data(), silenceByte_, size_);
    }

    //=========================================================================
    // Push methods (write to buffer)
    //=========================================================================

    /**
     * @brief Push PCM data directly (no conversion)
     */
    size_t push(const uint8_t* data, size_t len) {
        size_t free = getFreeSpace();
        if (len > free) len = free;
        if (len == 0) return 0;

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - wp);

        std::memcpy(buffer_.data() + wp, data, firstChunk);
        if (firstChunk < len) {
            std::memcpy(buffer_.data(), data + firstChunk, len - firstChunk);
        }

        writePos_.store((wp + len) % size_, std::memory_order_release);
        return len;
    }

    /**
     * @brief Push with 24-bit packing (4 bytes in -> 3 bytes out, S24_P32 format)
     * Auto-detects whether input is LSB-aligned (bytes 0-2) or MSB-aligned (bytes 1-3)
     * @return Input bytes consumed
     */
    size_t push24BitPacked(const uint8_t* data, size_t inputSize) {
        size_t numSamples = inputSize / 4;
        size_t outSize = numSamples * 3;
        size_t free = getFreeSpace();

        if (outSize > free) {
            numSamples = free / 3;
            outSize = numSamples * 3;
        }
        if (numSamples == 0) return 0;

        // Auto-detect S24 format on first push after clear/resize
        if (m_s24PackMode == S24PackMode::Unknown) {
            m_s24PackMode = detectS24PackMode(data, numSamples);
        }

        size_t wp = writePos_.load(std::memory_order_acquire);

        // Byte offset: 0 for LSB-aligned (bytes 0-2), 1 for MSB-aligned (bytes 1-3)
        const int byteOffset = (m_s24PackMode == S24PackMode::MsbAligned) ? 1 : 0;

        for (size_t i = 0; i < numSamples; i++) {
            const uint8_t* src = data + i * 4 + byteOffset;
            size_t dstPos = (wp + i * 3) % size_;

            buffer_[dstPos] = src[0];
            buffer_[(dstPos + 1) % size_] = src[1];
            buffer_[(dstPos + 2) % size_] = src[2];
        }

        writePos_.store((wp + outSize) % size_, std::memory_order_release);
        return numSamples * 4;  // Return input bytes consumed
    }

    /**
     * @brief Push with 16-to-32 bit upsampling
     * @return Input bytes consumed
     */
    size_t push16To32(const uint8_t* data, size_t inputSize) {
        size_t numSamples = inputSize / 2;
        size_t outSize = numSamples * 4;
        size_t free = getFreeSpace();

        if (outSize > free) {
            numSamples = free / 4;
            outSize = numSamples * 4;
        }
        if (numSamples == 0) return 0;

        size_t wp = writePos_.load(std::memory_order_acquire);

        for (size_t i = 0; i < numSamples; i++) {
            const uint8_t* src = data + i * 2;
            size_t dstPos = (wp + i * 4) % size_;

            // Convert 16-bit to 32-bit: shift left by 16 bits (little-endian)
            buffer_[dstPos] = 0;
            buffer_[(dstPos + 1) % size_] = 0;
            buffer_[(dstPos + 2) % size_] = src[0];
            buffer_[(dstPos + 3) % size_] = src[1];
        }

        writePos_.store((wp + outSize) % size_, std::memory_order_release);
        return inputSize;
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
        size_t bytesPerChannel = inputSize / numChannels;
        size_t completeGroups = bytesPerChannel / 4;
        size_t usableOutput = completeGroups * 4 * numChannels;
        size_t free = getFreeSpace();

        if (usableOutput > free) {
            completeGroups = free / (4 * numChannels);
            usableOutput = completeGroups * 4 * numChannels;
        }
        if (completeGroups == 0) return 0;

        size_t wp = writePos_.load(std::memory_order_acquire);

        // Pack planar data into 4-byte groups per channel
        for (size_t g = 0; g < completeGroups; g++) {
            for (int c = 0; c < numChannels; c++) {
                const uint8_t* channelData = data + c * bytesPerChannel;
                size_t srcOffset = g * 4;
                size_t dstPos = (wp + g * 4 * numChannels + c * 4) % size_;

                uint8_t b0 = channelData[srcOffset];
                uint8_t b1 = channelData[srcOffset + 1];
                uint8_t b2 = channelData[srcOffset + 2];
                uint8_t b3 = channelData[srcOffset + 3];

                // Apply bit reversal if needed (MSB<->LSB conversion)
                if (bitReverseTable) {
                    b0 = bitReverseTable[b0];
                    b1 = bitReverseTable[b1];
                    b2 = bitReverseTable[b2];
                    b3 = bitReverseTable[b3];
                }

                // Write bytes - swap order for LITTLE endian targets
                if (byteSwap) {
                    buffer_[dstPos] = b3;
                    buffer_[(dstPos + 1) % size_] = b2;
                    buffer_[(dstPos + 2) % size_] = b1;
                    buffer_[(dstPos + 3) % size_] = b0;
                } else {
                    buffer_[dstPos] = b0;
                    buffer_[(dstPos + 1) % size_] = b1;
                    buffer_[(dstPos + 2) % size_] = b2;
                    buffer_[(dstPos + 3) % size_] = b3;
                }
            }
        }

        writePos_.store((wp + usableOutput) % size_, std::memory_order_release);
        return completeGroups * 4 * numChannels;  // Return input bytes consumed
    }

    //=========================================================================
    // Pop method (read from buffer)
    //=========================================================================

    /**
     * @brief Pop data from buffer
     */
    size_t pop(uint8_t* dest, size_t len) {
        size_t avail = getAvailable();
        if (len > avail) len = avail;
        if (len == 0) return 0;

        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - rp);

        std::memcpy(dest, buffer_.data() + rp, firstChunk);
        if (firstChunk < len) {
            std::memcpy(dest + firstChunk, buffer_.data(), len - firstChunk);
        }

        readPos_.store((rp + len) % size_, std::memory_order_release);
        return len;
    }

    uint8_t* data() { return buffer_.data(); }
    const uint8_t* data() const { return buffer_.data(); }

private:
    /**
     * @brief S24 format auto-detection
     * Detects whether 24-bit samples are LSB-aligned (bytes 0-2) or MSB-aligned (bytes 1-3)
     */
    S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) {
        // Check first 32 samples to determine alignment
        size_t checkSamples = std::min<size_t>(numSamples, 32);
        for (size_t i = 0; i < checkSamples; i++) {
            // If byte 0 is non-zero, data is in bytes 0-2 (LSB-aligned, standard S24_LE)
            if (data[i * 4] != 0x00) {
                return S24PackMode::LsbAligned;
            }
        }
        // All byte 0s are zero, assume data is in bytes 1-3 (MSB-aligned, S24_32BE-style)
        return S24PackMode::MsbAligned;
    }

    std::vector<uint8_t> buffer_;
    size_t size_ = 0;
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
    uint8_t silenceByte_ = 0;
};

#endif // DIRETTA_RING_BUFFER_H
