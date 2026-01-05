## README.md

# Diretta UPnP Renderer (Community Fork)

> 🍴 **This is a community fork of [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) by Dominique COMET**

**A native UPnP/DLNA renderer with Diretta protocol support**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)
[![No Support](https://img.shields.io/badge/Support-None-red.svg)]()

---

## ⚠️ IMPORTANT DISCLAIMERS

### No Support Provided

**This software is provided "AS IS", without warranty of any kind.**

| | |
|---|---|
| ❌ **No support** | I maintain this fork in my spare time for personal use |
| ❌ **No guarantees** | Features may break, updates are not guaranteed |
| ❌ **No liability** | Use at your own risk |
| ❌ **No response guarantee** | Issues and PRs may not receive responses |
| ✅ **Community contributions** | Welcome, but no promise to review or merge |

**If you need supported software, consider the [original project](https://github.com/cometdom/DirettaRendererUPnP) or commercial alternatives.**

### Personal Use Only

This renderer uses the **Diretta Host SDK**, which is proprietary software by Yu Harada available for **personal use only**. Commercial use is strictly prohibited. See [LICENSE](LICENSE) for details.

---

## 🍴 About This Fork

This is a community-maintained fork of the original DirettaRendererUPnP project.

| | |
|---|---|
| **Original Project** | [github.com/cometdom/DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) |
| **Original Author** | Dominique COMET |
| **Fork Maintainer** | [@SwissMontainsBear](https://github.com/SwissMontainsBear) |
| **Purpose** | Personal use, shared with community as courtesy |

### Differences from Original

Please see FORK_CHANGES.md
---

## 📖 Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Supported Formats](#supported-formats)
- [Performance](#performance)
- [Compatible Control Points](#compatible-control-points)
- [System Optimization](#system-optimization)
- [Command Line Options](#command-line-options)
- [Advanced Settings](#advanced-settings)
- [Multi-Homed Systems](#multi-homed-systems--network-interface-selection)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)
- [Disclaimer](#disclaimer)

---

## Overview

This is a **native UPnP/DLNA renderer** that streams high-resolution audio using the **Diretta protocol** for bit-perfect playback. Unlike software-based solutions that go through the OS audio stack, this renderer sends audio directly to a **Diretta Target endpoint** (such as Memory Play, GentooPlayer, or hardware with Diretta support), which then connects to your DAC.

### What is Diretta?

Diretta is a proprietary audio streaming protocol developed by Yu Harada that enables ultra-low latency, bit-perfect audio transmission over Ethernet. The protocol uses two components:

- **Diretta Host**: Sends audio data (this renderer uses the Diretta Host SDK)
- **Diretta Target**: Receives audio data and outputs to DAC (e.g., Memory Play, GentooPlayer, or DACs with native Diretta support)

### Key Benefits

- ✅ **Bit-perfect streaming** - Bypasses OS audio stack entirely
- ✅ **Ultra-low latency** - Direct network-to-DAC path via Diretta Target
- ✅ **High-resolution support** - Up to DSD1024 and PCM 1536kHz
- ✅ **Gapless playback** - Seamless track transitions
- ✅ **UPnP/DLNA compatible** - Works with any UPnP control point
- ✅ **Network optimization** - Adaptive packet sizing with jumbo frame support



## Features

### Audio Quality
- **Bit-perfect streaming**: No resampling or processing
- **High-resolution support**:
  - PCM: Up to 32-bit/1536kHz
  - DSD: DSD64, DSD128, DSD256, DSD512, DSD1024
- **Format support**: FLAC, ALAC, WAV, AIFF, MP3, AAC, OGG
- **Gapless playback**: Seamless album listening experience

### UPnP/DLNA Features
- **Full transport control**: Play, Stop, Pause, Resume, Seek
- **Device discovery**: SSDP advertisement for automatic detection
- **Dynamic protocol info**: Exposes all supported formats to control points
- **Position tracking**: Real-time playback position updates

### Network Optimization
- **Adaptive packet sizing**: Optimized for different audio formats
- **Jumbo frame support**: Up to 16k MTU for maximum performance
- **Network interface detection**: Automatic MTU configuration

---

## Requirements

### Supported Architectures

- **x64** (Intel/AMD): v2 (baseline), v3 (AVX2), v4 (AVX512), zen4 (AMD Ryzen 7000+)
- **ARM64**: Raspberry Pi 4+
- **RISC-V**: Experimental support

### Platform Support

| Platform | Status |
|----------|--------|
| **Linux x64** | ✅ Supported |
| **Linux ARM64** | ✅ Supported |
| **Windows** | ❌ Not supported at this stage |
| **macOS** | ❌ Not supported |

### Hardware
- **Minimum**: Dual-core CPU, 1GB RAM, Gigabit Ethernet
- **Recommended**: Quad-core CPU, 2GB RAM, 2.5/10G Ethernet with jumbo frames
- **Diretta Target**: Separate device/computer running Diretta Target software
- **DAC**: Any DAC supported by your Diretta Target

### Software
- **OS**: Linux (Fedora, Ubuntu, Arch, or AudioLinux recommended)
- **Kernel**: Linux kernel 5.x+ (RT kernel recommended)
- **Diretta Host SDK**: Version 147 (download from [diretta.link](https://www.diretta.link/hostsdk.html))
- **Libraries**: FFmpeg, libupnp, pthread

---

## Quick Start

### 1. Install Dependencies

**Fedora:**
```bash
sudo dnf install -y gcc-c++ make ffmpeg-free-devel libupnp-devel
```

**Ubuntu/Debian:**

```bash
sudo apt install -y build-essential libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libupnp-dev
```

**Arch Linux:**

```bash
sudo pacman -S base-devel ffmpeg libupnp
```

### 2. Download Diretta Host SDK

1. Visit [diretta.link](https://www.diretta.link/hostsdk.html)
2. Navigate to "Download Preview" section
3. Download **DirettaHostSDK_147** (or latest version)
4. Extract to `~/DirettaHostSDK_147`

### 3. Clone and Build

```bash
# Clone repository
git clone https://github.com/SwissMontainsBear/YOUR-REPO-NAME.git
cd YOUR-REPO-NAME

# Build
make

# Or for production (no debug logs)
make NOLOG=1
```

### 4. Configure Network

Enable jumbo frames:

```bash
# Temporary (until reboot)
sudo ip link set enp4s0 mtu 9000

# Permanent (NetworkManager)
sudo nmcli connection modify "Your Connection" 802-3-ethernet.mtu 9000
sudo nmcli connection up "Your Connection"
```

### 5. Run

```bash
sudo ./bin/DirettaRendererUPnP --port 4005
```

### 6. List and Select Diretta Targets

```bash
# List available targets
sudo ./bin/DirettaRendererUPnP --list-targets

# Run with specific target
sudo ./bin/DirettaRendererUPnP --target 1 --port 4005
```

### 7. Connect from Control Point

Open your UPnP control point (JPlay, BubbleUPnP, etc.) and look for "Diretta Renderer" in available devices.

---

## Supported Formats

| Format Type | Bit Depth    | Sample Rates      | Container             |
| ----------- | ------------ | ----------------- | --------------------- |
| **PCM**     | 16/24/32-bit | 44.1kHz - 1536kHz | FLAC, ALAC, WAV, AIFF |
| **DSD**     | 1-bit        | DSD64 - DSD1024   | DSF, DFF              |
| **Lossy**   | Variable     | Up to 192kHz      | MP3, AAC, OGG         |

---

## Performance

## Compatible Control Points

| Control Point  | Platform    | Rating |
| -------------- | ----------- | ------ |
| **JPlay iOS**  | iOS         | ⭐⭐⭐⭐⭐ |
| **BubbleUPnP** | Android     | Not tested  |
| **mConnect**   | iOS/Android | ⭐⭐⭐⭐⭐ |
| **Linn Kazoo** | iOS/Android | Not tested  |

---

## Command-Line Options

### Basic Options

```bash
--name, -n <name>       Renderer name (default: Diretta Renderer)
--port, -p <port>       UPnP port (default: auto)
--target, -t <index>    Select Diretta target by index (1, 2, 3...)
--no-gapless            Disable gapless playback
--verbose               Enable verbose debug output
```

### Network Options

```bash
--interface <name>     Bind to specific network interface (e.g., eth0)
--bind-ip <address>    Bind to specific IP address
--mtu <bytes>          Force specific MTU
```

### Advanced Diretta SDK Options

```bash
--thread-mode <value>   Real-time thread behavior (bitmask)
--cycle-time <µs>       Transfer packet cycle max time (default: 10000)
--cycle-min-time <µs>   Transfer packet cycle min time (default: 333)
--info-cycle <µs>       Information packet cycle time (default: 5000)
```

---

## System Optimization

### CPU Governor

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Real-Time Priority

```bash
sudo setcap cap_sys_nice+ep ./bin/DirettaRendererUPnP
```

### Network Tuning

```bash
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
```

---

## Troubleshooting

> ⚠️ **Reminder:** No support is provided. These are common solutions that may or may not work for your setup.

### Renderer Not Found

```bash
# Check if running
ps aux | grep DirettaRendererUPnP

# Check firewall
sudo firewall-cmd --list-all
```

### No Audio Output

1. Verify Diretta Target is running
2. Check network connectivity


---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Note:** Contributions are welcome but there is no guarantee they will be reviewed or merged.

---

## Credits

### Original Author

**Dominique COMET** ([@cometdom](https://github.com/cometdom)) - Original development

### Fork Maintainer

**SwissMontainsBear** ([@SwissMontainsBear](https://github.com/SwissMontainsBear))

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

**THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND.**

The maintainer ([@SwissMontainsBear](https://github.com/SwissMontainsBear)):

- **Does NOT provide technical support**
- **Does NOT guarantee updates, bug fixes, or maintenance**
- **Does NOT accept liability** for any issues arising from use of this software
- **Does NOT guarantee responses** to issues or pull requests
- Shares this code **purely as a courtesy** to the community

**Use this software entirely at your own risk.**

The maintainer is not responsible for any:

- Hardware damage
- Data loss
- Audio equipment damage
- Any other issues that may arise

For questions about the Diretta protocol, contact [diretta.link](https://www.diretta.link).

---

**Enjoy bit-perfect audio streaming! 🎵**

*This fork is provided as-is for the audiophile community.*
