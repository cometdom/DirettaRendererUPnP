# PCM FIFO and Bypass Optimization

**Date:** 2026-01-15
**Based on:** Optimization designs from @leeeanh fork
**Implementation:** Selective cherry-pick with preservation of existing bug fixes

## Overview

This document describes four interconnected optimizations to the PCM audio path that improve both performance and bit-perfect playback capability. These optimizations were adapted from design documents in the leeeanh fork, with careful preservation of bug fixes already present in the main repository.

## Optimization 1: Enhanced S24 Detection

### Problem

The original S24 (24-bit in 32-bit container) detection had limitations:
- Only checked LSB position, assuming MSB would be zero
- Failed when track starts with silence (all zeros)
- No way to provide hints from decoder metadata

### Solution

Implemented a hybrid detection system with three layers:

1. **Sample-based detection** (highest priority)
   - Checks both LSB (byte 0) and MSB (byte 3) positions
   - Returns definitive result when non-zero samples found
   - Returns `Deferred` state for silence

2. **Hint from FFmpeg metadata** (fallback)
   - Decoder provides alignment hint based on codec ID
   - Used when sample detection sees only silence
   - Can be overridden by subsequent sample detection

3. **Timeout mechanism** (safety)
   - After ~1 second of silence (48000 samples), defaults to LSB
   - Prevents indefinite deferral

### Code Changes

**DirettaRingBuffer.h:**
```cpp
enum class S24PackMode { Unknown, LsbAligned, MsbAligned, Deferred };

void setS24PackModeHint(S24PackMode hint);
S24PackMode getS24PackMode() const;
S24PackMode getS24Hint() const;

// New members
S24PackMode m_s24Hint = S24PackMode::Unknown;
bool m_s24DetectionConfirmed = false;
size_t m_deferredSampleCount = 0;
static constexpr size_t DEFERRED_TIMEOUT_SAMPLES = 48000;
```

**Detection algorithm:**
```cpp
S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) {
    bool allZeroLSB = true, allZeroMSB = true;
    for (size_t i = 0; i < checkSamples; i++) {
        if (data[i * 4] != 0x00) allZeroLSB = false;      // LSB position
        if (data[i * 4 + 3] != 0x00) allZeroMSB = false;  // MSB position
    }

    if (!allZeroLSB && allZeroMSB) return LsbAligned;  // Data in LSB
    if (allZeroLSB && !allZeroMSB) return MsbAligned;  // Data in MSB
    if (allZeroLSB && allZeroMSB) return Deferred;     // Silence
    return LsbAligned;  // Ambiguous - default to LSB
}
```

---

## Optimization 2: AVAudioFifo for PCM Overflow

### Problem

The original PCM overflow handling used a manual buffer with `memmove()`:
- O(n) complexity for shifting remaining samples
- Memory allocation on overflow
- Shared buffer between DSD and PCM paths

### Solution

Replaced with FFmpeg's `AVAudioFifo`:
- O(1) circular buffer operations
- Pre-allocated at resampler initialization
- Separate buffers for DSD (packet fragments) and PCM (sample overflow)

### Performance Comparison

| Operation | Before | After |
|-----------|--------|-------|
| Read overflow | `memcpy` + `memmove` O(n) | `av_audio_fifo_read` O(1) |
| Write overflow | `memcpy` + potential realloc | `av_audio_fifo_write` O(1) |
| Memory pattern | Dynamic resize | Pre-allocated circular |

### Code Changes

**AudioEngine.h:**
```cpp
// DSD packet remainder (separate from PCM)
AudioBuffer m_dsdPacketRemainder;
size_t m_dsdRemainderCount = 0;

// PCM FIFO for sample overflow
AVAudioFifo* m_pcmFifo = nullptr;
```

**FIFO sizing (dynamic based on sample rate):**
```cpp
// Scale with sample rate using 64-bit math to avoid overflow
int fifoSize = static_cast<int>((static_cast<int64_t>(8192) * outputRate) / 48000);
if (fifoSize < 4096) fifoSize = 4096;      // Minimum
if (fifoSize > 262144) fifoSize = 262144;  // Maximum
```

| Sample Rate | FIFO Size |
|-------------|-----------|
| 44.1 kHz | 7,529 samples |
| 48 kHz | 8,192 samples |
| 96 kHz | 16,384 samples |
| 192 kHz | 32,768 samples |
| 384 kHz | 65,536 samples |
| 768 kHz | 131,072 samples |

---

## Optimization 3: PCM Bypass Mode

### Problem

Even when source and output formats match exactly, audio was still processed through SwrContext:
- Unnecessary CPU overhead
- Potential for subtle quality degradation
- No true bit-perfect path

### Solution

Added bypass mode that skips SwrContext entirely when:
- Sample rates match exactly
- Channel counts match
- Format is packed integer (S16 or S32)
- Bit depth matches (or S32 container with 24-bit content)

**Important:** Float formats are NEVER bypassed - they always require conversion.

### Code Changes

**AudioEngine.h:**
```cpp
bool m_bypassMode = false;
bool m_resamplerInitialized = false;

bool canBypass(uint32_t outputRate, uint32_t outputBits) const;
```

**Bypass eligibility check:**
```cpp
bool AudioDecoder::canBypass(uint32_t outputRate, uint32_t outputBits) const {
    if (m_trackInfo.isDSD) return false;
    if (m_codecContext->sample_rate != (int)outputRate) return false;
    if (m_codecContext->ch_layout.nb_channels != (int)m_trackInfo.channels) return false;

    AVSampleFormat fmt = m_codecContext->sample_fmt;
    bool isPackedInteger = (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S32);
    if (!isPackedInteger) return false;

    // Bit depth check...
    return true;
}
```

**Read path with bypass:**
```cpp
if (m_bypassMode) {
    // BYPASS PATH: Direct copy (bit-perfect)
    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
} else if (m_swrContext) {
    // RESAMPLING PATH
    swr_convert(...);
} else {
    // FALLBACK: Direct copy
    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
}
```

### When Bypass is Enabled

| Source | Target | Bypass? |
|--------|--------|---------|
| 44.1kHz S16 | 44.1kHz 16-bit | YES |
| 44.1kHz S32 (24-bit) | 44.1kHz 24-bit | YES |
| 44.1kHz S32P (planar) | 44.1kHz 24-bit | NO (planar) |
| 44.1kHz FLTP (float) | 44.1kHz 24-bit | NO (float) |
| 44.1kHz S16 | 48kHz 16-bit | NO (rate) |
| 96kHz S32 | 96kHz 24-bit | YES |

---

## Optimization 4: S24 Hint Propagation

### Problem

The S24 detection hint from FFmpeg metadata wasn't reaching the ring buffer.

### Solution

Added propagation path: `TrackInfo` → `DirettaRenderer` → `DirettaSync` → `DirettaRingBuffer`

**TrackInfo (AudioEngine.h):**
```cpp
enum class S24Alignment { Unknown, LsbAligned, MsbAligned };
S24Alignment s24Alignment;
```

**Detection in AudioDecoder::open():**
```cpp
if (realBitDepth == 24) {
    if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
    }
    else if (codecpar->codec_id == AV_CODEC_ID_FLAC ||
             codecpar->codec_id == AV_CODEC_ID_ALAC) {
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
    }
    // ...
}
```

**Propagation in DirettaRenderer.cpp:**
```cpp
if (!format.isDSD && bitDepth == 24 &&
    trackInfo.s24Alignment != TrackInfo::S24Alignment::Unknown) {
    DirettaRingBuffer::S24PackMode hint =
        (trackInfo.s24Alignment == TrackInfo::S24Alignment::LsbAligned)
            ? DirettaRingBuffer::S24PackMode::LsbAligned
            : DirettaRingBuffer::S24PackMode::MsbAligned;
    m_direttaSync->setS24PackModeHint(hint);
}
```

---

## Files Modified

| File | Lines Changed | Description |
|------|---------------|-------------|
| `src/DirettaRingBuffer.h` | ~100 | S24 detection enhancement |
| `src/DirettaSync.h` | ~10 | `setS24PackModeHint()` method |
| `src/AudioEngine.h` | ~25 | FIFO, bypass mode, S24Alignment |
| `src/AudioEngine.cpp` | ~150 | All PCM optimizations |
| `src/DirettaRenderer.cpp` | ~15 | S24 hint propagation |

---

## Preserved Bug Fixes

These optimizations were carefully integrated to preserve existing bug fixes:

1. **FFmpeg ABI Compatibility** (`av_find_best_stream()`) - Preserved
2. **ARM64 Compilation** (`DIRETTA_HAS_AVX2` guards) - Preserved
3. **DSD Transition Silence** (`sendPreTransitionSilence()`) - Preserved
4. **DSD Per-Channel Buffers** (`m_dsdLeftBuffer`, `m_dsdRightBuffer`) - Preserved
5. **DSD512 Zen3 Warmup** (MTU-aware scaling) - Preserved

---

## Testing Recommendations

1. **24-bit with fade-in**: Play 24-bit FLAC starting with silence
   - Expected: S24 detection uses hint, then confirms with samples

2. **Native rate playback**: Play 44.1kHz file with 44.1kHz target
   - Expected: Log shows "PCM BYPASS enabled - bit-perfect path"

3. **High-res playback**: Play 192kHz or higher
   - Expected: Larger FIFO allocated, no overflow issues

4. **Rate conversion**: Play 44.1kHz file with 48kHz target
   - Expected: Bypass NOT enabled, resampler used

5. **DSD playback**: Play DSD64/128/256/512
   - Expected: Unchanged behavior, uses separate remainder buffer
