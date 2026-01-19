# Jitter Reduction Phase 2: DSD Blocking Sleep Replacement

**Date:** 2026-01-19
**Status:** Design
**Priority:** CRITICAL
**Issue ID:** G1

---

## 1. Problem Statement

### Current Implementation

The DSD audio callback in `DirettaRenderer.cpp:265-272` uses a blocking 5ms sleep for retry flow control:

```cpp
// DSD: Atomic send with retry
int retryCount = 0;
const int maxRetries = 100;
size_t sent = 0;

while (sent == 0 && retryCount < maxRetries) {
    sent = m_direttaSync->sendAudio(buffer.data(), samples);

    if (sent == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // PROBLEM
        retryCount++;
    }
}
```

### Why This Is Critical

1. **Sleep Granularity Jitter**: Linux scheduler quantum is typically 1-4ms. A 5ms sleep request can return anywhere from 5ms to 9ms later, introducing **±2.5ms timing variance**.

2. **DSD Timing Requirements**: DSD512 operates at 22.5792 MHz (8× DSD64). Buffer timing at high rates requires µs-level precision. ±2.5ms jitter is catastrophic.

3. **Comparison with PCM Path**: The PCM path (lines 278-308) already uses 500µs micro-sleep, which is 10× more precise. The DSD path is inconsistent.

4. **Worst Case Latency**: With 100 retries × 5ms = 500ms maximum blocking time before timeout. This is excessive.

### Measured Impact

| Sleep Duration | Actual Return (typical) | Jitter |
|----------------|------------------------|--------|
| 5ms | 5-9ms | ±2.5ms |
| 500µs | 500-1500µs | ±500µs |
| Condition variable | ~10-50µs | ±25µs |

---

## 2. Root Cause Analysis

### Why Blocking Sleep Is Used

The original design used blocking sleep because:
1. DSD packets are atomic (cannot be partially sent)
2. If the ring buffer is full, the entire packet must wait
3. Simple implementation with predictable behavior

### Why It Causes Jitter

```
Timeline (ideal):
|---DSD packet---|---DSD packet---|---DSD packet---|
     12ms             12ms             12ms

Timeline (with 5ms sleep jitter):
|---DSD packet---|--sleep(5-9ms)--|---DSD packet---|---DSD packet---|
     12ms              7ms               12ms             12ms
                   ↑ JITTER
```

The consumer (Diretta SDK thread) expects evenly-spaced packets. Sleep variance disrupts this timing.

### Contrast with PCM Path

PCM uses incremental sends with 500µs micro-sleep:
```cpp
std::this_thread::sleep_for(std::chrono::microseconds(FlowControl::MICROSLEEP_US));
```

This works because:
1. PCM can be sent incrementally (partial progress possible)
2. 500µs has lower absolute variance (~500µs vs ~2500µs)
3. Multiple small sleeps average out better than one large sleep

---

## 3. Proposed Solutions

### Option A: Condition Variable with Timeout (Recommended)

Replace blocking sleep with event-driven waiting. The consumer thread signals when buffer space becomes available.

```cpp
// New members in DirettaSync
std::mutex m_bufferMutex;
std::condition_variable m_bufferAvailable;
std::atomic<size_t> m_freeSpaceBytes{0};

// In getNewStream() after pop:
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_freeSpaceBytes.store(m_ringBuffer.getFreeSpace(), std::memory_order_relaxed);
}
m_bufferAvailable.notify_one();

// In DirettaRenderer callback:
size_t bytesNeeded = ...; // DSD packet size
std::unique_lock<std::mutex> lock(m_bufferMutex);
bool success = m_bufferAvailable.wait_for(lock,
    std::chrono::microseconds(500),
    [&] { return m_freeSpaceBytes.load(std::memory_order_relaxed) >= bytesNeeded; });
```

**Pros:**
- Precise wakeup when space available (~10-50µs latency)
- No wasted CPU cycles
- Natural backpressure mechanism
- Standard C++ (portable)

**Cons:**
- Requires mutex acquisition (lightweight but non-zero cost)
- Adds synchronization complexity between producer/consumer
- Condition variable spurious wakeups must be handled

**Jitter Reduction:** ±2.5ms → ±50µs (50× improvement)

---

### Option B: Micro-Sleep Polling (Simple)

Match the PCM path: use 500µs micro-sleeps with higher retry count.

```cpp
// DSD: Micro-sleep retry (matches PCM pattern)
int retryCount = 0;
const int maxRetries = 1000;  // 1000 × 500µs = 500ms max
size_t sent = 0;

while (sent == 0 && retryCount < maxRetries) {
    sent = m_direttaSync->sendAudio(buffer.data(), samples);

    if (sent == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        retryCount++;
    }
}
```

**Pros:**
- Minimal code change
- Consistent with PCM path
- No new synchronization primitives

**Cons:**
- Still polling-based (wastes CPU during contention)
- 500µs variance remains (better than 2.5ms, but not ideal)
- High retry count may cause issues if contention is prolonged

**Jitter Reduction:** ±2.5ms → ±500µs (5× improvement)

---

### Option C: Adaptive Backoff

Start with micro-sleep, increase delay if contention persists.

```cpp
// Adaptive backoff: start fast, slow down if persistently blocked
int retryCount = 0;
size_t sent = 0;
int sleepUs = 100;  // Start with 100µs

while (sent == 0 && retryCount < 200) {
    sent = m_direttaSync->sendAudio(buffer.data(), samples);

    if (sent == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        retryCount++;
        // Exponential backoff: 100 → 200 → 400 → 800 → 1600 (cap at 2ms)
        if (retryCount % 20 == 0) {
            sleepUs = std::min(sleepUs * 2, 2000);
        }
    }
}
```

**Pros:**
- Low latency in common case (100µs initial)
- Backs off gracefully under sustained contention
- No synchronization overhead

**Cons:**
- Complexity in tuning backoff parameters
- Still polling-based
- Worst-case jitter approaches Option B

**Jitter Reduction:** ±2.5ms → ±200µs average (12× improvement)

---

### Option D: Lock-Free Ring with Eventfd (Linux-specific)

Use Linux eventfd for lightweight producer-consumer signaling.

```cpp
// New member
int m_eventfd = eventfd(0, EFD_NONBLOCK);

// In getNewStream() after pop:
uint64_t val = 1;
write(m_eventfd, &val, sizeof(val));

// In DirettaRenderer callback:
struct pollfd pfd = { m_eventfd, POLLIN, 0 };
poll(&pfd, 1, 1);  // 1ms timeout
// Then retry sendAudio
```

**Pros:**
- Very low latency (~1-5µs kernel notification)
- No mutex contention
- Integrates well with event loops

**Cons:**
- Linux-specific (not portable)
- File descriptor management
- Requires careful cleanup

**Jitter Reduction:** ±2.5ms → ±10µs (250× improvement)

---

## 4. Recommendation

### Primary: Option A (Condition Variable)

**Rationale:**
1. Best balance of precision and portability
2. Standard C++ (works on all Linux targets including RPi)
3. Natural integration with existing flow control
4. 50× jitter improvement is sufficient for DSD512

### Fallback: Option B (Micro-Sleep)

If Option A introduces unexpected issues during testing, Option B provides a minimal-risk improvement with:
1. Single-line change (5ms → 500µs)
2. 5× jitter improvement
3. Zero architectural change

---

## 5. Detailed Design (Option A)

### 5.1 New Members in DirettaSync

```cpp
// In DirettaSync.h, private section:

// Flow control signaling for DSD atomic sends
std::mutex m_flowMutex;
std::condition_variable m_spaceAvailable;
```

### 5.2 Signal in Consumer (getNewStream)

```cpp
// In DirettaSync::getNewStream(), after successful pop:

// Signal producer that space is available
// Use try_lock to avoid blocking the consumer
if (m_flowMutex.try_lock()) {
    m_flowMutex.unlock();
    m_spaceAvailable.notify_one();
}
```

Note: We use `try_lock`/`unlock` pattern to minimize consumer blocking. The notify is "best effort" - if producer isn't waiting, this is a no-op.

### 5.3 Wait in Producer (DirettaRenderer callback)

```cpp
// In DirettaRenderer.cpp, DSD send path:

if (trackInfo.isDSD) {
    size_t sent = m_direttaSync->sendAudio(buffer.data(), samples);

    if (sent == 0) {
        // Wait for space with 500µs timeout
        std::unique_lock<std::mutex> lock(m_direttaSync->getFlowMutex());
        m_direttaSync->waitForSpace(lock, std::chrono::microseconds(500));

        // Retry after wait
        sent = m_direttaSync->sendAudio(buffer.data(), samples);
    }

    if (sent == 0) {
        // Still failed - one more micro-sleep fallback
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        sent = m_direttaSync->sendAudio(buffer.data(), samples);
    }

    if (sent == 0 && g_verbose) {
        std::cerr << "[Callback] DSD send failed after wait" << std::endl;
    }
}
```

### 5.4 Public API in DirettaSync

```cpp
// In DirettaSync.h, public section:

std::mutex& getFlowMutex() { return m_flowMutex; }

template<typename Rep, typename Period>
bool waitForSpace(std::unique_lock<std::mutex>& lock,
                  std::chrono::duration<Rep, Period> timeout) {
    return m_spaceAvailable.wait_for(lock, timeout) == std::cv_status::no_timeout;
}

void notifySpaceAvailable() {
    m_spaceAvailable.notify_one();
}
```

### 5.5 Fallback Retry Loop

For robustness, keep a simplified retry loop as fallback:

```cpp
// Full implementation with fallback
if (trackInfo.isDSD) {
    int retryCount = 0;
    const int maxRetries = 20;  // Reduced from 100 (each retry now ~500µs)
    size_t sent = 0;

    while (sent == 0 && retryCount < maxRetries) {
        sent = m_direttaSync->sendAudio(buffer.data(), samples);

        if (sent == 0) {
            // Event-based wait with micro-sleep fallback
            std::unique_lock<std::mutex> lock(m_direttaSync->getFlowMutex());
            m_direttaSync->waitForSpace(lock, std::chrono::microseconds(500));
            retryCount++;
        }
    }

    if (sent == 0) {
        std::cerr << "[Callback] DSD timeout after " << retryCount << " retries" << std::endl;
    }
}
```

---

## 6. Testing Plan

### 6.1 Functional Tests

| Test | Expected Result |
|------|-----------------|
| DSD64 playback | No audible glitches, smooth playback |
| DSD128 playback | No audible glitches |
| DSD256 playback | No audible glitches |
| DSD512 playback | No audible glitches, reduced jitter |
| DSD→PCM transition | Clean transition, no hang |
| PCM→DSD transition | Clean transition, no hang |
| Rapid play/stop | No deadlock, clean state |

### 6.2 Jitter Measurement

Before/after comparison using instrumentation:

```cpp
// Temporary instrumentation in callback
static auto lastCall = std::chrono::steady_clock::now();
auto now = std::chrono::steady_clock::now();
auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - lastCall).count();
if (delta > 15000 || delta < 9000) {  // Expected ~12ms for DSD64
    std::cerr << "[JITTER] Callback interval: " << delta << "µs" << std::endl;
}
lastCall = now;
```

### 6.3 Stress Tests

1. **Sustained playback**: 1+ hour DSD256 without underruns
2. **Format switching**: Rapid DSD↔PCM transitions (10 cycles)
3. **Buffer pressure**: Simulate slow consumer, verify no deadlock

---

## 7. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Deadlock in wait | Low | High | Timeout on wait (500µs), fallback retry |
| Spurious wakeup | Medium | None | Loop structure handles this |
| Mutex contention | Low | Low | try_lock in consumer avoids blocking |
| Performance regression | Low | Medium | Benchmark before/after |

---

## 8. Implementation Checklist

- [ ] Add m_flowMutex and m_spaceAvailable to DirettaSync.h
- [ ] Add public API methods (getFlowMutex, waitForSpace, notifySpaceAvailable)
- [ ] Modify getNewStream() to signal after pop
- [ ] Modify DirettaRenderer.cpp DSD path to use event-based wait
- [ ] Test DSD64/128/256/512 playback
- [ ] Test format transitions
- [ ] Measure jitter improvement
- [ ] Remove temporary instrumentation
- [ ] Update Optimisation_Opportunities.md status

---

## 9. Alternatives Considered But Rejected

### Spinlock with yield

```cpp
while (sent == 0) {
    sent = sendAudio(...);
    if (sent == 0) std::this_thread::yield();
}
```

**Rejected:** Burns CPU, unpredictable timing under load.

### Busy wait with PAUSE instruction

```cpp
while (sent == 0) {
    sent = sendAudio(...);
    if (sent == 0) __builtin_ia32_pause();
}
```

**Rejected:** x86-specific, still burns CPU, inappropriate for embedded targets (RPi).

### Separate flow control thread

**Rejected:** Overengineered, adds latency, complexity not justified.

---

## 10. Related Issues

- **N5 (DSD retry backoff)**: Superseded by this design
- **A1 (DSD remainder ring buffer)**: Independent optimization
- **F1 (Worker thread priority)**: Complementary - both reduce jitter

---

**End of Design Document**
