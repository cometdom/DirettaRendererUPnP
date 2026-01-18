# Hot Path Generation Counters Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce per-call atomic load overhead in producer and consumer hot paths using generation counters and ring buffer optimizations.

**Architecture:** Add generation counters to batch stable configuration loads (format on producer, state on consumer). Optimize ring buffer with direct write API and inlined position loads. Lighter memory ordering in RingAccessGuard where safe.

**Tech Stack:** C++17, std::atomic, lock-free patterns

---

## Task 1: Add Format Generation Counter Members (P1 - Part 1)

**Files:**
- Modify: `src/DirettaSync.h:323` (after m_underrunCount)

**Step 1: Add generation counter and cached format members**

In `src/DirettaSync.h`, after line 323 (`std::atomic<uint32_t> m_underrunCount{0};`), add:

```cpp
    // Format generation counter - incremented on ANY format change
    std::atomic<uint32_t> m_formatGeneration{0};

    // Cached format values for sendAudio fast path (producer thread only)
    uint32_t m_cachedFormatGen{0};
    bool m_cachedDsdMode{false};
    bool m_cachedPack24bit{false};
    bool m_cachedUpsample16to32{false};
    bool m_cachedNeedBitReversal{false};
    bool m_cachedNeedByteSwap{false};
    int m_cachedChannels{2};
    int m_cachedBytesPerSample{2};
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS (new members unused yet)

**Step 3: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
refactor(P1): add format generation counter members

Add m_formatGeneration atomic and cached format values for sendAudio
fast path optimization. Members are added but not yet used.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Increment Format Generation in Configure Functions (P1 - Part 2)

**Files:**
- Modify: `src/DirettaSync.cpp:815` (end of configureRingPCM)
- Modify: `src/DirettaSync.cpp:848` (end of configureRingDSD)

**Step 1: Add increment at end of configureRingPCM**

In `src/DirettaSync.cpp`, find line 815 (the closing brace of configureRingPCM after the DIRETTA_LOG). Add before the closing brace:

```cpp
    // Increment format generation to invalidate cached values
    m_formatGeneration.fetch_add(1, std::memory_order_release);
```

**Step 2: Add increment at end of configureRingDSD**

In `src/DirettaSync.cpp`, find line 848 (the closing brace of configureRingDSD after the DIRETTA_LOG). Add before the closing brace:

```cpp
    // Increment format generation to invalidate cached values
    m_formatGeneration.fetch_add(1, std::memory_order_release);
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 4: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
refactor(P1): increment format generation in configure functions

configureRingPCM and configureRingDSD now increment m_formatGeneration
to signal format changes to sendAudio.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Use Format Generation Counter in sendAudio (P1 - Part 3)

**Files:**
- Modify: `src/DirettaSync.cpp:941-948` (atomic loads in sendAudio)

**Step 1: Replace atomic loads with generation check**

Find lines 941-948 in sendAudio():

```cpp
    // Snapshot config state
    bool dsdMode = m_isDsdMode.load(std::memory_order_acquire);
    bool pack24bit = m_need24BitPack.load(std::memory_order_acquire);
    bool upsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
    bool needBitReversal = m_needDsdBitReversal.load(std::memory_order_acquire);
    bool needByteSwap = m_needDsdByteSwap.load(std::memory_order_acquire);
    int numChannels = m_channels.load(std::memory_order_acquire);
    int bytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
```

Replace with:

```cpp
    // Generation counter optimization: single atomic load in common case
    uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
    if (gen != m_cachedFormatGen) {
        // Cold path: reload all format values (only on format change)
        m_cachedDsdMode = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedPack24bit = m_need24BitPack.load(std::memory_order_acquire);
        m_cachedUpsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
        m_cachedNeedBitReversal = m_needDsdBitReversal.load(std::memory_order_acquire);
        m_cachedNeedByteSwap = m_needDsdByteSwap.load(std::memory_order_acquire);
        m_cachedChannels = m_channels.load(std::memory_order_acquire);
        m_cachedBytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
        m_cachedFormatGen = gen;
    }

    // Hot path: use cached values
    bool dsdMode = m_cachedDsdMode;
    bool pack24bit = m_cachedPack24bit;
    bool upsample16to32 = m_cachedUpsample16to32;
    bool needBitReversal = m_cachedNeedBitReversal;
    bool needByteSwap = m_cachedNeedByteSwap;
    int numChannels = m_cachedChannels;
    int bytesPerSample = m_cachedBytesPerSample;
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
perf(P1): use format generation counter in sendAudio

Replace 7 atomic loads with single generation check in common case.
Format values are cached and only reloaded when generation changes.

Hot path: 1 atomic load + comparison (~99.9% of calls)
Cold path: 7 atomic loads (only on format change)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Add Direct Write API to Ring Buffer (P2)

**Files:**
- Modify: `src/DirettaRingBuffer.h:150` (before push methods section)

**Step 1: Add getDirectWriteRegion and commitDirectWrite methods**

In `src/DirettaRingBuffer.h`, find line 150 (the push methods section comment). Add before it:

```cpp
    //=========================================================================
    // Direct Write API (zero-copy fast path)
    //=========================================================================

    /**
     * @brief Get direct write pointer for zero-copy writes
     * @param needed Minimum bytes needed
     * @param region Output: pointer to contiguous write region
     * @param available Output: bytes available in region
     * @return true if contiguous space >= needed is available
     */
    bool getDirectWriteRegion(size_t needed, uint8_t*& region, size_t& available) {
        if (size_ == 0) return false;

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);

        // Contiguous space from writePos to either readPos or end of buffer
        size_t toEnd = size_ - wp;
        size_t totalFree = (rp - wp - 1) & mask_;
        size_t contiguous = std::min(toEnd, totalFree);

        if (contiguous >= needed) {
            region = buffer_.data() + wp;
            available = contiguous;
            return true;
        }
        return false;
    }

    /**
     * @brief Commit a direct write, advancing write pointer
     * @param written Number of bytes written to the region
     */
    void commitDirectWrite(size_t written) {
        size_t wp = writePos_.load(std::memory_order_relaxed);
        writePos_.store((wp + written) & mask_, std::memory_order_release);
    }

```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "$(cat <<'EOF'
refactor(P2): add direct write API to ring buffer

Add getDirectWriteRegion() and commitDirectWrite() for zero-copy
writes when contiguous space is available (~99% of operations).

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Optimize push() with Direct Write and Inlined Loads (P2 + P3)

**Files:**
- Modify: `src/DirettaRingBuffer.h:157-173` (push method)

**Step 1: Replace push() implementation**

Find the push() method (lines 157-173):

```cpp
    size_t push(const uint8_t* data, size_t len) {
        if (size_ == 0) return 0;
        size_t free = getFreeSpace();
        if (len > free) len = free;
        if (len == 0) return 0;

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - wp);

        memcpy_audio(buffer_.data() + wp, data, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(buffer_.data(), data + firstChunk, len - firstChunk);
        }

        writePos_.store((wp + len) & mask_, std::memory_order_release);
        return len;
    }
```

Replace with:

```cpp
    size_t push(const uint8_t* data, size_t len) {
        // Fast path: try direct write (no wraparound)
        uint8_t* region;
        size_t available;
        if (getDirectWriteRegion(len, region, available)) {
            memcpy_audio(region, data, len);
            commitDirectWrite(len);
            return len;
        }

        // Slow path: handle wraparound with inlined position loads
        if (size_ == 0) return 0;

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t free = (rp - wp - 1) & mask_;

        if (len > free) len = free;
        if (len == 0) return 0;

        size_t firstChunk = std::min(len, size_ - wp);
        memcpy_audio(buffer_.data() + wp, data, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(buffer_.data(), data + firstChunk, len - firstChunk);
        }

        writePos_.store((wp + len) & mask_, std::memory_order_release);
        return len;
    }
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "$(cat <<'EOF'
perf(P2,P3): optimize push() with direct write and inlined loads

- Fast path uses getDirectWriteRegion for zero-copy (~99% of calls)
- Slow path inlines position loads to avoid redundant atomic reads
- Eliminates getFreeSpace() call which loaded both positions

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Optimize pop() with Inlined Position Loads (P3)

**Files:**
- Modify: `src/DirettaRingBuffer.h:509-525` (pop method)

**Step 1: Replace pop() implementation**

Find the pop() method (lines 509-525):

```cpp
    size_t pop(uint8_t* dest, size_t len) {
        if (size_ == 0) return 0;
        size_t avail = getAvailable();
        if (len > avail) len = avail;
        if (len == 0) return 0;

        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t firstChunk = std::min(len, size_ - rp);

        memcpy_audio(dest, buffer_.data() + rp, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(dest + firstChunk, buffer_.data(), len - firstChunk);
        }

        readPos_.store((rp + len) & mask_, std::memory_order_release);
        return len;
    }
```

Replace with:

```cpp
    size_t pop(uint8_t* dest, size_t len) {
        if (size_ == 0) return 0;

        // Inline position loads to avoid redundant atomic reads
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t avail = (wp - rp) & mask_;

        if (len > avail) len = avail;
        if (len == 0) return 0;

        // rp already loaded, reuse directly
        size_t firstChunk = std::min(len, size_ - rp);

        memcpy_audio(dest, buffer_.data() + rp, firstChunk);
        if (firstChunk < len) {
            memcpy_audio(dest + firstChunk, buffer_.data(), len - firstChunk);
        }

        readPos_.store((rp + len) & mask_, std::memory_order_release);
        return len;
    }
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "$(cat <<'EOF'
perf(P3): optimize pop() with inlined position loads

Inline getAvailable() logic to avoid loading readPos_ twice.
Both positions loaded once and reused.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Add Consumer State Generation Counter Members (C1 - Part 1)

**Files:**
- Modify: `src/DirettaSync.h:332` (after m_cachedBytesPerSample)

**Step 1: Add consumer generation counter and cached state members**

In `src/DirettaSync.h`, after the P1 cached members (after `int m_cachedBytesPerSample{2};`), add:

```cpp

    // Consumer state generation - incremented on config changes
    std::atomic<uint32_t> m_consumerStateGen{0};

    // Cached consumer state for getNewStream fast path (worker thread only)
    uint32_t m_cachedConsumerGen{0};
    int m_cachedBytesPerBuffer{176};
    uint32_t m_cachedFramesRemainder{0};
    int m_cachedBytesPerFrame{0};
    bool m_cachedConsumerIsDsd{false};
    uint8_t m_cachedSilenceByte{0};
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
refactor(C1): add consumer state generation counter members

Add m_consumerStateGen atomic and cached consumer state values for
getNewStream fast path optimization.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Increment Consumer Generation in Configure Functions (C1 - Part 2)

**Files:**
- Modify: `src/DirettaSync.cpp` (configureRingPCM and configureRingDSD)

**Step 1: Add consumer generation increment after format generation increment**

In both `configureRingPCM()` and `configureRingDSD()`, after the line:
```cpp
    m_formatGeneration.fetch_add(1, std::memory_order_release);
```

Add:
```cpp
    m_consumerStateGen.fetch_add(1, std::memory_order_release);
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
refactor(C1): increment consumer state generation in configure functions

Both configureRingPCM and configureRingDSD now increment
m_consumerStateGen alongside m_formatGeneration.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Use Consumer State Generation Counter in getNewStream (C1 - Part 3)

**Files:**
- Modify: `src/DirettaSync.cpp:1026-1040` (start of getNewStream)

**Step 1: Replace atomic loads with generation check**

Find lines 1026-1040 in getNewStream():

```cpp
bool DirettaSync::getNewStream(DIRETTA::Stream& stream) {
    m_workerActive = true;

    int currentBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
    uint32_t remainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
    if (remainder != 0) {
        int bytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        uint32_t acc = m_framesPerBufferAccumulator.load(std::memory_order_relaxed);
        acc += remainder;
        if (acc >= 1000) {
            acc -= 1000;
            currentBytesPerBuffer += bytesPerFrame;
        }
        m_framesPerBufferAccumulator.store(acc, std::memory_order_relaxed);
    }
    uint8_t currentSilenceByte = m_ringBuffer.silenceByte();
```

Replace with:

```cpp
bool DirettaSync::getNewStream(DIRETTA::Stream& stream) {
    m_workerActive = true;

    // Generation counter optimization: single atomic load in common case
    uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
    if (gen != m_cachedConsumerGen) {
        // Cold path: reload stable configuration (only on format change)
        m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
        m_cachedFramesRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
        m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedSilenceByte = m_ringBuffer.silenceByte();
        m_cachedConsumerGen = gen;
    }

    // Hot path: use cached values
    int currentBytesPerBuffer = m_cachedBytesPerBuffer;
    uint32_t remainder = m_cachedFramesRemainder;
    if (remainder != 0) {
        int bytesPerFrame = m_cachedBytesPerFrame;
        uint32_t acc = m_framesPerBufferAccumulator.load(std::memory_order_relaxed);
        acc += remainder;
        if (acc >= 1000) {
            acc -= 1000;
            currentBytesPerBuffer += bytesPerFrame;
        }
        m_framesPerBufferAccumulator.store(acc, std::memory_order_relaxed);
    }
    uint8_t currentSilenceByte = m_cachedSilenceByte;
```

**Step 2: Update isDsd usage later in function**

Find line 1055:
```cpp
    bool currentIsDsd = m_isDsdMode.load(std::memory_order_acquire);
```

Replace with:
```cpp
    bool currentIsDsd = m_cachedConsumerIsDsd;
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 4: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
perf(C1): use consumer state generation counter in getNewStream

Replace 5 atomic loads with single generation check in common case.
Stable configuration cached and only reloaded on format change.

Volatile state (m_stopRequested, m_silenceBuffersRemaining, etc.)
still checked fresh as it can change mid-playback.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Lighten RingAccessGuard Memory Ordering (C2)

**Files:**
- Modify: `src/DirettaSync.cpp:14-40` (RingAccessGuard class)

**Step 1: Update memory ordering in RingAccessGuard**

Find the RingAccessGuard class (lines 14-40):

```cpp
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) {
            return;
        }
        users_.fetch_add(1, std::memory_order_acq_rel);
        if (reconfiguring.load(std::memory_order_acquire)) {
            users_.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        active_ = true;
    }

    ~RingAccessGuard() {
        if (active_) {
            users_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool active() const { return active_; }

private:
    std::atomic<int>& users_;
    bool active_;
};
```

Replace with:

```cpp
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) {
            return;
        }
        // MUST use acquire: ensures increment visible to beginReconfigure()
        // before any ring buffer operations
        users_.fetch_add(1, std::memory_order_acquire);
        if (reconfiguring.load(std::memory_order_acquire)) {
            // Bail-out: never entered guarded section, relaxed is safe
            users_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        active_ = true;
    }

    ~RingAccessGuard() {
        if (active_) {
            // Release: ensures all ring ops complete before decrement
            users_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool active() const { return active_; }

private:
    std::atomic<int>& users_;
    bool active_;
};
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
perf(C2): lighten RingAccessGuard memory ordering

- Increment: stays acquire (required for correctness)
- Decrement: acq_rel -> release (sufficient for exit)
- Bail-out decrement: relaxed (never entered guarded section)

Preserves synchronization with beginReconfigure() while reducing
atomic operation overhead where safe.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Final Build and Test

**Step 1: Full rebuild**

Run: `make clean && make -j4`
Expected: BUILD SUCCESS

**Step 2: Manual testing checklist**

Test the following scenarios:
- [ ] PCM 16-bit/44.1kHz playback
- [ ] PCM 24-bit/96kHz playback
- [ ] PCM 24-bit/192kHz playback
- [ ] DSD64/DSD128 playback (if available)
- [ ] Format changes mid-stream
- [ ] Start/stop cycles (no hangs)
- [ ] Gapless track transitions

**Step 3: Create summary commit (squash if desired)**

If all tests pass, optionally create a summary tag:

```bash
git tag -a v1.x.x-hot-path-opt -m "Hot path generation counter optimizations"
```

---

## Summary

| Task | Optimization | Files Changed |
|------|--------------|---------------|
| 1-3 | P1: Format generation counter | DirettaSync.h, DirettaSync.cpp |
| 4-5 | P2: Direct write API + push() | DirettaRingBuffer.h |
| 6 | P3: pop() inlined loads | DirettaRingBuffer.h |
| 7-9 | C1: Consumer state generation | DirettaSync.h, DirettaSync.cpp |
| 10 | C2: Lighter guard ordering | DirettaSync.cpp |
| 11 | Final build and test | - |

**Total commits:** 10
