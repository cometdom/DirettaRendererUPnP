# Systemd Service Guide - Diretta UPnP Renderer

## 📖 Overview

This guide explains how to run the Diretta UPnP Renderer as a systemd service, allowing automatic startup on boot and easy management.

**Credits:** Service configuration based on recommendations from **olm52** (AudioLinux developer).

---

## 🚀 Quick Installation

```bash
# 1. Build the renderer
make

# 2.Go to systemd folder
cd systemd

# 3. Install as systemd service
chmod +x install-systemd.sh
sudo ./install-systemd.sh

# 4. Start the service
sudo systemctl start diretta-renderer

# 5. Check it's running
sudo systemctl status diretta-renderer
```

---

## 📁 Installed Files

| File | Location | Purpose |
|------|----------|---------|
| **Binary** | `/opt/diretta-renderer-upnp/DirettaRendererUPnP` | The renderer executable |
| **Config** | `/etc/default/diretta-renderer` | Service configuration |
| **Service** | `/etc/systemd/system/diretta-renderer.service` | Systemd unit file |

---

## ⚙️ Configuration

### Edit Configuration

```bash
sudo nano /etc/default/diretta-renderer
```

### Available Options

```bash
# Target Diretta device (1 = first found)
TARGET=1

# UPnP port (default: 4005)
PORT=4005

# Gapless playback
# Add "--no-gapless" to disable, leave empty to enable
GAPLESS=""

# Log verbosity
# Options: "" (normal), "--verbose" (debug), "--quiet" (warnings only)
VERBOSE=""

# Network interface for multi-homed systems (default: auto-detect)
# Use interface name ("eth0") or IP address ("192.168.1.10")
NETWORK_INTERFACE=""
```

### Apply Changes

After editing the configuration:

```bash
sudo systemctl daemon-reload
sudo systemctl restart diretta-renderer
```

---

## 🎮 Service Management

### Start/Stop/Restart

```bash
# Start the service
sudo systemctl start diretta-renderer

# Stop the service
sudo systemctl stop diretta-renderer

# Restart the service
sudo systemctl restart diretta-renderer

# Check status
sudo systemctl status diretta-renderer
```

### Auto-Start on Boot

```bash
# Enable (start on boot)
sudo systemctl enable diretta-renderer

# Disable (don't start on boot)
sudo systemctl disable diretta-renderer

# Check if enabled
systemctl is-enabled diretta-renderer
```

---

## 📊 Monitoring

### View Logs

```bash
# View recent logs
sudo journalctl -u diretta-renderer

# Follow logs in real-time
sudo journalctl -u diretta-renderer -f

# View logs since boot
sudo journalctl -u diretta-renderer -b

# View logs from last hour
sudo journalctl -u diretta-renderer --since "1 hour ago"

# View with full output (no truncation)
sudo journalctl -u diretta-renderer -f --no-pager
```

### Check Service Status

```bash
# Detailed status
systemctl status diretta-renderer

# Is the service running?
systemctl is-active diretta-renderer

# Is the service enabled?
systemctl is-enabled diretta-renderer

# Show service configuration
systemctl cat diretta-renderer
```

---

## 🔧 Advanced Configuration

### Service File Location

```bash
/etc/systemd/system/diretta-renderer.service
```

### Service File Structure

```ini
[Unit]
Description=Diretta UPnP Renderer
After=network-online.target        # Wait for network
Wants=network-online.target

[Service]
Type=simple
User=root                          # Start as root for network init
WorkingDirectory=/opt/diretta-renderer-upnp
EnvironmentFile=-/etc/default/diretta-renderer
ExecStart=/opt/diretta-renderer-upnp/start-renderer.sh

Restart=on-failure
RestartSec=5

StandardOutput=journal
StandardError=journal
SyslogIdentifier=diretta-renderer

# Capabilities: NET_RAW/ADMIN for Diretta raw sockets, SYS_NICE for SCHED_FIFO
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE

# Filesystem: read-only except private /tmp
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadOnlyPaths=/opt/diretta-renderer-upnp
ReadWritePaths=/var/log

# Kernel/device isolation (no /dev access, no kernel tuning)
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectKernelLogs=true
ProtectControlGroups=true
ProtectClock=true
ProtectHostname=true

# Security restrictions
LockPersonality=true
MemoryDenyWriteExecute=true
RestrictRealtime=false             # Needed for SCHED_FIFO audio threads
RestrictSUIDSGID=true
RemoveIPC=true
RestrictNamespaces=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_NETLINK AF_UNIX AF_PACKET
SystemCallArchitectures=native
SystemCallFilter=~@mount @keyring @debug @module @swap @reboot @obsolete

# Performance
# Nice and IOScheduling are now configurable via /etc/default/diretta-renderer
# (NICE_LEVEL, IO_SCHED_CLASS, IO_SCHED_PRIORITY) and applied by start-renderer.sh

[Install]
WantedBy=multi-user.target
```

The service runs as root to ensure full access to raw sockets (Diretta protocol) and real-time thread priorities (SCHED_FIFO). The `CapabilityBoundingSet` restricts the process to only the capabilities it needs: `CAP_NET_RAW`/`CAP_NET_ADMIN` for network operations and `CAP_SYS_NICE` for real-time scheduling. Process priority (nice level and I/O scheduling) is configurable via `/etc/default/diretta-renderer` and applied by the startup wrapper script.

---

## 🎯 Common Tasks

### Change Port

```bash
# Edit config
sudo nano /etc/default/diretta-renderer
# Change: PORT=4006

# Restart service
sudo systemctl restart diretta-renderer
```

### Update Binary After Rebuild

```bash
# Stop service
sudo systemctl stop diretta-renderer

# Copy new binary
sudo cp ./bin/DirettaRendererUPnP /opt/diretta-renderer-upnp/

# Start service
sudo systemctl start diretta-renderer
```

---

## 🐛 Troubleshooting

### Service Won't Start

```bash
# Check detailed status
systemctl status diretta-renderer -l

# View error logs
sudo journalctl -u diretta-renderer -n 50

# Check if binary exists
ls -l /opt/diretta-renderer-upnp/DirettaRendererUPnP

# Check permissions
ls -l /etc/systemd/system/diretta-renderer.service
```

### Service Keeps Restarting

```bash
# View restart count
systemctl status diretta-renderer | grep -i restart

# View recent failures
sudo journalctl -u diretta-renderer --since "10 minutes ago"

# Common causes:
# - Diretta Target not running
# - Network not ready (increase RestartSec)
# - Invalid configuration
```

### Can't See Logs

```bash
# Ensure journald is running
systemctl status systemd-journald

# Check journal size
journalctl --disk-usage

# If logs are too large, clean old entries
sudo journalctl --vacuum-time=7d
```

---

## 🔄 Uninstallation

```bash
# Stop and disable service
sudo systemctl stop diretta-renderer
sudo systemctl disable diretta-renderer

# Remove service file
sudo rm /etc/systemd/system/diretta-renderer.service

# Reload systemd
sudo systemctl daemon-reload

# Remove installation directory (optional)
sudo rm -rf /opt/diretta-renderer-upnp
```

---

## 💡 Tips

### Run Multiple Instances

To run multiple renderers (e.g., for different DACs):

```bash
# Copy service file
sudo cp /etc/systemd/system/diretta-renderer.service \
        /etc/systemd/system/diretta-renderer-2.service

# Create second config
sudo cp /etc/default/diretta-renderer \
        /etc/default/diretta-renderer-2

# Edit second config with different port
sudo nano /etc/default/diretta-renderer-2
# Set: PORT=4006, TARGET=2

# Edit second service to use second config
sudo nano /etc/systemd/system/diretta-renderer-2.service
# Change: EnvironmentFile=-/opt/diretta-renderer-upnp/diretta-renderer-2.conf

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable diretta-renderer-2
sudo systemctl start diretta-renderer-2
```

### Performance Tuning (Optional)

Process priority is configurable in `/etc/default/diretta-renderer`:

```bash
# CPU scheduling priority (-20 = highest, 19 = lowest)
NICE_LEVEL=-10

# I/O scheduling: realtime, best-effort, idle
IO_SCHED_CLASS=realtime
IO_SCHED_PRIORITY=0
```

These settings can also be adjusted through the web UI under "Process Priority".

For CPU affinity (pinning to specific cores), add to the service file:

```ini
[Service]
CPUAffinity=0 1
```

**Warning:** Extreme priority settings may affect system stability. Test thoroughly!

---

## 📚 References

- [systemd service documentation](https://www.freedesktop.org/software/systemd/man/systemd.service.html)
- [journalctl documentation](https://www.freedesktop.org/software/systemd/man/journalctl.html)
- [AudioLinux](https://www.pitt-pro.com/audiolinux.html) - Optimized Linux for audio

---

## 🙏 Credits

Service configuration based on recommendations from **Piero** (AudioLinux developer), with approval from **Yu Harada** (Diretta creator).

---

## ❓ FAQ

**Q: Why does the service run as root?**
A: The Diretta SDK needs `CAP_NET_RAW` and `CAP_NET_ADMIN` for raw socket access, and `CAP_SYS_NICE` for real-time thread priorities (SCHED_FIFO). Running as root on a dedicated audio machine is the recommended approach — it guarantees optimal scheduling for the audio worker threads.

**Q: How do I see if the renderer is actually playing audio?**
A: Check the logs:
```bash
sudo journalctl -u diretta-renderer -f
```
Look for playback messages.

**Q: Does the service auto-restart if it crashes?**
A: Yes, `Restart=on-failure` with `RestartSec=5` ensures automatic recovery.
