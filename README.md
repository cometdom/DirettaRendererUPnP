# Diretta UPnP Renderer v2.4.0

**The world's first native UPnP/DLNA renderer with Diretta protocol support - Low-Latency Edition**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

![Version](https://img.shields.io/badge/version-2.4.0-blue.svg)
![Low Latency](https://img.shields.io/badge/Latency-Low-green.svg)
![SDK](https://img.shields.io/badge/SDK-DIRETTA::Sync-orange.svg)
![Audirvana](https://img.shields.io/badge/Audirvana-Compatible-green.svg)

---

## What's New in v2.4.0

**Target network link tuning, IRQ affinity, SMT toggle, isolcpus documentation.**

- **Target network link tuning** (PR #67 by Daniel/Koala887) — New "Advanced Network Settings" section in the web UI to force the speed and duplex of the host NIC used to reach the Diretta target via `ethtool` (some audiophile users report perceived sound-quality differences when constraining the link, typically to 100 Mbit). Configurable via `TARGET_INTERFACE`, `TARGET_SPEED`, and `TARGET_DUPLEX`; leave the interface empty to keep the default behaviour. Bandwidth-vs-format reminder included so users don't accidentally pick a link speed too narrow for hi-res PCM or DSD.
- **IRQ affinity for the target NIC(s)** — New `IRQ_INTERFACE` / `IRQ_CPUS` config keys (also in the web UI) pin all hardware interrupts of one or more NICs (single name or comma-separated list, e.g. `enp1s0,enp2s0`), including MSI-X queues, to a specific CPU list at service start. Multi-interface support covers hosts with one NIC for the upstream source and another for the Diretta target. Used in conjunction with `--cpu-audio`, this keeps network IRQ activity off the audio worker core — a known source of jitter on busy LANs.
- **SMT (Hyper-Threading) toggle at service start** — New `SMT` config key (`on` / `off` / `forceoff`, also in the web UI under "CPU Affinity") writes the chosen value to `/sys/devices/system/cpu/smt/control` before launching DRUP. Lets dedicated audio machines disable SMT cleanly without BIOS reflashing or kernel cmdline edits, and the wrapper re-applies the setting on every service start. System-wide setting; review your `CPU_AUDIO` / `CPU_OTHER` values when toggling because the logical-CPU numbering changes.
- **isolcpus / nohz_full / rcu_nocbs documentation** — `docs/CONFIGURATION.md` now covers how to fully reserve a CPU core for the Diretta worker via the kernel cmdline, with bootloader-specific instructions (GRUB, systemd-boot) and recovery guidance.

See [CHANGELOG.md](CHANGELOG.md) for details.

### Previous Versions

| Version | Highlights |
|---------|-----------|
| **v2.3.0** | Multi-core CPU affinity, configurable buffers, Audirvana internet radio fix (grajaw) |
| **v2.2.3** | Complete CPU isolation, build system optimization, Web UI Stop button |
| **v2.2.2** | Clang + LTO build support (sheviks), 32-bit 768kHz playlist fix (abase) |
| **v2.2.1** | Larger PCM buffer for CDN resilience, FFmpeg detection fix (sheviks) |
| **v2.2.0** | CPU affinity, AIFF support, MinimServer DSD transcoding fix |
| **v2.1.11** | AIFF support (FFmpeg build config) |
| **v2.1.10** | Config variable alignment for GentooPlayer/downstream integrations |
| **v2.1.9** | Track restart fix (same URI shortcut removed) |
| **v2.1.8** | Minimal UPnP mode (`--minimal-upnp`) for audiophile-grade playback |
| **v2.1.7** | UAPP SCPD fix (missing GetPositionInfo arguments) |
| **v2.1.6** | UAPP async Play response, service startup fix (Pascal) |
| **v2.1.5** | DAC bit depth negotiation, Audirvana white noise fix (herisson-88), first-play glitch, UAPP milliseconds |
| **v2.1.4** | Audirvana link-local detection, resilient UPnP startup |
| **v2.1.3** | Fix target retry pre-check bypass |
| **v2.1.2** | Resilient target discovery (retry at startup instead of immediate exit) |
| **v2.1.1** | UAPP compatibility, format transition stability, high sample rate buffers, build capabilities logging |
| **v2.1.0** | Web Configuration UI, Advanced SDK settings, stop fix (herisson-88), libupnp auto-detect |
| **v2.0.6** | Advanced SDK settings, config migration, stop fix (herisson-88), libupnp auto-detect |
| **v2.0.5** | Stop fix for Holo Red (herisson-88), libupnp auto-detection, privilege drop removed |
| **v2.0.4** | Centralized logging, rebuffering on underrun, ARM NEON SIMD, systemd hardening, unit tests |
| **v2.0.3** | Audirvana compatibility (UPnP event deduplication), adaptive buffer for remote streaming |
| **v2.0.2** | Native DSD from Audirvana (DSDIFF parser), UPnP event notifications, gapless preload |
| **v2.0.1** | 24-bit white noise fix for DACs without 32-bit support |
| **v2.0.0** | Complete rewrite — low-latency architecture, lock-free ring buffer, AVX2 SIMD |

See [CHANGELOG.md](CHANGELOG.md) for full version history.

---

## Support This Project

If you find this renderer valuable, you can support development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cometdom)

**Important notes:**
- Donations are **optional** and appreciated
- Help cover test equipment and coffee
- **No guarantees** for features, support, or timelines
- The project remains free and open source for everyone

---

## IMPORTANT - PERSONAL USE ONLY

This renderer uses the **Diretta Host SDK**, which is proprietary software by Yu Harada available for **personal use only**. Commercial use is strictly prohibited. See [LICENSE](LICENSE) for details.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Requirements](#requirements)
- [Upgrading](#upgrading)
- [Quick Start](#quick-start)
- [Supported Formats](#supported-formats)
- [Performance](#performance)
- [Compatible Control Points](#compatible-control-points)
- [System Optimization](#system-optimization)
- [CPU Tuning](#cpu-isolation--tuning-advanced)
- [Command Line Options](#command-line-options)
- [Web Configuration UI](#web-configuration-ui)
- [Troubleshooting](#troubleshooting)
- [Documentation](#documentation)
- [Credits](#credits)
- [License](#license)

---

## Overview

This is a **native UPnP/DLNA renderer** that streams high-resolution audio using the **Diretta protocol** for bit-perfect playback. Unlike software-based solutions that go through the OS audio stack, this renderer sends audio directly to a **Diretta Target endpoint** (such as Memory Play, GentooPlayer, or hardware with Diretta support), which then connects to your DAC.

### What is Diretta?

Diretta is a proprietary audio streaming protocol developed by Yu Harada that enables ultra-low latency, bit-perfect audio transmission over Ethernet. The protocol uses two components:

- **Diretta Host**: Sends audio data (this renderer uses the Diretta Host SDK)
- **Diretta Target**: Receives audio data and outputs to DAC (e.g., Memory Play, GentooPlayer, or DACs with native Diretta support)

### Key Benefits

- **Bit-perfect streaming** - Bypasses OS audio stack entirely
- **Ultra-low latency** - ~300ms PCM buffer (vs ~1s in v1.x)
- **High-resolution support** - Up to DSD1024 and PCM 1536kHz
- **Gapless playback** - Seamless track transitions
- **UPnP/DLNA compatible** - Works with any UPnP control point
- **Network optimization** - Adaptive packet sizing with jumbo frame support

---

## Architecture

Version 2.x uses a simplified, performance-focused architecture:

```
┌─────────────────────────────┐
│  UPnP Control Point         │  (JPlay, BubbleUPnP, mConnect, etc.)
└─────────────┬───────────────┘
              │ UPnP/DLNA Protocol (HTTP/SOAP/SSDP)
              ▼
┌───────────────────────────────────────────────────────────────┐
│  DirettaRendererUPnP v2.x                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
│  │   UPnPDevice    │─▶│ DirettaRenderer │─▶│  AudioEngine  │  │
│  │ (discovery,     │  │ (orchestrator,  │  │ (FFmpeg       │  │
│  │  transport)     │  │  threading)     │  │  decode)      │  │
│  └─────────────────┘  └────────┬────────┘  └───────┬───────┘  │
│                                │                   │          │
│                                ▼                   ▼          │
│                  ┌─────────────────────────────────────────┐  │
│                  │           DirettaSync                   │  │
│                  │  ┌───────────────────────────────────┐  │  │
│                  │  │       DirettaRingBuffer           │  │  │
│                  │  │  (lock-free SPSC, AVX2 convert)   │  │  │
│                  │  └───────────────────────────────────┘  │  │
│                  │              │                          │  │
│                  │              ▼ getNewStream() callback  │  │
│                  │  ┌───────────────────────────────────┐  │  │
│                  │  │      DIRETTA::Sync (SDK)          │  │  │
│                  │  └───────────────────────────────────┘  │  │
│                  └─────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
              │ Diretta Protocol (UDP/Ethernet)
              ▼
┌─────────────────────────────┐
│      Diretta TARGET         │  (Memory Play, GentooPlayer, etc.)
└─────────────┬───────────────┘
              ▼
┌─────────────────────────────┐
│            DAC              │
└─────────────────────────────┘
```

### v2.0 vs v1.x Architecture

| Component | v1.x | v2.0 |
|-----------|------|------|
| SDK Base Class | `DIRETTA::SyncBuffer` | `DIRETTA::Sync` |
| Data Model | Push (SDK manages timing) | Pull (`getNewStream()` callback) |
| Ring Buffer | Standard | Lock-free SPSC with AVX2 |
| Format Conversion | Per-sample | SIMD batch (8-32 samples) |
| Thread Safety | Mutex-based | Lock-free atomics |

---

## Features

### Audio Quality
- **Bit-perfect streaming**: No resampling or processing (when formats match)
- **PCM Bypass mode**: Direct path for bit-perfect playback when no conversion needed
- **High-resolution support**:
  - PCM: Up to 32-bit/1536kHz
  - DSD: DSD64, DSD128, DSD256, DSD512, DSD1024
- **Format support**: FLAC, ALAC, WAV, AIFF, DSF, DFF, MP3, AAC, OGG
- **Gapless playback**: Seamless album listening experience

### Low-Latency Optimizations
- **Reduced buffers**: 300ms PCM (was 1s), 800ms DSD
- **Micro-sleeps**: 500µs flow control (was 10ms)
- **Lock-free path**: Zero mutex in audio hot path
- **SIMD conversions**: AVX2 for 8-32x throughput
- **Zero allocations**: Pre-allocated buffers in steady state

### UPnP/DLNA Features
- **Full transport control**: Play, Stop, Pause, Resume, Seek
- **Device discovery**: SSDP advertisement for automatic detection
- **Dynamic protocol info**: Exposes all supported formats to control points
- **Position tracking**: Real-time playback position updates

### Network Optimization
- **Adaptive packet sizing**: Synchronized with SDK cycle time
- **Jumbo frame support**: Up to 16KB MTU for maximum performance
- **Automatic MTU detection**: Configures optimal packet size

### Robustness
- **Resilient startup**: Retries target discovery indefinitely if target is not yet available, with periodic status logging
- **Auto-release**: Diretta target released after idle timeout for coexistence with other Diretta hosts

---

## Requirements

### Supported Architectures

The renderer automatically detects and optimizes for your CPU:

| Architecture | Variants | Notes |
|--------------|----------|-------|
| **x64 (Intel/AMD)** | v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4 | AVX2 recommended but not required |
| **ARM64** | Standard (4KB pages), k16 (16KB pages) | Pi 4/5 supported |
| **RISC-V** | Experimental | riscv64 |

**Note on older x64 CPUs:** CPUs without AVX2 (Sandy Bridge, Ivy Bridge - 2011-2012) are fully supported. The build system automatically detects CPU capabilities and uses optimized scalar implementations when AVX2 is not available. Use the `x64-linux-15v2` SDK variant for these systems.

### Platform Support

| Platform | Status |
|----------|--------|
| **Linux x64** | Supported (Fedora, Ubuntu, Arch, AudioLinux) |
| **Linux ARM64** | Supported (Raspberry Pi 4/5) |
| **Windows** | Not supported |
| **macOS** | Not supported |

### Hardware
- **Minimum**: Dual-core CPU, 1GB RAM, Gigabit Ethernet
- **Recommended**: Quad-core CPU, 2GB RAM, 2.5/10G Ethernet with jumbo frames
- **Network**: Gigabit Ethernet minimum (10G recommended for DSD512+)
- **MTU**: 1500 minimum, jumbo frames recommended (9014 or 16128, must match Diretta Target)

### Software
- **OS**: Linux with kernel 5.x+ (RT kernel recommended)
- **Diretta Host SDK**: Version 148 (download from [diretta.link](https://www.diretta.link/hostsdk.html))
- **FFmpeg**: Version 5.x or later
- **libupnp**: UPnP/DLNA library

---

## Upgrading

### From v2.0.x to v2.1.0

```bash
# 1. Stop the service
sudo systemctl stop diretta-renderer

# 2. Pull the latest version
cd ~/DirettaRendererUPnP
git pull

# 3. Re-run the installer (rebuilds, migrates config, reinstalls service)
./install.sh
# Select: Full install or Build + Service

# 4. Optionally install the web configuration UI
./install.sh --webui

# 5. Restart the service
sudo systemctl start diretta-renderer
```

### From v2.0.4/v2.0.5 to v2.0.6

The configuration file has changed significantly in v2.0.6 (new SDK settings, removed options). The installer **automatically migrates your settings**:

```bash
# 1. Stop the service
sudo systemctl stop diretta-renderer

# 2. Pull the latest version
cd ~/DirettaRendererUPnP
git pull

# 3. Re-run the installer (rebuilds, migrates config, reinstalls service)
./install.sh
# Select: Full install or Build + Service

# 4. Restart the service
sudo systemctl start diretta-renderer
```

**What happens during upgrade:**
- Your old `diretta-renderer.conf` is backed up as `diretta-renderer.conf.bak`
- A fresh config template is installed with all new v2.0.6 options
- Your existing settings (TARGET, PORT, NETWORK_INTERFACE, etc.) are **automatically migrated** to the new file
- Obsolete settings (e.g., `DROP_USER`) are detected and skipped with a warning
- New advanced SDK settings appear commented with their default values, ready to customize

> **Tip:** After upgrading, review the new options in `/etc/default/diretta-renderer` — the advanced Diretta SDK settings section allows fine-tuning of thread priority, transfer mode, and timing parameters.

### From v1.x (clean install required)

Version 2.x has a completely different architecture and configuration format. A clean installation is required:

```bash
# Stop and remove old installation
sudo systemctl stop diretta-renderer
sudo systemctl disable diretta-renderer
sudo rm -rf /opt/diretta-renderer-upnp
sudo rm -f /etc/systemd/system/diretta-renderer.service
sudo systemctl daemon-reload
```

Then follow the [Quick Start](#quick-start) instructions for a fresh installation.

> **Note:** The v2.x configuration file (`diretta-renderer.conf`) has a different format than v1.x. Your old configuration will not work with v2.x.

---

## Quick Start

### 1. Install Dependencies

**Fedora:**
```bash
sudo dnf install -y gcc-c++ make ffmpeg-free-devel libupnp-devel
```

**Ubuntu/Debian:**
```bash
sudo apt install -y build-essential libavformat-dev libavcodec-dev \
    libavutil-dev libswresample-dev libupnp-dev
```

**Arch Linux:**
```bash
sudo pacman -S base-devel ffmpeg libupnp
```

### 2. Download Diretta Host SDK

1. Visit [diretta.link](https://www.diretta.link/hostsdk.html)
2. Download **DirettaHostSDK_148** (or latest version)
3. Extract to `~/DirettaHostSDK_148`

> **Tip: Transferring files from Windows to Linux**
>
> If you downloaded the SDK on Windows and need to transfer it to your Linux machine:
>
> **Using PowerShell or CMD** (OpenSSH is built into Windows 10/11):
> ```powershell
> # Transfer the SDK archive to your Linux machine
> # Replace with actual filename (e.g., DirettaHostSDK_148_5.tar.zst)
> scp C:\Users\YourName\Downloads\DirettaHostSDK_XXX_Y.tar.zst user@linux-ip:~/
> ```
>
> **Using WSL** (Windows Subsystem for Linux):
> ```bash
> # Windows files are accessible under /mnt/c/
> cp /mnt/c/Users/YourName/Downloads/DirettaHostSDK_*.tar.zst ~/
> ```
>
> Then extract on Linux:
> ```bash
> cd ~
> tar --zstd -xf DirettaHostSDK_*.tar.zst
> ```

### 3. Clone and Install

```bash
# Clone repository
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP

# Make the install script executable
chmod +x install.sh

# Run the interactive installer
./install.sh
```

The installer provides an interactive menu with options for:
- Building the application (auto-detects architecture and SDK)
- Installing as a systemd service
- Configuring automatic startup
- Setting up the Diretta target
- Installing the web configuration UI

#### Alternative: Build with Clang + LTO

GCC is the default compiler. Users who prefer Clang with Link-Time Optimization can use:

```bash
# Build with Clang + LTO (auto-installs clang and lld)
env LLVM=1 ./install.sh             # Interactive installer
# or
make LLVM=1                         # Direct build (requires clang/lld already installed)
```

When using `install.sh` with `LLVM=1`, `clang` and `lld` are installed automatically if not already present (supports Fedora, Debian/Ubuntu, Arch). When building directly with `make LLVM=1`, install them manually:

```bash
sudo dnf install clang lld          # Fedora
sudo apt install clang lld          # Debian/Ubuntu
sudo pacman -S clang lld            # Arch
```

Clang+LTO is opt-in and may offer different performance and sound characteristics. (Added in v2.2.2, PR #64 by sheviks)

### 4. Configure Network (Recommended)

Enable jumbo frames for best performance:

```bash
# Temporary (until reboot)
sudo ip link set eth0 mtu 9000

# Permanent (NetworkManager)
sudo nmcli connection modify "Your Connection" 802-3-ethernet.mtu 9000
sudo nmcli connection up "Your Connection"
```

### 5. Run

```bash
# List available Diretta targets
sudo ./bin/DirettaRendererUPnP --list-targets

# Run with specific target
sudo ./bin/DirettaRendererUPnP --target 1

# Run with verbose logging (for troubleshooting)
sudo ./bin/DirettaRendererUPnP --target 1 --verbose
```

### 6. Connect from Control Point

Open your UPnP control point (JPlay, BubbleUPnP, mConnect, etc.) and look for "Diretta Renderer" in available devices.

---

## Supported Formats

| Format Type | Bit Depth | Sample Rates | Container | SIMD Optimization |
|-------------|-----------|--------------|-----------|-------------------|
| **PCM** | 16-bit | 44.1kHz - 384kHz | FLAC, WAV, AIFF | AVX2 16x / NEON 8x |
| **PCM** | 24-bit | 44.1kHz - 384kHz | FLAC, ALAC, WAV | AVX2 8x / NEON 4x |
| **PCM** | 32-bit | 44.1kHz - 1536kHz | WAV | memcpy |
| **DSD** | 1-bit | DSD64 - DSD1024 | DSF, DFF | AVX2 32x / NEON 16x |
| **Lossy** | Variable | Up to 192kHz | MP3, AAC, OGG | - |

### PCM Bypass Mode

When source and target formats match exactly, the renderer uses a **bypass mode** that skips all processing for true bit-perfect playback. Log message: `[AudioDecoder] PCM BYPASS enabled - bit-perfect path`

### DSD Conversion Modes

DSD conversion mode is selected once per track for optimal performance:

| Mode | Use Case |
|------|----------|
| Passthrough | DSF→LSB target, DFF→MSB target |
| BitReverseOnly | DSF→MSB target, DFF→LSB target |
| ByteSwapOnly | Little-endian targets |
| BitReverseAndSwap | Little-endian + bit order mismatch |

---

## Performance

### Buffer Configuration

| Parameter | v1.x | v2.0 | Benefit |
|-----------|------|------|---------|
| PCM Buffer | ~1000ms | ~300ms | 70% lower latency |
| DSD Buffer | ~1000ms | ~800ms | Better stability |
| PCM Prefill | 50ms | 30ms | Faster start |
| Flow Control | 10ms sleep | 500µs wait | 96% less jitter |

### SIMD Throughput

| Conversion | Function | Throughput |
|------------|----------|------------|
| 24-bit pack (LSB) | `convert24BitPacked_AVX2()` | 8 samples/instruction |
| 24-bit pack (MSB) | `convert24BitPackedShifted_AVX2()` | 8 samples/instruction |
| 16→32 upsample | `convert16To32_AVX2()` | 16 samples/instruction |
| DSD interleave | `convertDSD_*()` | 32 bytes/instruction |

### Audio Quality Tuning

For the best possible audio quality, the following system-level optimizations are recommended:

#### CPU Affinity (v2.2.0+)

Pinning audio threads to dedicated CPU cores reduces jitter and improves soundstage clarity:

```ini
# In /etc/default/diretta-renderer
CPU_AUDIO=2    # Diretta worker thread (critical hot path)
CPU_OTHER=3    # Decode, UPnP, and other threads
```

Use cores on the same CCD (AMD) or same P-core cluster (Intel) and avoid core 0 (used by kernel/interrupts). Also configurable via the web UI under "CPU Affinity".

#### Disable SMT (Hyperthreading)

Simultaneous Multithreading (SMT/HT) shares physical core resources between two logical threads, which can introduce micro-jitter on the audio path. Disabling SMT ensures each core is fully dedicated.

**Via DirettaRendererUPnP (v2.4.0+, recommended)** — set in the web UI under **CPU Affinity → SMT**, or in `/etc/default/diretta-renderer`:

```ini
SMT=off          # or "on", "forceoff", or empty for "no change"
```

The wrapper re-applies this on every service start, so the setting survives service restarts. Note that toggling SMT changes the logical-CPU numbering — if you also use `CPU_AUDIO` / `CPU_OTHER`, make sure those values reference logical CPUs that exist in the chosen state (e.g. a 12-core CPU exposes CPUs 0-23 with SMT on, 0-11 with SMT off).

**Manual one-shot equivalent**:

```bash
# Disable SMT (temporary, until reboot)
echo off | sudo tee /sys/devices/system/cpu/smt/control

# Verify
cat /sys/devices/system/cpu/smt/active   # Should show "0"
```

**Permanent across reboots** — add `nosmt` to your kernel cmdline (in `/etc/default/grub`, then run `grub2-mkconfig`). This bypasses the BIOS default at boot and is the most robust option for a dedicated audio machine.

#### Minimal UPnP Mode (v2.1.8+)

Reduces CPU wakeups during playback by disabling position polling and event notifications:

```ini
MINIMAL_UPNP=1
```

Recommended for JPlay iOS, LMS via slim2UPnP, and Roon. See [Minimal UPnP Mode](#minimal-upnp-mode) for details.

### Network Requirements

#### PCM Formats

| Audio Format | Data Rate | Cycle Time (MTU 1500) | MTU 1500 | MTU 9000 |
|--------------|-----------|----------------------|----------|----------|
| CD Quality (16/44.1) | ~172 KB/s | ~8.7 ms | ✅ OK | ✅ OK |
| Hi-Res (24/96) | ~690 KB/s | ~2.2 ms | ✅ OK | ✅ OK |
| Hi-Res (24/192) | ~1.4 MB/s | ~1.1 ms | ⚠️ Marginal | ✅ OK |
| Hi-Res (32/384) | ~3.7 MB/s | ~0.4 ms | ❌ Not viable | ✅ OK |

#### DSD Formats (Critical: Jumbo Frames Required for DSD128+)

| DSD Format | Data Rate | Cycle Time (MTU 1500) | Packets/sec | MTU 1500 | MTU 9000 |
|------------|-----------|----------------------|-------------|----------|----------|
| **DSD64** | ~0.7 MB/s | ~2.1 ms | ~470 | ✅ OK | ✅ OK |
| **DSD128** | ~1.4 MB/s | ~1.0 ms | ~940 | ⚠️ **Marginal** | ✅ OK |
| **DSD256** | ~2.8 MB/s | ~0.5 ms | ~1,880 | ❌ Not viable | ✅ OK |
| **DSD512** | ~5.6 MB/s | ~0.27 ms | ~3,770 | ❌ Not viable | ✅ OK |
| **DSD1024** | ~11.3 MB/s | ~0.13 ms | ~7,540 | ❌ Not viable | ⚠️ Marginal |

> **Why DSD128 is problematic with MTU 1500:** The Diretta protocol requires precise timing. With a ~1ms cycle time, there's almost no margin for network jitter or system latency. Even small delays cause audible dropouts. Jumbo frames (MTU 9000) increase the cycle time to ~6ms, providing 6× more tolerance.

#### Checking Your MTU

```bash
# Check current MTU on your network interface
ip link show eth0 | grep mtu

# Test actual path MTU to your Diretta Target
ping -M do -s 8972 <target_ip>   # Tests MTU 9000 (8972 + 28 bytes header)
ping -M do -s 1472 <target_ip>   # Tests MTU 1500 (1472 + 28 bytes header)
```

#### Recommended MTU by Format

| Usage | Minimum MTU |
|-------|-------------|
| PCM up to 96kHz, DSD64 only | 1500 (standard) |
| PCM up to 192kHz, DSD64 | 1500 (standard) |
| **DSD128** | **9000 (jumbo) recommended** |
| **DSD256 and above** | **9000 (jumbo) required** |

---

## Compatible Control Points

| Control Point | Platform | Rating | Notes |
|---------------|----------|--------|-------|
| **Audirvana** | macOS/Windows | Excellent | DSD native (DFF), gapless natively supported — disable "Universal Gapless" in Audirvana |
| **JPlay iOS** | iOS | Excellent | Full feature support |
| **BubbleUPnP** | Android | Excellent | Highly configurable |
| **mConnect** | iOS/Android | Very Good | Clean interface |
| **Linn Kazoo** | iOS/Android | Good | Needs OpenHome (BubbleUPnP server) |

---

## System Optimization

### CPU Governor
```bash
# Performance mode for best audio quality
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Real-Time Priority
```bash
# Allow real-time scheduling (renderer sets SCHED_FIFO priority 50)
sudo setcap cap_sys_nice+ep ./bin/DirettaRendererUPnP
```

### Network Tuning
```bash
# Increase network buffers
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
```

### CPU Isolation & Tuning (Advanced)

> ⚠️ **Heads-up — the tuner scripts predate DirettaRendererUPnP's native
> CPU affinity (v2.2.0) and IRQ affinity (v2.4.0) features.** For most
> users, configuring `CPU_AUDIO` / `CPU_OTHER` / `IRQ_INTERFACE` /
> `IRQ_CPUS` via the web UI, plus the **Manual Setup** walkthrough below,
> is simpler and avoids overlap. In particular, the tuner's post-start
> thread distribution applies `taskset` round-robin to the renderer's
> threads after launch, which **overrides** the per-thread pinning DRUP
> performs natively. If you set `CPU_AUDIO=8` and then run the tuner, the
> tuner will move the audio worker off CPU 8 to a different core. The
> tuner scripts are kept for users who want a one-shot system-level
> configuration on machines that aren't using the in-DRUP CPU affinity
> options, but the **Manual Setup (Alternative to Tuner Scripts)** below
> is the recommended path going forward.

For maximum audio quality, you can isolate CPU cores for the renderer using the included tuner scripts. This prevents system tasks from interrupting audio processing.

**Features:**
- Automatic CPU topology detection (AMD Ryzen, Intel Core)
- CPU isolation via kernel parameters (isolcpus, nohz_full, rcu_nocbs)
- IRQ affinity to housekeeping cores
- Real-time FIFO scheduling
- Thread distribution across isolated cores

#### Quick Start

```bash
# 1. Preview detected CPU topology (no changes)
sudo ./diretta-renderer-tuner.sh detect

# Example output for Ryzen 9 5900X:
#   Vendor:          AuthenticAMD
#   Model:           AMD Ryzen 9 5900X
#   Physical cores:  12
#   Logical CPUs:    24
#   SMT/HT:          true (2 threads/core)
#   Housekeeping:    CPUs 0,12
#   Renderer:        CPUs 1-11,13-23
```

#### Apply Tuning

Choose one of two modes:

**With SMT (Hyper-Threading enabled):**
```bash
sudo ./diretta-renderer-tuner.sh apply
# Reboot required
```

**Without SMT (physical cores only, lower latency):**
```bash
sudo ./diretta-renderer-tuner-nosmt.sh apply
# Reboot required
```

#### Check Status

```bash
sudo ./diretta-renderer-tuner.sh status
```

#### Revert Changes

```bash
sudo ./diretta-renderer-tuner.sh revert
# Reboot required
```

#### Which Mode to Choose?

| Mode | CPUs Available | Latency | Use Case |
|------|----------------|---------|----------|
| **With SMT** | All logical CPUs | Good | General use, multi-tasking |
| **Without SMT** | Physical cores only | Best | Dedicated audio machine |

For a dedicated audio server, **nosmt** mode provides more consistent latency because each core has no resource contention from SMT siblings.

#### Manual Setup (Alternative to Tuner Scripts)

If you prefer to apply isolation by hand — for instance on appliance distros
where the tuner scripts don't fit (GentooPlayer, AudioLinux), or to keep
control over the exact cmdline — the following is the minimum viable recipe.

**1. Pick the CPU you want dedicated to the Diretta worker.** This will be
the same value you'll pass to `--cpu-audio`. On a Ryzen 9 5900X with SMT
disabled, picking `8` (a CCD 1 core, away from CPU 0) is a common choice.

**2. Add this to the kernel cmdline:**

```
isolcpus=8 nohz_full=8 rcu_nocbs=8
```

- `isolcpus=8` — removes CPU 8 from the general scheduler load balancer.
- `nohz_full=8` — disables the periodic scheduler tick on CPU 8 when only
  one task is running (i.e. the Diretta worker), eliminating one more
  source of jitter.
- `rcu_nocbs=8` — moves RCU callback handling off CPU 8.

**3. Apply via your bootloader and reboot.**

For GRUB (most distros — Fedora, Ubuntu, Debian, Arch):

```bash
# Edit /etc/default/grub and append the three params to GRUB_CMDLINE_LINUX_DEFAULT
sudo nano /etc/default/grub

# Regenerate the bootloader config (pick the line for your distro):
sudo grub2-mkconfig -o /boot/grub2/grub.cfg     # Fedora/RHEL/CentOS
sudo update-grub                                # Ubuntu/Debian
sudo grub-mkconfig -o /boot/grub/grub.cfg       # Arch

sudo reboot
```

**4. Verify after reboot:**

```bash
cat /proc/cmdline                       # confirm the three params are present
cat /sys/devices/system/cpu/isolated    # should show: 8
```

**5. Tell DirettaRendererUPnP to use that core.** In the web UI (or
`/etc/default/diretta-renderer`):

```bash
CPU_AUDIO=8           # pin Diretta worker to the isolated CPU
CPU_OTHER=10,11       # decode/UPnP/position threads on neighbouring cores
IRQ_INTERFACE=enp4s0  # NIC name (whichever talks to the target)
IRQ_CPUS=0-5          # push NIC interrupts AWAY from CPU 8
```

**Caveats:**
- A typo in the cmdline can prevent boot — if that happens, edit the cmdline
  at the GRUB menu (press `e`) to recover, then fix `/etc/default/grub`.
- Don't isolate every core. The kernel still needs CPUs to run system tasks.
  On a 12-core CPU, isolating 1–2 cores is the usual sweet spot.
- `isolcpus=` only *removes* the core from the default scheduler. The core
  becomes useful for audio only once you also pin DRUP to it via
  `--cpu-audio` / `CPU_AUDIO`.

For systemd-boot setups, additional bootloader recipes, and recovery
guidance, see [docs/CONFIGURATION.md](docs/CONFIGURATION.md#3-cpu-isolation-with-isolcpus-kernel-boot-parameter).

---

## Command Line Options

### Basic Options

```bash
--name, -n <name>       Renderer name (default: Diretta Renderer)
--port, -p <port>       UPnP port (default: auto)
--target, -t <index>    Select Diretta target by index (1, 2, 3...)
--list-targets          List available Diretta targets and exit
--verbose, -v           Enable verbose debug output (log level: DEBUG)
--quiet, -q             Quiet mode - only errors and warnings (log level: WARN)
--interface <name>      Bind to specific network interface
```

### Advanced Diretta SDK Settings

These options allow fine-tuning the Diretta SDK transmission behavior. **Leave at defaults unless you have a specific reason to change them.** See [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for detailed documentation.

```bash
--thread-mode <mode>        SDK thread mode bitmask (default: 1=CRITICAL)
--cycle-time <us>           Max cycle time in microseconds (333-10000, default: auto)
--cycle-min-time <us>       Min cycle time in microseconds (random mode only)
--info-cycle <us>           Info packet cycle in microseconds (default: 100000)
--transfer-mode <mode>      Transfer mode: auto, varmax, varauto, fixauto, random
--target-profile-limit <us> Target profile limit (0=SelfProfile (stable), default: 0, >0=experimental)
--mtu <bytes>               MTU override (default: auto-detect)
```

> **Note:** These options were available in v1.3.3 and have been reintroduced with the v2.x architecture. They can also be set in `diretta-renderer.conf` for systemd service use.

### Examples

```bash
# List targets
sudo ./bin/DirettaRendererUPnP --list-targets

# Basic usage
sudo ./bin/DirettaRendererUPnP --target 1

# Custom name and port
sudo ./bin/DirettaRendererUPnP --target 1 --name "Living Room" --port 4005

# Verbose mode for troubleshooting
sudo ./bin/DirettaRendererUPnP --target 1 --verbose

# Bind to specific network interface
sudo ./bin/DirettaRendererUPnP --target 1 --interface eth0

# Advanced: Custom thread mode and transfer mode
sudo ./bin/DirettaRendererUPnP --target 1 --thread-mode 17 --transfer-mode fixauto
```

---

## Web Configuration UI

Configure the renderer from your browser — no SSH or manual file editing needed.

### Installation

```bash
# Via installer menu (option 6)
./install.sh

# Or directly via command line
./install.sh --webui
```

### Usage

Once installed, access the web UI at:
```
http://<your-ip>:8080
```

**Features:**
- Edit all renderer settings (target, port, name, gapless, verbose, network interface)
- Advanced Diretta SDK settings (thread-mode, transfer-mode, cycle-time, etc.)
- **Save & Restart** — applies settings and restarts the service in one click
- **Restart Only** — restart the service without changing settings

**Service management:**
```bash
sudo systemctl status diretta-renderer-webui
sudo systemctl stop diretta-renderer-webui
sudo systemctl restart diretta-renderer-webui
```

> **Note:** The web UI runs as a separate Python process (`diretta-renderer-webui.service`) and has zero impact on audio quality or latency.

---

## Troubleshooting

### Renderer Not Found by Control Point

```bash
# Check if renderer is running
ps aux | grep DirettaRendererUPnP

# Check firewall
sudo firewall-cmd --list-all

# Try binding to specific interface
sudo ./bin/DirettaRendererUPnP --interface eth0 --target 1
```

### No Audio Output

1. Verify Diretta Target is running and connected to DAC
2. Check network connectivity: `ping <target_ip>`
3. Run with `--verbose` to see detailed logs
4. Ensure MTU is at least 1500 bytes

### Stuttering or Dropouts

1. **Check MTU**: Ensure your network supports at least 1500 bytes end-to-end
2. **Enable jumbo frames**: Set MTU to 9000 for hi-res audio
3. **Check CPU load**: Use `htop` to ensure no CPU bottleneck
4. **Network quality**: Run `ping -c 100 <target>` to check for packet loss

### Format Change Issues

Format transitions (e.g., DSD→PCM, 44.1→96kHz) include settling delays:
- DSD→PCM: 800ms
- DSD rate change: 400ms
- PCM rate change: 200ms

This is normal and ensures clean transitions.

---

## Documentation

| Document | Description |
|----------|-------------|
| [CHANGELOG.md](CHANGELOG.md) | Version history and changes |
| [CLAUDE.md](CLAUDE.md) | Technical reference for developers |
| [docs/INSTALLATION.md](docs/INSTALLATION.md) | Step-by-step installation guide |
| [docs/SYSTEMD_GUIDE.md](docs/SYSTEMD_GUIDE.md) | Systemd service setup and hardening |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Detailed troubleshooting guide |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Configuration reference |
| [docs/FORK_CHANGES.md](docs/FORK_CHANGES.md) | Differences from v1.x |

---

## Credits

### Author
**Dominique COMET** ([@cometdom](https://github.com/cometdom)) - Original development and v2.0

### Special Thanks

- **Yu Harada** - Creator of Diretta protocol and SDK, guidance on low-level API usage

#### Key Contributors

- **SwissMountainsBear** - Ported and adapted the core Diretta integration code from his [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin). The `DIRETTA::Sync` architecture, `getNewStream()` callback implementation, same-format fast path, and buffer management patterns were directly contributed from his plugin. This project would not exist in its current form without his code contribution.

- **leeeanh** - Brilliant optimization strategies that transformed v2.0 performance. His contributions include:
  - Lock-free SPSC ring buffer design with atomic operations
  - Power-of-2 buffer sizing with bitmask modulo (10-20x faster)
  - Cache-line separation (`alignas(64)`) to eliminate false sharing
  - Consumer hot path analysis leading to zero-allocation audio path
  - AVX2 SIMD strategy for batch format conversions

#### Also Thanks To

- **FFmpeg team** - Audio decoding library
- **libupnp developers** - UPnP/DLNA implementation
- **Audiophile community** - Testing and feedback

### Third-Party Components
- [Diretta Host SDK](https://www.diretta.link) - Proprietary (personal use only)
- [FFmpeg](https://ffmpeg.org) - LGPL/GPL
- [libupnp](https://pupnp.sourceforge.io/) - BSD License

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

**IMPORTANT**: The Diretta Host SDK is proprietary software by Yu Harada and is licensed for **personal use only**. Commercial use is prohibited.

---

## Disclaimer

This software is provided "as is" without warranty. While designed for high-quality audio reproduction, results depend on your specific hardware, network configuration, Diretta Target setup, and DAC. Always test thoroughly with your own equipment.

---

**Enjoy bit-perfect, low-latency audio streaming!**

*Last updated: 2026-04-30 (v2.4.0)*
