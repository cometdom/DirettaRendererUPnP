# Configuration Guide - Diretta UPnP Renderer

Detailed configuration options and tuning guide.

## Table of Contents

1. [Command-Line Options](#command-line-options)
2. [Network Configuration](#network-configuration)
3. [Audio Buffer Settings](#audio-buffer-settings)
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

#### `--port <number>`
**Default**: 4005  
**Description**: UPnP control port  
**Example**:
```bash
sudo ./DirettaRendererUPnP --port 4005
```

**Note**: Port 4006 (port+1) is automatically used for HTTP server.

#### `--buffer <seconds>`
**Default**: 2.0  
**Range**: 1.0 - 10.0  
**Description**: Audio buffer size in seconds  
**Example**:
```bash
sudo ./DirettaRendererUPnP --buffer 2.0
```

**Recommendations**:
- **2.0s**: Default, good balance (recommended)
- **1.5s**: Lower latency, requires good network
- **3.0s**: Maximum stability for problematic networks
- **<1.0s**: Not recommended, may cause dropouts

#### `--name <string>>`
**Default**: "Diretta Renderer"  
**Description**: Friendly name shown in UPnP control points  
**Example**:
```bash
sudo ./DirettaRendererUPnP --name "Living Room Audio"
```

#### `--uuid <string>`
**Default**: Auto-generated  
**Description**: Unique device identifier  
**Example**:
```bash
sudo ./DirettaRendererUPnP --uuid "uuid:my-custom-id-123"
```

**Note**: Keep the same UUID to maintain device identity across restarts.

#### `--no-gapless`
**Default**: Gapless enabled  
**Description**: Disable gapless playback  
**Example**:
```bash
sudo ./DirettaRendererUPnP --no-gapless
```
**Use case**: For compatibility with certain control points.

#### `--verbose`
**Default**: no verbose By default, the renderer now displays only essential user-facing messages
**Description** Technical debug information can be enabled using the --verbose flag for debug operations.
**Example**:
```bash
 sudo ./DirettaRendererUPnP --port 4005 --target 1 --buffer 0.9 --verbose
```
**Use case**: Debugging

### Combined Example

```bash
sudo ./DirettaRendererUPnP \
  --port 4005 \
  --buffer 2.0 \
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

## Audio Buffer Settings

### Buffer Size Impact

| Buffer | Latency | Stability | Use Case |
|--------|---------|-----------|----------|
| 1.0s | Low | Risky | Local playback, perfect network |
| 1.5s | Medium | Good | Home network, most scenarios |
| 2.0s | Medium-High | Excellent | Recommended default |
| 3.0s+ | High | Maximum | Problematic networks, Wi-Fi |

### Calculating Buffer Requirements

**Formula**: `Buffer = Network Latency × 10`

Example:
- Network ping: 5ms → Buffer: 50ms minimum
- Network jitter: 20ms → Buffer: 200ms minimum
- **Add safety margin**: Multiply by 10

### Dynamic Buffer Adjustment

The renderer adapts packet sizes automatically:

- **16bit/44.1kHz**: Small packets (~1-3k) regardless of MTU
- **24bit+/Hi-Res**: Large packets (~16k) when MTU allows
- **DSD**: Maximum packet size for efficiency

**No manual adjustment needed!**

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
# Set to power mode
echo "power" | sudo tee /sys/module/snd_usb_audio/parameters/power_save

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

Edit `Makefile` for compilation options:

#### Debug Build

```makefile
CXXFLAGS = -std=c++17 -Wall -Wextra -g -DDEBUG -pthread
```

#### Optimized Build

```makefile
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -pthread
```

#### Logging Levels

Add to compile flags:
```makefile
CXXFLAGS += -DVERBOSE_LOGGING    # More logs
CXXFLAGS += -DQUIET_MODE         # Minimal logs
```

### Source Code Modifications

Common customizations:

#### Change Default Buffer

Edit `main.cpp`:
```cpp
config.bufferSeconds = 3.0;  // Change from 2.0 to 3.0
```

#### Change Device Name

Edit `DirettaRenderer.cpp`:
```cpp
m_config.name = "My Custom Renderer";
```

#### Adjust Network MTU

Edit `DirettaRenderer.cpp`:
```cpp
m_networkMTU = 9000;  // Change from 16128
```

### Environment Variables

```bash
# Set Diretta SDK path
export DIRETTA_SDK_PATH=/path/to/sdk

# Enable debug mode
export DIRETTA_DEBUG=1

# Custom log file
export DIRETTA_LOG_FILE=/var/log/diretta-renderer.log
```

---

## Configuration Profiles

### Profile 1: Maximum Quality (Hi-Res Focus)

```bash
# Network
sudo ip link set enp4s0 mtu 9000
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Renderer
sudo ./DirettaRendererUPnP --buffer 2.0
```

**Best for**: DSD, 24bit/192k+, audiophile setups

### Profile 2: Maximum Compatibility

```bash
# Network
sudo ip link set enp4s0 mtu 1500

# Renderer
sudo ./DirettaRendererUPnP --buffer 3.0
```

**Best for**: Standard networks, CD-quality playback

### Profile 3: Low Latency

```bash
# Network
sudo ip link set enp4s0 mtu 9000

# Renderer
sudo ./DirettaRendererUPnP --buffer 1.5
```

**Best for**: Local playback, minimal latency requirements

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

## Configuration Files

Currently, all configuration is via command-line.

**Future enhancement**: Config file support (`/etc/diretta-renderer.conf`)

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
- Adjust buffer size accordingly
- Verify network stability

---

## Getting Help

- Check logs: `sudo journalctl -u diretta-renderer -f`
- GitHub Issues: Report problems with full logs
- Diretta community: For DAC-specific questions

---

**Your renderer is now optimally configured!** 🎵
