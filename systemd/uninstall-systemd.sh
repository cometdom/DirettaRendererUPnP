#!/bin/bash
# Diretta UPnP Renderer - Systemd Uninstall Script

set -e

INSTALL_DIR="/opt/diretta-renderer-upnp"
SERVICE_FILE="/etc/systemd/system/diretta-renderer.service"

echo "════════════════════════════════════════════════════════"
echo "  Diretta UPnP Renderer - Systemd Service Uninstall"
echo "════════════════════════════════════════════════════════"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "❌ Please run as root (sudo ./uninstall-systemd.sh)"
    exit 1
fi

# Check if service exists
if [ ! -f "$SERVICE_FILE" ]; then
    echo "ℹ️  Service not installed, nothing to do."
    exit 0
fi

echo "This will remove:"
echo "  - Systemd service: $SERVICE_FILE"
echo "  - Installation directory: $INSTALL_DIR"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Cancelled."
    exit 0
fi

echo ""
echo "1. Stopping service..."
if systemctl is-active --quiet diretta-renderer; then
    systemctl stop diretta-renderer
    echo "   ✓ Service stopped"
else
    echo "   ℹ️  Service was not running"
fi

echo "2. Disabling service..."
if systemctl is-enabled --quiet diretta-renderer 2>/dev/null; then
    systemctl disable diretta-renderer
    echo "   ✓ Service disabled"
else
    echo "   ℹ️  Service was not enabled"
fi

echo "3. Removing service file..."
rm -f "$SERVICE_FILE"
echo "   ✓ Service file removed"

echo "4. Reloading systemd daemon..."
systemctl daemon-reload
echo "   ✓ Systemd reloaded"

echo "5. Removing installation directory..."
if [ -d "$INSTALL_DIR" ]; then
    rm -rf "$INSTALL_DIR"
    echo "   ✓ Installation directory removed"
else
    echo "   ℹ️  Installation directory not found"
fi

echo ""
echo "════════════════════════════════════════════════════════"
echo "  ✓ Uninstallation Complete!"
echo "════════════════════════════════════════════════════════"
echo ""
echo "The renderer has been completely removed from systemd."
echo "Your project files remain in the source directory."
echo ""
