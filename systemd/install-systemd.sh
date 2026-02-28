#!/bin/bash
# Diretta UPnP Renderer - Systemd Installation Script
# 
# This script installs the renderer as a systemd service

set -e

INSTALL_DIR="/opt/diretta-renderer-upnp"
SERVICE_FILE="/etc/systemd/system/diretta-renderer.service"
CONFIG_FILE="$INSTALL_DIR/diretta-renderer.conf"
WRAPPER_SCRIPT="$INSTALL_DIR/start-renderer.sh"

echo "════════════════════════════════════════════════════════"
echo "  Diretta UPnP Renderer - Systemd Service Installation"
echo "════════════════════════════════════════════════════════"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "❌ Please run as root (sudo ./install-systemd.sh)"
    exit 1
fi

# Detect script location and find binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BINARY_PATH="$PROJECT_ROOT/bin/DirettaRendererUPnP"

echo "📂 Script location: $SCRIPT_DIR"
echo "📂 Project root:    $PROJECT_ROOT"
echo "📂 Looking for binary at: $BINARY_PATH"
echo ""

# Check if binary exists
if [ ! -f "$BINARY_PATH" ]; then
    echo "❌ Binary not found at: $BINARY_PATH"
    echo ""
    echo "Please ensure you have built the renderer:"
    echo "  cd $PROJECT_ROOT"
    echo "  make"
    exit 1
fi

echo "✓ Binary found: $BINARY_PATH"
echo ""

echo "1. Creating installation directory..."
mkdir -p "$INSTALL_DIR"

echo "2. Copying binary..."
cp "$BINARY_PATH" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/DirettaRendererUPnP"
echo "   ✓ Binary copied to $INSTALL_DIR/DirettaRendererUPnP"

echo "3. Installing wrapper script..."
cp "$SCRIPT_DIR/start-renderer.sh" "$WRAPPER_SCRIPT"
chmod +x "$WRAPPER_SCRIPT"
echo "   ✓ Wrapper script installed: $WRAPPER_SCRIPT"

echo "4. Creating default configuration file..."
if [ ! -f "$CONFIG_FILE" ]; then
    cp "$SCRIPT_DIR/diretta-renderer.conf" "$CONFIG_FILE"
    echo "   ✓ Configuration file created: $CONFIG_FILE"
else
    echo "   ℹ Configuration file already exists, keeping current settings"
fi

echo "5. Installing systemd service..."
cp "$SCRIPT_DIR/diretta-renderer.service" "$SERVICE_FILE"
echo "   ✓ Service file installed: $SERVICE_FILE"

echo "6. Reloading systemd daemon..."
systemctl daemon-reload

echo "7. Enabling service (start on boot)..."
systemctl enable diretta-renderer.service

echo ""
echo "════════════════════════════════════════════════════════"
echo "  ✓ Installation Complete!"
echo "════════════════════════════════════════════════════════"
echo ""
echo "📝 Configuration file: $CONFIG_FILE"
echo "📝 Service file:       $SERVICE_FILE"
echo "📝 Wrapper script:     $WRAPPER_SCRIPT"
echo "📁 Installation dir:   $INSTALL_DIR"
echo ""
echo "🎯 Next steps:"
echo ""
echo "  1. Edit configuration (optional):"
echo "     sudo nano $CONFIG_FILE"
echo ""
echo "  2. Start the service:"  
echo "     sudo systemctl daemon-reload"
echo ""
echo "  3. Start the service:"  
echo "     sudo systemctl start diretta-renderer"
echo ""
echo "  4. Check status:"
echo "     sudo systemctl status diretta-renderer"
echo ""
echo "  5. View logs:"
echo "     sudo journalctl -u diretta-renderer -f"
echo ""
echo "  6. Stop the service:"
echo "     sudo systemctl stop diretta-renderer"
echo ""
echo "  7. Disable auto-start:"
echo "     sudo systemctl disable diretta-renderer"
echo ""
echo "════════════════════════════════════════════════════════"
