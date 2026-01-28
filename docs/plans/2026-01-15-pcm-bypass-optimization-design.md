# PCM Bypass and Bit-Perfect Optimization Design

**Date:** 2026-01-15
**Status:** Approved
**Goal:** Achieve bit-perfect PCM playback with reduced CPU overhead

## Overview

This design implements five optimizations to eliminate unnecessary audio processing when the source format matches the output format, and to improve robustness and efficiency in the PCM path.

## Optimizations

### 1. True PCM Bypass

**Problem:** Current code always creates `SwrContext` even when no conversion is needed.

**Solution:** Detect when formats match exactly and skip SwrContext creation entirely.

**CRITICAL:** Request packed format BEFORE `avcodec_open2()` - FFmpeg ignores it afterward.

In `AudioDecoder::open()`, after `avcodec_parameters_to_context()` but BEFORE `avcodec_open2()`:

```cpp
// Request packed output format from decoder (integer formats ONLY)
// IMPORTANT: Float formats (FLT, FLTP) are NOT eligible for bypass
// because downstream expects signed integer PCM. Float would corrupt audio.
AVSampleFormat preferredFormat = AV_SAMPLE_FMT_NONE;
AVSampleFormat srcFmt = m_codecContext->sample_fmt;

if (srcFmt == AV_SAMPLE_FMT_S16P)
    preferredFormat = AV_SAMPLE_FMT_S16;
else if (srcFmt == AV_SAMPLE_FMT_S32P)
    preferredFormat = AV_SAMPLE_FMT_S32;
// NOTE: Do NOT request FLT for FLTP - float bypass would corrupt audio

// CRITICAL: Check if decoder actually supports packed format before requesting
// Some decoders only support planar output - requesting unsupported format
// causes avcodec_open2() to fail with EINVAL
if (preferredFormat != AV_SAMPLE_FMT_NONE && codec->sample_fmts != nullptr) {
    bool packedSupported = false;
    for (const AVSampleFormat* fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; fmt++) {
        if (*fmt == preferredFormat) {
            packedSupported = true;
            break;
        }
    }
    if (packedSupported) {
        m_codecContext->request_sample_fmt = preferredFormat;
        DEBUG_LOG("[AudioDecoder] Requesting packed format: " << av_get_sample_fmt_name(preferredFormat));
    } else {
        DEBUG_LOG("[AudioDecoder] Packed format not supported by decoder, will use swr");
    }
}

// NOW open the codec
if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
    // error handling...
}
```

**Why capability check is required:** FFmpeg treats `request_sample_fmt` as a hard requirement for many decoders. If a decoder only advertises planar output in `codec->sample_fmts`, requesting packed format causes `avcodec_open2()` to fail with `EINVAL`, breaking playback entirely.

In `AudioDecoder::initResampler()`:

```cpp
// Check if bypass is possible (integer formats only)
bool isIntegerFormat = (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S16 ||
                        m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32);
bool canBypass =
    isIntegerFormat &&
    (m_codecContext->sample_rate == outputRate) &&
    (m_codecContext->ch_layout.nb_channels == m_trackInfo.channels) &&
    formatMatchesOutput(m_codecContext->sample_fmt, outputBits);

if (canBypass) {
    m_bypassMode = true;
    // Skip SwrContext creation entirely
    // Still create m_pcmFifo for overflow handling
    DEBUG_LOG("[AudioDecoder] PCM BYPASS enabled - bit-perfect path");
    return true;
}
```

**IMPORTANT:** Float formats (FLT, FLTP from AAC/Vorbis/etc.) are NEVER bypassed. The downstream pipeline (DirettaSync, ring buffer) interprets 32-bit as signed integer. Bypassing float would produce corrupted audio. SwrContext handles float→S32 conversion.

**New members in AudioDecoder:**
- `bool m_bypassMode = false;`

### 2. Request Packed Output from Decoders

**Problem:** FFmpeg decoders often output planar formats (S16P, S32P, FLTP) requiring conversion.

**Solution:** Use `request_sample_fmt` to ask decoder for packed integer output BEFORE codec open. If decoder honors the request and produces S16/S32, bypass is enabled; if it produces float or planar, fall back to SwrContext.

**Float handling:** Codecs like AAC/Vorbis output FLTP. We do NOT request FLT because float bypass would corrupt audio. These codecs always go through SwrContext for float→integer conversion.

This is integrated with optimization #1 above.

### 3. Hardened 24-bit Pack Detection

**Problem:** Current `detectS24PackMode()` checks first 32 samples for zero bytes. Silence or fade-ins cause mis-detection.

**Solution:** Hybrid approach where sample-based detection takes priority, with FFmpeg hint as fallback only when detection is inconclusive.

**CRITICAL:** The hint is NON-AUTHORITATIVE. `bits_per_coded_sample` only indicates container size, not byte alignment. MSB-aligned 24-in-32 sources (left-justified PCM) would be mispacked if we trusted the hint blindly.

```cpp
enum class S24PackMode { Unknown, LsbAligned, MsbAligned, Deferred };

S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) {
    // Phase 1: ALWAYS try sample-based detection first (authoritative)
    size_t checkSamples = std::min<size_t>(numSamples, 64);
    bool hasNonZeroLsb = false;
    bool hasNonZeroMsb = false;
    bool allZero = true;

    for (size_t i = 0; i < checkSamples; i++) {
        uint8_t b0 = data[i * 4 + 0];  // LSB position
        uint8_t b3 = data[i * 4 + 3];  // MSB position

        if (b0 != 0x00) hasNonZeroLsb = true;
        if (b3 != 0x00) hasNonZeroMsb = true;
        if (b0 != 0x00 || data[i*4+1] != 0x00 || data[i*4+2] != 0x00)
            allZero = false;
    }

    // Phase 2: If samples are conclusive, use them (ignore hint)
    if (hasNonZeroLsb) return S24PackMode::LsbAligned;
    if (!allZero) return S24PackMode::MsbAligned;  // Has non-zero but not in LSB position

    // Phase 3: Samples inconclusive (all zero) - use hint as fallback
    if (m_s24MetadataHint != S24PackMode::Unknown) {
        DEBUG_LOG("[S24] All-zero samples, using metadata hint");
        return m_s24MetadataHint;
    }

    // Phase 4: No hint available - defer decision
    return S24PackMode::Deferred;
}
```

**Priority order:**
1. Sample-based detection (authoritative) - if non-zero samples exist, they determine alignment
2. FFmpeg metadata hint (fallback) - only used when all samples are zero
3. Deferred (last resort) - wait for non-zero audio data

**Deferred handling:** Buffer data and re-check on next call. Use LSB-aligned as safe default after 500ms timeout.

**New members in DirettaRingBuffer:**
- `S24PackMode m_s24MetadataHint = S24PackMode::Unknown;`
- Setter that ALSO resets detection state:

```cpp
void setS24PackHint(S24PackMode hint) {
    m_s24MetadataHint = hint;
    // CRITICAL: Reset pack mode to force re-detection on next push
    // Without this, gapless transitions retain the previous track's
    // pack mode even when alignment differs, corrupting audio
    m_s24PackMode = S24PackMode::Unknown;
}
```

**Why reset is required:** On gapless transitions where the ring buffer isn't resized (same sample rate/bit depth), `push24BitPacked()` only calls `detectS24PackMode()` when `m_s24PackMode == Unknown`. Without resetting, the previous track's pack mode stays latched and the new hint is ignored, potentially packing the next track with wrong byte ordering.

**API propagation path for metadata hint:**

The hint must flow from FFmpeg decoder metadata to the ring buffer. Data path:

```
AudioDecoder (has codecpar->bits_per_raw_sample)
    ↓ via TrackInfo or AudioCallback
DirettaRenderer (receives track info)
    ↓ via DirettaSync API
DirettaSync (owns ring buffer)
    ↓ via new setter
DirettaRingBuffer (uses hint)
```

**Implementation:**

1. In `AudioEngine.h`, extend `TrackInfo`:
```cpp
struct TrackInfo {
    // ... existing fields ...

    // 24-bit alignment hint from FFmpeg (for S24_P32 packing)
    enum class S24Alignment { Unknown, LsbAligned, MsbAligned };
    S24Alignment s24Alignment = S24Alignment::Unknown;
};
```

2. In `AudioDecoder::open()`, detect alignment hint from FFmpeg:
```cpp
// Provide alignment HINT from codec parameters (non-authoritative!)
// NOTE: bits_per_coded_sample only indicates container size, NOT byte alignment
// This hint is ONLY used when sample-based detection sees all-zero data
if (m_trackInfo.bitDepth == 24) {
    if (codecpar->bits_per_coded_sample == 32) {
        // Most 24-in-32 is LSB-aligned, but MSB-aligned exists (left-justified)
        // Mark as hint, sample detection will override if it sees non-zero data
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
    }
    // Note: This is a HINT only - sample-based detection takes priority
}
```

3. In `DirettaSync.h`, add setter:
```cpp
void setS24PackHint(S24PackMode hint) {
    m_ringBuffer.setS24PackHint(hint);
}
```

4. In `DirettaRenderer` (or audio callback), propagate on track change:
```cpp
void onTrackChange(const TrackInfo& info) {
    if (info.bitDepth == 24 && info.s24Alignment != TrackInfo::S24Alignment::Unknown) {
        S24PackMode hint = (info.s24Alignment == TrackInfo::S24Alignment::MsbAligned)
            ? S24PackMode::MsbAligned : S24PackMode::LsbAligned;
        m_direttaSync->setS24PackHint(hint);
    } else {
        m_direttaSync->setS24PackHint(S24PackMode::Unknown);  // Use detection
    }
}
```

### 4. Adaptive PCM Chunk Sizing

**Problem:** Fixed chunk sizes cause bursty writes and jitter when decoder produces variable-sized frames.

**Solution:** Use `getBufferLevel()` to keep ring buffer at ~50% full.

```cpp
size_t AudioEngine::getAdaptiveChunkSize(size_t maxSamples) const {
    float level = m_bufferLevelCallback ? m_bufferLevelCallback() : 0.5f;

    constexpr float TARGET_LEVEL = 0.50f;
    constexpr float DEADBAND = 0.10f;
    constexpr float MIN_SCALE = 0.25f;
    constexpr float MAX_SCALE = 1.50f;

    float scale = 1.0f;
    float deviation = level - TARGET_LEVEL;

    if (deviation > DEADBAND) {
        // Buffer too full - reduce chunk size
        scale = 1.0f - ((deviation - DEADBAND) / (1.0f - TARGET_LEVEL - DEADBAND));
        scale = std::max(scale, MIN_SCALE);
    } else if (deviation < -DEADBAND) {
        // Buffer too empty - increase chunk size
        scale = 1.0f + ((-deviation - DEADBAND) / (TARGET_LEVEL - DEADBAND)) * 0.5f;
        scale = std::min(scale, MAX_SCALE);
    }

    return static_cast<size_t>(maxSamples * scale);
}
```

**New members in AudioEngine:**
- `using BufferLevelCallback = std::function<float()>;`
- `void setBufferLevelCallback(const BufferLevelCallback& cb);`

### 5. Dynamic FIFO Sizing

**Problem:** Fixed 8192-sample FIFO. High-rate streams can overflow; low-rate streams waste memory.

**Solution:** Scale FIFO size based on output rate.

```cpp
constexpr int64_t BASE_RATE = 44100;
constexpr int64_t BASE_FRAME_SIZE = 4096;
constexpr int MIN_FIFO_SAMPLES = 4096;
constexpr int MAX_FIFO_SAMPLES = 32768;

// CRITICAL: Use 64-bit arithmetic to avoid overflow at high sample rates
// At 384kHz: 2 * 384000 * 4096 = 3,145,728,000 which exceeds INT_MAX (2,147,483,647)
int64_t fifoSamples64 = (2 * static_cast<int64_t>(outputRate) * BASE_FRAME_SIZE) / BASE_RATE;
int fifoSamples = static_cast<int>(std::clamp(fifoSamples64,
                                               static_cast<int64_t>(MIN_FIFO_SAMPLES),
                                               static_cast<int64_t>(MAX_FIFO_SAMPLES)));

m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSamples);
```

**Why 64-bit arithmetic:** At 384kHz, the intermediate product `2 * 384000 * 4096 = 3,145,728,000` exceeds `INT_MAX` (2,147,483,647). Without 64-bit arithmetic, the value wraps negative and `std::clamp` produces `MIN_FIFO_SAMPLES`, dramatically undersizing the FIFO and causing underruns.

**Resulting sizes:**

| Output Rate | FIFO Samples | Duration |
|-------------|--------------|----------|
| 44.1 kHz    | 8192         | ~186ms   |
| 96 kHz      | 17825        | ~186ms   |
| 192 kHz     | 32768 (cap)  | ~170ms   |
| 384 kHz     | 32768 (cap)  | ~85ms    |

**Bypass mode:** Smaller FIFO since no resampling expansion:
```cpp
if (m_bypassMode) {
    fifoSamples = std::clamp(BASE_FRAME_SIZE * 2, MIN_FIFO_SAMPLES, 8192);
}
```

## Integration

**Initialization flow in `AudioDecoder::open()`:**

1. `avformat_open_input()` - open container (existing)
2. `avformat_find_stream_info()` - find streams (existing)
3. `avcodec_find_decoder()` - find decoder (existing)
4. `avcodec_alloc_context3()` - allocate codec context (existing)
5. `avcodec_parameters_to_context()` - copy params (existing)
6. **Request packed integer format (NEW)** - set `request_sample_fmt` for S16P→S16, S32P→S32 (NOT for FLTP)
7. `avcodec_open2()` - open codec (existing, but AFTER step 6)
8. Store actual output format for bypass detection
9. Detect S24 alignment hint from `bits_per_coded_sample` (NEW)

**First call to `readSamples()` - lazy resampler init:**

```cpp
if (!m_resamplerInitialized && !m_trackInfo.isDSD) {
    m_bypassMode = canBypass(outputRate, outputBits);
    if (!m_bypassMode) {
        initResampler(outputRate, outputBits);
    }
    initFifo(outputRate, m_bypassMode);
    m_resamplerInitialized = true;
}
```

**Bypass path in decode loop:**

```cpp
if (m_bypassMode) {
    size_t frameSamples = m_frame->nb_samples;
    size_t bytesToCopy = frameSamples * m_bytesPerSample * m_trackInfo.channels;
    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
    // FIFO overflow handling same as before
}
```

## Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `m_bypassMode`, `m_resamplerInitialized` to `AudioDecoder`; extend `TrackInfo` with `S24Alignment` enum |
| `src/AudioEngine.cpp` | Packed format request BEFORE `avcodec_open2()` (line 236) with capability check; bypass flag check (line 710); dynamic FIFO sizing (line 1027); S24 alignment detection |
| `src/DirettaRingBuffer.h` | Add `S24PackMode::Deferred`, `m_s24MetadataHint`, `setS24PackHint()`, `setSampleRate()`, `m_deferredSampleCount`; enhanced `detectS24PackMode()` with sample-first priority; timeout handling in `push24BitPacked()` |
| `src/DirettaSync.h` | Add `setS24PackHint()` to propagate hint; call `setSampleRate()` in `open()` |
| `src/DirettaRenderer.cpp` | Wire adaptive chunk sizing in `audioThreadFunc()` (lines 533, 578); add `calculateAdaptiveChunkSize()` helper; propagate S24 hint on track change |

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Decoder ignores packed request | `canBypass()` returns false, uses swr |
| Decoder only supports planar | Capability check skips request, `avcodec_open2()` succeeds, uses swr |
| Decoder outputs float (FLT/FLTP) | Bypass explicitly disabled, swr converts to S32 |
| AAC/Vorbis/MP3 streams | Always use swr (these output float), no bypass |
| FLAC/ALAC/WAV streams | May bypass if decoder outputs S16/S32 and rate matches |
| Mid-stream format change | Reset `m_resamplerInitialized`, re-evaluate bypass |
| Seek operation | FIFO reset (existing), bypass state preserved |
| 24-bit silence at start | Deferred detection, LSB default after 500ms timeout |
| Buffer callback not set | `getAdaptiveChunkSize()` returns `maxSamples` unchanged |
| S24 hint not propagated | Falls back to sample-based detection (existing behavior) |
| Gapless 24-bit transition | `setS24PackHint()` resets `m_s24PackMode` to force re-detection |
| 384kHz high-rate PCM | 64-bit FIFO arithmetic prevents overflow, correct sizing |

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Bypass incorrectly enabled | Conservative `canBypass()` requires integer format (S16/S32), matching rate, and matching layout |
| Float bypass corruption | Float formats explicitly excluded from bypass check |
| request_sample_fmt breaks codec | Capability check via `codec->sample_fmts` before requesting |
| request_sample_fmt ignored | Check actual format after `avcodec_open2()`, not requested format |
| FIFO overflow at high rates | 64-bit arithmetic prevents `INT_MAX` wraparound |
| FIFO too small at edge rates | Clamped to safe minimum 4096 samples |
| Deferred detection never resolves | Timeout with LSB default after 500ms |
| Adaptive sizing causes underrun | Deadband prevents over-correction, MIN_SCALE = 0.25 |
| S24 hint propagation breaks | Graceful fallback to sample detection if hint is Unknown |
| S24 hint incorrect (MSB source) | Sample-based detection takes priority; hint only used for all-zero data |
| Gapless S24 mode mismatch | `setS24PackHint()` resets `m_s24PackMode` on track change |

## Testing

1. Compare byte-for-byte output with bypass enabled vs disabled for known PCM files (FLAC, WAV)
2. Verify no SwrContext allocation in logs for matching integer formats
3. Verify SwrContext IS created for float formats (AAC, Vorbis, MP3) - bypass must NOT activate
4. Test with silence-leading tracks for 24-bit detection
5. Monitor buffer levels during playback to verify adaptive sizing
6. Test S24 hint propagation by checking logs for "hint: LsbAligned" on 24-bit FLAC
7. Verify `request_sample_fmt` is set BEFORE `avcodec_open2()` in debug logs
8. Test decoder that only supports planar - verify no EINVAL from `avcodec_open2()`
9. Test 384kHz playback - verify FIFO size is 32768 (not collapsed to 4096)
10. Test gapless transition between tracks with different S24 alignment
11. Test MSB-aligned 24-bit source - verify sample detection overrides LSB hint

## Implementation Notes (from review)

These clarifications address specific codebase integration points:

### 1. PCM Bypass Init Flag

**Issue:** `readSamples()` calls `initResampler()` whenever `m_swrContext` is null (src/AudioEngine.cpp:710). Without a flag, bypass mode would continually try to create a resampler.

**Solution:** Add `m_resamplerInitialized` flag to `AudioDecoder`:

```cpp
// In readSamples(), replace current check:
if (!m_swrContext) {  // OLD - keeps trying

// With:
if (!m_resamplerInitialized && !m_trackInfo.isDSD) {
    m_bypassMode = canBypass(outputRate, outputBits);
    if (!m_bypassMode) {
        initResampler(outputRate, outputBits);
    }
    initFifo(outputRate, m_bypassMode);  // FIFO needed even in bypass
    m_resamplerInitialized = true;
}
```

**Existing code reuse:** The no-resample direct copy path already exists (src/AudioEngine.cpp:930). Bypass mode can reuse it once `initResampler()` is skipped.

### 2. S24 Deferred Timeout

**Issue:** `DirettaRingBuffer` has no timing context. The "500ms timeout" cannot be implemented without sample-rate or timestamp information.

**Solution:** Add sample-rate setter from `DirettaSync` (which knows rate):

```cpp
// In DirettaRingBuffer.h
void setSampleRate(uint32_t rate) { m_sampleRate = rate; }

// In push24BitPacked(), track samples processed:
if (m_s24PackMode == S24PackMode::Deferred) {
    m_deferredSampleCount += numSamples;
    // Timeout: 500ms worth of samples
    if (m_sampleRate > 0 && m_deferredSampleCount > m_sampleRate / 2) {
        m_s24PackMode = S24PackMode::LsbAligned;  // Safe default
        DEBUG_LOG("[S24] Timeout after 500ms silence, defaulting to LSB");
    }
}
```

**Caller:** `DirettaSync::open()` calls `m_ringBuffer.setSampleRate(format.sampleRate)`.

### 3. Member Location Correction

**Issue:** Design said "add m_bypassMode to AudioEngine.h" but it belongs on `AudioDecoder`.

**Confirmed location:** `AudioDecoder` class (src/AudioEngine.h:66) - this is where the design's code snippets already place it.

### 4. Adaptive Chunk Size Wiring

**Issue:** `audioThreadFunc()` in DirettaRenderer uses fixed sizes (src/DirettaRenderer.cpp:533, 578). The adaptive sizing method needs to be called there.

**Solution:** Wire adaptive sizing in `DirettaRenderer`, not `AudioEngine`:

```cpp
// In DirettaRenderer.cpp audioThreadFunc()
// Replace fixed size:
size_t samplesNeeded = 4096;  // OLD

// With adaptive sizing:
float bufferLevel = m_direttaSync->getBufferLevel();
size_t samplesNeeded = calculateAdaptiveChunkSize(4096, bufferLevel);
```

**Alternative:** Move `getAdaptiveChunkSize()` to `DirettaRenderer` instead of `AudioEngine`, since `DirettaRenderer` owns the `DirettaSync` instance and can directly query buffer level.

### 5. Existing Code Touchpoints Summary

| Location | Change |
|----------|--------|
| src/AudioEngine.cpp:236 | Insert `request_sample_fmt` + capability check BEFORE `avcodec_open2()` |
| src/AudioEngine.cpp:710 | Add `m_resamplerInitialized` flag check |
| src/AudioEngine.cpp:930 | Reuse existing direct-copy path for bypass |
| src/AudioEngine.cpp:1027 | Replace fixed 8192 FIFO with dynamic sizing |
| src/DirettaRingBuffer.h:625 | Extend S24 detection with hint + deferred mode |
| src/DirettaRenderer.cpp:533,578 | Wire adaptive chunk sizing |
