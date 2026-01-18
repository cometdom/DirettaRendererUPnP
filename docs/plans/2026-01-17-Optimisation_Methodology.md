# Optimisation Methodology

**Date:** 2026-01-17 (updated 2026-01-18)
**Source:** Analysis of docs/plans/ design documents
**Context:** Collaboration with high-level expert on audio rendering quality

## Overview

This document captures the optimisation patterns identified in the DirettaRendererUPnP-X codebase improvements. Beyond the two primary techniques (hot path simplification and SIMD/hardware delegation), several additional patterns emerged that contribute to improved audio reproduction quality.

The underlying philosophy: **minimise variance in execution time, not just average execution time**. In audio rendering, jitter (timing variance) directly affects perceived quality.

---

## Design Document Structure

Each optimisation should be documented with the following structure:

### 1. Identification

Assign each optimisation a unique ID for tracking:
- **P1, P2, P3...** - Producer-side (sendAudio path)
- **C1, C2, C3...** - Consumer-side (getNewStream path)
- **R1, R2...** - Ring buffer operations
- **D1, D2...** - Decoder/audio engine

### 2. Impact Summary Table

Provide quantified before/after metrics at the document start:

| ID | Optimisation | Location | Impact |
|----|--------------|----------|--------|
| P1 | Format generation counter | sendAudio | 7 atomics → 1 |
| C1 | State generation counter | getNewStream | 5 atomics → 1 |

### 3. Problem-Solution Format

For each optimisation:

```markdown
## P1: Format Generation Counter

### Problem
`sendAudio()` loads 7 atomics on every call (DirettaSync.cpp:942-948):
[code snippet showing current inefficiency]

Format rarely changes (~0.1% of calls), yet we pay full cost every time.

### Solution
[code snippet showing the fix]

### Increment/Modification Points
- `configureRingPCM()` - at end of function
- `configureRingDSD()` - at end of function
```

### 4. Hot/Cold Path Classification

Explicitly label execution frequency:
- **Hot path** (99.9% of calls): Single generation counter check
- **Cold path** (format change only): Full reload of cached values

### 5. State Classification

For caching optimisations, classify state as:
- **Stable state**: Configuration set at track open, cached via generation counter
- **Volatile state**: Can change mid-playback, must check fresh every call

Example from C1:
```cpp
// Stable state - use cached values
int currentBytesPerBuffer = m_cachedBytesPerBuffer;

// Volatile state - check fresh
if (m_stopRequested.load(std::memory_order_acquire)) { ... }
```

### 6. Memory Ordering Justification

When modifying atomic operations, document why each ordering is safe:

```markdown
- **Increment must stay acquire**: Ensures visibility to beginReconfigure()
  before any ring buffer operations
- **Decrement can use release**: Ensures all ring ops complete before
  count decrements
- **Bail-out decrement uses relaxed**: Never entered guarded section,
  no ordering needed
```

### 7. Files Modified Summary

| File | Changes |
|------|---------|
| `src/DirettaSync.h` | Add generation counters and cached members |
| `src/DirettaSync.cpp` | Generation checks, increments, lighter ordering |

### 8. Testing Checklist

#### Functional
- [ ] PCM 16-bit/44.1kHz playback
- [ ] PCM 24-bit/96kHz playback
- [ ] DSD64/DSD128 playback
- [ ] Format changes mid-stream
- [ ] Gapless track transitions

#### Stress
- [ ] Rapid format switching
- [ ] High CPU load during playback
- [ ] Extended sessions (memory stability)

#### Listening
- [ ] A/B comparison with previous build

---

## Pattern 1: Memory Allocation Elimination

**Principle:** Pre-allocate objects and reuse them across iterations rather than allocating per-call.

**Examples:**
- `m_packet` and `m_frame` allocated once in `AudioDecoder::open()`, reused for all reads
- Staging buffers allocated per-format at track open, reused for all conversions
- `m_pcmRemainder` vector pre-sized to avoid reallocation during playback

**Rationale:** Memory allocation involves syscalls and has highly variable latency. Pre-allocation moves this cost to the cold path (track open) rather than the hot path (per-sample processing).

---

## Pattern 2: Processing Layer Bypass

**Principle:** Skip entire processing stages when the data already matches the target format.

**Examples:**
- PCM bypass: skip `SwrContext` entirely when input/output formats match
- Raw PCM mode: bypass `avcodec_send_packet()`/`avcodec_receive_frame()` for uncompressed WAV files
- S24→S24_P32: request packed format from FFmpeg to avoid unnecessary unpacking/repacking

**Rationale:** The fastest code is code that doesn't run. Format-matching detection at track open allows entire processing stages to be skipped.

---

## Pattern 3: Decision Point Relocation

**Principle:** Move format decisions from per-sample (hot path) to per-track (cold path).

**Examples:**
- DSD conversion mode: determined once at track open, cached in `m_dsdConversionMode`
- S24 pack mode: detected at track open with metadata hint, not per-iteration detection
- Function pointer selection: choose specialised function once, call without branching

**Key Insight:** "The conversion mode is determined at track open and never changes during playback."

**Rationale:** Conditional branches have variable timing due to branch prediction. Moving decisions to track open eliminates per-sample branching entirely.

---

## Pattern 4: O(1) Data Structures

**Principle:** Replace O(n) operations with O(1) alternatives.

**Examples:**
- `AVAudioFifo` for circular buffer: O(1) read/write vs `memmove` O(n)
- Power-of-2 bitmask modulo: `& mask_` (1 cycle) vs `% size_` (20-100 cycles)
- Ring buffer with separate read/write positions vs shifting array

**Rationale:** O(n) operations have data-dependent timing. O(1) operations execute in constant time regardless of data size.

---

## Pattern 5: Timing Variance Reduction

**Principle:** Ensure code paths execute in predictable, consistent time.

**Examples:**
- Overlapping stores: write fixed iteration count regardless of actual data size
- Fixed staging buffer size: same cache footprint every iteration
- Consistent-timing memcpy: identical instruction sequence regardless of length
- Avoid early-exit optimisations that create timing differences

**Rationale:** Even if an optimisation reduces average time, if it increases variance, it may degrade audio quality. Predictable timing is preferred over faster-but-variable timing.

---

## Pattern 6: Cache Locality Optimisation

**Principle:** Keep frequently-accessed data in fast cache levels.

**Examples:**
- Staging buffers sized to fit L2 cache (~64KB)
- `alignas(64)` for cache-line separation of read/write positions (prevents false sharing)
- Single consolidated bit-reversal LUT vs 4 copies in different functions
- Zen 4-specific prefetch tuning for streaming data

**Rationale:** Cache misses have highly variable latency (L1: ~4 cycles, L2: ~12 cycles, L3: ~40 cycles, RAM: ~200+ cycles). Keeping hot data in cache reduces both latency and variance.

---

## Pattern 7: Flow Control Tuning

**Principle:** Adaptive scheduling based on buffer state.

**Examples:**
- Micro-sleep (500µs) when buffer is healthy vs 10ms blocking when nearly empty
- Early return on critical buffer levels
- Adaptive chunk sizing: smaller chunks when buffer is low, larger when healthy

**Rationale:** Aggressive sleeping saves CPU but risks underruns. Adaptive flow control maintains buffer health while minimising CPU usage during steady-state playback.

---

## Pattern 8: Direct Write APIs

**Principle:** Eliminate intermediate buffer copies by writing directly to destination.

**Examples:**
- `getWriteSpan()`/`commitWrite()`: expose ring buffer memory for zero-copy writes
- `swr_convert()` output directly to FIFO when sample counts align
- Target: reduce copies from 2-3 to 0-1 for 32-bit WAV playback

**Rationale:** Each memory copy adds latency and cache pressure. Direct writes eliminate intermediate buffers entirely.

---

## Pattern 9: Syscall Elimination

**Principle:** Remove kernel transitions from the audio path.

**Examples:**
- Replace mutex/condition variable with lock-free atomics
- Count underruns with atomic increment, log at session end (not in hot path)
- Spin-wait with `std::this_thread::yield()` vs `notify_all()` syscall
- Deferred I/O: accumulate statistics, write once at session end

**Rationale:** Syscalls involve context switches with highly variable latency (1-10µs typical, but can spike to milliseconds under load). Lock-free primitives keep execution entirely in userspace.

---

## Pattern 10: Generation Counter Caching

**Principle:** Use a single generation counter to batch multiple atomic loads into one check.

**Problem:**
```cpp
// Before: 7 atomic loads on EVERY call
bool dsdMode = m_isDsdMode.load(std::memory_order_acquire);
bool pack24bit = m_need24BitPack.load(std::memory_order_acquire);
bool upsample = m_need16To32Upsample.load(std::memory_order_acquire);
// ... 4 more atomics
```

Format rarely changes during playback (~0.1% of calls), yet we pay for 7 atomic loads every time.

**Solution:**
```cpp
// After: 1 atomic load in common case
uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
if (gen != m_cachedFormatGen) {
    // Cold path: reload all (only on format change)
    m_cachedDsdMode = m_isDsdMode.load(std::memory_order_acquire);
    // ... reload others
    m_cachedFormatGen = gen;
}
// Hot path: use cached values
bool dsdMode = m_cachedDsdMode;
```

**State Classification:**
- **Stable state** (cached): Format parameters set at track open
- **Volatile state** (checked fresh): `m_stopRequested`, `m_silenceBuffersRemaining`

**Implementation Pattern:**
1. Add generation counter atomic: `std::atomic<uint32_t> m_formatGeneration{0}`
2. Add cached values (non-atomic, thread-local access only)
3. Increment generation at configuration points
4. Check generation before using cached values

**Rationale:** Reduces N atomic loads to 1 in the common case. The cache coherency overhead of a single atomic is much lower than N separate atomics, especially on multi-core systems where each atomic may require cache line invalidation.

---

## Pattern Taxonomy

| Category | Pattern | Primary Benefit |
|----------|---------|-----------------|
| **Temporal** | Decision relocation | Eliminates per-sample branching |
| **Spatial** | Cache locality | Reduces memory access variance |
| **Structural** | Layer bypass | Eliminates unnecessary processing |
| **Algorithmic** | O(1) structures | Constant-time operations |
| **Timing** | Variance reduction | Predictable execution time |
| **System** | Syscall elimination | Avoids kernel transitions |
| **Memory** | Allocation elimination | Moves allocation to cold path |
| **Data Flow** | Direct write APIs | Reduces copy count |
| **Scheduling** | Flow control tuning | Balances latency vs CPU usage |
| **Atomic** | Generation counter caching | Batches N atomics into 1 check |

---

## Application Guidelines

### When to Apply Each Pattern

1. **Memory Allocation Elimination** - Apply to any object created per-iteration in the hot path
2. **Processing Layer Bypass** - Apply when format detection can identify no-op cases
3. **Decision Point Relocation** - Apply to any conditional that depends on track-level (not sample-level) data
4. **O(1) Data Structures** - Apply when data size varies and affects operation count
5. **Timing Variance Reduction** - Apply to innermost loops where consistency matters most
6. **Cache Locality Optimisation** - Apply to frequently-accessed data structures
7. **Flow Control Tuning** - Apply to producer/consumer boundaries
8. **Direct Write APIs** - Apply when intermediate buffers serve no transformation purpose
9. **Syscall Elimination** - Apply to any synchronisation or I/O in the hot path
10. **Generation Counter Caching** - Apply when multiple atomics are loaded together and change infrequently

### Measurement Approach

When evaluating optimisations, measure:
- **Mean latency** - Average execution time
- **P99 latency** - 99th percentile (captures variance)
- **Jitter** - Standard deviation of execution time
- **Cache miss rate** - Via hardware performance counters

An optimisation that reduces mean latency but increases P99 or jitter may degrade audio quality.

---

## Implementation Task Structure

For implementation plans, use the following task template:

```markdown
## Task N: [Brief Description] (Optimization ID)

**Files:**
- Modify: `src/File.cpp:123` (description)

**Step 1: [Action]**
[Code or instructions]

**Step 2: Verify compilation**
Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**
git commit -m "$(cat <<'EOF'
type(ID): brief description

Detailed explanation of what changed and why.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

**Commit Message Prefixes:**
- `perf(P1):` - Performance improvement
- `refactor(C1):` - Code restructuring without behavior change
- `fix:` - Bug fix
- `feat:` - New feature

**Task Granularity:**
- One task per logical change
- Build verification after each task
- Commit after each task (enables bisection)

---

## References

- `docs/plans/2026-01-11-audio-memory-optimization-design.md` - Staging buffers, SIMD conversions
- `docs/plans/2026-01-12-PCM Latency and Jitter Optimization Design.md` - Allocation elimination, flow control
- `docs/plans/2026-01-14-resample-memcpy-optimization-design.md` - Direct write path, AVAudioFifo
- `docs/plans/2026-01-15-pcm-bypass-optimization-design.md` - PCM bypass, S24 detection
- `docs/plans/2026-01-15-dsd-conversion-optimization-design.md` - Function specialisation
- `docs/plans/2026-01-16-direct-pcm-fast-path-design.md` - Ring buffer direct write
- `docs/plans/2026-01-17-Hot Path Simplification Report.md` - Implementation summary
- `docs/plans/2026-01-18-hot-path-generation-counters-design.md` - Generation counter pattern
- `docs/plans/2026-01-18-hot-path-generation-counters-impl.md` - Task-based implementation example
