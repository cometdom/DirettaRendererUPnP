# Technical Review: Code Simplification Opportunities

**Date:** 2026-01-17 (Revision 3 - Second Pass)
**Reviewer:** Claude (Code Review)
**Codebase:** DirettaRendererUPnP-X
**Goal:** Identify simplification opportunities to improve code predictability and audio quality

---

## Execution Path Analysis

### Hot Path (Continuous During Playback)

```
┌─ Audio Thread ─────────────────────────────────────────────────────┐
│                                                                    │
│  AudioEngine::process()                                            │
│      └─► AudioDecoder::readSamples()                               │
│              └─► Audio callback (DirettaRenderer.cpp:154-311)      │
│                      ├─► m_callbackMutex.lock()      ← SYSCALL!    │
│                      ├─► Guard destructor            ← SYSCALL!    │
│                      ├─► Format comparison (6 checks)              │
│                      └─► DirettaSync::sendAudio()                  │
│                              ├─► 3 atomic loads (entry checks)     │
│                              ├─► RingAccessGuard (2 atomics)       │
│                              ├─► 5 atomic loads (config snapshot)  │
│                              └─► DirettaRingBuffer::push*()        │
│                                      └─► writeToRing()             │
└────────────────────────────────────────────────────────────────────┘

┌─ SDK Thread (Diretta callback) ────────────────────────────────────┐
│                                                                    │
│  DirettaSync::getNewStream()                                       │
│      ├─► RingAccessGuard (2 atomics)                               │
│      ├─► Multiple atomic loads                                     │
│      ├─► std::cerr on underrun    ← I/O in hot path!              │
│      └─► DirettaRingBuffer::pop()                                  │
└────────────────────────────────────────────────────────────────────┘
```

---

## CRITICAL: Hot Path Issues (NEW in Second Pass)

### C0. **Mutex Lock + Notify in Audio Callback** ⚠️ HIGHEST PRIORITY

**Location:** `DirettaRenderer.cpp:159-172`

**Execution frequency:** **Every audio frame** (thousands/sec)

**Issue:** The audio callback takes a mutex lock and calls `notify_all()` on every single frame:

```cpp
// Constructor - runs EVERY frame
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_callbackRunning = true;
}

// Destructor - runs EVERY frame
~Guard() {
    {
        std::lock_guard<std::mutex> lk(self->m_callbackMutex);
        self->m_callbackRunning = false;
    }
    self->m_callbackCV.notify_all();  // SYSCALL on every frame!
}
```

**Impact:**
- `std::mutex::lock()` = potential syscall (futex on Linux)
- `notify_all()` = **guaranteed syscall** even with no waiters
- Memory barriers on every frame
- This is the **single biggest source of timing variance** in the hot path

**Root cause analysis:** This guard was designed to allow safe shutdown by tracking when callback is running. But the implementation pays the cost on **every frame** instead of only during shutdown.

**Fix options:**

1. **Atomic flag instead of mutex** (recommended):
```cpp
// Replace mutex+CV with:
std::atomic<bool> m_callbackRunning{false};

// In callback:
m_callbackRunning.store(true, std::memory_order_release);
// ... work ...
m_callbackRunning.store(false, std::memory_order_release);

// In shutdown:
while (m_callbackRunning.load(std::memory_order_acquire)) {
    std::this_thread::yield();
}
```

2. **Lazy notification** - only notify if someone is waiting:
```cpp
~Guard() {
    self->m_callbackRunning.store(false, std::memory_order_release);
    if (self->m_shutdownRequested.load(std::memory_order_acquire)) {
        self->m_callbackCV.notify_all();
    }
}
```

**Effort:** Medium | **Risk:** Medium (test shutdown) | **Impact:** **Very High**

---

### C1. Modulo Operation in writeToRing

**Location:** `DirettaRingBuffer.h:1071`

**Execution frequency:** Every buffer write

**Issue:** Uses `%` division instead of `&` bitmask:
```cpp
size_t newWritePos = (writePos + len) % size;  // Division: 10-20 cycles
```

**Fix:** `size_t newWritePos = (writePos + len) & mask_;`

**Effort:** Trivial | **Risk:** None | **Impact:** High

---

### C2. Excessive Atomic Loads in sendAudio

**Location:** `DirettaSync.cpp:1208-1212`

**Execution frequency:** Every audio frame

**Issue:** 5 atomic loads for values that **never change during playback**:
```cpp
bool dsdMode = m_isDsdMode.load(std::memory_order_acquire);
bool pack24bit = m_need24BitPack.load(std::memory_order_acquire);
bool upsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
int numChannels = m_channels.load(std::memory_order_acquire);
int bytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
```

These values are set in `open()` and never change until next track.

**Impact:** 5 memory barriers per frame × thousands of frames/sec = significant overhead

**Fix:** Cache these values in a non-atomic struct, update only in `open()`:
```cpp
struct PlaybackConfig {
    bool dsdMode;
    bool pack24bit;
    bool upsample16to32;
    int numChannels;
    int bytesPerSample;
};
// Update atomically once in open(), read freely in sendAudio()
```

**Effort:** Medium | **Risk:** Low | **Impact:** High

---

### C3. Format Comparison on Every Frame

**Location:** `DirettaRenderer.cpp:213-216`

**Execution frequency:** Every audio frame

**Issue:** 6 comparisons run on every frame even though format changes are extremely rare:
```cpp
bool formatChanged = (currentSyncFormat.sampleRate != format.sampleRate ||
                     currentSyncFormat.bitDepth != format.bitDepth ||
                     currentSyncFormat.channels != format.channels ||
                     currentSyncFormat.isDSD != format.isDSD);
```

**Impact:** Low individually, but contributes to branch misprediction and code size.

**Fix:** Only check format when `needsOpen` would be true anyway, or use a single hash/version number:
```cpp
// In DirettaSync, maintain a format version
std::atomic<uint32_t> m_formatVersion{0};

// In callback, just compare versions:
bool formatChanged = (m_lastFormatVersion != m_direttaSync->getFormatVersion());
```

**Effort:** Medium | **Risk:** Low | **Impact:** Medium

---

### C4. Dual memcpy Dispatch in writeToRing

**Location:** `DirettaRingBuffer.h:1056-1068`

**Execution frequency:** Every buffer write

**Issue:** Branch on size before memcpy:
```cpp
if (firstChunk >= 32) {
    memcpy_audio_fixed(ring + writePos, staged, firstChunk);
} else if (firstChunk > 0) {
    std::memcpy(ring + writePos, staged, firstChunk);
}
```

**Fix:** Remove branch, use `memcpy_audio_fixed` for all sizes.

**Effort:** Trivial | **Risk:** Low | **Impact:** Medium

---

### C5. RingAccessGuard Atomic Operations

**Location:** `DirettaSync.cpp:16-27`

**Execution frequency:** Every sendAudio() and getNewStream() call

**Issue:** Two `fetch_add`/`fetch_sub` with `acq_rel` barriers per function call:
```cpp
users_.fetch_add(1, std::memory_order_acq_rel);  // Entry
// ... work ...
users_.fetch_sub(1, std::memory_order_acq_rel);  // Exit
```

**Impact:** Full memory barriers, even though reconfiguration is rare.

**Potential fix:** Use thread-local or per-thread counters, aggregate on reconfiguration. However, this is complex and the current implementation is correct. **Lower priority** than C0-C2.

**Effort:** High | **Risk:** High | **Impact:** Medium

---

### C6. I/O on Underrun

**Location:** `DirettaSync.cpp:1386-1387`

**Execution frequency:** Only on underrun (rare, but when it happens, it's bad)

**Issue:** `std::cerr` I/O during underrun:
```cpp
std::cerr << "[DirettaSync] UNDERRUN #" << count
          << " avail=" << avail << " need=" << currentBytesPerBuffer << std::endl;
```

**Impact:** Underruns are performance problems. Adding I/O (potential syscall, buffering) makes recovery slower.

**Fix:** Increment atomic counter, log asynchronously or at end of playback:
```cpp
m_underrunCount.fetch_add(1, std::memory_order_relaxed);
// Log in stop() or periodically
```

**Effort:** Low | **Risk:** None | **Impact:** Medium (during underrun recovery)

---

### C7. Bit-Reversal LUT Cache Locality

**Location:** `DirettaRingBuffer.h:654, 688, 834, 868`

**Execution frequency:** Every DSD sample

**Issue:** 256-byte LUT defined 4 times inside functions. Should be single class-scope definition.

**Effort:** Medium | **Risk:** Low | **Impact:** Medium (cache)

---

### C8. S24 Detection Branch Complexity

**Location:** `DirettaRingBuffer.h:217-234`

**Execution frequency:** Every 24-bit PCM frame (but becomes predictable after confirmation)

**Issue:** Complex 3-way OR condition:
```cpp
if (m_s24PackMode == S24PackMode::Unknown || m_s24PackMode == S24PackMode::Deferred ||
    (m_s24PackMode == m_s24Hint && !m_s24DetectionConfirmed)) {
```

**Fix:** Simplify to single flag check:
```cpp
if (!m_s24DetectionConfirmed) {
```

**Effort:** Medium | **Risk:** Medium | **Impact:** Low (branch predictor handles it)

---

## SECONDARY: Track Initialization Issues

These run once per track and don't affect continuous playback.

### S1. Remove Disabled Code Blocks

**Location:** `DirettaSync.cpp:730-758, 1145-1193`

~65 lines of `#if 0` and unreachable code.

---

### S2. Remove Legacy pushDSDPlanar

**Location:** `DirettaRingBuffer.h:290-312` + `convertDSDPlanar_AVX2`

~120 lines of dead code (never called).

---

### S3. Consolidate Format Transition Logic

**Location:** `DirettaSync.cpp:335-534`

~200 lines of nested conditionals in `open()`.

---

### S4. Consolidate Retry Constants

**Location:** `DirettaSync.cpp` scattered

Magic numbers for retry counts and delays.

---

### S5. Remove DSD Diagnostic Code

**Location:** `AudioEngine.cpp:348-390, 666-707`

Packet diagnostics with allocations at track open.

---

## Summary: Prioritized by Impact

### CRITICAL (Hot Path) - Ordered by Impact

| Priority | Issue | Location | Frequency | Effort | Impact |
|----------|-------|----------|-----------|--------|--------|
| **C0** | **Mutex + notify_all in callback** | Renderer:159 | Every frame | Medium | **VERY HIGH** |
| **C1** | Modulo in writeToRing | RingBuffer:1071 | Every write | Trivial | High |
| **C2** | Excessive atomic loads | DirettaSync:1208 | Every frame | Medium | High |
| **C3** | Format comparison every frame | Renderer:213 | Every frame | Medium | Medium |
| **C4** | Dual memcpy dispatch | RingBuffer:1056 | Every write | Trivial | Medium |
| **C5** | RingAccessGuard atomics | DirettaSync:16 | Every call | High | Medium |
| **C6** | I/O on underrun | DirettaSync:1386 | On underrun | Low | Medium |
| **C7** | LUT cache locality | RingBuffer | Every DSD sample | Medium | Medium |
| **C8** | S24 detection branch | RingBuffer:217 | Every 24-bit frame | Medium | Low |

### SECONDARY (Track Init)

| # | Issue | Location | Lines |
|---|-------|----------|-------|
| S1 | Disabled code blocks | DirettaSync.cpp | ~65 |
| S2 | Legacy pushDSDPlanar | RingBuffer | ~120 |
| S3 | Format transition logic | DirettaSync.cpp | (reorganize) |
| S4 | Retry constants | DirettaSync.cpp | (clarity) |
| S5 | DSD diagnostics | AudioEngine.cpp | ~60 |

---

## Recommended Implementation Order

### Phase 1: Eliminate Syscalls from Hot Path (Highest Impact)
```
C0 (mutex/notify) → Test playback and shutdown
```
This single change could have the largest impact on timing consistency.

### Phase 2: Hot Path Quick Wins
```
C1 (modulo) → C4 (memcpy branch) → Test playback
```
Both are trivial single-line changes.

### Phase 3: Reduce Atomic Operations
```
C2 (config snapshot) → C3 (format check) → Test all formats
```
Requires caching strategy, moderate effort.

### Phase 4: DSD Optimization
```
C7 (LUT locality) → Test DSD playback
```

### Phase 5: Cleanup
```
S1 → S2 → S5 → S3 → S4
```

---

## New Findings Summary (Second Pass)

| # | Finding | Severity | Missed in First Pass Because |
|---|---------|----------|------------------------------|
| C0 | Mutex + notify_all every frame | **Critical** | Didn't trace into DirettaRenderer callback |
| C2 | 5 atomic loads per frame | High | Assumed atomics were necessary |
| C3 | Format comparison every frame | Medium | Focused on ring buffer, not callback |
| C5 | RingAccessGuard overhead | Medium | Assumed guard was lightweight |
| C6 | I/O during underrun | Medium | Didn't consider error paths |

---

## Philosophy Confirmation

The second pass strongly confirms the hypothesis that simpler code leads to better audio quality:

1. **Syscalls are the enemy** - The mutex/notify pattern in C0 introduces kernel transitions on every audio frame
2. **Atomics have hidden costs** - Memory barriers affect CPU pipeline and cache coherency
3. **Branches accumulate** - Each conditional adds branch prediction pressure
4. **Hot path should be minimal** - Only the absolutely necessary operations should run on every frame

The most impactful optimization is **C0**: removing the mutex and `notify_all()` from the audio callback hot path.

---

## Appendix A: Implementation Details

This appendix provides copy-paste ready code changes for future implementation sessions.

---

### A.1 C0: Remove Mutex/notify_all from Audio Callback

**Files to modify:**
- `src/DirettaRenderer.h`
- `src/DirettaRenderer.cpp`

**Step 1: Modify DirettaRenderer.h (lines 78-80)**

BEFORE:
```cpp
    // Callback synchronization
    mutable std::mutex m_callbackMutex;
    std::condition_variable m_callbackCV;
    bool m_callbackRunning{false};
```

AFTER:
```cpp
    // Callback synchronization (lock-free for hot path)
    std::atomic<bool> m_callbackRunning{false};
    std::atomic<bool> m_shutdownRequested{false};
```

**Step 2: Modify DirettaRenderer.cpp - waitForCallbackComplete() (lines 93-101)**

BEFORE:
```cpp
void DirettaRenderer::waitForCallbackComplete() {
    std::unique_lock<std::mutex> lk(m_callbackMutex);
    bool completed = m_callbackCV.wait_for(lk, std::chrono::seconds(5),
        [this]{ return !m_callbackRunning; });
    if (!completed) {
        std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
        m_callbackRunning = false;
    }
}
```

AFTER:
```cpp
void DirettaRenderer::waitForCallbackComplete() {
    m_shutdownRequested.store(true, std::memory_order_release);

    auto start = std::chrono::steady_clock::now();
    while (m_callbackRunning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) {
            std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
            break;
        }
    }

    m_shutdownRequested.store(false, std::memory_order_release);
}
```

**Step 3: Modify audio callback (lines 158-172)**

BEFORE:
```cpp
m_audioEngine->setAudioCallback(
    [this](const AudioBuffer& buffer, size_t samples,
           uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

        // RAII guard
        {
            std::lock_guard<std::mutex> lk(m_callbackMutex);
            m_callbackRunning = true;
        }
        struct Guard {
            DirettaRenderer* self;
            ~Guard() {
                {
                    std::lock_guard<std::mutex> lk(self->m_callbackMutex);
                    self->m_callbackRunning = false;
                }
                self->m_callbackCV.notify_all();
            }
        } guard{this};
```

AFTER:
```cpp
m_audioEngine->setAudioCallback(
    [this](const AudioBuffer& buffer, size_t samples,
           uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

        // Check if shutdown requested (avoid work during teardown)
        if (m_shutdownRequested.load(std::memory_order_acquire)) {
            return false;
        }

        // Lightweight atomic flag (no syscalls)
        m_callbackRunning.store(true, std::memory_order_release);
        struct Guard {
            std::atomic<bool>& flag;
            ~Guard() { flag.store(false, std::memory_order_release); }
        } guard{m_callbackRunning};
```

**Testing:** Verify playback works and stop/shutdown completes without hanging.

---

### A.2 C1: Fix Modulo Operation in writeToRing

**File:** `src/DirettaRingBuffer.h`
**Line:** 1071

BEFORE:
```cpp
        size_t newWritePos = (writePos + len) % size;
```

AFTER:
```cpp
        size_t newWritePos = (writePos + len) & mask_;
```

**Note:** `mask_` is already a member variable set to `size_ - 1` in `resize()`.

---

### A.3 C2: Cache Atomic Config Values in sendAudio

**File:** `src/DirettaSync.h`

ADD after other member variables (around line 150):
```cpp
    // Cached playback config (set in open(), read in sendAudio())
    // Avoids atomic loads on hot path since these never change during playback
    struct PlaybackConfig {
        bool dsdMode = false;
        bool pack24bit = false;
        bool upsample16to32 = false;
        int numChannels = 2;
        int bytesPerSample = 4;
    };
    PlaybackConfig m_playbackConfig;
    std::atomic<bool> m_configValid{false};  // Set true after open() populates config
```

**File:** `src/DirettaSync.cpp`

In `open()` function, after setting the atomic values, ADD:
```cpp
    // Cache config for hot path (avoid atomic loads in sendAudio)
    m_playbackConfig.dsdMode = m_isDsdMode.load(std::memory_order_relaxed);
    m_playbackConfig.pack24bit = m_need24BitPack.load(std::memory_order_relaxed);
    m_playbackConfig.upsample16to32 = m_need16To32Upsample.load(std::memory_order_relaxed);
    m_playbackConfig.numChannels = m_channels.load(std::memory_order_relaxed);
    m_playbackConfig.bytesPerSample = m_bytesPerSample.load(std::memory_order_relaxed);
    m_configValid.store(true, std::memory_order_release);
```

In `sendAudio()` (lines 1208-1212), REPLACE:
```cpp
    // Snapshot config state
    bool dsdMode = m_isDsdMode.load(std::memory_order_acquire);
    bool pack24bit = m_need24BitPack.load(std::memory_order_acquire);
    bool upsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
    int numChannels = m_channels.load(std::memory_order_acquire);
    int bytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
```

WITH:
```cpp
    // Use cached config (single atomic check instead of 5)
    if (!m_configValid.load(std::memory_order_acquire)) return 0;
    const auto& cfg = m_playbackConfig;
    bool dsdMode = cfg.dsdMode;
    bool pack24bit = cfg.pack24bit;
    bool upsample16to32 = cfg.upsample16to32;
    int numChannels = cfg.numChannels;
    int bytesPerSample = cfg.bytesPerSample;
```

In `close()` or `stopPlayback()`, ADD:
```cpp
    m_configValid.store(false, std::memory_order_release);
```

---

### A.4 C4: Remove Dual memcpy Dispatch

**File:** `src/DirettaRingBuffer.h`
**Lines:** 1056-1068

BEFORE:
```cpp
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
```

AFTER:
```cpp
        if (firstChunk > 0) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        }

        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
        }
```

**Note:** `memcpy_audio_fixed()` in `memcpyfast_audio.h` already handles small sizes correctly (lines 96-114).

---

### A.5 C6: Remove I/O on Underrun

**File:** `src/DirettaSync.cpp`
**Lines:** 1386-1387

BEFORE:
```cpp
    // Underrun
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        std::cerr << "[DirettaSync] UNDERRUN #" << count
                  << " avail=" << avail << " need=" << currentBytesPerBuffer << std::endl;
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
```

AFTER:
```cpp
    // Underrun
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
```

ADD member variable in `DirettaSync.h`:
```cpp
    std::atomic<uint32_t> m_underrunCount{0};
```

ADD logging in `stop()` or `close()`:
```cpp
    uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
    if (underruns > 0) {
        std::cerr << "[DirettaSync] Session had " << underruns << " underruns" << std::endl;
    }
```

---

### A.6 C7: Consolidate Bit-Reversal LUT

**File:** `src/DirettaRingBuffer.h`

ADD at class scope (around line 103, after DSDConversionMode enum):
```cpp
private:
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
```

REMOVE the `static const uint8_t bitReverseLUT[256]` definitions at lines:
- 654-671 (in `convertDSD_BitReverse` scalar tail)
- 688-705 (in `convertDSD_BitReverse` scalar fallback)
- 834-851 (in `convertDSD_BitReverseSwap` scalar tail)
- 868-885 (in `convertDSD_BitReverseSwap` scalar fallback)

REPLACE all references from `bitReverseLUT[...]` to `kBitReverseLUT[...]`.

**File:** `src/DirettaSync.cpp`

REMOVE lines 47-64 (the unused `bitReverseTable` array) - it's only used by the legacy path.

---

### A.7 S1: Remove Disabled Code Blocks

**File:** `src/DirettaSync.cpp`

DELETE `sendPreTransitionSilence()` function body after `return;` (lines ~1150-1193):
```cpp
void DirettaSync::sendPreTransitionSilence() {
    DIRETTA_LOG("sendPreTransitionSilence: DISABLED FOR TESTING");
    return;
    // DELETE everything from here to end of function
}
```

DELETE `#if 0` block in `reopenForFormatChange()` (lines ~730-758).

---

### A.8 S2: Remove Legacy pushDSDPlanar

**File:** `src/DirettaRingBuffer.h`

DELETE function `pushDSDPlanar()` (lines 290-312).

DELETE function `convertDSDPlanar_AVX2()` (lines 903-1002).

DELETE function `convertDSDPlanar_Scalar()` (lines 1077-1112).

**Verification:** Grep for `pushDSDPlanar` - should have zero callers.

---

## Appendix B: Testing Checklist

After implementing changes, test the following:

### Basic Playback
- [ ] PCM 16-bit/44.1kHz (CD quality)
- [ ] PCM 24-bit/96kHz (high-res)
- [ ] PCM 24-bit/192kHz
- [ ] DSD64
- [ ] DSD128 (if supported by target)

### Format Transitions
- [ ] PCM → PCM (same rate)
- [ ] PCM → PCM (different rate)
- [ ] PCM → DSD
- [ ] DSD → PCM
- [ ] DSD → DSD (different rate)

### Shutdown/Control
- [ ] Stop during playback
- [ ] Pause/Resume
- [ ] Seek during playback
- [ ] Rapid play/stop cycles
- [ ] Clean shutdown (no hangs)

### Stress Tests
- [ ] Long playback (1+ hour)
- [ ] Gapless playback (multiple tracks)
- [ ] Check for underruns in log

---

**End of Report**
