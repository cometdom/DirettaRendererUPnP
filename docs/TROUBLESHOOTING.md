# Troubleshooting Guide - Diretta UPnP Renderer

Common problems and solutions for the Diretta UPnP Renderer.

## Table of Contents

1. [Discovery Issues](#discovery-issues)
2. [Playback Problems](#playback-problems)
3. [Network Issues](#network-issues)
4. [Audio Quality Issues](#audio-quality-issues)
5. [Build & Installation Issues](#build--installation-issues)
6. [Performance Issues](#performance-issues)
7. [Diagnostic Tools](#diagnostic-tools)

---

## Discovery Issues

### Renderer Not Found by Control Point

**Symptoms**: JPlay, BubbleUPnP, or other control points don't see the renderer.

#### Solution 1: Check Renderer is Running

```bash
# Check if process is running
ps aux | grep DirettaRenderer

# Check service status (if using systemd)
sudo systemctl status diretta-renderer

# Check logs
sudo journalctl -u diretta-renderer -n 50
```

**Expected output**: Should show renderer process running.

#### Solution 2: Check Network Connectivity

```bash
# Verify IP address
ip addr show

# Test connectivity from phone/tablet
ping <RENDERER_IP>
```

**Action**: Ensure renderer and control point are on same network/VLAN.

#### Solution 3: Check Firewall

```bash
# Check if ports are open
sudo netstat -tuln | grep -E '1900|4005|4006'

# Should show:
# udp 0.0.0.0:1900 (SSDP)
# tcp 0.0.0.0:4005 (Control)
# tcp 0.0.0.0:4006 (HTTP)

# Open ports if needed (Fedora)
sudo firewall-cmd --permanent --add-port=1900/udp
sudo firewall-cmd --permanent --add-port=4005/tcp
sudo firewall-cmd --permanent --add-port=4006/tcp
sudo firewall-cmd --reload
```

#### Solution 4: Restart SSDP

```bash
# Restart renderer to re-announce
sudo systemctl restart diretta-renderer

# Or manually kill and restart
sudo pkill DirettaRenderer
sudo ./bin/DirettaRendererUPnP --port 4005
```

#### Solution 5: Check UPnP Description URL

From a web browser on your phone/computer:

```
http://<RENDERER_IP>:4006/description.xml
```

**Expected**: Should show XML device description.  
**If 404**: HTTP server not running properly.

---

## Playback Problems

### Audirvana: Progress Bar and Track Title Not Updating During Gapless

**Symptoms**: When playing an album with Audirvana, the audio plays correctly through all tracks but the progress bar stays stuck at the end of the first track and the track title doesn't change.

**Solution**: Enable **Universal Gapless** in Audirvana settings. This allows Audirvana to properly detect gapless transitions performed by the renderer and update its UI accordingly.

### No Audio Output

**Symptoms**: Track plays on control point but no sound from DAC.

#### Solution 1: Check DAC Connection

```bash
# Check logs for Diretta connection
sudo journalctl -u diretta-renderer | grep -i diretta

# Should show:
# [DirettaOutput] Opening Diretta connection
# [DirettaOutput] ✓ Connected to target
```

**Action if not connected**:
- Verify DAC is powered on
- Check Diretta protocol is enabled on DAC
- Verify network cable is connected

#### Solution 2: Check Audio Format Support

```bash
# Check logs for format info
sudo journalctl -u diretta-renderer | grep -i format

# Look for:
# [DirettaRenderer] 🎵 Track: flac 44100Hz/16bit/2ch
```

**Action**: Verify your DAC supports the format being played.

#### Solution 3: Verify Network Path

```bash
# Ping DAC (if it has an IP)
ping <DAC_IP>

# Check for packet loss
# 0% loss = good, >5% loss = problem
```

### Audio Cuts Out / Dropouts

**Symptoms**: Audio plays but has interruptions, clicks, or dropouts.

#### Solution 1: Check Network Statistics

```bash
# Monitor network errors
ip -s link show enp4s0

# Look for:
# RX errors: should be 0 or very low
# TX dropped: should be 0
```

**If errors present**:
```bash
# Check cable
# Try different network port
# Verify switch supports jumbo frames (if using MTU > 1500)
```

#### Solution 2: Check Buffer Status

Since v2.0, the ring buffer is **fully automatic** and adaptive:
- **Local sources** (LAN servers): 0.5s buffer for low latency
- **Remote sources** (Qobuz, Tidal): 1.0s buffer to absorb internet jitter
- **DSD**: 0.8s buffer regardless of source

Check logs for buffer configuration:
```bash
sudo journalctl -u diretta-renderer | grep -i "ring\|buffer"
```

If dropouts persist, the issue is likely network-related (see below).

#### Solution 3: Check MTU Configuration

```bash
# For 16bit/44.1kHz files with dropouts
sudo ip link set enp4s0 mtu 1500  # Try standard MTU

# For Hi-Res files
sudo ip link set enp4s0 mtu 9000  # Ensure jumbo frames

# Test playback after each change
```

#### Solution 4: Check CPU Usage

```bash
# Monitor CPU during playback
top -p $(pgrep DirettaRenderer)

# If CPU > 80%:
# - Close other applications
# - Use performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Playback Won't Start

**Symptoms**: Press play, nothing happens.

#### Solution 1: Check File Format

```bash
# Check logs for error
sudo journalctl -u diretta-renderer | tail -20

# Look for:
# "Cannot open track"
# "Unsupported format"
```

**Action**: Verify file is accessible and format is supported.

#### Solution 2: Check Transport State

```bash
# Check logs for transport commands
sudo journalctl -u diretta-renderer | grep -E "Play|Stop|Pause"

# Should show:
# [UPnPDevice] Action: Play
# [DirettaRenderer] ✓ Play command received
```

#### Solution 3: Restart Audio Thread

```bash
# Sometimes audio thread gets stuck
sudo systemctl restart diretta-renderer

# Check it started properly
sudo systemctl status diretta-renderer
```

### Seek/Skip Not Working

**Symptoms**: Moving position slider has no effect or causes playback to stop.

#### Solution 1: Check Seek Support

```bash
# Check logs during seek
sudo journalctl -u diretta-renderer -f

# Try seeking, should show:
# [UPnPDevice] Action: Seek
# [DirettaRenderer] 🔍 SEEK REQUESTED
# [AudioEngine] ⏩ Seek to XX seconds
```

**If not showing**: Control point may not support seek properly.

#### Solution 2: File Format Limitations

Some formats don't support seeking well:
- **Good for seeking**: Local FLAC, ALAC, WAV
- **Poor for seeking**: Streaming URLs, some codecs

**Action**: Try with local files first.

---

## Network Issues

### Jumbo Frames Not Working

**Symptoms**: Dropouts with Hi-Res files, MTU won't set above 1500.

#### Solution 1: Verify Hardware Support

```bash
# Check if network card supports jumbo frames
ethtool enp4s0 | grep -i mtu

# Try setting incrementally
sudo ip link set enp4s0 mtu 2000  # Small jumbo
sudo ip link set enp4s0 mtu 4096  # Medium
sudo ip link set enp4s0 mtu 9000  # Full jumbo

# Test which works
ping -M do -s 1972 <DAC_IP>  # For MTU 2000
```

#### Solution 2: Check Switch Configuration

- Log into switch management interface
- Look for "Jumbo Frame" or "Max Frame Size" setting
- Enable and set to 9000 or 9216 bytes
- **All devices in path must support jumbo frames**

#### Solution 3: Use Standard MTU for Low Bitrate

For 16bit/44.1kHz files:
```bash
# These don't benefit from jumbo frames anyway
sudo ip link set enp4s0 mtu 1500

# Renderer automatically uses smaller packets
```

### High Network Latency

**Symptoms**: Ping times > 5ms to DAC.

#### Solution 1: Check Network Load

```bash
# Monitor network usage
iftop -i enp4s0

# Look for:
# - Other heavy traffic
# - Background downloads
```

**Action**: Eliminate competing traffic, use dedicated network if possible.

#### Solution 2: Disable Wi-Fi

```bash
# For renderer, use wired connection only
sudo nmcli radio wifi off

# Verify
nmcli device status
# enp4s0 should be connected, wlan0 disconnected
```

#### Solution 3: QoS Configuration

On managed switches:
- Enable QoS
- Prioritize UDP traffic on Diretta ports
- Set DSCP values for audio traffic

---

## Audio Quality Issues

### Audio Sounds Distorted / Garbled

**Symptoms**: Audio plays but sounds wrong (distortion, noise, artifacts).

#### Solution 1: Check Sample Rate Mismatch

```bash
# Check logs for format detection
sudo journalctl -u diretta-renderer | grep "Track:"

# Should show:
# [DirettaRenderer] 🎵 Track: flac 44100Hz/16bit/2ch

# Verify this matches file properties
ffprobe /path/to/audio/file.flac
```

**Action**: Ensure file format is correctly detected.

#### Solution 2: Check DSD Bit Order

For DSD files with issues:

```bash
# Check logs for DSD processing
sudo journalctl -u diretta-renderer | grep -i dsd

# Should show proper DSD configuration
```

**Note**: DSD bit reversal is handled automatically for DSF files.

#### Solution 3: Reduce MTU

```bash
# Sometimes large packets cause issues
sudo ip link set enp4s0 mtu 4096

# Test playback
# If better, keep this setting
```

### Clicks/Pops Between Tracks

**Symptoms**: Noise when transitioning between tracks.

#### Solution 1: Check Gapless Setting

```bash
# Gapless should be enabled by default
# Check startup logs
sudo journalctl -u diretta-renderer | grep -i gapless

# Should show:
# Gapless: enabled
```

#### Solution 2: Check Transition Logs

The renderer handles format transitions automatically. Check logs for details:
```bash
sudo journalctl -u diretta-renderer | grep -i "transition\|reopen\|format"
```

Clicks between tracks with different sample rates are expected (DAC needs to re-lock).

#### Solution 3: Check Control Point

Some control points don't support gapless properly:
- **Good**: JPlay, BubbleUPnP (paid)
- **Limited**: Some free apps

**Action**: Try different control point app.

---

## Build & Installation Issues

### Compilation Errors

#### Error: Cannot find Diretta SDK

```
error: Diretta/SyncBuffer: No such file or directory
```

**Solution**:
```bash
# Check SDK path in Makefile
grep SDK_PATH Makefile

# Should point to: ~/DirettaHostSDK_147 (or your location)

# Verify SDK exists
ls -la ~/DirettaHostSDK_147/Host/Diretta/
```

#### Error: FFmpeg headers not found

```
fatal error: libavformat/avformat.h: No such file
```

**Solution**:
```bash
# Install FFmpeg development packages
sudo dnf install ffmpeg-devel  # Fedora
sudo apt install libavformat-dev  # Ubuntu

# Verify installation
pkg-config --modversion libavformat
```

#### Error: UPnP library not found

```
fatal error: upnp/upnp.h: No such file
```

**Solution**:
```bash
# Install libupnp
sudo dnf install libupnp-devel  # Fedora
sudo apt install libupnp-dev  # Ubuntu
```

#### Linking Errors

```
cannot find -lDirettaHost_x64-linux-15v3
```

**Solution**:
```bash
# Check SDK libraries exist
ls -la ~/DirettaHostSDK_147/lib/

# Verify architecture matches
uname -m  # Should be x86_64

# Check library path in Makefile
grep -A5 "LDFLAGS" Makefile
```

### Runtime Library Errors

```
error while loading shared libraries: libDirettaHost_x64-linux-15v3.so
```

**Solution**:
```bash
# Add SDK lib directory to LD_LIBRARY_PATH
export LD_LIBRARY_PATH=~/DirettaHostSDK_147/lib:$LD_LIBRARY_PATH

# Or add to /etc/ld.so.conf.d/
echo "~/DirettaHostSDK_147/lib" | sudo tee /etc/ld.so.conf.d/diretta.conf
sudo ldconfig
```

### Crash on Startup: `SIGILL` / Invalid Opcode (AMD Zen3/Zen2 "Ryzen 7000" mobile)

**Symptoms**: the service never stays up — `systemctl status` flips between `active (running)` and `activating (auto-restart) (Result: core-dump)`, the restart counter climbs quickly, and the kernel log shows:

```
kernel: traps: DirettaRenderer[...] trap invalid opcode ...
systemd: diretta-renderer.service: Main process exited, code=dumped, status=4/ILL
```

A coredump stack trace points at the SDK connection constructor (`#0 _ZN7DIRETTA10ConnectionC2Ev`).

**Cause**: before **v2.5.5**, the Makefile's CPU auto-detection selected the Zen4 (AVX-512) build variant for CPUs whose model number is in the "Ryzen 7000" range but which are actually **Zen3/Zen2 with no AVX-512** — e.g. **Ryzen 7 7730U** (Barcelo), **Ryzen 5 7520U** (Mendocino). The binary then executes AVX-512 the CPU cannot run → `SIGILL`. Confirm the CPU lacks AVX-512:

```bash
grep -o -E 'avx512[a-z0-9]*' /proc/cpuinfo | head   # no output = no AVX-512
```

**Fix**: use **v2.5.5 or later** — the build now refuses the Zen4 variant on any CPU without AVX-512 and falls back to the AVX2 (`x64-linux-15v3`) build.

**Recovery on an already-affected machine — a full clean reinstall is the reliable path.** A bare `git pull` + rebuild can leave the old, crashing binary in place: the running service holds `/opt/diretta-renderer-upnp/DirettaRendererUPnP` open, so a plain copy over it can fail silently and the crash persists *even after a reboot* (the reboot just relaunches the same old binary).

- **Via fedora-audiophile-setup**: re-run the full installation — it clones a fresh `main` (with the fix), stops the service, replaces the binary, and restarts cleanly. This is what reliably cleared it in the field.
- **Manually**: `sudo systemctl stop diretta-renderer`, then `cd ~/DirettaRendererUPnP && git pull && ./install.sh`, then confirm `systemctl status diretta-renderer` stays `active (running)`.

After recovery, the startup capabilities log (or `ldd`) should show the AVX2 (`x64-linux-15v3`) variant, not `zen4`.

### `Error while loading shared libraries` After a System Update

**Symptoms**: DRUP refused to start right after a distro update, e.g.

```
error while loading shared libraries: libupnp.so.17.2.6: cannot open shared object file
```

(the same can happen with an FFmpeg `libav*.so.NN` after an FFmpeg update).

**Cause**: DRUP is a source build linked against the **system** shared libraries (`libupnp`, FFmpeg's `libav*`) at compile time. A distro update can replace the exact library file the binary references, so the old binary can no longer find it. This is normal ABI/version drift for any locally-compiled program.

**Fix — rebuild DRUP against the updated libraries** (no rollback needed), using the **same build flavour you originally installed with**:

```bash
cd ~/DirettaRendererUPnP
git pull

# Default GCC build:
./install.sh

# …or, if you originally built with Clang + LTO, rebuild the same way:
env LLVM=1 ./install.sh

sudo systemctl restart diretta-renderer
```

**Notes**:
- **Fresh installs are not affected** — they compile against whatever library version is currently installed, so new users won't hit this; only an already-built binary breaks when a library is bumped underneath it.
- If a library update is an **API** (source) change rather than just an ABI/file change, the symptom would be a **compilation error** during the rebuild rather than this load error — report it so the source can be adapted.
- A binary that names the *fully-versioned* file (`libupnp.so.17.2.6`) rather than the soname (`libupnp.so.17`) indicates the distro's library was packaged without a standard soname; the rebuild still resolves it, and DRUP itself links via `-lupnp` (the correct, soname-based way).

---

## Performance Issues

### High CPU Usage

**Symptoms**: CPU at 100%, system feels slow during playback.

#### Solution 1: Check Format

```bash
# DSD and high sample rates use more CPU
# Check current format in logs
sudo journalctl -u diretta-renderer | grep "Track:"
```

**Action**: This may be normal for DSD512+.

#### Solution 2: Optimize Build

```bash
# Rebuild with optimizations
make clean
# Edit Makefile, change:
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -pthread
make
```

#### Solution 3: Check Background Processes

```bash
# Find CPU hogs
top -o %CPU

# Stop unnecessary services
sudo systemctl stop bluetooth
sudo systemctl stop cups
```

### Memory Usage Growing

**Symptoms**: RAM usage increases over time.

#### Solution 1: Check for Memory Leaks

```bash
# Monitor over time
watch -n 5 'ps aux | grep DirettaRenderer'

# If steadily growing, report as bug
```

**Workaround**: Restart renderer daily via cron:
```bash
# Add to crontab
0 4 * * * systemctl restart diretta-renderer
```

---

## Diagnostic Tools

### Essential Commands

```bash
# 1. Check renderer status
sudo systemctl status diretta-renderer

# 2. View recent logs
sudo journalctl -u diretta-renderer -n 100

# 3. Follow logs in real-time
sudo journalctl -u diretta-renderer -f

# 4. Check network
ip addr show
ip -s link show enp4s0

# 5. Test MTU
ping -M do -s 8972 <TARGET_IP>

# 6. Monitor CPU
top -p $(pgrep DirettaRenderer)

# 7. Monitor network
iftop -i enp4s0

# 8. Check processes
ps aux | grep Diretta
```

### Log Analysis

```bash
# Find errors
sudo journalctl -u diretta-renderer | grep -i error

# Find warnings
sudo journalctl -u diretta-renderer | grep -i warning

# Check playback events
sudo journalctl -u diretta-renderer | grep -E "Play|Stop|Pause|Seek"

# Check format changes
sudo journalctl -u diretta-renderer | grep "Track:"

# Check Diretta connection
sudo journalctl -u diretta-renderer | grep -i "diretta"
```

### Network Diagnostics

```bash
# Packet capture (advanced)
sudo tcpdump -i enp4s0 -w /tmp/capture.pcap

# Analyze with Wireshark later

# Check for dropped packets
netstat -s | grep -i drop

# Monitor real-time statistics
nload -u K enp4s0
```

---

## Getting More Help

### Before Asking for Help

1. **Check logs**: `sudo journalctl -u diretta-renderer -n 200`
2. **Note your setup**:
   - OS and version
   - Network configuration (MTU, switch)
   - DAC model
   - Audio format having issues
3. **Try basic troubleshooting** from this guide

### Reporting Issues

When opening a GitHub issue, include:

```bash
# System info
uname -a
cat /etc/os-release

# Renderer version
./bin/DirettaRendererUPnP --version

# Network config
ip addr show
ip link show

# Recent logs
sudo journalctl -u diretta-renderer -n 200 > logs.txt

# Attach logs.txt to issue
```

### Community Resources

- **GitHub Issues**: For bugs and feature requests
- **Diretta Official**: https://www.diretta.link (for DAC/SDK questions)
- **Audiophile Forums**: General discussion and experiences

---

## Common Error Messages

| Error Message | Meaning | Solution |
|---------------|---------|----------|
| `No Diretta target found` | DAC not discovered | Check DAC power, network, Diretta enabled |
| `Cannot open track` | File access issue | Check file exists and is readable |
| `Seek failed` | Format doesn't support seeking | Normal for some streaming URLs |
| `Buffer underrun` | Network too slow | Check network stability, MTU settings |
| `Permission denied` | Need root access | Run with `sudo` |
| `Address already in use` | Port conflict | Check if another instance running |

---

**Still having issues?** Open a GitHub issue with full diagnostic information! 🔧
