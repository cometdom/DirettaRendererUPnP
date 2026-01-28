# Fork Changes: DirettaRendererUPnP-X vs 1.2.1

This document describes the architectural changes and differences between this fork (DirettaRendererUPnP-X) and the original DirettaRendererUPnP v1.2.1.

## Summary

| Metric | Original (1.2.1) | This Fork (X) | Change |
|--------|------------------|---------------|--------|
| Total lines of code | 7,104 | 6,474 | -9% (-630 lines) |
| DirettaRenderer | 1,058 lines | 697 lines | -34% |
| Diretta core files | 1,655 lines | 1,733 lines | +5% (better separation) |
| SDK API | `DIRETTA::SyncBuffer` | `DIRETTA::Sync` | Lower-level |
| Architecture | 2-class design | 3-class design | Cleaner separation |
| Version | 1.2.1 "Gapless Pro" | 1.2.0-simplified | Refactored |

---

## Architectural Changes

### Original 1.2.1 Structure

```
src/
├── DirettaOutput.cpp    (1,370 lines) - Connection, format, gapless, buffering
├── DirettaOutput.h      (285 lines)
├── DirettaRenderer.cpp  (957 lines)   - UPnP + threading + callbacks + gapless
├── DirettaRenderer.h    (101 lines)
├── AudioEngine.cpp      (1,810 lines)
├── AudioEngine.h        (414 lines)
├── UPnPDevice.cpp       (1,403 lines)
├── UPnPDevice.hpp       (185 lines)
├── ProtocolInfoBuilder.h (289 lines)
└── main.cpp             (290 lines)
```

### This Fork Structure

```
src/
├── DirettaSync.cpp      (1,150 lines) - Unified adapter (inherits DIRETTA::Sync)
├── DirettaSync.h        (331 lines)
├── DirettaRingBuffer.h  (252 lines)   - Extracted lock-free ring buffer
├── DirettaRenderer.cpp  (613 lines)   - Simplified orchestrator
├── DirettaRenderer.h    (84 lines)
├── AudioEngine.cpp      (1,710 lines)
├── AudioEngine.h        (356 lines)
├── UPnPDevice.cpp       (1,343 lines)
├── UPnPDevice.hpp       (178 lines)
├── ProtocolInfoBuilder.h (289 lines)
└── main.cpp             (168 lines)
```

---

## Key Technical Differences

### 1. SDK API Approach

| Aspect | Original (1.2.1) | This Fork (X) |
|--------|------------------|---------------|
| Base class | Uses `DIRETTA::SyncBuffer` | Inherits from `DIRETTA::Sync` |
| Model | Push-only (write to buffer) | Push/pull hybrid |
| Buffer control | SDK manages internally | App manages via DirettaRingBuffer |
| Timing control | SDK-controlled | Full control via `getNewStream()` override |
| Gapless method | SDK native (`writeStreamStart`/`addStream`) | Ring buffer continuous feed |

**Original 1.2.1 approach:**
```cpp
m_syncBuffer = std::make_unique<DIRETTA::SyncBuffer>();
m_syncBuffer->open(...);
m_syncBuffer->write(data, size);

// Gapless Pro:
m_syncBuffer->writeStreamStart(stream);
m_syncBuffer->addStream(nextTrackStream);
```

**This fork approach:**
```cpp
class DirettaSync : public DIRETTA::Sync {
    bool getNewStream(DIRETTA::Stream& stream) override {
        // Pull data from ring buffer on SDK request
        m_ringBuffer.pop(dest, bytesNeeded);
        return true;
    }
};

// Push data to ring buffer
m_ringBuffer.push(data, size);
```

### 2. Features in 1.2.1 NOT in This Fork

| Feature | Description |
|---------|-------------|
| SDK gapless methods | Uses `writeStreamStart()`/`addStream()` (same result as X) |
| Buffer seconds config | `--buffer` CLI option (default 2.0s) |
| Advanced SDK settings | `--thread-mode`, `--cycle-time`, `--cycle-min-time`, `--info-cycle` |
| MTU override | `--mtu` CLI option |
| Network optimization | `optimizeNetworkConfig()` per format (DSD/Hi-Res/Standard) |
| Bind IP option | `--bind-ip` in addition to `--interface` |
| SSDP thread | Separate thread for SSDP handling |

### 3. Features in This Fork NOT in 1.2.1

| Feature | Description |
|---------|-------------|
| DirettaRingBuffer | Extracted reusable ring buffer class |
| Pull model support | `getNewStream()` override for SDK callback |
| DSD byte swap | `m_needDsdByteSwap` for LITTLE endian targets |
| Cycle calculator | `DirettaCycleCalculator` class for timing |
| Format reopen | `reopenForFormatChange()` with silence buffers |
| Transfer modes | Explicit `DirettaTransferMode` enum |
| Buffer config | `DirettaBuffer` namespace with tuning constants |

### 4. Command-Line Options Comparison

**Original 1.2.1:**
```
--name, -n          Device name
--port, -p          UPnP port
--target, -t        Target index
--buffer, -b        Buffer seconds (float)
--no-gapless        Disable gapless
--thread-mode       SDK thread mode
--cycle-time        Cycle time (µs)
--cycle-min-time    Minimum cycle time
--info-cycle        Info cycle time
--mtu               MTU override
--interface         Network interface
--bind-ip           Bind to IP address
--list-targets, -l  List targets
--verbose, -v       Verbose logging
--help, -h          Help
```

**This fork (X):**
```
--name, -n          Device name
--port, -p          UPnP port
--target, -t        Target index
--no-gapless        Disable gapless
--interface         Network interface
--list-targets, -l  List targets
--verbose, -v       Verbose logging
--help, -h          Help
```

### 5. Ring Buffer Extraction

The ring buffer is now a separate, reusable class with specialized push methods:

```cpp
class DirettaRingBuffer {
    // Direct PCM copy
    size_t push(const uint8_t* data, size_t len);

    // 24-bit packing (S24_P32 → 24-bit)
    size_t push24BitPacked(const uint8_t* data, size_t inputSize);

    // 16-bit to 32-bit upsampling
    size_t push16To32(const uint8_t* data, size_t inputSize);

    // DSD planar to interleaved with optional bit reversal & byte swap
    size_t pushDSDPlanar(const uint8_t* data, size_t inputSize,
                         int numChannels,
                         const uint8_t* bitReverseTable,
                         bool byteSwap = false);
};
```

### 6. Format Transition Handling

**Original 1.2.1:** Light reconfigure with network optimization
```cpp
void DirettaOutput::optimizeNetworkConfig(const AudioFormat& format) {
    if (format.isDSD) {
        // VarMax for DSD
    } else if (format.sampleRate >= 192000) {
        // Adaptive for Hi-Res
    } else {
        // Fixed for standard
    }
}
```

**This fork:** Full `reopenForFormatChange()`:
```cpp
bool DirettaSync::reopenForFormatChange() {
    // 1. Send silence buffers (100 for DSD, 30 for PCM)
    requestShutdownSilence(m_isDsdMode ? 100 : 30);

    // 2. Wait for silence to be sent
    while (m_silenceBuffersRemaining > 0) { ... }

    // 3. Full SDK shutdown
    stop();
    disconnect(true);
    DIRETTA::Sync::close();

    // 4. Wait for DAC stabilization
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // 5. Reopen SDK
    DIRETTA::Sync::open(...);
    setSink(...);

    return true;
}
```

---

## DSD Handling Comparison

| Feature | Original (1.2.1) | This Fork (X) |
|---------|------------------|---------------|
| Source format detection | DSF/DFF via extension | DSF/DFF via extension |
| Bit reversal flag | `m_needDsdBitReversal` | `m_needDsdBitReversal` |
| Byte swap support | No | Yes (`m_needDsdByteSwap`) |
| Format propagation | `TrackInfo::DSDSourceFormat` | `AudioFormat::DSDFormat` |

---

## Bug Fixes (Common to Both)

### 1. DSD Source Format Detection
**Issue:** Source format (DSF/DFF) not considered when deciding bit reversal
**Fix:** Detect format from file extension, pass to Diretta configuration

### 2. Multi-Interface Support
**Issue:** `--interface` option parsed but ignored
**Fix:** Propagate `networkInterface` to UPnPDevice config

---

## Files Changed Summary

| File | 1.2.1 | X | Status |
|------|-------|---|--------|
| `DirettaOutput.cpp/h` | 1,655 | - | **Removed** (merged into DirettaSync) |
| `DirettaSync.cpp/h` | - | 1,481 | **New** |
| `DirettaRingBuffer.h` | - | 252 | **New** |
| `DirettaRenderer.cpp/h` | 1,058 | 697 | **Simplified** (-34%) |
| `AudioEngine.cpp/h` | 2,224 | 2,066 | **Modified** (-7%) |
| `UPnPDevice.cpp/hpp` | 1,588 | 1,521 | **Modified** (-4%) |
| `main.cpp` | 290 | 168 | **Simplified** (-42%) |

---

## Compatibility

| Aspect | Compatibility |
|--------|---------------|
| Command-line interface | Partial (fewer advanced options) |
| UPnP behavior | Unchanged |
| Control points | Same compatibility |
| SDK requirement | Same (Diretta Host SDK v1.47) |
| Gapless playback | Equivalent performance, different implementation |

---

## When to Use Which Version

**Use Original 1.2.1 if you need:**
- Advanced SDK tuning options (`--thread-mode`, `--cycle-time`, etc.)
- Network optimization per format (VarMax/Adaptive/Fixed)
- Configurable buffer size (`--buffer`)

**Use This Fork (X) if you need:**
- Independence from `SyncBuffer` class for more fine-tuning freedom
- Direct control over timing via `DIRETTA::Sync` and `getNewStream()` callback
- DSD byte swap for LITTLE endian targets
- Full format transition control (reopen with silence buffers)

**Note:** Both versions achieve equivalent gapless playback performance through different implementations.

---

## Credits

- Original DirettaRendererUPnP by Dominique (cometdom)
- MPD Diretta Output Plugin v0.4.0 for DIRETTA::Sync API patterns
- Claude Code for refactoring assistance
