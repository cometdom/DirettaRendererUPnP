# HowTo: Install DirettaRendererUPnP on Fedora 43 Minimal
Author SwissBearMontains

## Introduction

This guide walks you through setting up a high-fidelity audio renderer on a headless machine using Fedora 43 Minimal. We support both x86_64 (Intel/AMD) and ARM64 (Raspberry Pi 4/5) architectures.

**Estimated time:** 45-60 minutes

**What you need:**
- A computer: x86_64 (Intel NUC, mini PC) or ARM64 (Raspberry Pi 4/5)
- USB drive (8GB minimum) for installation
- USB-Ethernet adapter with RTL8156 chipset (recommended for Diretta audio)
- Network connection (Ethernet recommended for audio)
- Another computer to prepare the USB and transfer files
- Temporarily: a screen, keyboard, and mouse for the initial setup

---

# PART A: At the Machine

*You'll need a screen, keyboard, and mouse for this part.*

---

## Step 1: Download and Create Installation Media

On your main computer, prepare the bootable USB.

### 1.1 Download Fedora 43 Minimal

**For x86_64 (Intel/AMD):**
- Go to: https://fedoraproject.org/server/download
- Select **Network Install** (netinst) for x86_64
- File: `Fedora-Server-netinst-x86_64-43-*.iso`

**For ARM64 (aarch64):**
- Go to: https://fedoraproject.org/server/download
- Select **Network Install** (netinst) for aarch64
- File: `Fedora-Server-netinst-aarch64-43-*.iso`

### 1.2 Create Bootable USB with balenaEtcher

1. Download [balenaEtcher](https://etcher.balena.io/) for your system
2. Install and launch balenaEtcher
3. Click **Flash from file** → select the Fedora ISO
4. Click **Select target** → choose your USB drive
5. Click **Flash!**
6. Wait for completion and verification

---

## Step 2: Install Fedora 43 Minimal

### 2.1 Boot from USB

1. Insert the USB drive into your audio PC
2. Connect screen, keyboard, mouse, and Ethernet cable
3. Power on and enter BIOS/UEFI (usually F2, F12, Del, or Esc)
4. Set USB as first boot device
5. Save and restart

### 2.2 Installation Steps

When the installer starts:

1. **Select Language** → English (or your preference) → Continue

2. **Installation Destination**
   - Select your target disk
   - Choose "Automatic" partitioning
   - Click Done

3. **Software Selection** → **Minimal Install**
   - This is crucial - select only "Minimal Install"
   - Do NOT add any additional software groups
   - Click Done

4. **Network & Hostname**
   - Enable your network interface
   - Set a hostname (e.g., `diretta-renderer`)
   - Click Done

5. **Root Password**
   - Set a strong root password
   - Check "Allow root SSH login with password"

6. **User Creation**
   - Create a user account (e.g., `audiophile`)
   - Make this user administrator
   - Set a password

7. Click **Begin Installation**

8. Wait for installation to complete, then **Reboot**

---

## Step 3: Enable SSH and Note the IP Address

After reboot, remove the USB drive and login with your user account.

### 3.1 Install and Enable SSH

```bash
sudo dnf install -y openssh-server
sudo systemctl enable sshd
sudo systemctl start sshd
```

### 3.2 Note Your IP Address

```bash
ip addr show
```

Look for an address like `192.168.1.100` — write it down!

---

## Step 4: Connect the USB-Ethernet Adapter (RTL8156)

Plug your USB-Ethernet adapter into a USB port on the audio PC. The RTL8156 chipset is recommended for optimal Diretta audio streaming.

Connect the Ethernet cable from your audio network to this adapter.

### 4.1 Verify Detection

```bash
lsusb | grep -i realtek
```

You should see something like: `Realtek Semiconductor Corp. RTL8156`

### 4.2 Check the Network Interface

```bash
ip link
```

You should see a new interface named `eth1` or `enxXXXXXXXXXXXX` (where X is the MAC address).

---

## Step 5: Disconnect Screen, Keyboard, Mouse

You're done at the machine. Unplug the screen, keyboard, and mouse.

Your audio PC is now headless and ready for remote configuration.

---

# PART B: From the Couch

*Everything from here is done remotely from your main computer via SSH.*

---

## Step 6: Connect via SSH

From your main computer (Terminal on Mac/Linux, PowerShell on Windows):

```bash
ssh audiophile@192.168.1.100
```

Replace `192.168.1.100` with the IP address you noted earlier.

---

## Step 7: Run the Optimization Script

### 7.1 Create the Script

```bash
nano ~/optimize-fedora-audio.sh
```

Choose and paste the appropriate script for your architecture:

---

#### SCRIPT FOR x86_64 (Intel/AMD) - with CachyOS RT Kernel

```bash
#!/bin/bash
# Fedora 43 Audio Optimization Script - x86_64 with RT Kernel

set -e
echo "=== DirettaRendererUPnP Optimization for x86_64 ==="

echo "=== Installing required packages ==="
sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl wget unzip htop

echo "=== Disabling unnecessary services ==="

# Disable audit daemon
sudo systemctl disable auditd 2>/dev/null || true
sudo systemctl stop auditd 2>/dev/null || true

# Remove firewall (not needed for dedicated audio)
sudo systemctl stop firewalld 2>/dev/null || true
sudo dnf remove -y firewalld 2>/dev/null || true

# Remove SELinux (simplifies audio setup)
sudo dnf remove -y selinux-policy 2>/dev/null || true

# Disable journald (reduces disk writes)
sudo systemctl disable systemd-journald 2>/dev/null || true
sudo systemctl stop systemd-journald 2>/dev/null || true

# Disable OOM daemon
sudo systemctl disable systemd-oomd 2>/dev/null || true
sudo systemctl stop systemd-oomd 2>/dev/null || true

# Disable home daemon
sudo systemctl disable systemd-homed 2>/dev/null || true
sudo systemctl stop systemd-homed 2>/dev/null || true

# Remove PolicyKit
sudo systemctl stop polkitd 2>/dev/null || true
sudo dnf remove -y polkit 2>/dev/null || true

echo "=== Installing CachyOS Real-Time Kernel ==="
sudo dnf copr enable -y bieszczaders/kernel-cachyos
sudo dnf install -y kernel-cachyos-rt kernel-cachyos-rt-devel-matched

echo "=== Configuring kernel boot parameters ==="
sudo grubby --update-kernel=ALL --args="audit=0 zswap.enabled=0 skew_tick=1 nosoftlockup default_hugepagesz=1G intel_pstate=enable"

echo ""
echo "=== Optimization complete! ==="
echo "The system will reboot in 10 seconds..."
sleep 10
sudo reboot
```

---

#### SCRIPT FOR ARM64 (aarch64) - without RT Kernel

```bash
#!/bin/bash
# Fedora 43 Audio Optimization Script - ARM64

set -e
echo "=== DirettaRendererUPnP Optimization for ARM64 ==="

echo "=== Installing required packages ==="
sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl wget unzip htop

echo "=== Disabling unnecessary services ==="

# Disable audit daemon
sudo systemctl disable auditd 2>/dev/null || true
sudo systemctl stop auditd 2>/dev/null || true

# Remove firewall (not needed for dedicated audio)
sudo systemctl stop firewalld 2>/dev/null || true
sudo dnf remove -y firewalld 2>/dev/null || true

# Remove SELinux (simplifies audio setup)
sudo dnf remove -y selinux-policy 2>/dev/null || true

# Disable journald (reduces disk writes)
sudo systemctl disable systemd-journald 2>/dev/null || true
sudo systemctl stop systemd-journald 2>/dev/null || true

# Disable OOM daemon
sudo systemctl disable systemd-oomd 2>/dev/null || true
sudo systemctl stop systemd-oomd 2>/dev/null || true

# Disable home daemon
sudo systemctl disable systemd-homed 2>/dev/null || true
sudo systemctl stop systemd-homed 2>/dev/null || true

# Remove PolicyKit
sudo systemctl stop polkitd 2>/dev/null || true
sudo dnf remove -y polkit 2>/dev/null || true

echo "=== Configuring kernel boot parameters ==="
sudo grubby --update-kernel=ALL --args="audit=0 zswap.enabled=0 skew_tick=1 nosoftlockup"

echo ""
echo "=== Optimization complete! ==="
echo "The system will reboot in 10 seconds..."
sleep 10
sudo reboot
```

---

Save and exit: **Ctrl+O**, **Enter**, **Ctrl+X**

### 7.2 Run the Script

```bash
chmod +x ~/optimize-fedora-audio.sh
sudo ~/optimize-fedora-audio.sh
```

The system will reboot automatically. Wait 1-2 minutes, then reconnect:

```bash
ssh audiophile@192.168.1.100
```

### 7.3 Verify RT Kernel (x86_64 only)

```bash
uname -r
# Should show something like: 6.x.x-cachyos-rt
```

---

## Step 8: Transfer the Diretta SDK

The Diretta Host SDK is required for compilation. Download it on your main computer:

1. Go to [diretta.link](https://www.diretta.link/hostsdk.html)
2. Download **DirettaHostSDK_148** (or the latest version)
3. Expected file: `DirettaHostSDK_148_8.tar.zst` (or similar)

### 8.1 Transfer via SCP

From your main computer (Terminal on Mac/Linux, PowerShell on Windows):

```bash
scp DirettaHostSDK_148_8.tar.zst audiophile@192.168.1.100:~/
```

> **From Windows (PowerShell):**
> ```powershell
> scp C:\Users\YourName\Downloads\DirettaHostSDK_148_8.tar.zst audiophile@192.168.1.100:~/
> ```

---

## Step 9: Install DirettaRendererUPnP

SSH back into the Fedora machine:

```bash
ssh audiophile@192.168.1.100
```

### 9.1 Install Build Dependencies

```bash
sudo dnf install -y git gcc-c++ make ffmpeg-free-devel libupnp-devel
```

### 9.2 Extract the Diretta SDK

```bash
cd ~
tar --zstd -xf DirettaHostSDK_148_8.tar.zst
```

The SDK will be extracted to `~/DirettaHostSDK_148/` (or similar).

### 9.3 Clone the Repository and Install

```bash
cd ~
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP
chmod +x install.sh
./install.sh
```

The installation script provides an interactive menu with options for:
- Building the application (auto-detects architecture and SDK)
- Installing as a systemd service
- Configuring automatic startup
- Setting up the Diretta target

---

## Step 10: Verify and Enjoy

### 10.1 List Available Diretta Targets

```bash
sudo ./bin/DirettaRendererUPnP --list-targets
```

### 10.2 Run the Renderer

```bash
# Run with target number 1
sudo ./bin/DirettaRendererUPnP --target 1

# Run with verbose logging (for troubleshooting)
sudo ./bin/DirettaRendererUPnP --target 1 --verbose
```

### 10.3 Test with Your UPnP Controller

On your network, use a UPnP control point:
- **Audirvana** (macOS/Windows) — Native DSD, gapless natively supported (disable "Universal Gapless" in Audirvana)
- **JPlay iOS** (iOS) — Full feature support
- **BubbleUPnP** (Android) — Highly configurable
- **mConnect** (iOS/Android) — Clean interface
- **foobar2000** (Windows) — with UPnP plugin

The renderer should appear as "Diretta Renderer" in the device list.

### 10.4 Updating Later

To update to a newer version:

```bash
cd ~/DirettaRendererUPnP
git pull
make clean && make
```

---

## Quick Reference

```bash
# === PART A: At the machine ===
# Install Fedora, then:
sudo dnf install -y openssh-server
sudo systemctl enable sshd
sudo systemctl start sshd
ip addr show   # Note the IP address!

# === PART B: From the couch ===
ssh audiophile@<IP>

# Run optimization script
nano ~/optimize-fedora-audio.sh
chmod +x ~/optimize-fedora-audio.sh
sudo ~/optimize-fedora-audio.sh

# Transfer the SDK (from main computer)
scp DirettaHostSDK_148_8.tar.zst audiophile@<IP>:~/

# Install build dependencies
sudo dnf install -y git gcc-c++ make ffmpeg-free-devel libupnp-devel

# Extract the SDK
tar --zstd -xf DirettaHostSDK_148_8.tar.zst

# Clone and install DirettaRendererUPnP
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP
chmod +x install.sh
./install.sh

# Run the renderer
sudo ./bin/DirettaRendererUPnP --list-targets
sudo ./bin/DirettaRendererUPnP --target 1
```

---

## Troubleshooting

### Cannot SSH after reboot
- Wait 2-3 minutes for the system to fully boot
- Try: `ping diretta-renderer.local`
- Check your router's admin page for the device

### USB-Ethernet adapter not detected
```bash
# Check USB devices
lsusb

# Check kernel messages
dmesg | tail -30

# List network interfaces
ip link
```

If the adapter is detected but has no IP, check your DHCP server or configure it manually.

### tar cannot extract .tar.zst
```bash
sudo dnf install -y zstd
tar --zstd -xvf file.tar.zst
```

### Renderer not appearing on network
```bash
sudo systemctl status diretta-renderer
sudo systemctl restart diretta-renderer
```

---

## Support

For questions or issues, please open an issue on the GitHub repository.

Happy listening! 🎶
