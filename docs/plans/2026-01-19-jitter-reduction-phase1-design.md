# Jitter Reduction Phase 1 Design

**Date:** 2026-01-19
**Status:** Ready for implementation
**Scope:** AudioEngine (DSD path, resampler), DirettaSync (logging, threading)

## Objective

Reduce timing variance (jitter) in the audio pipeline by eliminating allocation spikes, replacing O(n) operations with O(1) alternatives, removing kernel I/O from hot paths, and improving thread scheduling determinism.

## Optimisations

| ID | Optimisation | Location | Impact |
|----|--------------|----------|--------|
| A1 | DSD remainder ring buffer | AudioEngine.cpp | 5-50µs/frame saved |
| A2 | Resampler buffer pre-allocation | AudioEngine.cpp | 50-200µs spikes eliminated |
| A3 | Non-blocking logging | DirettaSync.cpp | 5-10ms I/O blocks eliminated |
| F1 | Worker thread priority elevation | DirettaSync.cpp | Scheduling jitter eliminated |

---

## A1: DSD Remainder Ring Buffer

### Problem

`AudioDecoder::readSamples()` uses a linear buffer for DSD packet remainders (`AudioEngine.cpp:609-630`). When packets don't align with the requested frame size, remaining bytes are stored and retrieved on the next call.

Current implementation:
```cpp
// Store remainder (O(1) - acceptable)
size_t remainder = totalBytes - usedBytes;
memcpy(m_dsdPacketRemainder.data(), packetData + usedBytes, remainder);
m_dsdRemainderCount = remainder;

// Retrieve remainder (O(n) - problematic)
if (m_dsdRemainderCount > 0) {
    size_t toUse = std::min(m_dsdRemainderCount, bytesNeeded);
    memcpy(destBuffer, m_dsdPacketRemainder.data(), toUse);

    // Shift remaining data to front - O(n) memmove!
    memmove(m_dsdPacketRemainder.data(),
            m_dsdPacketRemainder.data() + toUse,
            m_dsdRemainderCount - toUse);
    m_dsdRemainderCount -= toUse;
}
```

For DSD512 at high frame rates, `memmove()` adds 5-50µs variance per frame depending on remainder size.

### Solution

Replace the linear buffer with a small power-of-2 ring buffer (4KB). Both push and pop become O(1) operations.

**New members (AudioEngine.h):**

```cpp
// DSD remainder ring buffer (replaces m_dsdPacketRemainder + m_dsdRemainderCount)
static constexpr size_t DSD_REMAINDER_SIZE = 4096;  // Power of 2
static constexpr size_t DSD_REMAINDER_MASK = DSD_REMAINDER_SIZE - 1;
alignas(64) uint8_t m_dsdRemainderRing[DSD_REMAINDER_SIZE];
size_t m_dsdRemainderReadPos{0};
size_t m_dsdRemainderWritePos{0};
```

**Helper methods (AudioEngine.cpp):**

```cpp
// Available bytes in remainder ring
size_t dsdRemainderAvailable() const {
    return (m_dsdRemainderWritePos - m_dsdRemainderReadPos) & DSD_REMAINDER_MASK;
}

// Free space in remainder ring (leave 1 byte to distinguish full from empty)
size_t dsdRemainderFree() const {
    return (m_dsdRemainderReadPos - m_dsdRemainderWritePos - 1) & DSD_REMAINDER_MASK;
}

// Push to remainder ring (returns bytes actually written)
size_t dsdRemainderPush(const uint8_t* data, size_t len) {
    size_t free = dsdRemainderFree();
    if (len > free) len = free;
    if (len == 0) return 0;

    size_t wp = m_dsdRemainderWritePos;
    size_t firstChunk = std::min(len, DSD_REMAINDER_SIZE - wp);

    memcpy(m_dsdRemainderRing + wp, data, firstChunk);
    if (firstChunk < len) {
        memcpy(m_dsdRemainderRing, data + firstChunk, len - firstChunk);
    }

    m_dsdRemainderWritePos = (wp + len) & DSD_REMAINDER_MASK;
    return len;
}

// Pop from remainder ring (returns bytes actually read)
size_t dsdRemainderPop(uint8_t* dest, size_t len) {
    size_t avail = dsdRemainderAvailable();
    if (len > avail) len = avail;
    if (len == 0) return 0;

    size_t rp = m_dsdRemainderReadPos;
    size_t firstChunk = std::min(len, DSD_REMAINDER_SIZE - rp);

    memcpy(dest, m_dsdRemainderRing + rp, firstChunk);
    if (firstChunk < len) {
        memcpy(dest + firstChunk, m_dsdRemainderRing, len - firstChunk);
    }

    m_dsdRemainderReadPos = (rp + len) & DSD_REMAINDER_MASK;
    return len;
}

// Clear remainder ring (on seek or track change)
void dsdRemainderClear() {
    m_dsdRemainderReadPos = 0;
    m_dsdRemainderWritePos = 0;
}
```

**Modified readSamples() DSD path:**

```cpp
// Retrieve from remainder (O(1) now!)
size_t fromRemainder = dsdRemainderPop(destBuffer, bytesNeeded);
destBuffer += fromRemainder;
bytesNeeded -= fromRemainder;

// ... read more packets if needed ...

// Store remainder (O(1) - unchanged complexity)
if (remainder > 0) {
    dsdRemainderPush(packetData + usedBytes, remainder);
}
```

### State Classification

- **Ring buffer pointers**: Thread-local to audio thread, no synchronisation needed
- **Ring data**: Only accessed by audio thread

### Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add ring buffer members, remove m_dsdPacketRemainder |
| `src/AudioEngine.cpp` | Replace memmove with ring buffer operations |

### Testing

- [ ] DSD64 playback (remainder handling)
- [ ] DSD512 playback (high frame rate)
- [ ] Seek during DSD playback (remainder clear)
- [ ] Track transitions (remainder clear)

---

## A2: Resampler Buffer Pre-allocation

### Problem

`AudioDecoder::readSamples()` dynamically resizes the resampler buffer when capacity is exceeded (`AudioEngine.cpp:911-918`):

```cpp
size_t tempBufferSize = totalOutSamples * bytesPerSample;
if (tempBufferSize > m_resampleBufferCapacity) {
    // Reallocation on threshold crossing - causes allocation spike!
    size_t newCapacity = static_cast<size_t>(tempBufferSize * 1.5);
    m_resampleBuffer.resize(newCapacity);  // delete[] + new[]
    m_resampleBufferCapacity = m_resampleBuffer.size();
}
```

When capacity threshold is crossed (e.g., first time playing 768kHz after 48kHz), this triggers 50-200µs allocation spikes.

### Solution

Pre-allocate a fixed 256KB buffer in `initResampler()` that covers all supported sample rates (up to 768kHz/32-bit stereo).

**Calculation:**
- Max sample rate: 768,000 Hz
- Max bit depth: 32-bit (4 bytes)
- Max channels: 2 (stereo)
- Typical frame size: 4096 samples
- Buffer need: 4096 × 4 × 2 = 32,768 bytes per frame
- With 1.5× headroom for resampler delay: ~49KB
- Fixed at 256KB for safety margin (covers future multichannel)

**Modified initResampler():**

```cpp
bool AudioDecoder::initResampler(/* ... */) {
    // ... existing setup code ...

    // Pre-allocate resampler buffer to fixed capacity (never reallocate)
    static constexpr size_t RESAMPLER_BUFFER_CAPACITY = 262144;  // 256KB
    if (m_resampleBuffer.size() < RESAMPLER_BUFFER_CAPACITY) {
        m_resampleBuffer.resize(RESAMPLER_BUFFER_CAPACITY);
    }
    m_resampleBufferCapacity = RESAMPLER_BUFFER_CAPACITY;

    // ... rest of init ...
}
```

**Modified readSamples():**

```cpp
size_t tempBufferSize = totalOutSamples * bytesPerSample;

// Safety check (should never trigger with 256KB buffer)
if (tempBufferSize > m_resampleBufferCapacity) {
    // Log warning but don't reallocate in hot path
    // This indicates a configuration we didn't anticipate
    DIRETTA_LOG("[AudioDecoder] Warning: resampler buffer insufficient: "
                << tempBufferSize << " > " << m_resampleBufferCapacity);
    return 0;  // Or handle gracefully
}
```

### Memory Ordering

No atomics involved - buffer is thread-local to audio thread.

### Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.cpp` | Pre-allocate buffer in initResampler(), remove dynamic resize |

### Testing

- [ ] PCM 44.1kHz playback (baseline)
- [ ] PCM 768kHz playback (high rate)
- [ ] Format transitions 44.1kHz → 768kHz (no allocation spike)
- [ ] Multiple track plays (buffer reused)

---

## A3: Non-Blocking Logging Infrastructure

### Problem

Verbose logging in `sendAudio()` uses `std::cout` which blocks on kernel I/O (`DirettaSync.cpp:1206-1213`):

```cpp
if (g_verbose) {
    int count = m_pushCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 3 || count % 500 == 0) {
        DIRETTA_LOG("[DirettaSync] sendAudio: pushed " << written << " bytes");
    }
}
```

When verbose logging is enabled:
- `std::cout` triggers kernel I/O
- Can block 5-10ms waiting for terminal buffer
- Affects audio timing even though logging is "conditional"

### Solution

Replace direct `std::cout` with a lock-free ring buffer that a background thread drains.

**New log ring buffer (DirettaSync.h):**

```cpp
// Lock-free log ring buffer
struct LogEntry {
    std::chrono::steady_clock::time_point timestamp;
    char message[256];
    uint8_t level;  // 0=debug, 1=info, 2=warn, 3=error
};

class LogRing {
public:
    static constexpr size_t CAPACITY = 1024;  // Must be power of 2
    static constexpr size_t MASK = CAPACITY - 1;

    // Lock-free push (returns false if full)
    bool push(const LogEntry& entry) {
        size_t wp = m_writePos.load(std::memory_order_relaxed);
        size_t rp = m_readPos.load(std::memory_order_acquire);

        if (((wp + 1) & MASK) == rp) {
            return false;  // Full, drop message
        }

        m_entries[wp] = entry;
        m_writePos.store((wp + 1) & MASK, std::memory_order_release);
        return true;
    }

    // Pop for drain thread
    bool pop(LogEntry& entry) {
        size_t rp = m_readPos.load(std::memory_order_relaxed);
        size_t wp = m_writePos.load(std::memory_order_acquire);

        if (rp == wp) {
            return false;  // Empty
        }

        entry = m_entries[rp];
        m_readPos.store((rp + 1) & MASK, std::memory_order_release);
        return true;
    }

private:
    LogEntry m_entries[CAPACITY];
    alignas(64) std::atomic<size_t> m_writePos{0};
    alignas(64) std::atomic<size_t> m_readPos{0};
};
```

**Modified DIRETTA_LOG macro (DirettaLog.h or inline):**

```cpp
#define DIRETTA_LOG_ASYNC(msg) do { \
    if (g_logRing) { \
        LogEntry entry; \
        entry.timestamp = std::chrono::steady_clock::now(); \
        entry.level = 1; \
        std::ostringstream oss; \
        oss << msg; \
        strncpy(entry.message, oss.str().c_str(), sizeof(entry.message) - 1); \
        entry.message[sizeof(entry.message) - 1] = '\0'; \
        g_logRing->push(entry); \
    } \
} while(0)
```

**Log drain thread (started in DirettaRenderer or main):**

```cpp
void logDrainThread(LogRing* ring, std::atomic<bool>* stop) {
    while (!stop->load(std::memory_order_acquire)) {
        LogEntry entry;
        while (ring->pop(entry)) {
            // Format and output (can block here, it's a background thread)
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()).count();
            std::cout << "[" << elapsed << "] " << entry.message << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Drain remaining on shutdown
    LogEntry entry;
    while (ring->pop(entry)) {
        std::cout << entry.message << std::endl;
    }
}
```

### Hot/Cold Path Classification

- **Hot path**: `DIRETTA_LOG_ASYNC()` - single atomic compare + memcpy (~50ns)
- **Cold path**: Log drain thread - can take arbitrary time, runs in background

### State Classification

- **m_writePos**: Modified by hot path (multiple threads potentially)
- **m_readPos**: Modified only by drain thread
- **m_entries[]**: Written by hot path, read by drain thread (protected by positions)

### Memory Ordering Justification

- **push() write position**: `release` ensures entry data visible before position update
- **pop() read position**: `release` ensures entry copied before position update
- **Acquire on opposite position**: Establishes synchronises-with relationship

### Files Modified

| File | Changes |
|------|---------|
| `src/DirettaSync.h` | Add LogRing class and global pointer |
| `src/DirettaSync.cpp` | Replace DIRETTA_LOG with DIRETTA_LOG_ASYNC in hot paths |
| `src/DirettaRenderer.cpp` | Start/stop log drain thread |
| `src/main.cpp` | Initialise global log ring |

### Testing

- [ ] Verbose logging enabled, no audio glitches
- [ ] Log messages appear (eventually) in output
- [ ] Clean shutdown drains remaining messages
- [ ] High-rate playback with verbose logging

---

## F1: Worker Thread Priority Elevation

### Problem

The Diretta sync worker thread (`getNewStream()` callback loop) runs at default OS priority. On a busy system, it can be preempted by background processes, causing scheduling jitter that manifests as audio glitches.

Linux default scheduling:
- `SCHED_OTHER` (time-sharing)
- Priority 0 (normal)
- Can be preempted by any higher-priority process

### Solution

Elevate worker thread to real-time scheduling with SCHED_FIFO policy.

**Thread priority elevation (DirettaSync.cpp):**

```cpp
#include <pthread.h>
#include <sched.h>

void DirettaSync::elevateThreadPriority() {
#ifdef __linux__
    pthread_t thread = pthread_self();

    // SCHED_FIFO: Real-time FIFO scheduling
    // Priority: max-1 (leave max for kernel threads)
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;

    int result = pthread_setschedparam(thread, SCHED_FIFO, &param);
    if (result != 0) {
        // Not fatal - may lack CAP_SYS_NICE capability
        DIRETTA_LOG("[DirettaSync] Warning: Could not set real-time priority: "
                    << strerror(result));
        DIRETTA_LOG("[DirettaSync] Run with sudo or set CAP_SYS_NICE capability");
    } else {
        DIRETTA_LOG("[DirettaSync] Worker thread elevated to SCHED_FIFO priority "
                    << param.sched_priority);
    }
#endif
}
```

**Call from worker thread entry:**

```cpp
// In the worker thread function (or getNewStream first call)
void DirettaSync::workerThreadEntry() {
    // Elevate priority at thread start
    elevateThreadPriority();

    // ... existing worker loop ...
}
```

**Alternative: Call at first getNewStream():**

```cpp
bool DirettaSync::getNewStream(DIRETTA::Stream& stream) {
    // One-time priority elevation
    static std::once_flag priorityFlag;
    std::call_once(priorityFlag, [this]() {
        elevateThreadPriority();
    });

    // ... existing implementation ...
}
```

### System Requirements

- **Linux**: Requires `CAP_SYS_NICE` capability or root
- **Setting capability**: `sudo setcap cap_sys_nice+ep ./bin/DirettaRendererUPnP`
- **Alternative**: Run with `sudo`

### Risk Assessment

- **Priority inversion**: Low risk - worker thread does minimal locking
- **Starvation**: Low risk - worker thread blocks waiting for SDK callbacks
- **System stability**: Priority is max-1, leaving headroom for kernel

### Files Modified

| File | Changes |
|------|---------|
| `src/DirettaSync.h` | Add elevateThreadPriority() declaration |
| `src/DirettaSync.cpp` | Implement priority elevation, call from worker |

### Testing

- [ ] Priority elevation succeeds with sudo
- [ ] Warning logged without sudo (graceful degradation)
- [ ] Audio playback under CPU load (stress test)
- [ ] No system instability with real-time priority
- [ ] `htop` shows worker thread with RT priority

---

## Implementation Order

Recommended order based on dependencies and risk:

1. **A2: Resampler buffer pre-allocation** - Very low risk, immediate benefit
2. **A1: DSD remainder ring buffer** - Low risk, significant benefit for DSD
3. **F1: Worker thread priority** - Low risk, significant benefit under load
4. **A3: Non-blocking logging** - Medium complexity, benefit only when logging enabled

## Combined Testing Checklist

### Functional

- [ ] PCM 16-bit/44.1kHz playback
- [ ] PCM 24-bit/96kHz playback
- [ ] PCM 24-bit/192kHz playback
- [ ] DSD64 playback
- [ ] DSD128/DSD256/DSD512 playback
- [ ] Format transitions (PCM→DSD, DSD→PCM)
- [ ] Gapless track transitions

### Stress

- [ ] High CPU load during playback (`stress -c 4`)
- [ ] Verbose logging during playback
- [ ] Rapid format switching
- [ ] Extended playback sessions (1+ hour)

### Measurement

Before and after each optimisation:
- [ ] P99 latency of audio callback
- [ ] Underrun count over test session
- [ ] CPU usage (should be similar or lower)

---

## Summary

| ID | Optimisation | Variance Eliminated | Risk |
|----|--------------|---------------------|------|
| A1 | DSD remainder ring buffer | 5-50µs/frame | Low |
| A2 | Resampler buffer pre-allocation | 50-200µs spikes | Very Low |
| A3 | Non-blocking logging | 5-10ms I/O blocks | Low |
| F1 | Worker thread priority | OS scheduling jitter | Low |

**Total commits expected:** 4 (one per optimisation)

**Implementation document:** See `2026-01-19-jitter-reduction-phase1-impl.md` for step-by-step tasks.
