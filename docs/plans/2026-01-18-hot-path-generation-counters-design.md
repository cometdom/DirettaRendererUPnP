# Hot Path Generation Counters Design

**Date:** 2026-01-18
**Status:** Ready for implementation
**Scope:** PCM and DSD hot paths (producer and consumer)

## Objective

Reduce per-call overhead in both producer (`sendAudio`) and consumer (`getNewStream`) hot paths by consolidating atomic operations and eliminating redundant loads. Build performance headroom for high sample rate playback.

## Optimizations

| ID | Optimization | Location | Impact |
|----|--------------|----------|--------|
| P1 | Format generation counter | sendAudio | 7 atomics → 1 |
| P2 | Direct write API | ring buffer push | Skip wraparound check ~99% |
| P3 | Inline position loads | ring buffer | 2 redundant loads eliminated |
| C1 | State generation counter | getNewStream | 5 atomics → 1 |
| C2 | Relaxed ordering in guard | RingAccessGuard | Lighter atomic ops |

---

## P1: Format Generation Counter (Producer Side)

### Problem

`sendAudio()` loads 7 atomics on every call (DirettaSync.cpp:942-948):

```cpp
bool dsdMode = m_isDsdMode.load(std::memory_order_acquire);
bool pack24bit = m_need24BitPack.load(std::memory_order_acquire);
bool upsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
bool needBitReversal = m_needDsdBitReversal.load(std::memory_order_acquire);
bool needByteSwap = m_needDsdByteSwap.load(std::memory_order_acquire);
int numChannels = m_channels.load(std::memory_order_acquire);
int bytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
```

Format rarely changes during playback (~0.1% of calls), yet we pay for 7 atomic loads every time.

### Solution

Add generation counter + cached values. Single atomic comparison in common case.

**New members (DirettaSync.h):**

```cpp
// Format generation - incremented in configureRingPCM/configureRingDSD
std::atomic<uint32_t> m_formatGeneration{0};

// Cached format values (non-atomic, only accessed by producer thread)
uint32_t m_cachedFormatGen{0};
bool m_cachedDsdMode{false};
bool m_cachedPack24bit{false};
bool m_cachedUpsample16to32{false};
bool m_cachedNeedBitReversal{false};
bool m_cachedNeedByteSwap{false};
int m_cachedChannels{2};
int m_cachedBytesPerSample{2};
```

**sendAudio() change:**

```cpp
uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
if (gen != m_cachedFormatGen) {
    // Cold path: reload all (only on format change)
    m_cachedDsdMode = m_isDsdMode.load(std::memory_order_acquire);
    m_cachedPack24bit = m_need24BitPack.load(std::memory_order_acquire);
    m_cachedUpsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
    m_cachedNeedBitReversal = m_needDsdBitReversal.load(std::memory_order_acquire);
    m_cachedNeedByteSwap = m_needDsdByteSwap.load(std::memory_order_acquire);
    m_cachedChannels = m_channels.load(std::memory_order_acquire);
    m_cachedBytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
    m_cachedFormatGen = gen;
}

// Hot path: use cached values directly
bool dsdMode = m_cachedDsdMode;
bool pack24bit = m_cachedPack24bit;
// ... etc
```

**Increment points:**

- `configureRingPCM()` - at end of function
- `configureRingDSD()` - at end of function

---

## P2: Direct Write API (Ring Buffer)

### Problem

Every `push()` and `writeToRing()` checks for wraparound, even though contiguous space is available ~99% of the time (wraparound only at buffer boundary).

### Solution

Add direct write API that returns a contiguous region when available.

**New methods (DirettaRingBuffer.h):**

```cpp
/**
 * Get direct write pointer for zero-copy writes.
 * Returns true if contiguous space >= needed is available.
 */
bool getDirectWriteRegion(size_t needed, uint8_t*& region, size_t& available) {
    if (size_ == 0) return false;

    size_t wp = writePos_.load(std::memory_order_acquire);
    size_t rp = readPos_.load(std::memory_order_acquire);

    // Contiguous space from writePos to either readPos or end of buffer
    size_t toEnd = size_ - wp;
    size_t toRead = ((rp - wp - 1) & mask_);  // total free
    size_t contiguous = std::min(toEnd, toRead);

    if (contiguous >= needed) {
        region = buffer_.data() + wp;
        available = contiguous;
        return true;
    }
    return false;
}

/**
 * Commit a direct write, advancing write pointer.
 */
void commitDirectWrite(size_t written) {
    size_t wp = writePos_.load(std::memory_order_relaxed);
    writePos_.store((wp + written) & mask_, std::memory_order_release);
}
```

**Optimized push():**

```cpp
size_t push(const uint8_t* data, size_t len) {
    uint8_t* region;
    size_t available;
    if (getDirectWriteRegion(len, region, available)) {
        // Fast path: single contiguous memcpy
        memcpy_audio(region, data, len);
        commitDirectWrite(len);
        return len;
    }
    // Slow path: existing wraparound handling
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

Same pattern applies to `writeToRing()` used by conversion methods.

---

## P3: Inline Position Loads (Ring Buffer)

### Problem

Current `push()` and `pop()` load atomic positions twice:

```cpp
// push() current:
size_t free = getFreeSpace();        // loads writePos_ AND readPos_
size_t wp = writePos_.load(...);     // loads writePos_ AGAIN

// pop() current:
size_t avail = getAvailable();       // loads writePos_ AND readPos_
size_t rp = readPos_.load(...);      // loads readPos_ AGAIN
```

### Solution

Inline the calculations and reuse loaded values (shown in P2 above for push).

**Optimized pop():**

```cpp
size_t pop(uint8_t* dest, size_t len) {
    if (size_ == 0) return 0;

    size_t wp = writePos_.load(std::memory_order_acquire);
    size_t rp = readPos_.load(std::memory_order_acquire);
    size_t avail = (wp - rp) & mask_;  // inline getAvailable logic

    if (len > avail) len = avail;
    if (len == 0) return 0;

    // rp already loaded, use directly
    size_t firstChunk = std::min(len, size_ - rp);

    memcpy_audio(dest, buffer_.data() + rp, firstChunk);
    if (firstChunk < len) {
        memcpy_audio(dest + firstChunk, buffer_.data(), len - firstChunk);
    }

    readPos_.store((rp + len) & mask_, std::memory_order_release);
    return len;
}
```

**Note:** `getAvailable()` and `getFreeSpace()` remain as public methods for external callers (like `getBufferLevel()`).

---

## C1: State Generation Counter (Consumer Side)

### Problem

`getNewStream()` loads 5+ state atomics on every call:

```cpp
int currentBytesPerBuffer = m_bytesPerBuffer.load(...);
uint32_t remainder = m_framesPerBufferRemainder.load(...);
int bytesPerFrame = m_bytesPerFrame.load(...);
bool currentIsDsd = m_isDsdMode.load(...);
// Plus silenceByte from ring buffer
```

### Solution

Similar generation counter pattern. Batch stable configuration; check volatile state fresh.

**New members (DirettaSync.h):**

```cpp
// Consumer state generation - incremented on config changes
std::atomic<uint32_t> m_consumerStateGen{0};

// Cached consumer state (only accessed by worker thread)
uint32_t m_cachedConsumerGen{0};
int m_cachedBytesPerBuffer{176};
uint32_t m_cachedFramesRemainder{0};
int m_cachedBytesPerFrame{0};
bool m_cachedConsumerIsDsd{false};
uint8_t m_cachedSilenceByte{0};
```

**getNewStream() change:**

```cpp
uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
if (gen != m_cachedConsumerGen) {
    m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
    m_cachedFramesRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
    m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
    m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
    m_cachedSilenceByte = m_ringBuffer.silenceByte();
    m_cachedConsumerGen = gen;
}

int currentBytesPerBuffer = m_cachedBytesPerBuffer;
// ... use cached values

// Still check volatile state fresh (can change mid-playback)
if (m_stopRequested.load(std::memory_order_acquire)) { ... }
if (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) { ... }
```

**Increment points:** Same as P1 - `configureRingPCM()` and `configureRingDSD()`.

---

## C2: Lighter Ordering in RingAccessGuard

### Problem

Current guard does 2 atomic RMW operations with acq_rel ordering per hot path call:

```cpp
users_.fetch_add(1, std::memory_order_acq_rel);  // constructor
users_.fetch_sub(1, std::memory_order_acq_rel);  // destructor
```

### Solution

Use minimal ordering that preserves correctness:

- **Increment must stay acquire** (or acq_rel): Ensures the increment is visible to `beginReconfigure()` before any ring buffer operations. Without this, `beginReconfigure()` could see `m_ringUsers == 0` and proceed to resize/clear the buffer while a thread is already inside the guarded section.

- **Decrement can use release**: The release ensures all ring buffer operations complete before the count decrements, which is sufficient for `beginReconfigure()` to safely proceed after seeing zero.

- **Bail-out decrement can use relaxed**: If we fail the second reconfiguring check, we never entered the guarded section, so no ordering is needed.

**Updated RingAccessGuard:**

```cpp
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) {
            return;
        }
        users_.fetch_add(1, std::memory_order_acquire);  // MUST stay acquire
        if (reconfiguring.load(std::memory_order_acquire)) {
            users_.fetch_sub(1, std::memory_order_relaxed);  // bail-out, no ordering needed
            return;
        }
        active_ = true;
    }

    ~RingAccessGuard() {
        if (active_) {
            users_.fetch_sub(1, std::memory_order_release);  // release sufficient
        }
    }

    bool active() const { return active_; }

private:
    std::atomic<int>& users_;
    bool active_;
};
```

**Net savings:** Decrement goes from acq_rel to release (lighter on some architectures), bail-out decrement uses relaxed. Increment stays acquire for correctness.

---

## Files Modified

| File | Changes |
|------|---------|
| `src/DirettaSync.h` | Add generation counters and cached value members |
| `src/DirettaSync.cpp` | Generation checks in sendAudio/getNewStream, increments in configure functions, lighter guard ordering |
| `src/DirettaRingBuffer.h` | Add getDirectWriteRegion/commitDirectWrite, inline position loads in push/pop |

---

## Testing

### Functional (after each phase)

- [ ] PCM 16-bit/44.1kHz playback
- [ ] PCM 24-bit/96kHz playback
- [ ] PCM 24-bit/192kHz playback
- [ ] DSD64/DSD128 playback
- [ ] Format changes mid-stream (verify generation increment)
- [ ] Start/stop cycles (no hangs)
- [ ] Gapless track transitions

### Stress Testing

- [ ] Rapid format switching
- [ ] High CPU load during playback
- [ ] Extended playback sessions (memory stability)

### Listening

- [ ] A/B comparison with previous build
