# Direct PCM Fast Path and Ring Buffer Direct Write Design

**Date:** 2026-01-16
**Status:** Draft
**Goal:** Eliminate FFmpeg decode overhead for WAV files and reduce memory copies in the PCM path

## Overview

This design implements two complementary optimizations for uncompressed PCM playback (WAV files):

1. **Direct PCM Fast Path:** Bypass FFmpeg's avcodec_* APIs for WAV files where packet payload IS the raw PCM data
2. **Ring Buffer Direct Write:** Expose a contiguous write span API to eliminate staging buffer copies

Together, these optimizations reduce the copy count from 2-3 to 0-1 for 32-bit WAV playback.

## Background

### Current Data Flow (WAV/AIFF)

```
av_read_frame() → avcodec_send_packet() → avcodec_receive_frame() → [swr/bypass] → AudioBuffer → sendAudio() → staging buffer → ring buffer
```

### Current Data Flow (DSD - already optimized)

```
av_read_frame() → packet data direct → AudioBuffer → ring buffer
```

### Key Insight

For uncompressed PCM codecs (PCM_S16LE, PCM_S24LE, PCM_S32LE), FFmpeg's "decoder" is essentially a pass-through that copies packet data to frame data. The `avcodec_send_packet()` and `avcodec_receive_frame()` calls add overhead without doing meaningful work.

## Target Hardware

- AMD Zen 4 (little-endian x86-64)
- Big-endian formats (AIFF) not required

## Optimization 1: Direct PCM Fast Path

### Detection

In `AudioDecoder::open()`, detect raw PCM codecs:

```cpp
bool isRawPCM = (
    codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
    codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
    codecpar->codec_id == AV_CODEC_ID_PCM_S32LE
);

if (isRawPCM) {
    m_rawPCM = true;
    m_pcmPackedBits = (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE) ? 24 : 0;

    // Extract format info from codecpar (don't need codec context)
    m_trackInfo.sampleRate = codecpar->sample_rate;
    m_trackInfo.channels = codecpar->ch_layout.nb_channels;
    m_trackInfo.bitDepth = (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE) ? 16 :
                           (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE) ? 24 : 32;

    // Allocate packet for raw reading
    m_packet = av_packet_alloc();

    DEBUG_LOG("[AudioDecoder] Raw PCM mode: " << m_trackInfo.bitDepth << "-bit LE");
    return true;  // Skip codec open
}
```

### Read Path

In `readSamples()`, add raw PCM handling (similar structure to existing `m_rawDSD` path):

```cpp
if (m_rawPCM) {
    if (m_eof) return 0;

    // Calculate bytes needed
    int inputBytesPerSample = (m_pcmPackedBits == 24) ? 3 : (m_trackInfo.bitDepth / 8);
    int outputBytesPerSample = (m_trackInfo.bitDepth == 16) ? 2 : 4;  // S24 uses S32 container
    size_t bytesPerFrame = inputBytesPerSample * m_trackInfo.channels;
    size_t outputBytesPerFrame = outputBytesPerSample * m_trackInfo.channels;

    size_t totalBytesNeeded = numSamples * outputBytesPerFrame;

    // Ensure buffer is large enough
    if (buffer.size() < totalBytesNeeded) {
        buffer.resize(totalBytesNeeded);
    }

    uint8_t* outputPtr = buffer.data();
    size_t samplesWritten = 0;

    // Use remainder from previous read
    if (m_pcmRemainderCount > 0) {
        size_t remainderSamples = m_pcmRemainderCount / bytesPerFrame;
        size_t samplesToUse = std::min(remainderSamples, numSamples);
        size_t bytesToUse = samplesToUse * bytesPerFrame;

        if (m_pcmPackedBits == 24) {
            // Expand 3-byte to 4-byte (S24_P32)
            expand24To32(outputPtr, m_pcmRemainder.data(), samplesToUse * m_trackInfo.channels);
            outputPtr += samplesToUse * outputBytesPerFrame;
        } else {
            memcpy_audio(outputPtr, m_pcmRemainder.data(), bytesToUse);
            outputPtr += bytesToUse;
        }

        samplesWritten += samplesToUse;

        // Shift remainder
        size_t leftover = m_pcmRemainderCount - bytesToUse;
        if (leftover > 0) {
            memmove(m_pcmRemainder.data(), m_pcmRemainder.data() + bytesToUse, leftover);
        }
        m_pcmRemainderCount = leftover;
    }

    // Read packets until we have enough
    while (samplesWritten < numSamples && !m_eof) {
        int ret = av_read_frame(m_formatContext, m_packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                m_eof = true;
            }
            break;
        }

        if (m_packet->stream_index != m_audioStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        // Process packet data directly
        const uint8_t* packetData = m_packet->data;
        size_t packetSize = m_packet->size;
        size_t packetSamples = packetSize / bytesPerFrame;
        size_t samplesNeeded = numSamples - samplesWritten;
        size_t samplesToUse = std::min(packetSamples, samplesNeeded);

        if (m_pcmPackedBits == 24) {
            // Expand 3-byte to 4-byte (S24_P32, sign-extended)
            expand24To32(outputPtr, packetData, samplesToUse * m_trackInfo.channels);
            outputPtr += samplesToUse * outputBytesPerFrame;
        } else {
            size_t bytesToCopy = samplesToUse * bytesPerFrame;
            memcpy_audio(outputPtr, packetData, bytesToCopy);
            outputPtr += bytesToCopy;
        }

        samplesWritten += samplesToUse;

        // Store excess in remainder buffer
        if (samplesToUse < packetSamples) {
            size_t excessBytes = (packetSamples - samplesToUse) * bytesPerFrame;
            if (m_pcmRemainder.size() < excessBytes) {
                m_pcmRemainder.resize(excessBytes);
            }
            memcpy_audio(m_pcmRemainder.data(),
                        packetData + samplesToUse * bytesPerFrame,
                        excessBytes);
            m_pcmRemainderCount = excessBytes;
        }

        av_packet_unref(m_packet);
    }

    return samplesWritten;
}
```

**Correctness note:** When expanding 24-bit PCM into 32-bit containers, the top byte must be
sign-extended from bit 23. Zero-filling produces unsigned samples and audible distortion if the
sink expects signed 32-bit PCM.

### S24 Expansion Helper

```cpp
// Expand packed 24-bit (3 bytes) to S32 (4 bytes, sign-extended)
void AudioDecoder::expand24To32(uint8_t* dst, const uint8_t* src, size_t numSamples) {
    for (size_t i = 0; i < numSamples; i++) {
        dst[i * 4 + 0] = src[i * 3 + 0];  // LSB
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];  // MSB of 24-bit (bit 23)
        // Sign extend: replicate bit 23 into the top byte
        dst[i * 4 + 3] = (src[i * 3 + 2] & 0x80) ? 0xFF : 0x00;
    }
}
```

**Note:** This can be SIMD-optimized later if profiling shows it's a bottleneck.

### New Members in AudioDecoder

```cpp
// Raw PCM mode (WAV direct read without FFmpeg decode)
bool m_rawPCM = false;
int m_pcmPackedBits = 0;              // 24 if S24LE (3-byte packed), else 0
std::vector<uint8_t> m_pcmRemainder;  // Partial packet buffer
size_t m_pcmRemainderCount = 0;
```

### Seek Handling

In `AudioDecoder::seek()`, clear the remainder buffer:

```cpp
if (m_rawPCM) {
    m_pcmRemainderCount = 0;
    m_eof = false;
    // av_seek_frame() for container-level seek
    return av_seek_frame(...) >= 0;
}
```

## Optimization 2: Ring Buffer Direct Write

### New API in DirettaRingBuffer

```cpp
struct WriteSpan {
    uint8_t* ptr;       // Pointer to write location (nullptr if no space)
    size_t maxBytes;    // Contiguous bytes available (up to wrap point)
};

/**
 * @brief Get contiguous writable region without wrap-around
 *
 * Returns a span where the caller can write directly. The span ends
 * at either the buffer wrap point or the read position, whichever is closer.
 *
 * After writing, call commitWrite() to advance the write pointer.
 */
WriteSpan getWriteSpan() const {
    if (size_ == 0) return { nullptr, 0 };

    size_t wp = writePos_.load(std::memory_order_acquire);
    size_t rp = readPos_.load(std::memory_order_acquire);

    // Total free space (leave 1 byte to distinguish full from empty)
    size_t totalFree = (rp - wp - 1) & mask_;
    if (totalFree == 0) return { nullptr, 0 };

    // Contiguous space from write position to end of buffer
    size_t toEnd = size_ - wp;

    // Return the smaller of contiguous space or total free space
    size_t contiguous = std::min(toEnd, totalFree);

    return { buffer_.data() + wp, contiguous };
}

/**
 * @brief Commit bytes after direct write
 *
 * Call this after writing to the span returned by getWriteSpan().
 * Only call with bytes <= the maxBytes returned by getWriteSpan().
 */
void commitWrite(size_t bytes) {
    if (bytes == 0) return;
    size_t wp = writePos_.load(std::memory_order_relaxed);
    writePos_.store((wp + bytes) & mask_, std::memory_order_release);
}
```

### Usage in DirettaSync::sendAudio()

```cpp
size_t DirettaSync::sendAudio(const uint8_t* data, size_t numSamples) {
    // ... existing format checks ...

    // Fast path: 32-bit PCM with no conversion needed
    if (!m_isDsdMode && !m_need24BitPack && !m_need16To32Upsample) {
        size_t inputBytes = numSamples * m_inputBytesPerSample * m_channels;

        auto span = m_ringBuffer.getWriteSpan();
        if (span.maxBytes > 0) {
            size_t toCopy = std::min(inputBytes, span.maxBytes);
            // Align to frame boundary
            size_t frameSize = m_inputBytesPerSample * m_channels;
            toCopy = (toCopy / frameSize) * frameSize;

            if (toCopy > 0) {
                memcpy_audio(span.ptr, data, toCopy);
                m_ringBuffer.commitWrite(toCopy);
                return toCopy;
            }
        }

        // Fall back to push() if contiguous space unavailable
        return m_ringBuffer.push(data, inputBytes);
    }

    // ... existing conversion paths (24-bit pack, 16→32, DSD) ...
}
```

## Combined Fast Path

For 32-bit WAV files at matching sample rate, the optimized path is:

```
av_read_frame() → packet.data → ring buffer (via getWriteSpan())
```

**Copies eliminated:**
- avcodec frame allocation/copy (eliminated)
- AudioBuffer intermediate (eliminated for direct write cases)
- Staging buffer (eliminated)

**Decision matrix in sendAudio():**

| Input Format | Output Bits | Ring Buffer API | Copies |
|--------------|-------------|-----------------|--------|
| S32LE WAV | 32 | `getWriteSpan()` direct | **0** |
| S24LE WAV | 24 (S24_P32) | `push24BitPacked()` | 1 |
| S16LE WAV | 16 | `push()` direct | **0** |
| S16LE WAV | 32 | `push16To32()` | 1 |
| FLAC/ALAC | any | existing bypass/swr | 1-2 |

## Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `m_rawPCM`, `m_pcmPackedBits`, `m_pcmRemainder`, `m_pcmRemainderCount` to `AudioDecoder`; add `expand24To32()` method |
| `src/AudioEngine.cpp` | Raw PCM detection in `open()` (~line 284); new rawPCM read path in `readSamples()`; seek handling for rawPCM |
| `src/DirettaRingBuffer.h` | Add `WriteSpan` struct, `getWriteSpan()`, `commitWrite()` methods |
| `src/DirettaSync.cpp` | Use direct write path for non-converting 32-bit PCM |

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Packet size not sample-aligned | Buffer remainder, combine with next packet |
| Ring buffer full | Return partial bytes consumed, caller retries |
| Seek operation | Clear `m_pcmRemainder`, reset `m_eof` |
| HTTP streaming WAV | Works - `av_read_frame()` handles network buffering |
| Format change mid-playlist | Existing format change detection triggers reopen |
| Gapless WAV→WAV | Works - both tracks use rawPCM path |
| WAV→FLAC transition | rawPCM flag reset on new track open |

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Packet boundaries misaligned | Low | Medium | Remainder buffer handles partial samples |
| Raw PCM flag set incorrectly | Very Low | High | Conservative detection: exact codec ID match only |
| Direct write corrupts ring | Low | High | `commitWrite()` only advances after successful copy |
| S24 expansion wrong byte order | Low | High | Unit test against FFmpeg decode output |
| HTTP packet fragmentation | Very Low | Low | `av_read_frame()` assembles complete packets |

## Testing Plan

1. **Bit-perfect verification:**
   - Decode same WAV file via rawPCM path and FFmpeg decode path
   - Compare output byte-for-byte

2. **Format coverage:**
   - S16LE: 44.1k, 96k, 192k
   - S24LE: 44.1k, 96k, 192k, 384k
   - S32LE: 44.1k, 96k, 192k, 384k

3. **Seek test:**
   - Seek to middle of WAV file
   - Verify `m_pcmRemainder` clears
   - Verify audio resumes correctly

4. **Stress test:**
   - 384kHz/32-bit WAV continuous playback
   - Monitor for underruns

5. **Gapless test:**
   - WAV→WAV transition
   - Verify no glitch at boundary

6. **Mixed format test:**
   - WAV→FLAC→WAV playlist
   - Verify correct path selection for each track

## Performance Expectations

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| CPU per frame (192k/32-bit WAV) | ~15-20μs | ~2-5μs | 4-10x |
| Memory copies per sample | 2-3 | 0-1 | 2-3x fewer |
| FFmpeg API calls per frame | 3 | 1 | 3x fewer |
| Cache pressure | Higher | Lower | Better locality |

## Future Optimizations (Phase 2+)

These are deferred to later phases:

- **AudioBuffer capacity-based growth:** Avoid realloc on every resize
- **SIMD S24 expansion:** Vectorize `expand24To32()` if it becomes a bottleneck
- **Resampler direct to FIFO:** Skip temp buffer when swr_convert output exceeds needed samples
