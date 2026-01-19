# Jitter Reduction Phase 2: Implementation Guide

**Date:** 2026-01-19
**Design Document:** `2026-01-19-jitter-reduction-phase2-design.md`
**Issue ID:** G1
**Status:** Ready for Implementation

---

## Overview

This document provides step-by-step implementation instructions for replacing the DSD 5ms blocking sleep with event-based flow control using condition variables (Option A from the design document).

**Expected Outcome:** Reduce DSD timing jitter from ±2.5ms to ±50µs (50× improvement).

---

## Prerequisites

- [ ] Read and understand the design document
- [ ] Ensure clean build before starting
- [ ] Create a backup branch: `git checkout -b feature/g1-dsd-flow-control`

---

## Implementation Steps

### Step 1: Add Flow Control Members to DirettaSync.h

**File:** `src/DirettaSync.h`

**Location:** After line 391 (after `std::mutex m_workerMutex;`)

**Add:**

```cpp
    // G1: Flow control for DSD atomic sends
    // Condition variable allows producer to wait for buffer space
    // without burning CPU or introducing sleep jitter
    std::mutex m_flowMutex;
    std::condition_variable m_spaceAvailable;
```

**Verification:** The new members should appear in the private section near other mutexes.

---

### Step 2: Add Public Flow Control API to DirettaSync.h

**File:** `src/DirettaSync.h`

**Location:** After line 293 (after `const AudioFormat& getFormat()`)

**Add:**

```cpp
    //=========================================================================
    // Flow Control (G1: DSD jitter reduction)
    //=========================================================================

    /**
     * @brief Get flow control mutex for condition variable wait
     * @return Reference to flow mutex
     */
    std::mutex& getFlowMutex() { return m_flowMutex; }

    /**
     * @brief Wait for buffer space with timeout
     * @param lock Unique lock on flow mutex (must be locked)
     * @param timeout Maximum wait duration
     * @return true if notified, false if timeout
     */
    template<typename Rep, typename Period>
    bool waitForSpace(std::unique_lock<std::mutex>& lock,
                      std::chrono::duration<Rep, Period> timeout) {
        return m_spaceAvailable.wait_for(lock, timeout) == std::cv_status::no_timeout;
    }

    /**
     * @brief Signal that buffer space is available
     * Called by consumer (getNewStream) after popping data
     */
    void notifySpaceAvailable() {
        m_spaceAvailable.notify_one();
    }
```

---

### Step 3: Signal Space Available in getNewStream()

**File:** `src/DirettaSync.cpp`

**Location:** After line 1365 (after `m_ringBuffer.pop(dest, currentBytesPerBuffer);`)

**Current code:**

```cpp
    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    m_workerActive = false;
    return true;
}
```

**Replace with:**

```cpp
    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }

    m_workerActive = false;
    return true;
}
```

**Rationale:** The `try_lock`/`unlock` pattern ensures the consumer never blocks. If the producer is waiting on the condition variable, it will be notified. If not, the notification is harmless.

---

### Step 4: Replace DSD Blocking Sleep in DirettaRenderer.cpp

**File:** `src/DirettaRenderer.cpp`

**Location:** Lines 259-276 (DSD send path in audio callback)

**Current code:**

```cpp
                // Send audio (DirettaSync handles all format conversions)
                if (trackInfo.isDSD) {
                    // DSD: Atomic send with retry
                    int retryCount = 0;
                    const int maxRetries = 100;
                    size_t sent = 0;

                    while (sent == 0 && retryCount < maxRetries) {
                        sent = m_direttaSync->sendAudio(buffer.data(), samples);

                        if (sent == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            retryCount++;
                        }
                    }

                    if (sent == 0) {
                        std::cerr << "[Callback] DSD timeout" << std::endl;
                    }
                }
```

**Replace with:**

```cpp
                // Send audio (DirettaSync handles all format conversions)
                if (trackInfo.isDSD) {
                    // DSD: Atomic send with event-based flow control (G1)
                    // Uses condition variable instead of blocking sleep
                    // Reduces jitter from ±2.5ms to ±50µs
                    int retryCount = 0;
                    const int maxRetries = 20;  // Reduced: each wait is ~500µs max
                    size_t sent = 0;

                    while (sent == 0 && retryCount < maxRetries) {
                        sent = m_direttaSync->sendAudio(buffer.data(), samples);

                        if (sent == 0) {
                            // Event-based wait: wake on space available or 500µs timeout
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

**Key changes:**
1. `maxRetries` reduced from 100 to 20 (total max wait: 10ms vs 500ms)
2. `sleep_for(milliseconds(5))` replaced with `waitForSpace(microseconds(500))`
3. Added mutex lock acquisition for condition variable wait

---

### Step 5: Add Required Include (if needed)

**File:** `src/DirettaRenderer.cpp`

**Location:** Near top of file with other includes

**Verify these includes exist:**

```cpp
#include <mutex>
#include <condition_variable>
```

If `<condition_variable>` is missing, add it. (It's likely already included via DirettaSync.h)

---

### Step 6: Build and Verify Compilation

```bash
cd /path/to/DirettaRendererUPnP-X
make clean && make
```

**Expected:** Clean compilation with no errors or warnings related to the changes.

---

## Testing Procedure

### Test 1: Basic DSD Playback

```bash
sudo ./bin/DirettaRendererUPnP --target 1 --verbose
```

Play DSD64, DSD128, DSD256 files. Verify:
- [ ] No audible glitches or dropouts
- [ ] Clean start/stop behavior
- [ ] No error messages in console

### Test 2: Format Transitions

Play a playlist with mixed formats:
1. PCM 44.1kHz → DSD64 → PCM 96kHz → DSD128

Verify:
- [ ] Clean transitions without hangs
- [ ] No deadlock conditions
- [ ] Silence between tracks is clean

### Test 3: Jitter Measurement (Optional)

Add temporary instrumentation to measure callback timing:

**File:** `src/DirettaRenderer.cpp` (temporary - remove after testing)

**Location:** At the start of the DSD send block

```cpp
                if (trackInfo.isDSD) {
                    // TEMPORARY: Jitter measurement
                    static auto lastDsdCall = std::chrono::steady_clock::now();
                    auto now = std::chrono::steady_clock::now();
                    auto deltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - lastDsdCall).count();
                    // DSD64 expects ~12ms intervals, flag deviations > 3ms
                    if (deltaUs > 15000 || (deltaUs < 9000 && deltaUs > 1000)) {
                        std::cerr << "[JITTER] DSD callback interval: " << deltaUs << "µs" << std::endl;
                    }
                    lastDsdCall = now;
                    // END TEMPORARY
```

**Expected:** Significantly fewer jitter warnings compared to before the change.

### Test 4: Stress Test

1. Play DSD256 for 30+ minutes continuously
2. Verify no memory leaks or degradation
3. Check underrun count at end of session

---

## Rollback Procedure

If issues are encountered, the fallback is Option B (simple micro-sleep):

**File:** `src/DirettaRenderer.cpp`

Replace the event-based wait with:

```cpp
                if (trackInfo.isDSD) {
                    // DSD: Micro-sleep retry (Option B fallback)
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

                    if (sent == 0) {
                        std::cerr << "[Callback] DSD timeout" << std::endl;
                    }
                }
```

This provides 5× jitter improvement (±500µs vs ±2.5ms) with minimal code change.

---

## Post-Implementation

### Update Documentation

After successful testing, update `Optimisation_Opportunities.md`:

1. Mark G1 as ✓ IMPLEMENTED
2. Update the summary table
3. Record measured jitter improvement

### Commit Message Template

```
G1: Replace DSD blocking sleep with condition variable

- Add flow control mutex and condition variable to DirettaSync
- Signal space available after ring buffer pop in getNewStream()
- Replace 5ms sleep with 500µs condition variable wait in callback
- Reduce max retries from 100 to 20 (total max wait: 10ms)

Reduces DSD timing jitter from ±2.5ms to ±50µs (50× improvement).

Fixes: G1 (DSD blocking sleep - CRITICAL jitter source)
See: docs/plans/2026-01-19-jitter-reduction-phase2-design.md

Co-Authored-By: Claude <noreply@anthropic.com>
```

---

## Summary of Changes

| File | Change Type | Lines Affected |
|------|-------------|----------------|
| `DirettaSync.h` | Add members | +2 lines (flow mutex, condvar) |
| `DirettaSync.h` | Add public API | +20 lines (getFlowMutex, waitForSpace, notifySpaceAvailable) |
| `DirettaSync.cpp` | Signal after pop | +5 lines in getNewStream() |
| `DirettaRenderer.cpp` | Replace sleep | ~15 lines modified |

**Total:** ~40 lines changed

---

## Appendix: Full Code Snippets

### A.1 Complete DirettaSync.h Additions

```cpp
// === In public section, after getFormat() ===

    //=========================================================================
    // Flow Control (G1: DSD jitter reduction)
    //=========================================================================

    /**
     * @brief Get flow control mutex for condition variable wait
     * @return Reference to flow mutex
     */
    std::mutex& getFlowMutex() { return m_flowMutex; }

    /**
     * @brief Wait for buffer space with timeout
     * @param lock Unique lock on flow mutex (must be locked)
     * @param timeout Maximum wait duration
     * @return true if notified, false if timeout
     */
    template<typename Rep, typename Period>
    bool waitForSpace(std::unique_lock<std::mutex>& lock,
                      std::chrono::duration<Rep, Period> timeout) {
        return m_spaceAvailable.wait_for(lock, timeout) == std::cv_status::no_timeout;
    }

    /**
     * @brief Signal that buffer space is available
     * Called by consumer (getNewStream) after popping data
     */
    void notifySpaceAvailable() {
        m_spaceAvailable.notify_one();
    }

// === In private section, after m_workerMutex ===

    // G1: Flow control for DSD atomic sends
    // Condition variable allows producer to wait for buffer space
    // without burning CPU or introducing sleep jitter
    std::mutex m_flowMutex;
    std::condition_variable m_spaceAvailable;
```

### A.2 Complete getNewStream() Modification

```cpp
    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }

    m_workerActive = false;
    return true;
}
```

### A.3 Complete DirettaRenderer.cpp DSD Path

```cpp
                // Send audio (DirettaSync handles all format conversions)
                if (trackInfo.isDSD) {
                    // DSD: Atomic send with event-based flow control (G1)
                    // Uses condition variable instead of blocking sleep
                    // Reduces jitter from ±2.5ms to ±50µs
                    int retryCount = 0;
                    const int maxRetries = 20;  // Reduced: each wait is ~500µs max
                    size_t sent = 0;

                    while (sent == 0 && retryCount < maxRetries) {
                        sent = m_direttaSync->sendAudio(buffer.data(), samples);

                        if (sent == 0) {
                            // Event-based wait: wake on space available or 500µs timeout
                            std::unique_lock<std::mutex> lock(m_direttaSync->getFlowMutex());
                            m_direttaSync->waitForSpace(lock, std::chrono::microseconds(500));
                            retryCount++;
                        }
                    }

                    if (sent == 0) {
                        std::cerr << "[Callback] DSD timeout after " << retryCount << " retries" << std::endl;
                    }
                } else {
                    // PCM path unchanged...
```

---

**End of Implementation Guide**
