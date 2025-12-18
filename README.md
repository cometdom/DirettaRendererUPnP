# Diretta UPnP Renderer

**The world's first native UPnP/DLNA renderer with Diretta protocol support**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

## ⚠️ IMPORTANT - PERSONAL USE ONLY

This renderer uses the **Diretta Host SDK**, which is proprietary software by Yu Harada available for **personal use only**. Commercial use is strictly prohibited. See [LICENSE](LICENSE) for details.

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
- [Documentation](#documentation)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
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

- ✅ **Bit-perfect streaming** - Bypasses OS audio stack entirely
- ✅ **Ultra-low latency** - Direct network-to-DAC path via Diretta Target
- ✅ **High-resolution support** - Up to DSD1024 and PCM 1536kHz
- ✅ **Gapless playback** - Seamless track transitions
- ✅ **UPnP/DLNA compatible** - Works with any UPnP control point
- ✅ **Network optimization** - Adaptive packet sizing with jumbo frame support

---

## Architecture

### Complete Signal Path

```
┌─────────────────────────┐
│  UPnP Control Point     │  (JPlay, BubbleUPnP, etc.)
│  (Phone/Tablet/PC)      │
└───────────┬─────────────┘
            │ UPnP/DLNA Protocol
            ▼
┌─────────────────────────┐
│  Diretta UPnP Renderer  │
│  ┌───────────────────┐  │
│  │  UPnP Device      │  │  Handles UPnP protocol
│  ├───────────────────┤  │
│  │  AudioEngine      │  │  Manages playback, FFmpeg decoding
│  ├───────────────────┤  │
│  │  DirettaOutput    │  │  Interfaces with Diretta SDK
│  └───────────────────┘  │
└───────────┬─────────────┘
            │ Diretta Protocol (UDP/Ethernet)
            │ Bit-perfect audio samples
            ▼
┌─────────────────────────┐
│     Diretta TARGET      │  
│  - Receives packets     │
│  - Clock synchronization│
│                         │
└───────────┬─────────────┘
            |
            |
            ▼
┌─────────────────────────┐
│          DAC            │  
│  - D/A conversion       │
└───────────┬─────────────┘
            │
            ▼
        🔊 Speakers
```

### Component Details

#### 1. **UPnP Control Point** (Your music player app)
- **Examples**: JPlay iOS, BubbleUPnP, mConnect, Linn Kazoo
- **Role**: Sends playback commands (Play, Stop, Pause, Seek)
- **Protocol**: UPnP/DLNA over HTTP

#### 2. **Diretta Renderer** (This application)
- **Components**:
  - `UPnPDevice`: Handles UPnP protocol and device discovery
  - `AudioEngine`: Decodes audio files (FLAC, WAV, ALAC, etc.) using FFmpeg
  - `DirettaOutput`: Interfaces with Diretta Host SDK
- **Role**: 
  - Receives UPnP commands from control point
  - Streams and decodes audio files from media server
  - Sends decoded PCM/DSD to Diretta Target via Diretta protocol
- **Network**: Uses Ethernet with jumbo frames (up to 16k MTU)

#### 3. **Diretta Target** (Endpoint software or hardware)
- **Examples**: 
  - **Software**: Memory Play (endpoint mode), GentooPlayer, Diretta Target PC
  - **Hardware**: Some DACs with native Diretta support (rare)
- **Role**: 
  - Receives Diretta audio stream
  - Outputs to DAC via USB, I2S, or SPDIF
  - Handles timing and synchronization
- **Location**: Usually runs on a separate computer/device connected to the DAC

#### 4. **DAC** (Digital-to-Analog Converter)
- **Examples**: Holo Audio Spring 3, May KTE, Rockna, T+A DAC 200
- **Role**: Converts digital audio to analog signal
- **Connection**: USB, I2S, SPDIF, or AES from Diretta Target

---

### Why This Architecture?

**Traditional Renderer → DAC:**
```
Renderer → OS Audio Stack → USB Driver → DAC
           ↑ Adds latency, jitter, potential quality loss
```

**Diretta Renderer → Target → DAC:**
```
Renderer → Ethernet (Diretta) → Target → DAC
           ↑ Bypasses OS audio stack
           ↑ Bit-perfect transmission
           ↑ Ultra-low latency
```

The **Diretta Target** acts as a dedicated audio endpoint that receives the pristine digital stream and outputs it directly to your DAC, completely bypassing the OS audio subsystem.

---

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
- **Adaptive packet sizing**:
  - CD-quality (16/44.1): ~1-3k packets (prevents fragmentation)
  - Hi-Res/DSD: ~16k packets (maximizes throughput)
- **Jumbo frame support**: Up to 16k MTU for maximum performance
- **Network interface detection**: Automatic MTU configuration
- **Buffer management**: Configurable buffer size (1-5 seconds)

### Diretta Integration
- **Native SDK integration**: Uses Diretta Host SDK 147
- **Automatic target discovery**: Finds Diretta Target endpoints on network
- **Format negotiation**: Automatic format compatibility checking
- **Connection management**: Robust error handling and reconnection

---

## Requirements

## Supported Architectures

The renderer automatically detects and optimizes for your CPU:

- **x64** (Intel/AMD): v2 (baseline), v3 (AVX2), v4 (AVX512), zen4 (AMD Ryzen 7000+)
- **ARM64**: Raspberry Pi 4+, Apple Silicon
- **RISC-V**: Experimental support

Simply run `make` - the Makefile will select the optimal library for your hardware!

### Custom Build Options
```bash
make VARIANT=15v4      # Force AVX512 (x64)
make VARIANT=15zen4    # AMD Zen 4 optimized
make NOLOG=1           # Production build (no debug logs)
make list-variants     # Show all available options
```
## Platform Support

### Officially Supported ✅
- **Linux x64** (Fedora, Ubuntu, Arch, AudioLinux)
- **Linux ARM64** (Raspberry Pi 4/5)

### Not Supported ❌
- **Windows**: No native Windows version planned
- **macOS**: Not tested, may work with modifications

### Why Linux only?
This is a personal project maintained by one developer in their free time. 
Supporting multiple platforms would require significant additional effort 
for development, testing, and user support.

**Community contributions welcome**, but Windows/macOS support is not a priority.

### Hardware
- **Minimum**: Dual-core CPU, 1GB RAM, Gigabit Ethernet
- **Recommended**: Quad-core CPU, 2GB RAM, 2.5/10G Ethernet with jumbo frames
- **Network**: Gigabit Ethernet (10G recommended for DSD512+)
- **Diretta Target**: Separate device/computer running Diretta Target software or hardware
- **DAC**: Any DAC supported by your Diretta Target (USB, I2S, SPDIF)

### Software
- **OS**: Linux (Fedora, Ubuntu, Arch, or AudioLinux recommended)
- **Kernel**: Linux kernel 5.x+ (RT kernel recommended for optimal performance)
- **Diretta Host SDK**: Version 147 (download from [diretta.link](https://www.diretta.link/hostsdk.html))
- **Libraries**: FFmpeg, libupnp, pthread

### Network
- **MTU**: 9000 bytes recommended (jumbo frames)
- **Switch**: Managed switch with jumbo frame support
- **Latency**: <1ms on local network

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

1. Visit [diretta.link](https://www.diretta.link/hostsdk.htmlk)
2. Navigate to "Download Preview" section
3. Download **DirettaHostSDK_147** (or latest version)
4. Extract to one of these locations:
   - `~/DirettaHostSDK_147`
   - `./DirettaHostSDK_147`
   - `/opt/DirettaHostSDK_147`

### 3. Clone and Build

```bash
# Clone repository
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP

# Build (Makefile auto-detects SDK location)
make

# Upgrade from a previous version:
If you have a previous version installed
cd DirettaRendererUPnP
git pull
make clean
make  or make NOLOG=1 if you want production version (better SQ has been reported)

# Install service
cd systemd
chmod +x install-systemd.sh
sudo ./install-systemd.sh

#Next steps:
 1. Edit configuration (optional):
     sudo nano /opt/diretta-renderer-upnp/diretta-renderer.conf
 2. Reload daemon:
     sudo systemctl daemon-reload
 3. Enable the service:
     sudo systemctl enable diretta-renderer
 4. Start the service:
     sudo systemctl start diretta-renderer
 5. Check status:
     sudo systemctl status diretta-renderer 
 6. View logs:
     sudo journalctl -u diretta-renderer -f
 7. Stop the service:
     sudo systemctl stop diretta-renderer
 8. Disable auto-start:
     sudo systemctl disable diretta-renderer       
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
sudo ./bin/DirettaRendererUPnP --port 4005 --buffer 2.0
```

### 6. Configure Your Diretta Target

Ensure your **Diretta Target** (Memory Play, GentooPlayer, etc.) is:
- Running on your network
- Connected to your DAC (USB/I2S/SPDIF)
- Configured to accept Diretta connections

### List Diretta Targets
Before starting the renderer, you can scan for Diretta target devices on the current network:

```bash
sudo ./bin/DirettaRendererUPnP --list-targets
```

Example output:

text
[1] Target #1
    IP Address: fe80::5c53:8aff:fefb:f63a,19644
    MTU: 1500 bytes

[2] Target #2
    IP Address: fe80::5c53:8aff:fefb:f63a,19646
    MTU: 1500 bytes

[3] Target #3
    IP Address: fe80::5c53:8aff:fefb:f63a,19648
    MTU: 1500 bytes
Where:
[1] / [2] / [3] are internal Target indices.

IP Address and MTU can be used to distinguish between different Diretta devices. 
**To be improved:** Currently cannot output user-friendly target names.

### Select Diretta Target

Based on the output of `--list-targets`, you can select a specific Diretta Target by its index.

Syntax:

```bash
sudo ./bin/DirettaRendererUPnP --target <index> [other_parameters]
```

Examples:

Use the first Target:

```bash
sudo ./bin/DirettaRendererUPnP --target 1
```

Use with port, buffer, and other parameters:

```bash
sudo ./bin/DirettaRendererUPnP --target 2 --port 4005 --buffer 2.0
```
Notes:
<index> starts from 1, corresponding to the [1] [2] [3] ... shown in the --list-targets output.
If --target is not specified and there is only one Diretta Target detected on the network, it will be used automatically.


### 7. Connect from Control Point

Open your UPnP control point (JPlay, BubbleUPnP, etc.) and look for "Diretta Renderer" in available devices.

---

## Supported Formats

| Format Type | Bit Depth | Sample Rates | Container | Notes |
|-------------|-----------|--------------|-----------|-------|
| **PCM** | 16/24/32-bit | 44.1kHz - 1536kHz | FLAC, ALAC, WAV, AIFF | Uncompressed or lossless compressed |
| **DSD** | 1-bit | DSD64 - DSD1024 | DSF, DFF | Native DSD support |
| **Lossy** | Variable | Up to 192kHz | MP3, AAC, OGG | Transcoded to PCM |

### Protocol Info (UPnP)

The renderer advertises the following formats via UPnP ProtocolInfo:
- `audio/flac` - FLAC files
- `audio/x-flac` - Alternative FLAC MIME type
- `audio/wav` - WAV files
- `audio/x-wav` - Alternative WAV MIME type
- `audio/L16` - Raw PCM
- `audio/mp3` - MP3 files
- `audio/mpeg` - MPEG audio
- And many more...

---

## Performance

### Network Performance

Adaptive packet sizing optimizes network usage based on audio format:

| Format | Sample Rate | Bit Depth | Packet Size | Bandwidth | Strategy |
|--------|-------------|-----------|-------------|-----------|----------|
| CD | 44.1kHz | 16-bit | 1-3KB | ~172 KB/s | Small packets (no fragmentation) |
| Hi-Res | 96kHz | 24-bit | Up to 16KB | ~690 KB/s | Jumbo frames (max throughput) |
| Hi-Res | 192kHz | 24-bit | Up to 16KB | ~1.4 MB/s | Jumbo frames |
| DSD64 | 2.8MHz | 1-bit | Up to 16KB | ~345 KB/s | Jumbo frames |
| DSD256 | 11.2MHz | 1-bit | Up to 16KB | ~1.4 MB/s | Jumbo frames |

### Buffer Settings

| Buffer Size | Latency | Stability | Use Case |
|-------------|---------|-----------|----------|
| 1.0s | Low | Good | Local network, CD-quality |
| 2.0s | Medium | Better | **Recommended default** |
| 3.0s | Medium-High | Best | Hi-Res, problematic networks |
| 4.0s+ | High | Maximum | DSD512+, maximum stability |

---

## Compatible Control Points

Tested and working with:

| Control Point | Platform | Rating | Notes |
|---------------|----------|--------|-------|
| **JPlay iOS** | iOS | ⭐⭐⭐⭐⭐ | Excellent, full feature support |
| **BubbleUPnP** | Android | ⭐⭐⭐⭐⭐ | Excellent, highly configurable |
| **mConnect** | iOS/Android | ⭐⭐⭐⭐ | Very good, clean interface |
| **Linn Kazoo** | iOS/Android | ⭐⭐⭐⭐ | Good, designed for Linn systems but needs Openhome (BubbleUPnP sever)
| **gerbera** | Web | ⭐⭐⭐ | Basic functionality |

### Recommended Settings
## JPLAY iOS
- **Force STOP before playback**: if you have problem switching from a track to another or album to another. You don't need it if switching is working fine.
- **Gapless**: On (if desired)

## AUDIRVANA
- **Universal gapless enabled**: is you notice pink noise after few seconds of playback
- **DSD**: DSD isn’t functioning properly, as the DAC plays PCM instead (e.g., DSD64 → PCM 352.4 kHz). So you can set NO DSD.

---

## System Optimization

### CPU Governor
```bash
# Performance mode for best audio quality
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Real-Time Priority
```bash
# Allow real-time scheduling
sudo setcap cap_sys_nice+ep ./bin/DirettaRendererUPnP
```

### Network Tuning
```bash
# Increase network buffers
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
```

### AudioLinux
If using AudioLinux distribution:
- RT kernel is pre-configured
- Network optimizations applied
- CPU governor set to performance
- Just configure jumbo frames and run!

---

## Documentation

- **[Installation Guide](docs/INSTALLATION.md)**: Detailed setup instructions
- **[Configuration Guide](docs/CONFIGURATION.md)**: All options and tuning
- **[Troubleshooting Guide](docs/TROUBLESHOOTING.md)**: Common issues and solutions
- **[Contributing Guidelines](CONTRIBUTING.md)**: How to contribute
- **[Changelog](CHANGELOG.md)**: Version history

---

## Troubleshooting

### Renderer Not Found
```bash
# Check if running
ps aux | grep DirettaRendererUPnP

# Check network
ip addr show

# Check firewall
sudo firewall-cmd --list-all
```

### No Audio Output
1. **Verify Diretta Target is running** and connected to DAC
2. **Check Diretta Target can see the renderer** (check Target's interface/logs)
3. **Test with Memory Play first** to ensure Diretta setup is correct
4. Check network connectivity between renderer and Diretta Target
5. Check buffer size: Try increasing to 3-4 seconds

### Audio Dropouts
```bash
# Increase buffer
./bin/DirettaRendererUPnP --buffer 3.0

# Check network
ping <DIRETTA_TARGET_IP>
iperf3 -c <DIRETTA_TARGET_IP>
```

### Connection Failed (0x0 Error)
This means the Diretta Target is refusing the connection:
1. **Ensure Diretta Target is running** and not in use by another application
2. **Check Target accepts your audio format** (16-bit, 24-bit, etc.)
3. **Test with Memory Play** sending to the same Target
4. Some Targets (like GentooPlayer) may require 24-bit minimum - see [Issue Tracker](https://github.com/cometdom/DirettaRendererUPnP/issues)

For more solutions, see the [Troubleshooting Guide](docs/TROUBLESHOOTING.md).

---

## FAQ

### Q: Do I need a DAC with Diretta support?
**A:** No! You need a **Diretta Target** endpoint (software like Memory Play or GentooPlayer), which then connects to any DAC via USB/I2S/SPDIF.

### Q: What's the difference between Diretta Host and Diretta Target?
**A:** 
- **Diretta Host** (this renderer): Sends audio over network via Diretta protocol
- **Diretta Target** (separate software/hardware): Receives Diretta audio and outputs to DAC

### Q: Can I use this without a Diretta Target?
**A:** No, you need a Diretta Target endpoint. The most common solution is to install **Memory Play** in endpoint/target mode on a computer connected to your DAC.

### Q: Does this work with Roon?
**A:** No, this is a UPnP/DLNA renderer. For Roon, you would need a Roon Bridge, not this renderer.

### Q: Why do I need jumbo frames?
**A:** Jumbo frames significantly reduce CPU overhead and improve timing precision for high-resolution audio (96kHz+) and DSD. They're optional but highly recommended.

### Q: What's better than a regular UPnP renderer?
**A:** This renderer bypasses the OS audio stack by using the Diretta protocol. The audio goes directly from network to your Diretta Target to DAC, maintaining bit-perfect quality.

### Q: Will there be a Windows version?
**A:** No, Windows support is not planned. This is a one-person project and 
I prefer to focus on making the Linux version excellent. 

You can use **WSL2** to run the renderer on Windows, or consider dual-booting Linux.

### Q: Can I pay for Windows support?
**A:** Even with funding, I don't have the time to properly support Windows. 
If you're interested in a Windows port, consider hiring a developer to fork 
the project.

### Q: Why is this free if it costs you time/money?
**A:** I built this for myself and the audiophile community. It's a passion 
project, not a business. However, if you find it valuable, see the "Support" 
section below.
---

## Roadmap

- [ ] Volume control support (RenderingControl service)
- [ ] Playlist support
- [ ] Web UI for configuration
- [ ] Raspberry Pi optimizations
- [ ] Docker container
- [ ] Metadata display improvements
- [ ] Multi-room synchronization (if Diretta SDK adds support)

---

## ❤️ Support This Project

If you find this renderer valuable, you can support development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cometdom)

**Important notes:**
- ✅ Donations are **optional** and appreciated
- ✅ Help cover test equipment and coffee ☕
- ❌ **No guarantees** for features, support, or timelines
- ❌ The project remains free and open source for everyone

This is a hobby project - donations support development but don't create obligations.

Thank you! 🎵

---
## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for:
- Code style guidelines
- How to submit pull requests
- Bug report templates
- Feature request process

---

## Credits

### Author
**Dominique** - Initial development and ongoing maintenance

### Special Thanks
- **Yu Harada** - Creator of Diretta protocol and SDK
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

**IMPORTANT**: The Diretta Host SDK is proprietary software by Yu Harada and is licensed for **personal use only**. Commercial use is prohibited. See LICENSE file for full details.

---

## Support

- **Issues**: [GitHub Issues](https://github.com/cometdom/DirettaRendererUPnP/issues)
- **Discussions**: [GitHub Discussions](https://github.com/cometdom/DirettaRendererUPnP/discussions)
- **Diretta Protocol**: [diretta.link](https://www.diretta.link)

---

## Disclaimer

This software is provided "as is" without warranty. While designed for high-quality audio reproduction, results depend on your specific hardware, network configuration, Diretta Target setup, and DAC. Always test thoroughly with your own equipment.

---

**Enjoy bit-perfect, high-resolution audio streaming! 🎵**

*Last updated: 2025-12-18*
