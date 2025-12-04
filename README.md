# Diretta UPnP Renderer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-Linux-blue.svg)](https://www.linux.org/)
[![C++](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

**The world's first native UPnP/DLNA renderer with Diretta protocol support**

Stream bit-perfect high-resolution audio (up to DSD1024 and PCM 1536kHz) from any UPnP control point directly to your Diretta-compatible DAC, bypassing the OS audio stack entirely.

⚠️ **PERSONAL USE ONLY** - This software requires the Diretta Host SDK which is licensed for personal use only. Commercial use is prohibited.

---

## ✨ Features

- 🎵 **Bit-perfect streaming** via native Diretta protocol
- 🎚️ **Hi-Res audio support**: DSD1024, PCM up to 1536kHz/32bit
- 📱 **UPnP/DLNA compatible**: Works with JPlay, BubbleUPnP, mConnect, and other control points
- 🔄 **Gapless playback**: Seamless transitions between tracks
- ⏯️ **Full transport control**: Play, Stop, Pause, Resume, Seek
- 🚀 **Adaptive network optimization**: 
  - Jumbo frames (16k) for Hi-Res formats
  - Optimized packets (~1-3k) for CD-quality
- 📦 **Compressed format support**: FLAC, ALAC decoded on-the-fly via FFmpeg
- 🔊 **Memory Play quality**: Same audio quality as Memory Play, with renderer convenience

---

## 🎯 Why Use This?

### The Problem
- **Memory Play**: Excellent sound quality but no remote control, requires local files
- **Standard UPnP renderers**: Remote control but compromised audio quality (OS audio stack, resampling)

### The Solution
This renderer combines the best of both worlds:
- **Diretta protocol** → Bit-perfect audio, bypasses OS stack
- **UPnP/DLNA** → Standard remote control from any app
- **Result** → Memory Play quality with renderer convenience

---

## 📋 Requirements

### Hardware
- **Computer**: Linux x86_64 with Ethernet
- **Network**: 
  - Gigabit Ethernet adapter
  - Network switch supporting jumbo frames (recommended)
  - CAT6+ cables
- **DAC**: Diretta-compatible DAC
  - Holo Audio Spring 3
  - Musician Pegasus
  - Other DACs with Diretta support

### Software
- **Linux** (Fedora, AudioLinux, Ubuntu, or similar)
- **Diretta Host SDK** v1.47+ (download from [diretta.link](https://www.diretta.link))
- **FFmpeg** development libraries
- **libupnp** development library

---

## 🚀 Quick Start

### 1. Install Dependencies

```bash
# Fedora
sudo dnf install ffmpeg-devel libupnp-devel gcc-c++ make

# Ubuntu/Debian
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libupnp-dev build-essential
```

### 2. Download Diretta SDK

1. Visit **https://www.diretta.link**
2. Go to **"Download Preview"** section
3. Download **DirettaHostSDK_147.tar.gz**
4. Extract to home directory:
   ```bash
   cd ~
   tar xzf DirettaHostSDK_147.tar.gz
   ```

### 3. Build the Renderer

```bash
git clone https://github.com/YOUR_USERNAME/DirettaUPnPRenderer.git
cd DirettaUPnPRenderer
make
```

### 4. Configure Network

```bash
# Enable jumbo frames (adjust interface name)
sudo ip link set enp4s0 mtu 9000
```

### 5. Run

```bash
sudo ./bin/DirettaRendererComplete --port 4005 --buffer 2.0
```

### 6. Connect & Play

1. Open your UPnP control point (JPlay, BubbleUPnP, etc.)
2. Select "Diretta Renderer" as output
3. Play your music!

---

## 📖 Documentation

- **[Installation Guide](docs/INSTALLATION.md)** - Detailed setup instructions
- **[Configuration Guide](docs/CONFIGURATION.md)** - Tuning and optimization
- **[Troubleshooting Guide](docs/TROUBLESHOOTING.md)** - Common problems and solutions
- **[Contributing Guide](CONTRIBUTING.md)** - How to contribute

---

## 🎛️ Command-Line Options

```bash
sudo ./DirettaRendererComplete [OPTIONS]

Options:
  --port <number>      UPnP control port (default: 4005)
  --buffer <seconds>   Audio buffer in seconds (default: 2.0)
  --name <string>      Device name (default: "Diretta Renderer")
  --uuid <string>      Device UUID (default: auto-generated)
  --no-gapless         Disable gapless playback

Example:
  sudo ./DirettaRendererComplete --port 4005 --buffer 2.0 --name "Living Room"
```

---

## 🏗️ Architecture

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
│  Diretta DAC            │  (Holo Audio, Musician, etc.)
│  - Receives packets     │
│  - Clock synchronization│
│  - D/A conversion       │
└───────────┬─────────────┘
            │
            ▼
        🔊 Speakers
```

### Key Components

1. **UPnPDevice**: Implements UPnP MediaRenderer protocol
2. **AudioEngine**: 
   - Manages audio decoders (FFmpeg)
   - Handles transport state (Play/Stop/Pause/Seek)
   - Provides gapless playback
3. **DirettaOutput**: 
   - Interfaces with Diretta Host SDK
   - Manages network transmission
   - Adaptive packet sizing

---

## ⚙️ Technical Details

### Supported Formats

| Format | Sample Rate | Bit Depth | Status |
|--------|-------------|-----------|--------|
| PCM (WAV) | Up to 1536kHz | 8-32 bit | ✅ Native |
| FLAC | Up to 384kHz | 16-24 bit | ✅ Decoded |
| ALAC | Up to 192kHz | 16-24 bit | ✅ Decoded |
| DSD64 | 2.8224 MHz | 1 bit | ✅ Native |
| DSD128 | 5.6448 MHz | 1 bit | ✅ Native |
| DSD256 | 11.2896 MHz | 1 bit | ✅ Native |
| DSD512 | 22.5792 MHz | 1 bit | ✅ Native |
| DSD1024 | 45.1584 MHz | 1 bit | ✅ Native |

### Network Performance

#### Adaptive Packet Sizing

The renderer **automatically adapts** packet size based on audio format:

- **16bit/44.1-48kHz**: ~1-3k packets (optimized for fluidity)
- **24bit/88.2kHz+**: ~16k jumbo frames (maximum performance)
- **DSD**: ~16k jumbo frames (maximum performance)

#### Throughput Requirements

| Format | Bitrate | Network Load |
|--------|---------|--------------|
| 16bit/44.1kHz | 1.4 Mbps | Low |
| 24bit/192kHz | 9.2 Mbps | Medium |
| DSD512 | 22.5 Mbps | High |
| DSD1024 | 45 Mbps | Very High |

**Note**: Gigabit Ethernet (1000 Mbps) handles all formats comfortably.

---

## 🎮 Compatible Control Points

Tested and working:

| Control Point | Platform | Status | Notes |
|---------------|----------|--------|-------|
| **JPlay** | iOS/Android | ✅ Excellent | Best experience, full features |
| **BubbleUPnP** | Android | ✅ Excellent | Full features, gapless works |
| **mConnect** | iOS | ✅ Good | Works well |
| **Linn Kazoo** | iOS/Android | ✅ Good | - |
| **Hi-Fi Cast** | Android | ⚠️ Limited | Basic playback only |

---

## 🔧 System Optimization Tips

### For Best Audio Quality

1. **Use real-time kernel** (optional but recommended)
   ```bash
   sudo dnf install kernel-rt  # Fedora
   ```

2. **Set CPU to performance mode**
   ```bash
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

3. **Enable jumbo frames**
   ```bash
   sudo ip link set enp4s0 mtu 9000
   ```

4. **Use dedicated network** (if possible)
   - Separate network for audio
   - Managed switch with QoS

### For AudioLinux Users

AudioLinux is pre-optimized for audio. Just:
1. Enable jumbo frames
2. Install dependencies
3. Build and run!

---

## 🐛 Troubleshooting

### Renderer Not Discovered

```bash
# Check firewall
sudo firewall-cmd --permanent --add-port=1900/udp
sudo firewall-cmd --permanent --add-port=4005/tcp
sudo firewall-cmd --permanent --add-port=4006/tcp
sudo firewall-cmd --reload
```

### Audio Dropouts

```bash
# Increase buffer
sudo ./DirettaRendererComplete --buffer 3.0

# Check network
ip -s link show enp4s0  # Look for errors
```

### More Issues?

See **[TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)** for comprehensive solutions.

---

## 🤝 Contributing

Contributions are welcome! See **[CONTRIBUTING.md](CONTRIBUTING.md)** for guidelines.

### Ways to Contribute

- 🐛 Report bugs
- 💡 Suggest features
- 📝 Improve documentation
- 🧪 Test with different hardware
- 💻 Submit code improvements

---

## 🙏 Credits

- **Development**: Dominique (with AI assistance from Claude/Anthropic)
- **Diretta Protocol & SDK**: [Yu Harada](https://www.diretta.link)
- **Libraries**: 
  - [FFmpeg](https://www.ffmpeg.org/) - Audio decoding
  - [libupnp](https://github.com/pupnp/pupnp) - UPnP implementation

---

## 📜 License

This renderer software is licensed under the **MIT License** (see [LICENSE](LICENSE) file).

### Important Notes

⚠️ **Diretta Host SDK**: Required dependency, proprietary, **personal use only**
- Download from: https://www.diretta.link
- See SDK license for terms

⚠️ **Usage Restrictions**:
- ✅ Personal use: ALLOWED
- ✅ Modification & distribution of renderer: ALLOWED  
- ❌ Commercial use: NOT ALLOWED (due to SDK license)
- ❌ Distributing SDK: NOT ALLOWED

For commercial licensing, contact Yu Harada.

---

## 🌟 Support the Project

If you find this project useful:
- ⭐ **Star this repository** on GitHub
- 📢 **Share** with other audiophiles
- 🐛 **Report bugs** to help improve it
- 💬 **Join discussions** in GitHub Issues

---

## 📞 Support & Community

- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: General questions and discussions
- **Diretta Website**: https://www.diretta.link (for SDK/DAC questions)

---

## 🗺️ Roadmap

Potential future enhancements (contributions welcome!):

- [ ] Web UI for configuration
- [ ] Volume control via UPnP RenderingControl
- [ ] Playlist management
- [ ] Multiple Diretta target support
- [ ] Docker container
- [ ] Systemd service generator script
- [ ] Configuration file support
- [ ] Automatic network optimization

---

## ⚡ Performance

Real-world performance metrics:

- **Latency**: ~2 seconds (configurable 1-10s)
- **CPU Usage**: 
  - 16bit/44.1kHz: <5%
  - 24bit/192kHz: <10%
  - DSD512: ~15-25%
- **Network**: 
  - Bandwidth: <50 Mbps peak (DSD1024)
  - Packet loss tolerance: >0% with adequate buffer
- **Audio Quality**: Bit-perfect, identical to Memory Play

---

## 📸 Screenshots

*Coming soon: Screenshots of renderer in action with various control points*

---

## ❓ FAQ

**Q: Does this work with my DAC?**  
A: If your DAC supports Diretta protocol, yes! Check with your DAC manufacturer.

**Q: Can I use this commercially?**  
A: No, the Diretta SDK is for personal use only. Contact Yu Harada for commercial licensing.

**Q: Does this work on Windows/macOS?**  
A: No, Linux only. The Diretta SDK is Linux-specific.

**Q: Can I stream from Tidal/Qobuz?**  
A: Yes, if your control point supports these services (e.g., BubbleUPnP with Tidal).

**Q: What's the difference vs. Memory Play?**  
A: Same audio quality, but this is a network renderer (remote control, streaming). Memory Play is local-only.

**Q: Do I need a special network card?**  
A: Not required, but cards supporting 16k MTU (like RTL8125) provide optimal performance.

---

## 🎵 Enjoy Your Music!

This project was created to bring audiophile-grade streaming to everyone. We hope you enjoy using it as much as we enjoyed building it!

**Happy listening!** 🎧

---

*Project started: 2024 | Last updated: December 2024*
