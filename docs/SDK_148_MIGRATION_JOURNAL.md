# SDK 148 Migration Journal

This document tracks issues encountered during migration from Diretta Host SDK 147 to SDK 148, along with analysis of responsibility (our code vs SDK behavior).

---

## CRITICAL: SDK 148 Breaking Changes

SDK 148 introduces **non-backward-compatible changes** that cause crashes after track changes:

### 1. `getNewStream()` Signature Changed
- **SDK 147:** `virtual bool getNewStream(Stream&)` (non-pure virtual)
- **SDK 148:** `virtual bool getNewStream(diretta_stream&) = 0` (pure virtual)

### 2. Stream Class Copy Semantics Deleted
- Copy/assignment operators are now `= delete`
- Only move semantics allowed

### 3. Inheritance Changed
- **SDK 147:** `class Stream : private diretta_stream`
- **SDK 148:** `class Stream : public diretta_stream`

### 4. Root Cause of Crashes
After a Stop→Play sequence (track change), SDK 148 provides Stream objects in a **corrupted state**. Any method call on these objects (`resize()`, `swap()`, `move()`, `get_16()`) causes segmentation fault.

---

## Solution Implemented

### Bypass Stream Class Methods Entirely

Instead of using `DIRETTA::Stream` methods, we:
1. Added a persistent buffer: `std::vector<uint8_t> m_streamData`
2. Directly set the C structure fields of `diretta_stream`:
   ```cpp
   baseStream.Data.P = m_streamData.data();
   baseStream.Size = currentBytesPerBuffer;
   ```

This works because the SDK only reads `Data.P` (pointer) and `Size` fields from `diretta_stream`. By managing our own buffer and bypassing the corrupted `Stream` class methods, we avoid the crash entirely.

### Behavior Matches SDK 147

With the buffer workaround in place:
- Quick resume for same-format track changes (no full reconnect)
- Full close/reopen only on format changes
- No need for EXPERIMENTAL force full reopen feature (removed)

---

## Issue History

### Issue #1: Use-After-Free in Worker Thread

**Date:** 2026-01-19
**Status:** FIXED
**Commit:** `7a15b42`

**Symptom:** Segfault during track changes.

**Root Cause:** Worker thread continued running while SDK was closed.

**Responsibility:** **OURS** - Classic lifecycle management bug.

---

### Issue #2: resize_noremap() Crash

**Date:** 2026-01-19
**Status:** SUPERSEDED by complete solution

**Symptom:** `resize_noremap()` crashed on fresh streams.

**Root Cause:** Part of the broader SDK 148 Stream corruption issue.

---

### Issue #3: Standard resize() Crash

**Date:** 2026-01-19
**Status:** SUPERSEDED by complete solution

**Symptom:** Even standard `resize()` crashed after reconnection.

**Root Cause:** SDK 148 Stream objects corrupted after Stop→Play.

---

### Issue #4: Complete Solution

**Date:** 2026-01-19
**Status:** IMPLEMENTED

**Solution:**
1. Bypass all `DIRETTA::Stream` methods
2. Use our own persistent buffer
3. Directly set `diretta_stream.Data.P` and `diretta_stream.Size`
4. Full SDK close on every track change

**Files Changed:**
- `src/DirettaSync.h` - Added `m_streamData` buffer
- `src/DirettaSync.cpp` - Rewrote `getNewStream()` and `close()`

---

## Responsibility Assessment

| Issue | Responsibility | Notes |
|-------|---------------|-------|
| Worker thread lifecycle | **OURS** | Stop threads before freeing resources |
| Stream corruption after reconnect | **SDK** | Breaking change, not documented |
| Our initial approach using Stream methods | **OURS** | Assumed backward compatibility |

**Overall Verdict:**
- SDK 148 has breaking changes that cause Stream corruption after reconnection
- SDK supplier should document these changes and provide migration guidance
- Our workaround bypasses the corrupted Stream class entirely

---

## Testing Protocol

After making SDK 148 changes, test:
1. Initial playback (first track)
2. Track changes (same format)
3. Format changes (PCM↔DSD, rate changes)
4. User-initiated skip (EXPERIMENTAL mode)
5. Run under GDB for any remaining crashes
6. Test high-rate formats (DSD512) which stress timing
