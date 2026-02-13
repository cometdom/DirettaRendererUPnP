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
| **Config** | `/opt/diretta-renderer-upnp/diretta-renderer.conf` | Service configuration |
| **Service** | `/etc/systemd/system/diretta-renderer.service` | Systemd unit file |

---

## ⚙️ Configuration

### Edit Configuration

```bash
sudo nano /opt/diretta-renderer-upnp/diretta-renderer.conf
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

# Verbose logging
# Add "--verbose" to enable debug logs, leave empty for normal output
VERBOSE=""
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
User=root                          # Run as root (for network capabilities)
WorkingDirectory=/opt/diretta-renderer-upnp
EnvironmentFile=-/opt/diretta-renderer-upnp/diretta-renderer.conf
ExecStart=/opt/diretta-renderer-upnp/start-renderer.sh

# Restart on failure
Restart=on-failure
RestartSec=5

# Logging to journald
StandardOutput=journal
StandardError=journal
SyslogIdentifier=diretta-renderer

# Network capabilities (for raw sockets)
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN

[Install]
WantedBy=multi-user.target
```

---

## 🎯 Common Tasks

### Change Port

```bash
# Edit config
sudo nano /opt/diretta-renderer-upnp/diretta-renderer.conf
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
sudo cp /opt/diretta-renderer-upnp/diretta-renderer.conf \
        /opt/diretta-renderer-upnp/diretta-renderer-2.conf

# Edit second config with different port
sudo nano /opt/diretta-renderer-upnp/diretta-renderer-2.conf
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

Add to service file under `[Service]`:

```ini
# Higher priority (use with caution)
Nice=-10

# Real-time I/O scheduling
IOSchedulingClass=realtime
IOSchedulingPriority=0

# CPU affinity (pin to specific cores)
CPUAffinity=0 1
```

**Warning:** These settings may affect system stability. Test thoroughly!

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

**Q: Why run as root?**
A: The renderer needs `CAP_NET_RAW` and `CAP_NET_ADMIN` capabilities for network operations. Running as root with `AmbientCapabilities` is the cleanest approach.

**Q: Can I run as a non-root user?**
A: Yes, but you'll need to configure capabilities manually:
```bash
sudo setcap cap_net_raw,cap_net_admin+eip /opt/diretta-renderer-upnp/DirettaRendererUPnP
```
Then change `User=root` to `User=audiouser` in the service file.

**Q: How do I see if the renderer is actually playing audio?**
A: Check the logs:
```bash
sudo journalctl -u diretta-renderer -f
```
Look for playback messages.

**Q: Does the service auto-restart if it crashes?**
A: Yes, `Restart=on-failure` with `RestartSec=5` ensures automatic recovery.
