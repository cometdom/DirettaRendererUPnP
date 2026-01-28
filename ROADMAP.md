# DirettaRendererUPnP Roadmap

## Current Version: v2.0.0

### Status: Stable

Core functionality complete with:
- Low-latency audio path (300ms PCM buffer)
- Lock-free SPSC ring buffer
- AVX2 SIMD format conversions
- Full DSD support (DSD64-DSD512)
- ARM64 support (RPi4, RPi5)
- CPU tuner scripts with auto-detection

---

## Planned: v2.1.0 - Configuration & Optimization

### Web Interface

A lightweight web-based configuration interface for DirettaPlayer.

**Technology:** Python + Flask

**Features:**
- [ ] CPU topology visualization
- [ ] Real-time service status
- [ ] Optimization toggle controls
- [ ] Thread distribution view
- [ ] Apply/revert with confirmation
- [ ] Reboot management

**Mockup:**
```
┌─────────────────────────────────────────────────────────────────┐
│  DirettaPlayer                                    [Status: ●]   │
├─────────────────────────────────────────────────────────────────┤
│  CPU: AMD Ryzen 9 5900X (12c/24t)                              │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ CCD0: [■][□][□][□][□][□]  CCD1: [□][□][□][□][□][□]     │   │
│  │       HK ────── Renderer ──────────────────────         │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  [✓] CPU Isolation        [✓] RT Scheduling    [ ] NUMA        │
│  [✓] IRQ Affinity         [✓] Performance Gov  [ ] Huge Pages  │
│                                                                 │
│  [Apply Changes]  [Revert All]                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Advanced CPU Optimizations

**NUMA/CCD Awareness:**
- [ ] Detect CCDs on Ryzen processors
- [ ] Keep renderer threads on same CCD for lower memory latency
- [ ] NUMA node pinning for multi-socket systems

**Huge Pages:**
- [ ] Enable transparent huge pages for audio buffers
- [ ] Reduce TLB misses

**Network IRQ Pinning:**
- [ ] Identify network adapter IRQs
- [ ] Pin to dedicated housekeeping core
- [ ] Optimize for Diretta UDP traffic

**C-State Control:**
- [ ] Disable deep C-states on renderer cores
- [ ] Reduce wakeup latency

**Kernel Parameters:**
- [ ] Timer frequency (1000Hz recommendation)
- [ ] Preempt settings
- [ ] RCU callback offloading

**UDP Buffer Tuning:**
- [ ] Optimal rmem/wmem for Diretta
- [ ] Network queue settings

---

## Future: v2.2.0 - Multi-Room & Extended Formats

### Ideas (not committed)

- Multi-target streaming (same audio to multiple Diretta targets)
- MQA passthrough support
- Roon RAAT bridge mode
- DLNA/UPnP renderer aggregation

---

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch (`feature/your-feature`)
3. Submit a pull request

For major changes, please open an issue first to discuss.

---

## Version History

| Version | Release Date | Focus |
|---------|--------------|-------|
| v1.0.0 | 2025 | Initial release |
| v1.2.1 | 2025 | Stability fixes |
| v1.3.3 | 2025 | DSD improvements |
| v2.0.0 | 2026-01 | Complete rewrite, low-latency |
| v2.1.0 | TBD | Web interface, advanced tuning |
