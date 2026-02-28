# Diretta UPnP Renderer v2.0.6

**The world's first native UPnP/DLNA renderer with Diretta protocol support - Low-Latency Edition**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

![Version](https://img.shields.io/badge/version-2.0.6-blue.svg)
![Low Latency](https://img.shields.io/badge/Latency-Low-green.svg)
![SDK](https://img.shields.io/badge/SDK-DIRETTA::Sync-orange.svg)
![Audirvana](https://img.shields.io/badge/Audirvana-Compatible-green.svg)

---

## What's New in v2.0.6

**Stability & compatibility fixes.**

- **Stop action fix** (by herisson-88) — uses `stopPlayback()` instead of `close()` on UPnP Stop, keeping the SDK connection open for faster resume and preventing white noise on hi-res transitions (Holo Red and similar targets)
- **libupnp auto-detection** — Makefile uses `pkg-config` to detect libupnp include paths, fixing compilation on GentooPlayer and other non-standard distributions

See [CHANGELOG.md](CHANGELOG.md) for details.

### Previous Versions

| Version | Highlights |
|---------|-----------|
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

### From v2.0.4 to v2.0.6

No configuration changes needed. Just rebuild and reinstall:

```bash
# 1. Stop the service
sudo systemctl stop diretta-renderer

# 2. Pull the latest version and rebuild
cd ~/DirettaRendererUPnP
git pull


# 3. Re-run the installer (reinstalls binary + systemd service)
./install.sh

# 4. Restart the service
sudo systemctl start diretta-renderer
```

> **Note:** Your existing configuration (`diretta-renderer.conf`) is preserved.

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
--user, -u <name>       Drop privileges to user after network init
--interface <name>      Bind to specific network interface
```

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
```

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

*Last updated: 2026-02-28 (v2.0.6)*
