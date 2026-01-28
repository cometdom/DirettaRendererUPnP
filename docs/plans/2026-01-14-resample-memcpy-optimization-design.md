# Resample Path Memory Copy Optimization Design

**Date:** 2026-01-14
**Goal:** Eliminate unnecessary memory copies in PCM resample path
**Benefits:** Reduced latency, lower CPU usage, less jitter

---

## 1. Architecture Overview

**Current bottlenecks:**

| Operation | Location | Cost |
|-----------|----------|------|
| `swr_convert()` → temp buffer | Always | 1 memcpy |
| temp buffer → outputPtr | Always | 1 memcpy |
| excess → m_remainingSamples | When excess exists | 1 memcpy |
| m_remainingSamples shift | Every consume | 1 memmove (O(n)) |

**Optimized flow:**

```
┌─────────────────────────────────────────────────────────────┐
│  readSamples() - Optimized Flow                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. Drain FIFO first (if has data)                          │
│     └─ av_audio_fifo_read() → outputPtr  [O(1) circular]    │
│                                                             │
│  2. For each decoded frame:                                 │
│     ├─ If samplesNeeded >= maxOutput:                       │
│     │   └─ swr_convert() → outputPtr     [DIRECT, no copy]  │
│     │                                                       │
│     └─ Else (need temp buffer):                             │
│         ├─ swr_convert() → m_resampleBuffer                 │
│         ├─ memcpy needed → outputPtr                        │
│         └─ av_audio_fifo_write(excess)   [O(1) circular]    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Expected improvement:** Eliminates 1-2 memcpy operations per frame in the common case, and replaces O(n) memmove with O(1) FIFO operations.

---

## 2. Direct Write Path

**Condition:** When `samplesNeeded >= maxOutput`, write directly to `outputPtr`.

```cpp
// Calculate max possible output from this frame
int64_t maxOutput = swr_get_out_samples(m_swrContext, frameSamples);

size_t samplesNeeded = numSamples - totalSamplesRead;

if ((size_t)maxOutput <= samplesNeeded) {
    // DIRECT PATH: write straight to output buffer
    uint8_t* outPtrs[1] = { outputPtr };

    int convertedSamples = swr_convert(
        m_swrContext,
        outPtrs,
        maxOutput,
        (const uint8_t**)m_frame->data,
        frameSamples
    );

    if (convertedSamples > 0) {
        outputPtr += convertedSamples * bytesPerSample;
        totalSamplesRead += convertedSamples;
    }
} else {
    // TEMP BUFFER PATH: use m_resampleBuffer, excess goes to FIFO
}
```

**Why this works:**
- `swr_get_out_samples()` returns the maximum possible output
- Actual output is always ≤ this estimate
- Since `samplesNeeded >= maxOutput >= actualOutput`, we never overflow
- Packed PCM (S16/S32 interleaved) uses single pointer in `outPtrs[1]`

---

## 3. AVAudioFifo Integration

**Replace PCM overflow handling with AVAudioFifo. Keep DSD buffer separate.**

**Current problem:** `m_remainingSamples`/`m_remainingCount` are shared between:
- DSD path: byte-level L/R channel buffering (count = bytes)
- PCM resample path: sample overflow (count = samples)
- PCM passthrough path: frame overflow (count = samples)

This overloading is fragile. Solution: separate concerns.

**Member changes in AudioDecoder:**

```cpp
class AudioDecoder {
private:
    // KEEP for DSD (rename for clarity):
    std::vector<uint8_t> m_dsdRemainderBuffer;  // was m_remainingSamples
    size_t m_dsdRemainderCount = 0;              // was m_remainingCount (bytes)

    // ADD for PCM (both resample and passthrough):
    AVAudioFifo* m_pcmFifo = nullptr;
};
```

**Lifecycle management:**

| Event | Action |
|-------|--------|
| `initResampler()` | Allocate PCM FIFO for output format (S16/S32, channels) |
| Format/channel change | Free old FIFO, allocate new one |
| `seek()` | `av_audio_fifo_reset(m_pcmFifo)` + `m_dsdRemainderCount = 0` |
| `close()` | `av_audio_fifo_free(m_pcmFifo)` |

**FIFO allocation (in initResampler):**

```cpp
// Free existing FIFO if format changed
if (m_pcmFifo) {
    av_audio_fifo_free(m_pcmFifo);
    m_pcmFifo = nullptr;
}

// Allocate for output format
AVSampleFormat fifoFormat = (outputBits == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
m_pcmFifo = av_audio_fifo_alloc(fifoFormat, m_trackInfo.channels, 8192);
if (!m_pcmFifo) {
    std::cerr << "[AudioDecoder] Failed to allocate PCM FIFO" << std::endl;
    return false;
}
```

**Reading from FIFO (top of PCM path in readSamples, before decode loop):**

```cpp
// Drain PCM FIFO first (replaces old m_remainingSamples read)
if (m_pcmFifo) {
    int fifoSamples = av_audio_fifo_size(m_pcmFifo);
    if (fifoSamples > 0) {
        int samplesToRead = std::min(fifoSamples, (int)(numSamples - totalSamplesRead));
        uint8_t* outPtrs[1] = { outputPtr };

        int read = av_audio_fifo_read(m_pcmFifo, (void**)outPtrs, samplesToRead);
        if (read > 0) {
            outputPtr += read * bytesPerSample;
            totalSamplesRead += read;
        }

        if (totalSamplesRead >= numSamples) {
            return totalSamplesRead;
        }
    }
}
```

**Writing excess to FIFO (resample path):**

```cpp
if ((size_t)convertedSamples > samplesToUse) {
    size_t excess = convertedSamples - samplesToUse;
    uint8_t* excessPtr = m_resampleBuffer.data() + samplesToUse * bytesPerSample;
    uint8_t* excessPtrs[1] = { excessPtr };

    av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
}
```

**Writing excess to FIFO (passthrough path - no resampler):**

When `m_swrContext == nullptr`, decoded frames go directly to output. Excess must also go to FIFO:

```cpp
} else {
    // No resampling - direct copy
    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
    size_t bytesToCopy = samplesToCopy * bytesPerSample;

    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
    outputPtr += bytesToCopy;
    totalSamplesRead += samplesToCopy;

    // Store excess in FIFO (replaces old m_remainingSamples)
    if (frameSamples > samplesToCopy) {
        size_t excess = frameSamples - samplesToCopy;
        uint8_t* excessPtr = m_frame->data[0] + bytesToCopy;
        uint8_t* excessPtrs[1] = { excessPtr };

        av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
    }
}
```

---

## 4. Implementation Plan

**Files to modify:**

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `AVAudioFifo* m_pcmFifo`; rename `m_remainingSamples` → `m_dsdRemainderBuffer`, `m_remainingCount` → `m_dsdRemainderCount` |
| `src/AudioEngine.cpp` | Refactor `readSamples()`, update `initResampler()`, `seek()`, `close()` |

**Implementation order:**

| Step | Task | Risk |
|------|------|------|
| 1 | Add `#include <libavutil/audio_fifo.h>` | None |
| 2 | Rename DSD buffer members in header | Low |
| 3 | Add `AVAudioFifo* m_pcmFifo` member | Low |
| 4 | Add FIFO alloc/free in `initResampler()` and `close()` | Low |
| 5 | Add FIFO reset in `seek()` (alongside DSD buffer clear) | Low |
| 6 | Update DSD path to use renamed members | Low |
| 7 | Refactor PCM path: drain FIFO at top (replaces old m_remainingSamples read) | Medium |
| 8 | Add direct write path for resample (samplesNeeded >= maxOutput) | Medium |
| 9 | Update resample excess to use FIFO write | Low |
| 10 | Update passthrough excess to use FIFO write | Low |
| 11 | Remove old memmove code from PCM path | Low |
| 12 | Test PCM playback (various sample rates/bit depths) | Validation |
| 13 | Test DSD playback (verify no regression) | Validation |

**DSD path:** Uses renamed `m_dsdRemainderBuffer`/`m_dsdRemainderCount`. Logic unchanged, just clearer naming.

---

## 5. Testing Strategy

**Functional tests:**

| Test | Purpose |
|------|---------|
| Play 44.1kHz/16-bit FLAC | Common case, verify basic playback |
| Play 96kHz/24-bit FLAC | Higher rate, exercises resampler |
| Play 192kHz/32-bit WAV | Uncompressed, tests passthrough + FIFO |
| Play DSD64 DSF | Verify DSD buffer rename didn't break anything |
| Play DSD128 DFF | Verify DSD bit reversal still works |
| Seek mid-track (PCM) | Verify FIFO reset works |
| Seek mid-track (DSD) | Verify DSD buffer clear works |
| Gapless transition (same format) | Verify FIFO drains correctly between tracks |
| Format change between tracks | Verify FIFO recreated properly |

**Edge cases to verify:**

| Case | Expected behavior |
|------|-------------------|
| FIFO empty at start | Skip FIFO read, go straight to decode |
| Direct path fits exactly | No excess, FIFO stays empty |
| Small samplesNeeded (< maxOutput) | Uses temp buffer path, excess to FIFO |
| Passthrough with large frame | Excess goes to FIFO, not dropped |
| Seek while FIFO has data | FIFO cleared, no stale audio |
| DSD with partial packet | Excess stored in m_dsdRemainderBuffer |

**Performance validation (optional):**
- Before/after comparison of CPU usage during playback
- Verify no new allocations in steady-state decode loop

---

## Scope

**In scope:**
- Direct write optimization for resample path
- AVAudioFifo for PCM overflow (both resample and passthrough)
- Rename DSD buffer members for clarity
- FIFO lifecycle management (init, seek, close)

**Out of scope:**
- DSD buffer logic changes (just renamed, not restructured)
- Runtime configuration
