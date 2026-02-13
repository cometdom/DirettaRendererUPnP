# Configuration Guide - Diretta UPnP Renderer v2.0

Detailed configuration options and tuning guide.

## Table of Contents

1. [Command-Line Options](#command-line-options)
2. [Network Configuration](#network-configuration)
3. [Audio Buffer (Automatic)](#audio-buffer-automatic)
4. [UPnP Device Settings](#upnp-device-settings)
5. [Performance Tuning](#performance-tuning)
6. [Advanced Settings](#advanced-settings)

---

## Command-Line Options

### Basic Usage

```bash
sudo ./DirettaRendererUPnP [OPTIONS]
```

### Available Options

#### `--target, -t <index>`
**Required**: Yes (or use `--list-targets` first)
**Description**: Select which Diretta target to use (1-based index)
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1
```

#### `--list-targets, -l`
**Description**: List all available Diretta targets and exit
**Example**:
```bash
sudo ./DirettaRendererUPnP --list-targets
```

#### `--port, -p <number>`
**Default**: Auto
**Description**: UPnP control port. Port+1 is automatically used for HTTP server.
**Example**:
```bash
sudo ./DirettaRendererUPnP --port 4005
```

#### `--name, -n <string>`
**Default**: "Diretta Renderer"
**Description**: Friendly name shown in UPnP control points
**Example**:
```bash
sudo ./DirettaRendererUPnP --name "Living Room Audio"
```

#### `--uuid <string>`
**Default**: Auto-generated
**Description**: Unique device identifier. Keep the same UUID to maintain device identity across restarts.
**Example**:
```bash
sudo ./DirettaRendererUPnP --uuid "uuid:my-custom-id-123"
```

#### `--interface <name>`
**Default**: Auto-detect
**Description**: Network interface to bind for UPnP discovery. Essential for multi-NIC setups (e.g., 3-tier architecture with separate control and audio networks).
**Example**:
```bash
sudo ./DirettaRendererUPnP --interface eth0
```

#### `--no-gapless`
**Default**: Gapless enabled
**Description**: Disable gapless playback
**Example**:
```bash
sudo ./DirettaRendererUPnP --no-gapless
```

#### `--verbose, -v`
**Default**: Disabled
**Description**: Enable detailed debug logging. Only use for troubleshooting.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --verbose
```

#### `--version, -V`
**Description**: Show version information and exit

### Combined Example

```bash
sudo ./DirettaRendererUPnP \
  --port 4005 \
  --target 1 \
  --name "Bedroom Diretta" \
  --uuid "uuid:bedroom-audio-001"
```

---

## Network Configuration

### MTU Settings

The renderer automatically detects and uses jumbo frames when available.

#### Optimal MTU Values

| Format | Recommended MTU | Packet Size | Performance |
|--------|----------------|-------------|-------------|
| 16bit/44.1kHz | 2048-4096 | ~1-3k | Optimized for fluidity |
| 24bit/44.1kHz+ | 9000-16000 | ~16k | Maximum throughput |
| DSD64-DSD1024 | 9000-16000 | ~16k | Maximum throughput |

#### Setting MTU

```bash
# Temporary (lost after reboot)
sudo ip link set enp4s0 mtu 9000

# Check current MTU
ip link show enp4s0 | grep mtu

# Test connectivity with large packets
ping -M do -s 8972 <DAC_IP>
# If this works, jumbo frames are working
```

#### MTU Troubleshooting

**Symptom**: Audio dropouts with Hi-Res files
**Solution**:
```bash
# Try different MTU values
sudo ip link set enp4s0 mtu 4096  # Conservative
sudo ip link set enp4s0 mtu 9000  # Standard jumbo
```

**Symptom**: No connectivity after setting MTU
**Solution**: Your switch may not support jumbo frames
```bash
# Reset to standard
sudo ip link set enp4s0 mtu 1500
```

### Firewall Configuration

Required ports:

```bash
# Fedora (firewalld)
sudo firewall-cmd --permanent --add-port=1900/udp  # SSDP
sudo firewall-cmd --permanent --add-port=4005/tcp  # Control
sudo firewall-cmd --permanent --add-port=4006/tcp  # HTTP
sudo firewall-cmd --reload

# Ubuntu/Debian (ufw)
sudo ufw allow 1900/udp
sudo ufw allow 4005/tcp
sudo ufw allow 4006/tcp

# iptables (manual)
sudo iptables -A INPUT -p udp --dport 1900 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 4005 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 4006 -j ACCEPT
```

### Quality of Service (QoS)

For optimal performance on busy networks:

```bash
# Set DSCP/TOS for audio traffic (advanced)
# This prioritizes Diretta packets
sudo iptables -t mangle -A OUTPUT -p udp --dport 50000:50100 -j DSCP --set-dscp 46
```

---

## Audio Buffer (Automatic)

Since v2.0, the Diretta ring buffer is **fully automatic** and adaptive. There is no `--buffer` command-line option.

### Adaptive Buffer Sizing

The renderer detects the audio source type and adjusts the buffer accordingly:

| Source | Ring Buffer | Prefill | Detection |
|--------|-----------|---------|-----------|
| **Local** (LAN server: Asset, JRiver, Audirvana) | 0.5s | 80ms | IP address (192.168.x, 10.x, 172.x) |
| **Remote** (Qobuz, Tidal, internet streams) | 1.0s | 150ms | Non-local IP or streaming service in URL |
| **DSD** (all sources) | 0.8s | 200ms | DSD format detected |

- **Local sources** get a smaller buffer for lower latency
- **Remote sources** get a larger buffer to absorb internet jitter and CDN reconnections
- **DSD** always uses 0.8s regardless of source type

### Packet Sizing

Packet sizes are also automatic based on format and MTU:

- **16bit/44.1kHz**: Small packets (~1-3k) regardless of MTU
- **24bit+/Hi-Res**: Large packets (~16k) when MTU allows
- **DSD**: Maximum packet size for efficiency

**No manual adjustment needed.**

---

## UPnP Device Settings

### Device Discovery

The renderer announces itself via SSDP on startup.

#### Manual Discovery Trigger

If your control point doesn't find the renderer:

```bash
# Restart SSDP announcement
sudo systemctl restart diretta-renderer

# Check SSDP is active
sudo netstat -an | grep 1900
# Should show: udp 0.0.0.0:1900
```

### Device Description

Location: Automatically generated at:
```
http://<RENDERER_IP>:4006/description.xml
```

View in browser to verify renderer is accessible.

### Supported UPnP Services

1. **AVTransport**: Play, Stop, Pause, Seek
2. **RenderingControl**: Volume (basic)
3. **ConnectionManager**: Protocol info

### Protocol Info

The renderer automatically advertises support for:
- All PCM sample rates (44.1kHz - 1536kHz)
- All bit depths (8, 16, 24, 32)
- DSD (64, 128, 256, 512, 1024)
- Compressed formats (FLAC, ALAC)

---

## Performance Tuning

### System Optimization

#### 1. CPU Governor

```bash
# Set to performance mode
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Verify
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

#### 2. IRQ Affinity (Advanced)

Bind network card IRQ to specific CPU:

```bash
# Find network card IRQ
cat /proc/interrupts | grep enp4s0

# Example: IRQ 16, bind to CPU 0
echo 1 | sudo tee /proc/irq/16/smp_affinity
```

#### 3. Process Priority

The audio thread runs with real-time priority automatically.

Manual adjustment (systemd service):
```ini
[Service]
Nice=-10
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=80
```

### Audio-Specific Tuning

#### AudioLinux Users

AudioLinux is pre-optimized. Additional tuning:

```bash
# Disable CPU frequency scaling
sudo cpupower frequency-set -g performance
```

#### Real-Time Kernel

For ultimate performance, use RT kernel:

```bash
# Fedora
sudo dnf install kernel-rt

# Arch/AudioLinux (usually pre-installed)
uname -r  # Should show "rt" in kernel version
```

### Network Tuning

#### Increase Network Buffers

```bash
# Add to /etc/sysctl.conf
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.tcp_rmem = 4096 87380 67108864
net.ipv4.tcp_wmem = 4096 65536 67108864

# Apply
sudo sysctl -p
```

#### Disable Network Power Management

```bash
# Disable for specific interface
sudo ethtool -s enp4s0 wol d

# Check settings
ethtool enp4s0 | grep Wake
```

---

## Advanced Settings

### Custom Makefile Options

#### Production Build (no SDK logging)

```bash
make NOLOG=1
```

#### Architecture-Specific Build

```bash
make ARCH_NAME=x64-linux-15v3      # x86-64 with AVX2 (most common)
make ARCH_NAME=x64-linux-15zen4    # AMD Zen 4
make ARCH_NAME=aarch64-linux-15    # Raspberry Pi 4 (4KB pages)
make ARCH_NAME=aarch64-linux-15k16 # Raspberry Pi 5 (16KB pages)
```

---

## Configuration Profiles

### Profile 1: Maximum Quality (Hi-Res Focus)

```bash
# Network
sudo ip link set enp4s0 mtu 9000
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Renderer
sudo ./DirettaRendererUPnP --target 1
```

**Best for**: DSD, 24bit/192k+, audiophile setups

### Profile 2: Maximum Compatibility

```bash
# Network
sudo ip link set enp4s0 mtu 1500

# Renderer
sudo ./DirettaRendererUPnP --target 1
```

**Best for**: Standard networks, CD-quality playback

### Profile 3: 3-Tier Architecture

```bash
# Network (jumbo frames on Diretta link)
sudo ip link set enp4s0 mtu 9000

# Renderer (bind to control network interface)
sudo ./DirettaRendererUPnP --target 1 --interface eth1
```

**Best for**: Separate control and audio networks

---

## Monitoring & Verification

### Check Active Configuration

```bash
# View current settings
ps aux | grep DirettaRenderer

# View logs
sudo journalctl -u diretta-renderer -n 50

# Check network statistics
ip -s link show enp4s0
```

### Performance Metrics

Monitor during playback:

```bash
# CPU usage
top -p $(pgrep DirettaRenderer)

# Network throughput
iftop -i enp4s0

# Buffer statistics
# (Check renderer logs for buffer underruns)
```

---

## Troubleshooting Configuration

### "Cannot set MTU to 9000"

- Your network card may not support jumbo frames
- Try 4096 or 1500

### "High CPU usage during playback"

- Use performance CPU governor
- Check for background processes
- Consider real-time kernel

### "Dropouts with specific formats"

- Check logs for which format
- Verify network stability
- For internet streaming (Qobuz/Tidal), ensure stable internet connection

---

## Getting Help

- Check logs: `sudo journalctl -u diretta-renderer -f`
- GitHub Issues: Report problems with full logs
- Diretta community: For DAC-specific questions
