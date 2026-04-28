# Configuration Guide - Diretta UPnP Renderer v2.0.4

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
**Description**: Enable detailed debug logging (log level: DEBUG). Only use for troubleshooting.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --verbose
```

#### `--quiet, -q`
**Default**: Disabled
**Description**: Quiet mode — only show warnings and errors (log level: WARN). Ideal for production use.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --quiet
```

#### `--version, -V`
**Description**: Show version information and exit

### Advanced Diretta SDK Settings

These options allow fine-tuning the Diretta SDK transmission behavior. **Leave at defaults unless you have a specific reason to change them.**

#### `--thread-mode <mode>`
**Default**: 1 (CRITICAL)
**Description**: SDK thread mode bitmask. Flags can be combined by adding values together.

| Flag | Value | Description |
|------|-------|-------------|
| CRITICAL | 1 | Set sending thread to critical priority |
| NOSHORTSLEEP | 2 | Busy-loop for short waits (reduces jitter, uses more CPU) |
| NOSLEEP4CORE | 4 | Only busy-loop if >= 4 CPU cores available |
| SOCKETNOBLOCK | 8 | Non-blocking socket |
| OCCUPIED | 16 | Pin SDK thread to CPU core |
| FEEDBACK | 32/64/128 | Moving average feedback (3 bits) |
| NOFASTFEEDBACK | 256 | Disable fast feedback mechanism |
| IDLEONE | 512 | Run idle handler once per cycle |
| IDLEALL | 1024 | Always run idle handler (busy-loop variant) |
| NOSLEEPFORCE | 2048 | Force busy-loop regardless of core count |
| LIMITRESEND | 4096 | Limit retransmission buffer |
| NOJUMBOFRAME | 8192 | Disable jumbo frame support |
| NOFIREWALL | 16384 | Don't send firewall discovery packets |
| NORAWSOCKET | 32768 | Disable raw socket mode |

**Examples**:
```bash
# Critical + NoShortSleep (reduced jitter)
sudo ./DirettaRendererUPnP --target 1 --thread-mode 3

# Critical + Occupied CPU
sudo ./DirettaRendererUPnP --target 1 --thread-mode 17
```

#### `--cycle-time <microseconds>`
**Default**: Auto-calculated (2620 µs base, adapts to format)
**Range**: 333-10000
**Description**: Maximum packet transmission cycle time. When specified, disables automatic cycle time calculation. Lower values = more frequent transmissions = lower latency but higher CPU.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --cycle-time 5000
```

#### `--info-cycle <microseconds>`
**Default**: 100000 (100ms)
**Description**: Information packet cycle time passed to the SDK `open()` method. Controls the interval for control/info packets, separate from the data transmission cycle.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --info-cycle 50000
```

#### `--cycle-min-time <microseconds>`
**Default**: Unused
**Description**: Minimum cycle time, only used in `random` transfer mode.
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --transfer-mode random --cycle-min-time 333
```

#### `--transfer-mode <mode>`
**Default**: auto
**Description**: Data transfer mode. Controls how packets are sized and scheduled.

| Mode | Description |
|------|-------------|
| `auto` | Automatic (VarMax for PCM Hi-Res, VarAuto for DSD/low-bitrate) |
| `varmax` | Flex cycle, maximum packet size |
| `varauto` | Flex cycle, auto-tuned |
| `fixauto` | Fixed cycle, auto-tuned |
| `random` | Random cycle (uses `--cycle-min-time` as minimum) |

**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --transfer-mode fixauto
```

#### `--target-profile-limit <microseconds>`
**Default**: 0
**Description**: Controls how the SDK manages transmission profiles.
- `0` = **SelfProfile** (stable, default): the renderer manages its own profile directly
- `>0` = **TargetProfile** (experimental): the SDK auto-adapts the profile based on the target device capabilities, with the specified value as the minimum cycle time limit. Under high system load, the SDK automatically falls back to lighter processing.

**Example**:
```bash
# Use SelfProfile (stable, default)
sudo ./DirettaRendererUPnP --target 1 --target-profile-limit 0

# Use TargetProfile with 200µs limit (experimental)
sudo ./DirettaRendererUPnP --target 1 --target-profile-limit 200
```

#### `--mtu <bytes>`
**Default**: Auto-detect
**Description**: Override MTU detection. Useful when auto-detection fails or for testing.
**Common values**: 1500 (standard), 9000 (jumbo), 16128 (max jumbo)
**Example**:
```bash
sudo ./DirettaRendererUPnP --target 1 --mtu 9000
```

### Combined Example

```bash
sudo ./DirettaRendererUPnP \
  --port 4005 \
  --target 1 \
  --name "Bedroom Diretta" \
  --uuid "uuid:bedroom-audio-001"
```

### Advanced Example

```bash
sudo ./DirettaRendererUPnP \
  --target 1 \
  --thread-mode 3 \
  --cycle-time 5000 \
  --transfer-mode fixauto \
  --mtu 9000
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

Network IRQ activity colliding with the audio worker thread is a measurable
source of jitter on busy LANs. Pinning the NIC's interrupts to CPUs *outside*
the one used by `--cpu-audio` keeps interrupt processing physically away from
the audio thread.

##### Built-in: IRQ_INTERFACE / IRQ_CPUS (since v2.4.0)

The simplest way is to set two values in `/etc/default/diretta-renderer` (or
via the web UI under **Advanced Network Settings**):

```bash
IRQ_INTERFACE="enp4s0"   # NIC name; matches all related IRQs incl. MSI-X queues
IRQ_CPUS="0-5"           # CPU list — pick CPUs OUTSIDE your --cpu-audio core
```

At service start, `start-renderer.sh` walks `/proc/interrupts`, matches every
line containing `IRQ_INTERFACE`, and writes `IRQ_CPUS` into each
`/proc/irq/N/smp_affinity_list`. The launcher logs a summary like:

```
IRQ affinity for enp4s0 -> CPU(s) 0-5: 8 pinned, 1 skipped (managed/read-only)
```

Some MSI-X queues are kernel-managed and refuse runtime reassignment — those
are counted as "skipped". This is normal and not a failure.

##### Manual fallback

For one-off tuning or non-systemd setups, you can pin a single IRQ by hand:

```bash
# Find network card IRQs (one line per MSI-X queue is normal)
grep enp4s0 /proc/interrupts

# Example: IRQ 16 -> CPU 0
echo 0 | sudo tee /proc/irq/16/smp_affinity_list
```

#### 3. CPU Isolation with `isolcpus=` (Kernel Boot Parameter)

`--cpu-audio` pins the Diretta worker thread to a specific core, but Linux's
default scheduler can still place other userland tasks on that core. To
*reserve* a core exclusively for DirettaRendererUPnP, add `isolcpus=` to the
kernel boot command line — the scheduler will then leave that core untouched
unless explicitly directed (which is exactly what `--cpu-audio` does).

This is a **system-level setting that requires a reboot** and is not
configurable from DirettaRendererUPnP itself.

##### What to add to the kernel cmdline

Pick the same CPU number you intend to use with `--cpu-audio` (e.g. `8` on a
12-core CPU). A typical hardened triple looks like:

```
isolcpus=8 nohz_full=8 rcu_nocbs=8
```

- `isolcpus=8` — removes CPU 8 from the general scheduler load balancer.
- `nohz_full=8` — disables the periodic scheduler tick on CPU 8 when only one
  task is running (i.e. the Diretta worker), eliminating one more source of
  jitter.
- `rcu_nocbs=8` — moves RCU callback handling off CPU 8 to other cores.

You can isolate multiple CPUs with comma lists or ranges:
`isolcpus=8-11 nohz_full=8-11 rcu_nocbs=8-11`.

##### How to apply (varies by bootloader)

**GRUB (most distros: Fedora, Ubuntu, Debian, Arch, etc.)**:

```bash
# Edit /etc/default/grub and append to GRUB_CMDLINE_LINUX_DEFAULT
sudo nano /etc/default/grub
# Then regenerate the config:
sudo grub2-mkconfig -o /boot/grub2/grub.cfg     # Fedora/RHEL/CentOS
sudo update-grub                                # Ubuntu/Debian
sudo grub-mkconfig -o /boot/grub/grub.cfg       # Arch
sudo reboot
```

**systemd-boot (some Arch / Fedora Silverblue setups)**:

```bash
# Edit the relevant entry in /boot/loader/entries/*.conf, append to the
# `options` line, then:
sudo bootctl update
sudo reboot
```

##### Verifying

After reboot:

```bash
cat /proc/cmdline                       # confirm isolcpus is present
cat /sys/devices/system/cpu/isolated    # should list the isolated CPU(s)
```

##### Caveats

- **Test conservatively.** A typo in the kernel cmdline can prevent boot; if
  that happens, edit the cmdline at the GRUB prompt (press `e` at the menu)
  to recover.
- **Don't isolate every core.** The kernel still needs CPUs to run system
  tasks. On a 12-core CPU isolating 1-2 cores is the usual sweet spot.
- **Pair with `--cpu-audio`.** `isolcpus` removes the core from the scheduler
  *for non-pinned tasks*; the core only becomes useful for audio if you also
  pin DRUP to it via `--cpu-audio`.

#### 4. Process Priority

The audio worker thread runs with SCHED_FIFO real-time priority (50) automatically.

Process-level nice and I/O scheduling are configurable in `/etc/default/diretta-renderer`:
```bash
NICE_LEVEL=-10              # -20 (highest) to 19 (lowest)
IO_SCHED_CLASS=realtime     # realtime, best-effort, idle
IO_SCHED_PRIORITY=0         # 0 (highest) to 7 (lowest)
```

These settings are also adjustable through the web UI under "Process Priority".

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

### Runtime Statistics (SIGUSR1)

Send `SIGUSR1` to the renderer process to dump live statistics to stdout (or journal if running as systemd service):

```bash
# Find the PID
pgrep DirettaRendererUPnP

# Dump statistics
kill -USR1 $(pgrep DirettaRendererUPnP)

# View in journal (systemd)
sudo journalctl -u diretta-renderer -n 20
```

Output includes: playback state, current format, buffer fill level, MTU, stream/push/underrun counters.

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

## Testing

Run the built-in test suite to verify format conversions on your platform:

```bash
make test
```

This runs 20 unit tests covering:
- Memory infrastructure (AVX2/NEON memcpy, buffer alignment)
- PCM format conversions (24-bit pack LSB/MSB, 16→32, 16→24)
- DSD conversions (all 4 modes: Passthrough, BitReverse, ByteSwap, BitReverseSwap)
- Ring buffer mechanics (wraparound, power-of-2 sizing, full/empty edge cases)
- Integration tests (push→pop round-trip)

Expected output: `=== Results: 20 passed, 0 failed ===`

---

## Getting Help

- Check logs: `sudo journalctl -u diretta-renderer -f`
- GitHub Issues: Report problems with full logs
- Diretta community: For DAC-specific questions
