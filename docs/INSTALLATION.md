# Installation Guide - Diretta UPnP Renderer

Complete step-by-step installation guide for the Diretta UPnP Renderer.

## Table of Contents

1. [System Requirements](#system-requirements)
2. [Preparing Your System](#preparing-your-system)
3. [Installing Dependencies](#installing-dependencies)
4. [Downloading Diretta SDK](#downloading-diretta-sdk)
5. [Building the Renderer](#building-the-renderer)
6. [Network Configuration](#network-configuration)
7. [First Run](#first-run)
8. [Creating a Systemd Service](#creating-a-systemd-service)

---

## System Requirements

### Minimum Hardware
- **CPU**: x86_64 processor (Intel/AMD)
- **RAM**: 2 GB minimum, 4 GB recommended
- **Network**: Gigabit Ethernet with jumbo frame support
- **Storage**: 100 MB for software + space for music files

### Recommended Hardware
- **CPU**: Modern multi-core processor for Hi-Res decoding
- **RAM**: 8 GB for optimal performance
- **Network**: 
  - Network card with RTL8125 chipset (or similar) supporting 16k MTU
  - Managed switch with jumbo frame support
  - Low-latency network infrastructure

### Compatible DACs
Any Diretta-compatible DAC, including:
- Holo Audio Spring 3
- Musician Pegasus
- Other DACs with Diretta protocol support

### Supported Operating Systems
- **Fedora** 38+ (tested)
- **AudioLinux** (tested, recommended for audiophiles)
- **Ubuntu/Debian** 22.04+
- **Arch Linux** (with manual dependency management)
- Other Linux distributions (may require adaptation)

---

## Preparing Your System

### 1. Update Your System

```bash
# Fedora/RHEL
sudo dnf update -y

# Debian/Ubuntu
sudo apt update && sudo apt upgrade -y

# Arch/AudioLinux
sudo pacman -Syu
```

### 2. Check Network Interface

```bash
# List network interfaces
ip link show

# Check current MTU (should show 1500 by default)
ip link show enp4s0 | grep mtu

# Note your interface name (e.g., enp4s0, eth0, etc.)
```

### 3. Install Build Tools

```bash
# Fedora/RHEL
sudo dnf groupinstall "Development Tools" -y

# Debian/Ubuntu
sudo apt install build-essential -y

# Arch/AudioLinux
sudo pacman -S base-devel
```

---

## Installing Dependencies

### Fedora / RHEL / CentOS

```bash
# Install FFmpeg libraries
sudo dnf install -y \
    ffmpeg-devel \
    libavformat-devel \
    libavcodec-devel \
    libavutil-devel \
    libswresample-devel

# Install UPnP library
sudo dnf install -y libupnp-devel

# Install additional tools
sudo dnf install -y git wget
```

### Debian / Ubuntu

```bash
# Install FFmpeg libraries
sudo apt install -y \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswresample-dev

# Install UPnP library
sudo apt install -y libupnp-dev

# Install additional tools
sudo apt install -y git wget
```

### Arch Linux / AudioLinux

```bash
# Install FFmpeg (usually pre-installed on AudioLinux)
sudo pacman -S ffmpeg

# Install UPnP library
sudo pacman -S libupnp

# Install git if needed
sudo pacman -S git
```

### Verify Installation

```bash
# Check FFmpeg libraries
pkg-config --modversion libavformat libavcodec libavutil libswresample

# Check UPnP library
pkg-config --modversion libupnp

# Should show version numbers for all
```

---

## Downloading Diretta SDK

### 1. Get the SDK

1. **Visit**: https://www.diretta.link/hostsdk.html
2. **Navigate to**: "Download Preview" section
3. **Download**: DirettaHostSDK_147.tar.gz (or latest version)

### 2. Extract the SDK

```bash

# Extract SDK to home directory
cd ~
tar xzf ~/Downloads/DirettaHostSDK_147.tar.gz

# Verify extraction
ls -la ~/DirettaHostSDK_147/
# Should show: Host/, lib/, include/, etc.
```

### 3. Set SDK Environment Variable (Optional not needed)

```bash
# Add to ~/.bashrc for convenience
echo 'export DIRETTA_SDK_PATH=~/DirettaHostSDK_147' >> ~/.bashrc
source ~/.bashrc
```

---

## Building the Renderer

### 1. Clone the Repository

```bash
cd ~/audio-projects
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP
```

### 2. Build

```bash
# Clean build (recommended for first time)
make clean
make

# You should see:
# Compiling main.cpp...
# Compiling DirettaRenderer.cpp...
# ...
# ✓ Build complete: bin/DirettaRendererUPnP
```

### 3. Verify Binary

```bash
ls -lh bin/DirettaRendererUPnP
# Should show the executable

# Check dependencies
ldd bin/DirettaRendererUPnP
# Should NOT show "not found" errors
```

---

## Network Configuration

### 1. Enable Jumbo Frames

```bash
# Temporary (lost after reboot)
sudo ip link set enp4s0 mtu 9000

# Verify
ip link show enp4s0 | grep mtu
# Should show: mtu 9000
```

### 2. Make Jumbo Frames Permanent

#### Method A: NetworkManager (Fedora/Ubuntu Desktop)

```bash
# Get connection name
nmcli connection show

# Set MTU
sudo nmcli connection modify "Wired connection 1" 802-3-ethernet.mtu 9000

# Restart connection
sudo nmcli connection down "Wired connection 1"
sudo nmcli connection up "Wired connection 1"
```

#### Method B: systemd-networkd

Create `/etc/systemd/network/10-ethernet.network`:

```ini
[Match]
Name=enp4s0

[Network]
DHCP=yes

[Link]
MTUBytes=9000
```

Then restart:
```bash
sudo systemctl restart systemd-networkd
```

#### Method C: /etc/network/interfaces (Debian)

Edit `/etc/network/interfaces`:

```
auto enp4s0
iface enp4s0 inet dhcp
    mtu 9000
```

### 3. Configure Your Network Switch

**Important**: Your network switch MUST support jumbo frames!

- Enable jumbo frames in switch management interface
- Typical setting: MTU 9000 or 9216
- Verify all devices in the path support jumbo frames

### 4. Test Network Performance

```bash
# Install iperf3 for testing
sudo dnf install iperf3  # or apt install iperf3

# On DAC computer (if accessible):
iperf3 -s

# On renderer computer:
iperf3 -c <DAC_IP_ADDRESS>
# Should show high throughput (900+ Mbps on Gigabit)
```

---

## First Run

### 1. Check Permissions

```bash
# The renderer needs root for raw network access
# Verify you can run with sudo
sudo -v
```

### 2. Start the Renderer

```bash
cd ~/audio-projects/DirettaUPnPRenderer/bin
sudo ./DirettaRendererUPnP --port 4005 --buffer 2.0
```

### 3. Expected Output

You should see:
```
═══════════════════════════════════════════════════════
  🎵 Diretta UPnP Renderer - Complete Edition
═══════════════════════════════════════════════════════

Configuration:
  Name:        Diretta Renderer
  Port:        4005
  Gapless:     enabled
  Buffer:      2 seconds

[DirettaRenderer] Created
🚀 Starting renderer...
[UPnP Thread] Started
[Audio Thread] Started
✓ Renderer started successfully!

📡 Waiting for UPnP control points...
```

### 4. Test Discovery

From your phone/tablet with JPlay or BubbleUPnP:
1. Open the app
2. Look for "Diretta Renderer" in available devices
3. Select it as output
4. Try playing a track

### 5. Stop the Renderer

Press `Ctrl+C` to stop gracefully.

---

## Creating a Systemd Service

To run the renderer automatically at boot:

### 1. Create Service File

```bash
sudo nano /etc/systemd/system/diretta-renderer.service
```

Content:
```ini
[Unit]
Description=Diretta UPnP Renderer
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/YOUR_USERNAME/DirettaUPnPRenderer/bin
ExecStart=/home/YOUR_USERNAME/DirettaUPnPRenderer/bin/DirettaRendererUPnP --port 4005 --buffer 2.0
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

# Network capabilities
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN

[Install]
WantedBy=multi-user.target
```

**Important**: Replace `YOUR_USERNAME` with your actual username!

### 2. Enable and Start Service

```bash
# Reload systemd
sudo systemctl daemon-reload

# Enable at boot
sudo systemctl enable diretta-renderer

# Start now
sudo systemctl start diretta-renderer

# Check status
sudo systemctl status diretta-renderer
```

### 3. View Logs

```bash
# Follow logs in real-time
sudo journalctl -u diretta-renderer -f

# View recent logs
sudo journalctl -u diretta-renderer -n 100

# View logs from today
sudo journalctl -u diretta-renderer --since today
```

---

## Optimization Tips

### 1. CPU Performance Mode

```bash
# Set CPU governor to performance
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### 2. Real-Time Priority (Advanced)

Edit service file to add:
```ini
[Service]
Nice=-10
IOSchedulingClass=realtime
IOSchedulingPriority=0
```

### 3. Disable Power Management

```bash
# Disable USB autosuspend
echo -1 | sudo tee /sys/module/usbcore/parameters/autosuspend
```

---

## Troubleshooting Installation

### Build Errors

**Error**: `cannot find -lDirettaHost_x64-linux-15v3`
- **Solution**: Check SDK path in Makefile

**Error**: `fatal error: libavformat/avformat.h: No such file`
- **Solution**: Install FFmpeg development packages

**Error**: `fatal error: upnp/upnp.h: No such file`
- **Solution**: Install libupnp-devel

### Runtime Errors

**Error**: `No Diretta target found`
- Check DAC is powered on
- Verify network connection
- Check firewall settings

**Error**: `Permission denied`
- Use `sudo` to run
- Check file permissions

---

## Next Steps

- Configure your UPnP control point → See README.md
- Troubleshoot issues → See TROUBLESHOOTING.md
- Optimize settings → See CONFIGURATION.md

---

**Installation complete!** 🎉 You're ready to enjoy bit-perfect audio streaming!
