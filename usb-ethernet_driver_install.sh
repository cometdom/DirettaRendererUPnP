#!/bin/bash
#
# r8152 Driver Installation Script
# Downloads, installs Realtek r8152 USB Ethernet driver and configures network
# Supports: Ubuntu/Debian, Fedora/RHEL/CentOS
#

set -e  # Exit on error

# Configuration
CONNECTION_NAME="diretta"
MTU_SIZE="16128"
WORK_DIR="$HOME/r8152"

# Realtek download page (driver must be downloaded manually or we parse the page)
REALTEK_PAGE="https://www.realtek.com/Download/List?cate_id=585"
# Direct link pattern (changes with versions)
# As of 2025, the driver is at:
REALTEK_DIRECT_URL="https://www.realtek.com/Download/ToDownloadPage?downloadType=Linux&downloadID=7605"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

# Detect distribution
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO_ID="$ID"
        DISTRO_LIKE="$ID_LIKE"
    elif [ -f /etc/redhat-release ]; then
        DISTRO_ID="rhel"
    elif [ -f /etc/debian_version ]; then
        DISTRO_ID="debian"
    else
        DISTRO_ID="unknown"
    fi

    # Determine package manager
    case "$DISTRO_ID" in
        ubuntu|debian|raspbian|linuxmint)
            PKG_MANAGER="apt"
            PKG_INSTALL="sudo apt install -y"
            PKG_UPDATE="sudo apt update"
            HEADERS_PKG="linux-headers-$(uname -r)"
            IMAGE_PKG="linux-image-$(uname -r)"
            ;;
        fedora)
            PKG_MANAGER="dnf"
            PKG_INSTALL="sudo dnf install -y"
            PKG_UPDATE="sudo dnf check-update || true"
            HEADERS_PKG="kernel-devel-$(uname -r)"
            IMAGE_PKG="kernel-$(uname -r)"
            ;;
        rhel|centos|rocky|almalinux)
            PKG_MANAGER="dnf"
            PKG_INSTALL="sudo dnf install -y"
            PKG_UPDATE="sudo dnf check-update || true"
            HEADERS_PKG="kernel-devel-$(uname -r)"
            IMAGE_PKG="kernel-$(uname -r)"
            ;;
        arch|manjaro)
            PKG_MANAGER="pacman"
            PKG_INSTALL="sudo pacman -S --noconfirm"
            PKG_UPDATE="sudo pacman -Sy"
            HEADERS_PKG="linux-headers"
            IMAGE_PKG=""
            ;;
        opensuse*|suse)
            PKG_MANAGER="zypper"
            PKG_INSTALL="sudo zypper install -y"
            PKG_UPDATE="sudo zypper refresh"
            HEADERS_PKG="kernel-devel"
            IMAGE_PKG=""
            ;;
        *)
            # Check by available package manager
            if command -v apt &>/dev/null; then
                PKG_MANAGER="apt"
                PKG_INSTALL="sudo apt install -y"
                PKG_UPDATE="sudo apt update"
                HEADERS_PKG="linux-headers-$(uname -r)"
                IMAGE_PKG="linux-image-$(uname -r)"
            elif command -v dnf &>/dev/null; then
                PKG_MANAGER="dnf"
                PKG_INSTALL="sudo dnf install -y"
                PKG_UPDATE="sudo dnf check-update || true"
                HEADERS_PKG="kernel-devel-$(uname -r)"
                IMAGE_PKG="kernel-$(uname -r)"
            elif command -v yum &>/dev/null; then
                PKG_MANAGER="yum"
                PKG_INSTALL="sudo yum install -y"
                PKG_UPDATE="sudo yum check-update || true"
                HEADERS_PKG="kernel-devel-$(uname -r)"
                IMAGE_PKG="kernel-$(uname -r)"
            else
                print_error "Unsupported distribution: $DISTRO_ID"
                exit 1
            fi
            ;;
    esac

    print_info "Detected: $DISTRO_ID (package manager: $PKG_MANAGER)"
}

# Install build dependencies
install_dependencies() {
    print_step "Installing build dependencies..."
    
    $PKG_UPDATE
    
    case "$PKG_MANAGER" in
        apt)
            $PKG_INSTALL build-essential $HEADERS_PKG wget curl
            [ -n "$IMAGE_PKG" ] && $PKG_INSTALL $IMAGE_PKG 2>/dev/null || true
            ;;
        dnf|yum)
            $PKG_INSTALL gcc make $HEADERS_PKG wget curl elfutils-libelf-devel
            [ -n "$IMAGE_PKG" ] && $PKG_INSTALL $IMAGE_PKG 2>/dev/null || true
            ;;
        pacman)
            $PKG_INSTALL base-devel $HEADERS_PKG wget curl
            ;;
        zypper)
            $PKG_INSTALL gcc make $HEADERS_PKG wget curl
            ;;
    esac
}

# Download driver from Realtek
download_driver() {
    print_step "Downloading r8152 driver from Realtek..."
    
    cd "$WORK_DIR"
    
    # If archive provided as argument, use it
    if [ -n "$1" ] && [ -f "$1" ]; then
        print_info "Using provided archive: $1"
        cp "$1" .
        DRIVER_ARCHIVE="$(basename "$1")"
        return 0
    fi
    
    # Try to download from Realtek
    # Note: Realtek's website requires accepting a license, so direct download may not work
    # We'll try common patterns
    
    print_info "Attempting to download from Realtek..."
    print_warn "Realtek requires license acceptance. If download fails, please:"
    echo "  1. Visit: $REALTEK_PAGE"
    echo "  2. Download 'RTL8156B(G)/RTL8156BG(S) Linux driver' manually"
    echo "  3. Run: $0 /path/to/downloaded-file.tar.gz"
    echo ""
    
    # Try wget with common driver filenames
    DRIVER_URLS=(
        "https://github.com/wget/r8152/archive/refs/heads/master.zip"
    )
    
    for url in "${DRIVER_URLS[@]}"; do
        print_info "Trying: $url"
        if wget -q --show-progress -O driver_download "$url" 2>/dev/null; then
            # Detect file type
            FILE_TYPE=$(file -b driver_download | cut -d' ' -f1)
            case "$FILE_TYPE" in
                gzip)
                    mv driver_download r8152-driver.tar.gz
                    DRIVER_ARCHIVE="r8152-driver.tar.gz"
                    ;;
                Zip)
                    mv driver_download r8152-driver.zip
                    DRIVER_ARCHIVE="r8152-driver.zip"
                    ;;
                *)
                    rm -f driver_download
                    continue
                    ;;
            esac
            print_info "Downloaded successfully!"
            return 0
        fi
    done
    
    # If we get here, manual download is required
    print_error "Automatic download failed."
    print_info "Please download manually from: $REALTEK_PAGE"
    print_info "Then run: $0 /path/to/r8152-x.xx.x.tar.gz"
    exit 1
}

# Extract driver archive
extract_driver() {
    print_step "Extracting driver..."
    
    cd "$WORK_DIR"
    
    case "$DRIVER_ARCHIVE" in
        *.tar.gz|*.tgz)
            tar xzf "$DRIVER_ARCHIVE"
            ;;
        *.tar.bz2)
            tar xjf "$DRIVER_ARCHIVE"
            ;;
        *.zip)
            unzip -q "$DRIVER_ARCHIVE"
            ;;
        *)
            print_error "Unknown archive format: $DRIVER_ARCHIVE"
            exit 1
            ;;
    esac
    
    # Find the Makefile (driver might extract to subdirectory)
    MAKEFILE_DIR=$(find . -name "Makefile" -type f -exec grep -l "r8152" {} \; | head -1 | xargs dirname)
    
    if [ -z "$MAKEFILE_DIR" ]; then
        print_error "Could not find r8152 Makefile in extracted archive"
        exit 1
    fi
    
    cd "$MAKEFILE_DIR"
    print_info "Driver source found in: $MAKEFILE_DIR"
}

# Compile and install driver
compile_install_driver() {
    print_step "Compiling driver..."
    
    # Clean previous builds
    make clean 2>/dev/null || true
    
    # Compile
    make
    
    print_step "Installing driver..."
    
    # Unload existing module
    sudo rmmod r8152 2>/dev/null || true
    
    # Install
    sudo make install
    
    # Update module dependencies (critical!)
    print_step "Updating module dependencies..."
    sudo depmod -a
    
    # Load new module
    print_step "Loading r8152 module..."
    sudo modprobe r8152
}

# Detect network interface
detect_interface() {
    print_step "Detecting r8152 network interface..."
    
    sleep 3  # Wait for interface to appear
    
    # Method 1: Check dmesg for renamed interface
    INTERFACE=$(dmesg | grep -i "r8152.*renamed" | tail -1 | grep -oP 'enx[a-f0-9]+' 2>/dev/null || true)
    
    # Method 2: Check ip link for enx interfaces
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(ip link | grep -oP 'enx[a-f0-9]+' | head -1 || true)
    fi
    
    # Method 3: Check for eth interfaces
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(ip link | grep -oP 'eth[0-9]+' | tail -1 || true)
    fi
    
    # Method 4: Check /sys for r8152 driver
    if [ -z "$INTERFACE" ]; then
        for iface in /sys/class/net/*; do
            if [ -d "$iface/device/driver" ]; then
                DRIVER=$(basename $(readlink "$iface/device/driver" 2>/dev/null) 2>/dev/null || true)
                if [ "$DRIVER" = "r8152" ]; then
                    INTERFACE=$(basename "$iface")
                    break
                fi
            fi
        done
    fi
    
    if [ -z "$INTERFACE" ]; then
        print_error "Could not detect r8152 interface"
        print_info "Available interfaces:"
        ip link
        print_info "Driver info:"
        lsmod | grep r8152 || echo "r8152 module not loaded"
        exit 1
    fi
    
    print_info "Detected interface: $INTERFACE"
}

# Configure NetworkManager connection
configure_network() {
    print_step "Configuring NetworkManager connection '$CONNECTION_NAME'..."
    
    # Check if NetworkManager is running
    if ! systemctl is-active --quiet NetworkManager; then
        print_warn "NetworkManager is not running, skipping network configuration"
        return 0
    fi
    
    if nmcli con show "$CONNECTION_NAME" &>/dev/null; then
        print_info "Connection '$CONNECTION_NAME' exists, modifying..."
        sudo nmcli con mod "$CONNECTION_NAME" \
            connection.interface-name "$INTERFACE" \
            ipv4.method disabled \
            ipv6.method link-local \
            ethernet.mtu "$MTU_SIZE"
    else
        print_info "Creating new connection '$CONNECTION_NAME'..."
        sudo nmcli con add \
            type ethernet \
            con-name "$CONNECTION_NAME" \
            ifname "$INTERFACE" \
            ipv4.method disabled \
            ipv6.method link-local \
            ethernet.mtu "$MTU_SIZE"
    fi
    
    # Restart connection
    print_info "Restarting connection..."
    sudo nmcli con down "$CONNECTION_NAME" 2>/dev/null || true
    sudo nmcli con up "$CONNECTION_NAME"
}

# Show installation summary
show_summary() {
    echo ""
    echo "=========================================="
    echo "  r8152 Driver Installation Complete"
    echo "=========================================="
    echo ""
    echo "Distribution: $DISTRO_ID"
    echo "Interface:    $INTERFACE"
    echo "Connection:   $CONNECTION_NAME"
    echo "MTU:          $MTU_SIZE"
    echo ""
    echo "=== Driver Info ==="
    modinfo r8152 | grep -E '^(filename|version|description)' || true
    echo ""
    echo "=== Interface Status ==="
    ip addr show "$INTERFACE" 2>/dev/null || true
    echo ""
    echo "=== Connection Status ==="
    nmcli con show "$CONNECTION_NAME" 2>/dev/null | grep -E '(connection.id|ipv4.method|ipv6.method|ethernet.mtu|GENERAL.STATE)' || true
    echo ""
    print_info "Installation successful!"
}

# Cleanup
cleanup() {
    if [ -d "$WORK_DIR" ]; then
        read -p "Remove working directory $WORK_DIR? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            rm -rf "$WORK_DIR"
            print_info "Cleaned up working directory"
        fi
    fi
}

# Usage
usage() {
    echo "Usage: $0 [OPTIONS] [driver-archive.tar.gz]"
    echo ""
    echo "Options:"
    echo "  -h, --help          Show this help"
    echo "  -c, --connection    Connection name (default: diretta)"
    echo "  -m, --mtu           MTU size (default: 16128)"
    echo "  -n, --no-network    Skip NetworkManager configuration"
    echo ""
    echo "Examples:"
    echo "  $0                              # Download and install"
    echo "  $0 r8152-2.21.4.tar.gz          # Install from local archive"
    echo "  $0 -c myconn -m 9000 driver.tar.gz"
    echo ""
}

# Parse arguments
SKIP_NETWORK=false
DRIVER_FILE=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -c|--connection)
            CONNECTION_NAME="$2"
            shift 2
            ;;
        -m|--mtu)
            MTU_SIZE="$2"
            shift 2
            ;;
        -n|--no-network)
            SKIP_NETWORK=true
            shift
            ;;
        *)
            DRIVER_FILE="$1"
            shift
            ;;
    esac
done

# Main execution
main() {
    echo "=========================================="
    echo "  r8152 Driver Installer"
    echo "=========================================="
    echo ""
    
    detect_distro
    
    # Create working directory
    print_step "Creating working directory..."
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"
    
    install_dependencies
    download_driver "$DRIVER_FILE"
    extract_driver
    compile_install_driver
    detect_interface
    
    if [ "$SKIP_NETWORK" = false ]; then
        configure_network
    fi
    
    show_summary
    cleanup
}

# Run
main