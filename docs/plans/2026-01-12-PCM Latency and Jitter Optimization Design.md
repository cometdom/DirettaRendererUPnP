# PCM Latency and Jitter Optimization Design

**Date:** 2026-01-12
**Goal:** Minimize latency and jitter in PCM decode/playback path
**Target:** ~300ms low-latency mode, jitter reduction through allocation elimination and flow control

---

## 1. Architecture Overview

**Problem:** Current PCM path has three jitter sources:
1. Per-call heap allocations in decode loop
2. 10ms blocking sleeps on backpressure
3. Fixed 8192-sample chunks regardless of buffer state

**Solution:** A layered approach that eliminates allocations, adds adaptive flow control, and tunes buffer constants for low latency.

```
┌─────────────────────────────────────────────────────────────┐
│  Compile-time Constants (src/DirettaSync.h)                 │
│  PCM_BUFFER_SECONDS = 0.3f    (was 1.0f)                    │
│  PCM_PREFILL_MS = 30          (was 50)                      │
│  MIN_BUFFER_BYTES = 65536     (was 3072000, small safety floor)  │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  AudioEngine (modified)                                     │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Reusable members (allocated once):                 │    │
│  │  - m_packet (AVPacket*)                             │    │
│  │  - m_frame (AVFrame*)                               │    │
│  │  - m_resampleBuffer (capacity-tracked)              │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  DirettaRenderer (modified)                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Chunk size: 2048 samples (was 8192)                │    │
│  │  Hybrid flow control:                               │    │
│  │  - Normal: 500µs sleep, 20ms cap                    │    │
│  │  - Critical (<10% buffer): early-return             │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

**Key principle:** Decode and allocation paths execute identical code regardless of data size. Flow control may branch on buffer state but with bounded, predictable timing.

---

## 2. Per-Call Allocation Elimination

**Current problem** (`src/AudioEngine.cpp:746-747, 860-862`):

```cpp
// Called every readSamples() - heap allocation each time
AVPacket* packet = av_packet_alloc();
AVFrame* frame = av_frame_alloc();

// Called every resampled frame - heap allocation each time
AudioBuffer tempBuffer(tempBufferSize);
```

**Solution:** Move to class members with lazy initialization and capacity tracking.

```cpp
class AudioDecoder {
private:
    // Reusable FFmpeg structures (allocated once on first use)
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;

    // Reusable resample buffer with capacity tracking
    AudioBuffer m_resampleBuffer;
    size_t m_resampleBufferCapacity = 0;
};
```

**Lifecycle:**
- **Initialization:** Allocate in `open()` or lazily on first `readSamples()` call
- **Reuse:** `av_packet_unref()` / `av_frame_unref()` between uses (resets without realloc)
- **Cleanup:** Free in destructor and `close()`

**Resample buffer growth policy:**

```cpp
// Only reallocate if capacity insufficient
if (tempBufferSize > m_resampleBufferCapacity) {
    m_resampleBuffer.resize(tempBufferSize * 1.5);  // 50% headroom
    m_resampleBufferCapacity = m_resampleBuffer.size();
}
uint8_t* tempPtr = m_resampleBuffer.data();
```

**Expected impact:** Eliminates 2-4 heap allocations per `readSamples()` call.

---

## 3. Hybrid Flow Control

**Current problem** (`src/DirettaRenderer.cpp:257-269`):

```cpp
while (remainingSamples > 0 && retryCount < maxRetries) {
    size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);
    if (sent > 0) {
        retryCount = 0;
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 10ms stall
        retryCount++;
    }
}
// maxRetries=50 → up to 500ms blocking
```

**Solution:** Hybrid approach with two modes based on buffer level.

```cpp
constexpr int MICROSLEEP_US = 500;
constexpr int MAX_WAIT_MS = 20;
constexpr int MAX_RETRIES = MAX_WAIT_MS * 1000 / MICROSLEEP_US;  // 40 retries
constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;

float bufferLevel = m_direttaSync->getBufferLevel();
bool criticalMode = (bufferLevel < CRITICAL_BUFFER_LEVEL);

while (remainingSamples > 0 && retryCount < MAX_RETRIES) {
    size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

    if (sent > 0) {
        remainingSamples -= sent / bytesPerSample;
        audioData += sent;
        retryCount = 0;
    } else {
        if (criticalMode) {
            DEBUG_LOG("[Audio] Early-return, buffer critical: " << bufferLevel);
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(MICROSLEEP_US));
        retryCount++;
    }
}
```

**Behavior summary:**

| Buffer Level | Backpressure Response               | Max Stall |
| ------------ | ----------------------------------- | --------- |
| ≥10%         | Micro-sleep 500µs, up to 40 retries | 20ms      |
| <10%         | Immediate early-return              | 0ms       |

**Why 10% threshold?** At 300ms buffer, 10% = 30ms of audio remaining. Early-return ensures `audioThreadFunc` can refill before underrun.

---

## 4. Buffer Constants

**Approach:** Tune compile-time `constexpr` values for low-latency operation. No runtime configuration needed initially.

**Changes to `src/DirettaSync.h`:**

```cpp
namespace DirettaBuffer {
    // Low-latency tuned values
    constexpr float PCM_BUFFER_SECONDS = 0.3f;   // Was 1.0f (now 300ms buffer)
    constexpr size_t PCM_PREFILL_MS = 30;        // Was 50 (faster start)

    // Reduce minimum clamp to a small safety floor
    // Old MIN_BUFFER_BYTES = 3072000 clamped to ~5s at 44.1kHz, ~2s at 192kHz
    // New value: 64KB = ~370ms floor at 44.1kHz/16-bit, negligible at higher rates
    constexpr size_t MIN_BUFFER_BYTES = 65536;
}
```

**Buffer size math:**

Note: 24-bit audio uses 4 bytes/sample internally (`AV_SAMPLE_FMT_S32`), not 3 bytes packed.

| Sample Rate | Bit Depth | Internal Bytes/Sample | Bytes/sec | 300ms Buffer  |
| ----------- | --------- | --------------------- | --------- | ------------- |
| 44.1kHz     | 16-bit    | 2                     | 176,400   | 52,920 bytes  |
| 96kHz       | 24-bit    | 4                     | 768,000   | 230,400 bytes |
| 192kHz      | 32-bit    | 4                     | 1,536,000 | 460,800 bytes |

With `PCM_BUFFER_SECONDS = 0.3f` and `MIN_BUFFER_BYTES = 65536`, the buffer achieves ~300ms for most formats. The 64KB floor causes ~370ms at 44.1kHz/16-bit, but higher sample rates are unaffected.

**Changes to `src/DirettaRenderer.cpp`:**

```cpp
// Smaller chunks for lower latency
size_t samplesPerCall = isDSD ? 32768 : 2048;  // Was 8192 for PCM
```

**Rationale:**
- 300ms buffer covers typical LAN jitter while reducing end-to-end latency
- 2048 samples/call = ~46ms at 44.1kHz (was ~186ms) - tighter scheduling
- 30ms prefill enables faster playback start

**Future:** If runtime configurability is needed, these can be promoted to CLI flags later.

---

## 5. Implementation Plan

**Files to modify:**

| File                      | Changes                                               |
| ------------------------- | ----------------------------------------------------- |
| `src/DirettaSync.h`       | Update buffer constants for low-latency               |
| `src/DirettaRenderer.cpp` | Change chunk size; implement hybrid flow control      |
| `src/AudioEngine.h`       | Add `m_packet`, `m_frame`, `m_resampleBuffer` members |
| `src/AudioEngine.cpp`     | Refactor `readSamples()` to use reusable members      |

**Implementation order:**

| Step | Task                            | File                              | Risk       |
| ---- | ------------------------------- | --------------------------------- | ---------- |
| 1    | Update buffer constants         | `src/DirettaSync.h`               | Low        |
| 2    | Change chunk size               | `src/DirettaRenderer.cpp:540`     | Low        |
| 3    | Hybrid flow control             | `src/DirettaRenderer.cpp:257-269` | Medium     |
| 4    | Per-call allocation elimination | `src/AudioEngine.cpp`             | Medium     |
| 5    | Integration test                | -                                 | Validation |

**Testing strategy:**
- Integration: Playback test, verify no underruns at 300ms buffer
- Comparison: Before/after latency measurement (optional)
- Stress: Network variance simulation (optional)

---

## Scope

**In scope:**
- Buffer constant tuning (compile-time)
- Per-call allocation elimination
- Hybrid send loop (micro-sleep + early-return on critical)

**Out of scope:**
- CLI flags for runtime configuration (future enhancement)
- AVX2 micro-optimizations (zero hoist)
- `memcpy_audio_fixed` for PCM ring writes (covered in memory optimization design)
- Config file changes
- Runtime toggle (UPnP/D-Bus)