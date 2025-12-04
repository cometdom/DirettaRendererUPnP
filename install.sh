#!/bin/bash
#
# Diretta UPnP Renderer - Installation Script
# 
# This script helps install dependencies and set up the renderer.
# Run with: bash install.sh
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored messages
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
if [ "$EUID" -eq 0 ]; then 
    print_error "Please do not run this script as root"
    print_info "The script will ask for sudo password when needed"
    exit 1
fi

echo "============================================"
echo " Diretta UPnP Renderer - Installation"
echo "============================================"
echo ""

# Detect Linux distribution
print_info "Detecting Linux distribution..."

if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VER=$VERSION_ID
    print_success "Detected: $PRETTY_NAME"
else
    print_error "Cannot detect Linux distribution"
    exit 1
fi

# Install dependencies based on distribution
print_info "Installing dependencies..."

case $OS in
    fedora|rhel|centos)
        print_info "Using DNF package manager..."
        sudo dnf install -y \
            gcc-c++ \
            make \
            git \
            ffmpeg-devel \
            libupnp-devel \
            wget
        ;;
    
    ubuntu|debian)
        print_info "Using APT package manager..."
        sudo apt update
        sudo apt install -y \
            build-essential \
            git \
            libavformat-dev \
            libavcodec-dev \
            libavutil-dev \
            libswresample-dev \
            libupnp-dev \
            wget
        ;;
    
    arch|manjaro)
        print_info "Using Pacman package manager..."
        sudo pacman -Sy --needed --noconfirm \
            base-devel \
            git \
            ffmpeg \
            libupnp \
            wget
        ;;
    
    *)
        print_error "Unsupported distribution: $OS"
        print_info "Please install dependencies manually:"
        print_info "  - gcc/g++ (C++ compiler)"
        print_info "  - make"
        print_info "  - FFmpeg development libraries"
        print_info "  - libupnp development library"
        exit 1
        ;;
esac

print_success "Dependencies installed"

# Check for Diretta SDK
print_info "Checking for Diretta Host SDK..."

SDK_PATH="$HOME/DirettaHostSDK_147"

if [ -d "$SDK_PATH" ]; then
    print_success "Found Diretta SDK at: $SDK_PATH"
else
    print_warning "Diretta SDK not found at: $SDK_PATH"
    echo ""
    echo "The Diretta Host SDK is required but not included in this repository."
    echo ""
    echo "Please download it from: https://www.diretta.link"
    echo "  1. Visit the website"
    echo "  2. Go to 'Download Preview' section"
    echo "  3. Download DirettaHostSDK_147.tar.gz"
    echo "  4. Extract to: $HOME/"
    echo ""
    read -p "Press Enter after you've downloaded and extracted the SDK..."
    
    if [ ! -d "$SDK_PATH" ]; then
        print_error "SDK still not found. Please extract it to: $SDK_PATH"
        exit 1
    fi
    
    print_success "SDK found!"
fi

# Verify SDK contents
if [ ! -f "$SDK_PATH/lib/libDirettaHost_x64-linux-15v3.so" ]; then
    print_error "SDK libraries not found. Please check SDK installation."
    exit 1
fi

# Build the renderer
print_info "Building Diretta UPnP Renderer..."

if [ ! -f "Makefile" ]; then
    print_error "Makefile not found. Are you in the correct directory?"
    exit 1
fi

# Update SDK path in Makefile if needed
print_info "Configuring Makefile..."
sed -i "s|SDK_PATH = .*|SDK_PATH = $SDK_PATH|g" Makefile

# Build
make clean
make

if [ ! -f "bin/DirettaRendererComplete" ]; then
    print_error "Build failed. Please check error messages above."
    exit 1
fi

print_success "Build successful!"

# Configure network
print_info "Configuring network..."

echo ""
echo "Available network interfaces:"
ip link show | grep -E "^[0-9]+:" | awk '{print $2}' | sed 's/://g'
echo ""

read -p "Enter your network interface name (e.g., enp4s0): " IFACE

if [ -z "$IFACE" ]; then
    print_warning "No interface specified, skipping network configuration"
else
    # Check if interface exists
    if ip link show "$IFACE" &> /dev/null; then
        print_info "Configuring MTU for $IFACE..."
        
        # Ask about jumbo frames
        read -p "Enable jumbo frames (MTU 9000)? [y/N]: " ENABLE_JUMBO
        
        if [[ "$ENABLE_JUMBO" =~ ^[Yy]$ ]]; then
            sudo ip link set "$IFACE" mtu 9000
            print_success "Jumbo frames enabled (MTU 9000)"
            
            # Offer to make permanent
            read -p "Make this permanent? [y/N]: " MAKE_PERMANENT
            
            if [[ "$MAKE_PERMANENT" =~ ^[Yy]$ ]]; then
                case $OS in
                    fedora|rhel|centos)
                        CONN_NAME=$(nmcli -t -f NAME,DEVICE connection show | grep "$IFACE" | cut -d: -f1)
                        if [ -n "$CONN_NAME" ]; then
                            sudo nmcli connection modify "$CONN_NAME" 802-3-ethernet.mtu 9000
                            print_success "MTU configured permanently in NetworkManager"
                        fi
                        ;;
                    ubuntu|debian)
                        print_info "Add this to /etc/network/interfaces:"
                        echo "  mtu 9000"
                        ;;
                    *)
                        print_info "Manual configuration required for permanent MTU"
                        ;;
                esac
            fi
        else
            print_info "Using standard MTU (1500)"
        fi
    else
        print_error "Interface $IFACE not found"
    fi
fi

# Firewall configuration
print_info "Configuring firewall..."

read -p "Configure firewall to allow UPnP? [y/N]: " CONFIG_FIREWALL

if [[ "$CONFIG_FIREWALL" =~ ^[Yy]$ ]]; then
    case $OS in
        fedora|rhel|centos)
            sudo firewall-cmd --permanent --add-port=1900/udp
            sudo firewall-cmd --permanent --add-port=4005/tcp
            sudo firewall-cmd --permanent --add-port=4006/tcp
            sudo firewall-cmd --reload
            print_success "Firewall configured (firewalld)"
            ;;
        ubuntu|debian)
            if command -v ufw &> /dev/null; then
                sudo ufw allow 1900/udp
                sudo ufw allow 4005/tcp
                sudo ufw allow 4006/tcp
                print_success "Firewall configured (ufw)"
            else
                print_warning "ufw not found, skipping firewall configuration"
            fi
            ;;
        *)
            print_info "Manual firewall configuration required"
            print_info "Open ports: 1900/udp, 4005/tcp, 4006/tcp"
            ;;
    esac
fi

# Create systemd service
print_info "Setting up systemd service..."

read -p "Create systemd service for auto-start? [y/N]: " CREATE_SERVICE

if [[ "$CREATE_SERVICE" =~ ^[Yy]$ ]]; then
    SERVICE_FILE="/etc/systemd/system/diretta-renderer.service"
    BIN_PATH="$(pwd)/bin/DirettaRendererComplete"
    
    sudo tee "$SERVICE_FILE" > /dev/null <<EOF
[Unit]
Description=Diretta UPnP Renderer
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=$(pwd)/bin
ExecStart=$BIN_PATH --port 4005 --buffer 2.0
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

# Network capabilities
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN

[Install]
WantedBy=multi-user.target
EOF
    
    sudo systemctl daemon-reload
    sudo systemctl enable diretta-renderer
    
    print_success "Systemd service created and enabled"
    print_info "Start with: sudo systemctl start diretta-renderer"
    print_info "View logs with: sudo journalctl -u diretta-renderer -f"
fi

# Installation complete
echo ""
echo "============================================"
echo " Installation Complete! 🎉"
echo "============================================"
echo ""
print_success "Diretta UPnP Renderer is ready to use!"
echo ""
echo "Quick Start:"
echo "  1. Start the renderer:"
echo "     sudo ./bin/DirettaRendererComplete --port 4005 --buffer 2.0"
echo ""
echo "  2. Or use systemd service:"
echo "     sudo systemctl start diretta-renderer"
echo ""
echo "  3. Open your UPnP control point (JPlay, BubbleUPnP, etc.)"
echo "  4. Select 'Diretta Renderer' as output device"
echo "  5. Enjoy your music! 🎵"
echo ""
echo "Documentation:"
echo "  - README.md - Overview and quick start"
echo "  - docs/INSTALLATION.md - Detailed installation"
echo "  - docs/CONFIGURATION.md - Configuration options"
echo "  - docs/TROUBLESHOOTING.md - Problem solving"
echo ""
echo "Support:"
echo "  - GitHub Issues: Report bugs or request features"
echo "  - Diretta Website: https://www.diretta.link"
echo ""
print_info "Have fun streaming! 🎧"
