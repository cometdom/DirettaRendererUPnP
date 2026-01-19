# Optimisation Methodology

**Date:** 2026-01-17 (updated 2026-01-19)
**Source:** Analysis of docs/plans/ design documents and leeeanh's contribution methodology
**Context:** Collaboration with high-level expert on audio rendering quality

## Overview

This document captures the optimisation patterns identified in the DirettaRendererUPnP-X codebase improvements. Beyond the two primary techniques (hot path simplification and SIMD/hardware delegation), several additional patterns emerged that contribute to improved audio reproduction quality.

The underlying philosophy: **minimise variance in execution time, not just average execution time**. In audio rendering, jitter (timing variance) directly affects perceived quality.

---

## Two-Phase Documentation Process

Optimisations should be documented in **two separate phases**, each with its own document:

### Phase 1: Design Document (`*-design.md`)

**Purpose:** Conceptual analysis of proposed optimisations—the *what* and *why*.

The design document focuses on understanding the problem, analysing the solution space, and justifying the approach. It should be readable by someone unfamiliar with the codebase implementation details.

**Key characteristics:**
- Problem-focused: explains what's inefficient and why it matters
- Solution-oriented: describes the approach conceptually with before/after code snippets
- Justification-rich: explains memory ordering choices, state classification, trade-offs
- Testable: includes verification criteria

**When to write:** Before implementation begins. The design document serves as the specification for the implementation.

### Phase 2: Implementation Document (`*-impl.md`)

**Purpose:** Step-by-step execution plan—the *how*.

The implementation document is a detailed task list that can be followed mechanically. Each task modifies specific files at specific line numbers, with exact code to add or replace.

**Key characteristics:**
- Task-focused: numbered tasks with clear completion criteria
- Location-specific: file paths and line numbers for every change
- Verifiable: compilation check after each task
- Committable: one commit per logical change, with commit message template

**When to write:** After the design document is complete and reviewed. The implementation document references the design document for rationale.

### Workflow

```
1. Identify optimisation opportunity
       ↓
2. Write design document (*-design.md)
   - Analyse problem with code snippets
   - Propose solution with impact estimates
   - Justify memory ordering/state classification
   - Define testing checklist
       ↓
3. Review design document
       ↓
4. Write implementation document (*-impl.md)
   - Break design into atomic tasks
   - Add file:line references
   - Add exact code changes
   - Add commit templates
       ↓
5. Execute implementation document task-by-task
       ↓
6. Test against design document checklist
```

**Exemplar documents:**
- Design: `docs/plans/2026-01-18-hot-path-generation-counters-design.md`
- Implementation: `docs/plans/2026-01-18-hot-path-generation-counters-impl.md`

---

## Opportunity Discovery Process

Before writing design documents, optimization opportunities must be identified through structured analysis. This section describes the multi-perspective review process.

### Prerequisites

Before beginning analysis, read the project context document:

```
./docs/CLAUDE.md
```

This provides essential understanding of:
- Project architecture and signal flow
- Core components and their responsibilities
- Build system and dependencies
- Code style conventions

### Multi-Pass Expert Analysis

Conduct **multiple passes** through the codebase, each from a distinct expert perspective. This ensures comprehensive coverage of both hardware/signal and software concerns.

#### Pass 1: Electrical Engineering Perspective

**Persona:** Top-level electrical engineering designer (MIT/Tokyo University caliber)

**Focus areas:**
- **Signal integrity**: Where does audio data risk corruption or degradation?
- **Timing determinism**: Which code paths have variable execution time?
- **Jitter analysis**: Where does timing variance accumulate in the signal chain?
- **Clock domain handling**: How are sample rate families (44.1kHz vs 48kHz) managed?
- **Buffer dimensioning**: Are buffer sizes optimal for latency vs. safety trade-offs?
- **Phase coherence**: Is multi-channel data handled correctly?
- **Noise sources**: What operations introduce unpredictable delays?

**Questions to ask:**
1. What is the jitter budget and where is it being spent?
2. Are there any O(n) operations in the sample-rate-critical path?
3. Could scheduling variance from the OS affect audio timing?
4. Are buffer boundaries aligned with sample rate boundaries?
5. What happens under worst-case system load?

**Output:** List of variance sources with estimated impact (µs or ms)

#### Pass 2: Software Engineering Perspective

**Persona:** High-level software engineer designer (MIT/Tokyo University caliber)

**Focus areas:**
- **Algorithmic complexity**: Are there O(n) operations that could be O(1)?
- **Memory management**: Where are allocations happening in hot paths?
- **Concurrency patterns**: Are atomics, locks, and barriers optimally placed?
- **Cache efficiency**: Is data laid out for cache-friendly access?
- **API design**: Do internal APIs encourage or prevent efficient usage?
- **Code maintainability**: Are optimizations understandable and safe?

**Questions to ask:**
1. Which functions are called most frequently and what do they cost?
2. Are there redundant computations that could be cached?
3. Could memory ordering be relaxed without affecting correctness?
4. Are data structures aligned to prevent false sharing?
5. What is the syscall footprint of the hot path?

**Output:** List of algorithmic and structural improvements with difficulty estimates

#### Pass 3: Cross-Reference with Existing Opportunities

After the expert passes, review the existing opportunities document:

```
./docs/plans/2026-01-17-Optimisation_Opportunities.md
```

**Actions:**
1. Verify that identified issues haven't already been addressed
2. Check if new findings relate to or extend existing opportunities
3. Identify patterns that apply across multiple findings
4. Prioritise based on impact vs. effort matrix

### Structured Output

Compile findings into the opportunities document using this format:

```markdown
#### [ID]: [Brief Title] ⭐ [PRIORITY if HIGH]

**Pattern:** #N ([Pattern Name] from methodology)
**Location:** `file.cpp:line-range`
**Status:** [Current state]

[Code snippet showing the problem]

**Fix:** [Brief description]
[Code snippet showing the solution]

**Variance Saved:** [Quantified impact]
**Effort:** [Very Low | Low | Medium | High]
**Risk:** [Very Low | Low | Medium | High]
**Impact:** [Low | Medium | High]
```

### Analysis Prompt Template

When requesting an optimization analysis (e.g., from an AI assistant), use this prompt structure:

```
Please analyse the DirettaRendererUPnP-X codebase for optimization opportunities.

**Step 0: Context**
Read ./docs/CLAUDE.md to understand the project architecture.

**Step 1: Electrical Engineering Pass**
Acting as a top-level electrical engineering designer (MIT/Tokyo University caliber),
analyse the codebase focusing on:
- Signal integrity and timing determinism
- Jitter sources and variance accumulation
- Buffer dimensioning and clock domain handling
- Thread scheduling and OS interaction

**Step 2: Software Engineering Pass**
Acting as a high-level software engineer designer (MIT/Tokyo University caliber),
analyse the codebase focusing on:
- Algorithmic complexity and data structures
- Memory allocation patterns
- Concurrency and cache efficiency
- API design and code structure

**Step 3: Cross-Reference**
Review ./docs/plans/2026-01-17-Optimisation_Opportunities.md to:
- Avoid duplicating already-identified issues
- Extend or connect to existing opportunities
- Apply the 10 optimization patterns from the methodology

**Step 4: Recommendations**
For each new opportunity, provide:
- Unique ID and brief title
- Pattern classification
- File location with line numbers
- Problem code snippet
- Solution code snippet
- Quantified impact estimate
- Effort/risk/impact assessment

Organise findings by category (Jitter, Atomics, Cache, Signal Path, etc.)
and include a priority matrix.
```

### Example Discovery Session

The 2026-01-19 analysis used this process to identify 13 new opportunities:

| Category | Opportunities | Key Findings |
|----------|---------------|--------------|
| A: Jitter Minimisation | A1, A2, A3 | DSD memmove, resampler allocation, logging I/O |
| B: Atomic Operations | B1, B2 | Diagnostic counters, guard bypass |
| C: Cache Efficiency | C1, C2 | DSD pre-allocation, stabilisation pre-compute |
| D: Signal Path | D1, D2 | SIMD interleaving, swr_get_delay caching |
| E: Buffer Dimensioning | E1, E2 | Sample-accurate prefill, adaptive sizing |
| F: Thread Scheduling | F1, F2 | Worker priority, CPU affinity |

See: `docs/plans/2026-01-19-jitter-reduction-phase1-design.md` for the resulting design document.

---

## Design Document Structure

Each design document (`*-design.md`) should include the following sections:

### 1. Header Block

Start with metadata for document management:

```markdown
# [Optimisation Name] Design

**Date:** YYYY-MM-DD
**Status:** Draft | Ready for implementation | Implemented
**Scope:** [Components affected]
```

### 2. Objective

A concise statement of what the optimisation achieves and why it matters:

```markdown
## Objective

Reduce per-call overhead in [component] by [technique]. Build performance
headroom for [use case].
```

### 3. Identification

Assign each optimisation a unique ID for tracking:
- **P1, P2, P3...** - Producer-side (sendAudio path)
- **C1, C2, C3...** - Consumer-side (getNewStream path)
- **R1, R2...** - Ring buffer operations
- **D1, D2...** - Decoder/audio engine

### 4. Impact Summary Table

Provide quantified before/after metrics at the document start:

| ID | Optimisation | Location | Impact |
|----|--------------|----------|--------|
| P1 | Format generation counter | sendAudio | 7 atomics → 1 |
| C1 | State generation counter | getNewStream | 5 atomics → 1 |

### 5. Problem-Solution Format

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

### 6. Hot/Cold Path Classification

Explicitly label execution frequency:
- **Hot path** (99.9% of calls): Single generation counter check
- **Cold path** (format change only): Full reload of cached values

### 7. State Classification

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

### 8. Memory Ordering Justification

When modifying atomic operations, document why each ordering is safe:

```markdown
- **Increment must stay acquire**: Ensures visibility to beginReconfigure()
  before any ring buffer operations
- **Decrement can use release**: Ensures all ring ops complete before
  count decrements
- **Bail-out decrement uses relaxed**: Never entered guarded section,
  no ordering needed
```

### 9. Files Modified Summary

| File | Changes |
|------|---------|
| `src/DirettaSync.h` | Add generation counters and cached members |
| `src/DirettaSync.cpp` | Generation checks, increments, lighter ordering |

### 10. Testing Checklist

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

## Implementation Document Structure

Each implementation document (`*-impl.md`) should include the following sections:

### 1. Header Block

```markdown
# [Optimisation Name] Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** [One-sentence summary of what the optimisation achieves]

**Architecture:** [Brief description of the technical approach]

**Tech Stack:** C++17, std::atomic, lock-free patterns
```

The instruction comment at the top guides AI assistants to execute the plan systematically.

### 2. Task Structure

Break the design into atomic, verifiable tasks. Each task should:
- Make one logical change
- Be independently committable
- Include verification step

```markdown
## Task N: [Brief Description] (Optimization ID - Part M)

**Files:**
- Modify: `src/File.cpp:123` (description of change)

**Step 1: [Action]**

In `src/File.cpp`, find line 123 (description of what to locate). Add/replace:

[Exact code to add or replace, with context]

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -20`
Expected: BUILD SUCCESS

**Step 3: Commit**

git add src/File.cpp
git commit -m "$(cat <<'EOF'
type(ID): brief description

Detailed explanation of what changed and why.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

### 3. Task Decomposition Guidelines

**Incremental implementation:**
- Task 1: Add new members (compiles but unused)
- Task 2: Add increment/modification points (members now updated)
- Task 3: Use new pattern in hot path (optimization takes effect)

This allows testing at each stage and simplifies debugging.

**Multi-file changes:**
If a logical change spans multiple files, include all files in one task to maintain atomicity:

```markdown
**Files:**
- Modify: `src/DirettaSync.h:323` (add member declarations)
- Modify: `src/DirettaSync.cpp:815` (increment at configureRingPCM)
- Modify: `src/DirettaSync.cpp:848` (increment at configureRingDSD)
```

### 4. Final Task: Build and Test

Every implementation document should end with a final verification task:

```markdown
## Task N: Final Build and Test

**Step 1: Full rebuild**

Run: `make clean && make -j4`
Expected: BUILD SUCCESS

**Step 2: Manual testing checklist**

Test the following scenarios:
- [ ] PCM 16-bit/44.1kHz playback
- [ ] PCM 24-bit/96kHz playback
- [ ] Format changes mid-stream
- [ ] Start/stop cycles (no hangs)
- [ ] Gapless track transitions

**Step 3: Create summary tag (optional)**

git tag -a v1.x.x-[optimisation-name] -m "[Description]"
```

### 5. Summary Table

Conclude with a table mapping tasks to optimisations:

```markdown
## Summary

| Task | Optimization | Files Changed |
|------|--------------|---------------|
| 1-3 | P1: Format generation counter | DirettaSync.h, DirettaSync.cpp |
| 4-5 | P2: Direct write API | DirettaRingBuffer.h |
| 6 | P3: pop() inlined loads | DirettaRingBuffer.h |

**Total commits:** N
```

### 6. Commit Message Conventions

**Prefixes:**
- `perf(P1):` - Performance improvement
- `refactor(C1):` - Code restructuring without behavior change
- `fix:` - Bug fix
- `feat:` - New feature

**Format:**
```
type(ID): brief imperative description

Longer explanation of what changed.
Optionally include before/after metrics.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
```

### 7. Task Granularity Principles

- **One task per logical change**: Enables `git bisect`
- **Build verification after each task**: Catches errors early
- **Commit after each task**: Creates clean history
- **Never leave the build broken**: Each commit should compile

---

## References

### Exemplar Documents (Two-Phase Process)

These documents from leeeanh demonstrate the two-phase documentation process:

- `docs/plans/2026-01-18-hot-path-generation-counters-design.md` - **Design document exemplar**: Shows problem-solution format, impact tables, memory ordering justification, and testing checklists
- `docs/plans/2026-01-18-hot-path-generation-counters-impl.md` - **Implementation document exemplar**: Shows task decomposition, file:line references, commit templates, and summary tables

### Design Documents Archive

Historical design documents (follow older format, predating two-phase process):

- `docs/plans/2026-01-11-audio-memory-optimization-design.md` - Staging buffers, SIMD conversions
- `docs/plans/2026-01-12-PCM Latency and Jitter Optimization Design.md` - Allocation elimination, flow control
- `docs/plans/2026-01-14-resample-memcpy-optimization-design.md` - Direct write path, AVAudioFifo
- `docs/plans/2026-01-15-pcm-bypass-optimization-design.md` - PCM bypass, S24 detection
- `docs/plans/2026-01-15-dsd-conversion-optimization-design.md` - Function specialisation
- `docs/plans/2026-01-16-direct-pcm-fast-path-design.md` - Ring buffer direct write
- `docs/plans/2026-01-17-Hot Path Simplification Report.md` - Implementation summary
