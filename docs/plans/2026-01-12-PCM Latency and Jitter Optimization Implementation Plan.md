# PCM Latency and Jitter Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Minimize latency and jitter in PCM decode/playback path through allocation elimination, hybrid flow control, and buffer tuning.

**Architecture:** Three-layer approach: (1) compile-time buffer constants for 300ms latency target, (2) reusable FFmpeg structures in AudioDecoder to eliminate per-call heap allocations, (3) hybrid flow control in DirettaRenderer with micro-sleeps and critical-mode early-return.

**Tech Stack:** C++17, FFmpeg (libavcodec/libavformat/libswresample), Diretta SDK

---

## Task 1: Update Buffer Constants for Low Latency

**Files:**
- Modify: `src/DirettaSync.h:76-107`

**Step 1: Update PCM_BUFFER_SECONDS constant**

Change from 1.0f to 0.3f for 300ms buffer target.

```cpp
namespace DirettaBuffer {
    constexpr float DSD_BUFFER_SECONDS = 0.8f;
    constexpr float PCM_BUFFER_SECONDS = 0.3f;  // Was 1.0f - low latency
```

**Step 2: Update PCM_PREFILL_MS constant**

Change from 50 to 30 for faster playback start.

```cpp
    constexpr size_t DSD_PREFILL_MS = 200;
    constexpr size_t PCM_PREFILL_MS = 30;       // Was 50 - faster start
    constexpr size_t PCM_LOWRATE_PREFILL_MS = 100;
```

**Step 3: Update MIN_BUFFER_BYTES constant**

Change from 3072000 to 65536 (64KB floor).

```cpp
    // UPnP push model needs larger buffers than MPD's pull model
    // 64KB = ~370ms floor at 44.1kHz/16-bit, negligible at higher rates
    constexpr size_t MIN_BUFFER_BYTES = 65536;  // Was 3072000
```

**Step 4: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 5: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
perf: tune buffer constants for 300ms low-latency PCM

- PCM_BUFFER_SECONDS: 1.0f -> 0.3f
- PCM_PREFILL_MS: 50 -> 30
- MIN_BUFFER_BYTES: 3072000 -> 65536

EOF
)"
```

---

## Task 2: Reduce PCM Chunk Size

**Files:**
- Modify: `src/DirettaRenderer.cpp:540`

**Step 1: Change PCM chunk size from 8192 to 2048**

Locate line 540 and update:

```cpp
// Adjust samples per call based on format
// 2048 samples = ~46ms at 44.1kHz (was 8192 = ~186ms)
size_t samplesPerCall = isDSD ? 32768 : 2048;  // Was 8192 for PCM
```

**Step 2: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 3: Commit**

```bash
git add src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
perf: reduce PCM chunk size to 2048 samples

Tighter scheduling: ~46ms at 44.1kHz (was ~186ms with 8192).
Enables more responsive buffer-level flow control.

EOF
)"
```

---

## Task 3: Implement Hybrid Flow Control

**Files:**
- Modify: `src/DirettaRenderer.cpp:247-275`

**Step 1: Add flow control constants at file scope**

Insert after the DEBUG_LOG macro (around line 22):

```cpp
// Hybrid flow control constants
namespace FlowControl {
    constexpr int MICROSLEEP_US = 500;
    constexpr int MAX_WAIT_MS = 20;
    constexpr int MAX_RETRIES = MAX_WAIT_MS * 1000 / MICROSLEEP_US;  // 40 retries
    constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;
}
```

**Step 2: Replace the PCM send loop (lines 254-269)**

Replace the existing PCM incremental send block:

```cpp
// PCM: Incremental send with hybrid flow control
const uint8_t* audioData = buffer.data();
size_t remainingSamples = samples;
size_t bytesPerSample = (bitDepth == 24 || bitDepth == 32)
    ? 4 * channels : (bitDepth / 8) * channels;

float bufferLevel = m_direttaSync->getBufferLevel();
bool criticalMode = (bufferLevel < FlowControl::CRITICAL_BUFFER_LEVEL);

int retryCount = 0;

while (remainingSamples > 0 && retryCount < FlowControl::MAX_RETRIES) {
    size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

    if (sent > 0) {
        size_t samplesConsumed = sent / bytesPerSample;
        remainingSamples -= samplesConsumed;
        audioData += sent;
        retryCount = 0;
    } else {
        if (criticalMode) {
            DEBUG_LOG("[Audio] Early-return, buffer critical: " << bufferLevel);
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(FlowControl::MICROSLEEP_US));
        retryCount++;
    }
}
```

**Step 3: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 4: Commit**

```bash
git add src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
perf: implement hybrid flow control for PCM send loop

- Normal mode: 500us micro-sleep, max 20ms stall (40 retries)
- Critical mode (<10% buffer): early-return to prioritize refill
- Reduces jitter from fixed 10ms blocking sleeps

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Add Reusable Members to AudioDecoder

**Files:**
- Modify: `src/AudioEngine.h:118-144`

**Step 1: Add reusable frame member**

The class already has `m_packet` (line 128) for DSD. Add `m_frame` for PCM reuse:

After line 128, the private section should include:

```cpp
// DSD Native Mode
bool m_rawDSD;           // True if reading raw DSD packets (no decoding)
AVPacket* m_packet;      // Reusable for raw packet reading (DSD and PCM)
AVFrame* m_frame;        // Reusable for decoded frames (PCM)
```

**Step 2: Add resample buffer with capacity tracking**

After line 134 (m_remainingCount), add:

```cpp
// Reusable resample buffer (eliminates per-call allocation)
AudioBuffer m_resampleBuffer;
size_t m_resampleBufferCapacity = 0;
```

**Step 3: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Build may fail due to missing initialization - that's expected, we'll fix in next task

**Step 4: Commit**

```bash
git add src/AudioEngine.h
git commit -m "$(cat <<'EOF'
refactor: add reusable FFmpeg members to AudioDecoder

Prepare for allocation elimination:
- m_frame: reusable AVFrame for PCM decoding
- m_resampleBuffer: capacity-tracked buffer for swr_convert

EOF
)"
```

---

## Task 5: Initialize and Clean Up Reusable Members

**Files:**
- Modify: `src/AudioEngine.cpp:73-88` (constructor)
- Modify: `src/AudioEngine.cpp:512-528` (close method)

**Step 1: Update AudioDecoder constructor**

Update the initializer list (lines 73-83):

```cpp
AudioDecoder::AudioDecoder()
    : m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_swrContext(nullptr)
    , m_audioStreamIndex(-1)
    , m_eof(false)
    , m_rawDSD(false)
    , m_packet(nullptr)
    , m_frame(nullptr)           // Add this
    , m_remainingCount(0)
    , m_resampleBufferCapacity(0) // Add this
{
}
```

**Step 2: Update close() to free m_frame**

In the close() method (around line 512-528), add cleanup for m_frame:

```cpp
void AudioDecoder::close() {
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_frame) {           // Add this block
        av_frame_free(&m_frame);
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
    m_audioStreamIndex = -1;
    m_eof = false;
    m_rawDSD = false;
    m_resampleBufferCapacity = 0;  // Reset capacity tracking
}
```

**Step 3: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 4: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
refactor: initialize and clean up reusable AudioDecoder members

- Initialize m_frame and m_resampleBufferCapacity in constructor
- Free m_frame in close()
- Reset capacity tracking on close

EOF
)"
```

---

## Task 6: Eliminate Per-Call AVPacket/AVFrame Allocation

**Files:**
- Modify: `src/AudioEngine.cpp:746-752` (readSamples PCM section)

**Step 1: Replace per-call allocations with lazy initialization**

Replace lines 746-752:

```cpp
// Old code:
// AVPacket* packet = av_packet_alloc();
// AVFrame* frame = av_frame_alloc();

// New code: Lazy initialization of reusable members
if (!m_packet) {
    m_packet = av_packet_alloc();
}
if (!m_frame) {
    m_frame = av_frame_alloc();
}

if (!m_packet || !m_frame) {
    return totalSamplesRead;
}
```

**Step 2: Replace av_packet_free/av_frame_free with unref**

At the end of readSamples() (around line 940-941), replace:

```cpp
// Old code:
// av_packet_free(&packet);
// av_frame_free(&frame);

// New code: Unref for reuse (no deallocation)
av_packet_unref(m_packet);
av_frame_unref(m_frame);
```

**Step 3: Update all references from local to member**

Throughout readSamples() PCM section:
- Replace `packet` with `m_packet`
- Replace `frame` with `m_frame`

Key locations:
- Line 757: `int ret = av_read_frame(m_formatContext, m_packet);`
- Line 790-791: `if (m_packet->stream_index != m_audioStreamIndex)`
- Line 796-797: `ret = avcodec_send_packet(m_codecContext, m_packet); av_packet_unref(m_packet);`
- Line 806: `ret = avcodec_receive_frame(m_codecContext, m_frame);`
- Line 819: `size_t frameSamples = m_frame->nb_samples;`
- Line 832-843: All `frame->` references become `m_frame->`
- Line 936: `av_frame_unref(m_frame);`

**Step 4: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 5: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
perf: eliminate per-call AVPacket/AVFrame allocation

- Lazy initialization: allocate once on first use
- Reuse via av_packet_unref/av_frame_unref between calls
- Eliminates 2 heap allocations per readSamples() call

EOF
)"
```

---

## Task 7: Eliminate Per-Call Resample Buffer Allocation

**Files:**
- Modify: `src/AudioEngine.cpp:860-863` (inside readSamples resampling section)

**Step 1: Replace per-call AudioBuffer with capacity-tracked member**

Replace lines 860-863:

```cpp
// Old code:
// size_t tempBufferSize = totalOutSamples * bytesPerSample;
// AudioBuffer tempBuffer(tempBufferSize);
// uint8_t* tempPtr = tempBuffer.data();

// New code: Reuse member buffer with capacity growth
size_t tempBufferSize = totalOutSamples * bytesPerSample;
if (tempBufferSize > m_resampleBufferCapacity) {
    // Grow with 50% headroom to reduce future reallocations
    size_t newCapacity = static_cast<size_t>(tempBufferSize * 1.5);
    m_resampleBuffer.resize(newCapacity);
    m_resampleBufferCapacity = m_resampleBuffer.size();
}
uint8_t* tempPtr = m_resampleBuffer.data();
```

**Step 2: Update all tempBuffer references**

Throughout the resampling section:
- Replace `tempBuffer.data()` with `m_resampleBuffer.data()`

Key locations (around lines 880, 895):
- Line 880: `memcpy_audio(outputPtr, m_resampleBuffer.data(), bytesToUse);`
- Line 895-897: `memcpy_audio(m_remainingSamples.data(), m_resampleBuffer.data() + bytesToUse, excessBytes);`

**Step 3: Build and verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: Clean build with no errors

**Step 4: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
perf: eliminate per-call resample buffer allocation

- Capacity-tracked member buffer with 50% growth headroom
- Eliminates 1 heap allocation per resampled frame
- Hot path now allocation-free for steady-state playback

EOF
)"
```

---

## Task 8: Integration Test

**Files:**
- None (testing only)

**Step 1: Full rebuild**

Run: `make clean && make -j$(nproc)`
Expected: Clean build with no errors or warnings

**Step 2: Run the application with verbose logging**

Run: `./bin/DirettaRendererUPnP --verbose --list-targets`
Expected: Should list available Diretta targets

**Step 3: Playback test (if target available)**

Run: `./bin/DirettaRendererUPnP --verbose`
- Play a PCM track (44.1kHz/16-bit) from control point
- Verify no underruns or glitches
- Check verbose logs for buffer level and flow control messages

**Step 4: Commit final state**

```bash
git add -A
git commit -m "$(cat <<'EOF'
test: verify PCM latency optimization integration

All components tested:
- 300ms buffer target
- 2048-sample chunks
- Hybrid flow control
- Allocation elimination

EOF
)"
```

---

## Summary

| Task | Description                     | Risk       | Estimated Changes |
| ---- | ------------------------------- | ---------- | ----------------- |
| 1    | Buffer constants                | Low        | 3 lines           |
| 2    | Chunk size                      | Low        | 1 line            |
| 3    | Hybrid flow control             | Medium     | ~25 lines         |
| 4    | Add reusable members (header)   | Low        | 3 lines           |
| 5    | Initialize/cleanup members      | Low        | ~10 lines         |
| 6    | Eliminate packet/frame allocs   | Medium     | ~15 lines         |
| 7    | Eliminate resample buffer alloc | Medium     | ~10 lines         |
| 8    | Integration test                | Validation | 0 lines           |

**Total changes:** ~67 lines across 3 files