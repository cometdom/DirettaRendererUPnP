#!/bin/bash
#
# Diretta UPnP Renderer - Installation Script
#
# This script helps install dependencies and set up the renderer.
# Run with: bash install.sh
#

set -e  # Exit on error

# =============================================================================
# CONFIGURATION
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_PATH="${DIRETTA_SDK_PATH:-$HOME/DirettaHostSDK_147}"
FFMPEG_BUILD_DIR="/tmp/ffmpeg-build"

# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

confirm() {
    local prompt="$1"
    local default="${2:-N}"
    local response

    if [[ "$default" =~ ^[Yy]$ ]]; then
        read -p "$prompt [Y/n]: " response
        response=${response:-Y}
    else
        read -p "$prompt [y/N]: " response
        response=${response:-N}
    fi

    [[ "$response" =~ ^[Yy]$ ]]
}

# =============================================================================
# SYSTEM DETECTION
# =============================================================================

detect_system() {
    print_header "System Detection"

    if [ "$EUID" -eq 0 ]; then
        print_error "Please do not run this script as root"
        print_info "The script will ask for sudo password when needed"
        exit 1
    fi

    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        print_success "Detected: $PRETTY_NAME"
    else
        print_error "Cannot detect Linux distribution"
        exit 1
    fi

    # Detect architecture
    ARCH=$(uname -m)
    print_info "Architecture: $ARCH"
}

# =============================================================================
# BASE DEPENDENCIES
# =============================================================================

install_base_dependencies() {
    print_header "Installing Base Dependencies"

    case $OS in
        fedora|rhel|centos)
            print_info "Using DNF package manager..."
            sudo dnf install -y \
                gcc-c++ \
                make \
                git \
                libupnp-devel \
                wget \
                nasm \
                yasm \
                pkg-config
            ;;
        ubuntu|debian)
            print_info "Using APT package manager..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                git \
                libupnp-dev \
                wget \
                nasm \
                yasm \
                pkg-config
            ;;
        arch|manjaro)
            print_info "Using Pacman package manager..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                git \
                libupnp \
                wget \
                nasm \
                yasm \
                pkgconf
            ;;
        *)
            print_error "Unsupported distribution: $OS"
            print_info "Please install dependencies manually:"
            print_info "  - gcc/g++ (C++ compiler)"
            print_info "  - make"
            print_info "  - libupnp development library"
            exit 1
            ;;
    esac

    print_success "Base dependencies installed"
}

# =============================================================================
# FFMPEG INSTALLATION
# =============================================================================

install_ffmpeg_build_deps() {
    print_info "Installing FFmpeg build dependencies..."

    case $OS in
        fedora|rhel|centos)
            sudo dnf install -y --skip-unavailable \
                gmp-devel \
                gnutls-devel \
                libdrm-devel \
                fribidi-devel \
                soxr-devel \
                libvorbis-devel \
                libxml2-devel
            ;;
        ubuntu|debian)
            sudo apt install -y \
                libgmp-dev \
                libgnutls28-dev \
                libdrm-dev \
                libfribidi-dev \
                libsoxr-dev \
                libvorbis-dev \
                libxml2-dev
            ;;
        arch|manjaro)
            sudo pacman -Sy --needed --noconfirm \
                gmp \
                gnutls \
                libdrm \
                fribidi \
                libsoxr \
                libvorbis \
                libxml2
            ;;
    esac
}

# Common FFmpeg configure options for audio-only build
get_ffmpeg_configure_opts() {
    cat <<'OPTS'
--prefix=/usr/local
--disable-debug
--enable-shared
--disable-stripping
--disable-autodetect
--enable-gmp
--enable-gnutls
--enable-gpl
--enable-libdrm
--enable-libfribidi
--enable-libsoxr
--enable-libvorbis
--enable-libxml2
--enable-postproc
--enable-swresample
--enable-lto
--disable-encoders
--disable-decoders
--disable-hwaccels
--disable-muxers
--disable-demuxers
--disable-parsers
--disable-bsfs
--disable-protocols
--disable-indevs
--disable-outdevs
--disable-devices
--disable-filters
--disable-doc
--enable-muxer=flac,mov,ipod,wav,w64,ffmetadata
--enable-demuxer=flac,mov,wav,w64,ffmetadata,dsf,dff,aac,hls,mpegts,mp3,ogg,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,lavfi
--enable-encoder=alac,flac,pcm_s16le,pcm_s24le,pcm_s32le
--enable-decoder=alac,flac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,vorbis,aac,aac_fixed,aac_latm,mp3,mp3float,mjpeg,png
--enable-parser=aac,aac_latm,flac,vorbis,mpegaudio,mjpeg
--enable-protocol=file,pipe,http,https,tcp,hls
--enable-filter=aresample,hdcd,sine,anull
--enable-version3
OPTS
}

get_gcc_major_version() {
    gcc -dumpversion 2>/dev/null | cut -d. -f1
}

build_ffmpeg_from_source() {
    local version="$1"
    local extra_flags="${2:-}"

    print_info "Building FFmpeg $version from source..."

    install_ffmpeg_build_deps

    mkdir -p "$FFMPEG_BUILD_DIR"
    cd "$FFMPEG_BUILD_DIR"

    local tarball="ffmpeg-${version}.tar.xz"
    local url="https://ffmpeg.org/releases/$tarball"

    if [ ! -f "$tarball" ]; then
        print_info "Downloading FFmpeg ${version}..."
        if ! wget -q --show-progress "$url"; then
            # Try .tar.bz2 for older versions
            tarball="ffmpeg-${version}.tar.bz2"
            url="https://ffmpeg.org/releases/$tarball"
            print_info "Trying alternative archive format..."
            wget -q --show-progress "$url" || {
                print_error "Failed to download FFmpeg $version"
                return 1
            }
        fi
    fi

    print_info "Extracting FFmpeg..."
    tar xf "$tarball"
    cd "ffmpeg-${version}"

    print_info "Configuring FFmpeg (optimized for audio)..."
    make distclean 2>/dev/null || true

    # Build configure command (convert newlines to spaces)
    local configure_opts
    configure_opts=$(get_ffmpeg_configure_opts | tr '\n' ' ')

    # Check GCC version for compatibility workarounds
    local gcc_ver
    gcc_ver=$(get_gcc_major_version)

    # FFmpeg 5.x has inline asm issues with GCC 14+
    # Disable LTO and inline-asm for compatibility
    local version_major="${version%%.*}"
    if [ "$version_major" = "5" ] && [ "$gcc_ver" -ge 14 ] 2>/dev/null; then
        print_warning "GCC $gcc_ver detected - applying FFmpeg 5.x compatibility workarounds"
        # Remove --enable-lto and add workarounds
        configure_opts="${configure_opts//--enable-lto/}"
        extra_flags="$extra_flags --disable-inline-asm"
    fi

    # Run configure
    ./configure $configure_opts $extra_flags

    print_info "Building FFmpeg (this may take a while)..."
    make -j$(nproc)

    print_info "Installing FFmpeg to /usr/local..."
    sudo make install
    sudo ldconfig

    cd "$SCRIPT_DIR"
}

configure_ffmpeg_paths() {
    print_info "Configuring library paths..."

    # Add to /etc/ld.so.conf.d/ for system-wide recognition
    echo "/usr/local/lib" | sudo tee /etc/ld.so.conf.d/ffmpeg-local.conf > /dev/null
    sudo ldconfig

    # Add to /etc/profile.d/ for all users
    sudo tee /etc/profile.d/ffmpeg-local.sh > /dev/null <<'EOF'
# FFmpeg installed to /usr/local
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
export PATH=/usr/local/bin:$PATH
EOF
    sudo chmod +x /etc/profile.d/ffmpeg-local.sh

    # Source for current session
    export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
    export PATH=/usr/local/bin:$PATH

    print_success "Library paths configured"
}

test_ffmpeg_installation() {
    print_info "Testing FFmpeg installation..."

    local ffmpeg_bin="${1:-/usr/local/bin/ffmpeg}"

    # Fallback to system ffmpeg if local not found
    if [ ! -x "$ffmpeg_bin" ]; then
        ffmpeg_bin=$(which ffmpeg 2>/dev/null || echo "")
    fi

    if [ -z "$ffmpeg_bin" ] || [ ! -x "$ffmpeg_bin" ]; then
        print_error "FFmpeg binary not found"
        return 1
    fi

    # Check version
    local ffmpeg_ver
    ffmpeg_ver=$("$ffmpeg_bin" -version 2>&1 | head -1)
    print_success "FFmpeg: $ffmpeg_ver"

    # Check for required decoders
    print_info "Checking audio decoders..."
    local decoders
    decoders=$("$ffmpeg_bin" -decoders 2>&1)

    local required_decoders="flac alac dsd_lsbf dsd_msbf pcm_s16le pcm_s24le pcm_s32le"
    local all_found=true

    for dec in $required_decoders; do
        if echo "$decoders" | grep -q " $dec "; then
            echo "  [OK] $dec"
        else
            echo "  [MISSING] $dec"
            all_found=false
        fi
    done

    # Check for required demuxers
    print_info "Checking demuxers..."
    local demuxers
    demuxers=$("$ffmpeg_bin" -demuxers 2>&1)

    local required_demuxers="flac wav dsf mov"
    for dem in $required_demuxers; do
        if echo "$demuxers" | grep -q " $dem "; then
            echo "  [OK] $dem"
        else
            echo "  [MISSING] $dem"
            all_found=false
        fi
    done

    # Check for required protocols
    print_info "Checking protocols..."
    local protocols
    protocols=$("$ffmpeg_bin" -protocols 2>&1)

    local required_protocols="http https file"
    for proto in $required_protocols; do
        if echo "$protocols" | grep -q "$proto"; then
            echo "  [OK] $proto"
        else
            echo "  [MISSING] $proto"
            all_found=false
        fi
    done

    if [ "$all_found" = true ]; then
        print_success "All required FFmpeg components found!"
    else
        print_warning "Some FFmpeg components are missing - audio playback may be limited"
    fi

    # Quick decode test
    print_info "Testing decoder functionality..."
    if "$ffmpeg_bin" -f lavfi -i "sine=frequency=1000:duration=0.1" -f null - 2>/dev/null; then
        print_success "FFmpeg decode test passed"
    else
        print_warning "FFmpeg decode test failed - there may be issues"
    fi
}

install_ffmpeg_rpm_fusion() {
    print_info "Installing FFmpeg from RPM Fusion..."

    # Enable RPM Fusion repositories
    print_info "Enabling RPM Fusion repositories..."
    sudo dnf install -y \
        "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm" \
        "https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm" \
        2>/dev/null || true

    # Install FFmpeg
    sudo dnf install -y ffmpeg ffmpeg-devel

    print_success "RPM Fusion FFmpeg installed"
}

install_ffmpeg_system() {
    print_info "Installing FFmpeg from system packages..."

    case $OS in
        fedora|rhel|centos)
            # Try ffmpeg-free first (Fedora repos)
            if ! sudo dnf install -y ffmpeg-free-devel 2>/dev/null; then
                print_warning "ffmpeg-free not available, trying ffmpeg-devel..."
                sudo dnf install -y ffmpeg-devel 2>/dev/null || {
                    print_error "No FFmpeg package found in repositories"
                    print_info "Consider enabling RPM Fusion or building from source"
                    return 1
                }
            fi
            ;;
        ubuntu|debian)
            sudo apt install -y \
                libavformat-dev \
                libavcodec-dev \
                libavutil-dev \
                libswresample-dev
            ;;
        arch|manjaro)
            sudo pacman -Sy --needed --noconfirm ffmpeg
            ;;
    esac

    print_success "System FFmpeg installed"
    print_warning "Note: System FFmpeg may lack some audio codecs (e.g., DSD)"
}

install_ffmpeg() {
    print_header "FFmpeg Installation"

    echo "FFmpeg is required for audio decoding."
    echo ""
    echo "Installation options:"
    echo ""
    echo "  1) Build FFmpeg 7.1 from source (recommended)"
    echo "     - Latest stable with LTO optimization"
    echo "     - Full DSD support, GCC 14/15 compatible"
    echo ""
    if [ "$OS" = "fedora" ]; then
    echo "  2) Install from RPM Fusion (Fedora)"
    echo "     - Pre-built packages with full codec support"
    echo "     - Quick installation"
    echo ""
    echo "  3) Use system packages (minimal)"
    echo "     - Fastest installation"
    echo "     - May lack DSD and some codecs"
    echo ""
    else
    echo "  2) Use system packages (minimal)"
    echo "     - Fastest installation"
    echo "     - May lack DSD and some codecs"
    echo ""
    fi

    local max_option=2
    [ "$OS" = "fedora" ] && max_option=3

    read -p "Choose option [1-$max_option] (default: 1): " FFMPEG_OPTION
    FFMPEG_OPTION=${FFMPEG_OPTION:-1}

    case $FFMPEG_OPTION in
        1)
            build_ffmpeg_from_source "7.1"
            configure_ffmpeg_paths
            rm -rf "$FFMPEG_BUILD_DIR"
            test_ffmpeg_installation "/usr/local/bin/ffmpeg"
            ;;
        2)
            if [ "$OS" = "fedora" ]; then
                install_ffmpeg_rpm_fusion
                test_ffmpeg_installation "$(which ffmpeg)"
            else
                install_ffmpeg_system
                test_ffmpeg_installation "$(which ffmpeg)"
            fi
            ;;
        3)
            if [ "$OS" = "fedora" ]; then
                install_ffmpeg_system
                test_ffmpeg_installation "$(which ffmpeg)"
            else
                print_error "Invalid option"
                exit 1
            fi
            ;;
        *)
            print_error "Invalid option: $FFMPEG_OPTION"
            exit 1
            ;;
    esac
}

# =============================================================================
# DIRETTA SDK
# =============================================================================

check_diretta_sdk() {
    print_header "Diretta SDK Check"

    # Check common SDK locations
    local sdk_locations=(
        "$SDK_PATH"
        "$HOME/DirettaHostSDK_147"
        "$HOME/DirettaHostSDK_147_19"
        "./DirettaHostSDK_147"
        "/opt/DirettaHostSDK_147"
    )

    for loc in "${sdk_locations[@]}"; do
        if [ -d "$loc" ] && [ -d "$loc/lib" ]; then
            SDK_PATH="$loc"
            print_success "Found Diretta SDK at: $SDK_PATH"
            return 0
        fi
    done

    print_warning "Diretta SDK not found"
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

    # Check again
    for loc in "${sdk_locations[@]}"; do
        if [ -d "$loc" ] && [ -d "$loc/lib" ]; then
            SDK_PATH="$loc"
            print_success "Found Diretta SDK at: $SDK_PATH"
            return 0
        fi
    done

    print_error "SDK still not found. Please extract it and try again."
    exit 1
}

# =============================================================================
# BUILD
# =============================================================================

build_renderer() {
    print_header "Building Diretta UPnP Renderer"

    cd "$SCRIPT_DIR"

    if [ ! -f "Makefile" ]; then
        print_error "Makefile not found in $SCRIPT_DIR"
        exit 1
    fi

    # Clean and build
    make clean 2>/dev/null || true

    # Set SDK path via environment variable
    export DIRETTA_SDK_PATH="$SDK_PATH"
    make

    if [ ! -f "bin/DirettaRendererUPnP" ]; then
        print_error "Build failed. Please check error messages above."
        exit 1
    fi

    print_success "Build successful!"
}

# =============================================================================
# NETWORK CONFIGURATION
# =============================================================================

configure_network() {
    print_header "Network Configuration"

    echo "Available network interfaces:"
    ip link show | grep -E "^[0-9]+:" | awk '{print "  " $2}' | sed 's/://g'
    echo ""

    read -p "Enter network interface for Diretta (e.g., enp4s0) or press Enter to skip: " IFACE

    if [ -z "$IFACE" ]; then
        print_info "Skipping network configuration"
        return 0
    fi

    if ! ip link show "$IFACE" &> /dev/null; then
        print_error "Interface $IFACE not found"
        return 1
    fi

    if confirm "Enable jumbo frames (MTU 16128) for better performance?"; then
        sudo ip link set "$IFACE" mtu 16128
        print_success "Jumbo frames enabled (MTU 16128)"

        if confirm "Make this permanent?"; then
            case $OS in
                fedora|rhel|centos)
                    local conn_name
                    conn_name=$(nmcli -t -f NAME,DEVICE connection show 2>/dev/null | grep "$IFACE" | cut -d: -f1)
                    if [ -n "$conn_name" ]; then
                        sudo nmcli connection modify "$conn_name" 802-3-ethernet.mtu 16128
                        print_success "MTU configured permanently in NetworkManager"
                    else
                        print_warning "Could not find NetworkManager connection for $IFACE"
                    fi
                    ;;
                ubuntu|debian)
                    print_info "Add 'mtu 16128' to /etc/network/interfaces for $IFACE"
                    ;;
                *)
                    print_info "Manual configuration required for permanent MTU"
                    ;;
            esac
        fi
    fi

    # Network buffer optimization
    if confirm "Optimize network buffers for audio streaming (16MB)?"; then
        print_info "Setting network buffer sizes..."
        sudo sysctl -w net.core.rmem_max=16777216
        sudo sysctl -w net.core.wmem_max=16777216
        print_success "Network buffers set to 16MB"

        if confirm "Make this permanent?"; then
            sudo tee /etc/sysctl.d/99-diretta.conf > /dev/null <<'SYSCTL'
# Diretta UPnP Renderer - Network buffer optimization
# Larger buffers help with high-resolution audio streaming
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
            sudo sysctl --system > /dev/null
            print_success "Network buffer settings saved to /etc/sysctl.d/99-diretta.conf"
        fi
    fi
}

# =============================================================================
# FIREWALL CONFIGURATION
# =============================================================================

configure_firewall() {
    print_header "Firewall Configuration"

    if ! confirm "Configure firewall to allow UPnP traffic?"; then
        print_info "Skipping firewall configuration"
        return 0
    fi

    case $OS in
        fedora|rhel|centos)
            if command -v firewall-cmd &> /dev/null; then
                sudo firewall-cmd --permanent --add-port=1900/udp  # SSDP
                sudo firewall-cmd --permanent --add-port=4005/tcp  # UPnP HTTP
                sudo firewall-cmd --permanent --add-port=4006/tcp  # UPnP HTTP alt
                sudo firewall-cmd --reload
                print_success "Firewall configured (firewalld)"
            else
                print_info "firewalld not installed, skipping"
            fi
            ;;
        ubuntu|debian)
            if command -v ufw &> /dev/null; then
                sudo ufw allow 1900/udp
                sudo ufw allow 4005/tcp
                sudo ufw allow 4006/tcp
                print_success "Firewall configured (ufw)"
            else
                print_info "ufw not installed, skipping"
            fi
            ;;
        *)
            print_info "Manual firewall configuration required"
            print_info "Open ports: 1900/udp, 4005/tcp, 4006/tcp"
            ;;
    esac
}

# =============================================================================
# SYSTEMD SERVICE
# =============================================================================

setup_systemd_service() {
    print_header "Systemd Service Setup"

    if ! confirm "Create systemd service for auto-start?"; then
        print_info "Skipping systemd service setup"
        return 0
    fi

    local service_file="/etc/systemd/system/diretta-renderer.service"
    local bin_path="$SCRIPT_DIR/bin/DirettaRendererUPnP"

    sudo tee "$service_file" > /dev/null <<EOF
[Unit]
Description=Diretta UPnP Renderer
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=$SCRIPT_DIR/bin
ExecStart=$bin_path --port 4005 --buffer 2.0
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
}

# =============================================================================
# FEDORA AGGRESSIVE OPTIMIZATION (OPTIONAL)
# =============================================================================

optimize_fedora_aggressive() {
    print_header "Aggressive Fedora Optimization"

    if [ "$OS" != "fedora" ]; then
        print_warning "This optimization is only for Fedora systems"
        return 1
    fi

    echo ""
    echo "WARNING: This will make aggressive changes to your system:"
    echo ""
    echo "  - Remove firewalld (firewall disabled)"
    echo "  - Remove SELinux policy (security framework disabled)"
    echo "  - Disable systemd-journald (no persistent logs)"
    echo "  - Disable systemd-oomd (out-of-memory daemon)"
    echo "  - Disable systemd-homed (home directory manager)"
    echo "  - Disable auditd (audit daemon)"
    echo "  - Remove polkit (privilege manager)"
    echo "  - Replace sshd with dropbear (lightweight SSH)"
    echo ""
    echo "This is intended for DEDICATED AUDIO SERVERS ONLY."
    echo "Do NOT use on general-purpose systems or servers with"
    echo "sensitive data."
    echo ""

    if ! confirm "Are you sure you want to proceed with aggressive optimization?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    echo ""
    if ! confirm "FINAL WARNING: This will significantly reduce system security. Continue?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    print_info "Starting aggressive optimization..."

    # Install kernel development tools (for potential future kernel builds)
    print_info "Installing development tools..."
    sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl which || true
    sudo dnf install -y gcc bc bison flex perl elfutils-libelf-devel elfutils-devel openssl openssl-devel rpm-build ncurses-devel || true

    # Disable and remove security services
    print_info "Disabling security services..."

    sudo systemctl disable auditd 2>/dev/null || true
    sudo systemctl stop auditd 2>/dev/null || true

    sudo systemctl stop firewalld 2>/dev/null || true
    sudo systemctl disable firewalld 2>/dev/null || true
    sudo dnf remove -y firewalld 2>/dev/null || true

    sudo dnf remove -y selinux-policy 2>/dev/null || true

    # Disable system services that add overhead
    print_info "Disabling system overhead services..."

    sudo systemctl disable systemd-journald 2>/dev/null || true
    sudo systemctl stop systemd-journald 2>/dev/null || true

    sudo systemctl disable systemd-oomd 2>/dev/null || true
    sudo systemctl stop systemd-oomd 2>/dev/null || true

    sudo systemctl disable systemd-homed 2>/dev/null || true
    sudo systemctl stop systemd-homed 2>/dev/null || true

    sudo systemctl stop polkitd 2>/dev/null || true
    sudo dnf remove -y polkit 2>/dev/null || true

    sudo dnf remove -y gssproxy 2>/dev/null || true

    # Replace sshd with dropbear
    print_info "Installing lightweight SSH server (dropbear)..."
    sudo dnf install -y dropbear || {
        print_warning "Failed to install dropbear, keeping sshd"
    }

    if command -v dropbear &> /dev/null; then
        sudo systemctl enable dropbear || true
        sudo systemctl start dropbear || true

        sudo systemctl disable sshd 2>/dev/null || true
        sudo systemctl stop sshd 2>/dev/null || true

        print_success "Dropbear installed and running"
    fi

    # Network buffer optimization for audio streaming
    print_info "Optimizing network buffers..."
    sudo sysctl -w net.core.rmem_max=16777216
    sudo sysctl -w net.core.wmem_max=16777216
    sudo tee /etc/sysctl.d/99-diretta.conf > /dev/null <<'SYSCTL'
# Diretta UPnP Renderer - Network buffer optimization
# Larger buffers help with high-resolution audio streaming
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
    sudo sysctl --system > /dev/null
    print_success "Network buffers optimized (16MB)"

    # Install useful tools
    sudo dnf install -y htop || true

    print_success "Aggressive optimization complete"
    print_warning "A reboot is recommended to apply all changes"

    if confirm "Reboot now?"; then
        sudo reboot
    fi
}

# =============================================================================
# MAIN MENU
# =============================================================================

show_main_menu() {
    echo ""
    echo "============================================"
    echo " Diretta UPnP Renderer - Installation"
    echo "============================================"
    echo ""
    echo "Installation options:"
    echo ""
    echo "  1) Full installation (recommended)"
    echo "     - Install dependencies, FFmpeg, build, configure"
    echo ""
    echo "  2) Install dependencies only"
    echo "     - Base packages and FFmpeg"
    echo ""
    echo "  3) Build only"
    echo "     - Assumes dependencies are installed"
    echo ""
    echo "  4) Configure only"
    echo "     - Network, firewall, systemd service"
    echo ""
    if [ "$OS" = "fedora" ]; then
    echo "  5) Aggressive Fedora optimization"
    echo "     - For dedicated audio servers only"
    echo ""
    fi
    echo "  q) Quit"
    echo ""
}

run_full_installation() {
    install_base_dependencies
    install_ffmpeg
    check_diretta_sdk
    build_renderer
    configure_network
    configure_firewall
    setup_systemd_service

    print_header "Installation Complete!"

    echo ""
    echo "Quick Start:"
    echo "  1. Start the renderer:"
    echo "     sudo ./bin/DirettaRendererUPnP --port 4005 --buffer 2.0"
    echo ""
    echo "  2. Or use systemd service:"
    echo "     sudo systemctl start diretta-renderer"
    echo ""
    echo "  3. Open your UPnP control point (JPlay, BubbleUPnP, etc.)"
    echo "  4. Select 'Diretta Renderer' as output device"
    echo ""
    echo "Documentation:"
    echo "  - README.md - Overview and quick start"
    echo "  - docs/CONFIGURATION.md - Configuration options"
    echo "  - docs/TROUBLESHOOTING.md - Problem solving"
    echo ""
}

# =============================================================================
# ENTRY POINT
# =============================================================================

main() {
    detect_system

    # Check for command-line arguments
    case "${1:-}" in
        --full|-f)
            run_full_installation
            exit 0
            ;;
        --deps|-d)
            install_base_dependencies
            install_ffmpeg
            exit 0
            ;;
        --build|-b)
            check_diretta_sdk
            build_renderer
            exit 0
            ;;
        --configure|-c)
            configure_network
            configure_firewall
            setup_systemd_service
            exit 0
            ;;
        --optimize|-o)
            optimize_fedora_aggressive
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [OPTION]"
            echo ""
            echo "Options:"
            echo "  --full, -f       Full installation"
            echo "  --deps, -d       Install dependencies only"
            echo "  --build, -b      Build only"
            echo "  --configure, -c  Configure only"
            echo "  --optimize, -o   Aggressive Fedora optimization"
            echo "  --help, -h       Show this help"
            echo ""
            echo "Without options, shows interactive menu."
            exit 0
            ;;
    esac

    # Interactive menu
    while true; do
        show_main_menu

        local max_option=4
        [ "$OS" = "fedora" ] && max_option=5

        read -p "Choose option [1-$max_option/q]: " choice

        case $choice in
            1)
                run_full_installation
                break
                ;;
            2)
                install_base_dependencies
                install_ffmpeg
                print_success "Dependencies installed"
                ;;
            3)
                check_diretta_sdk
                build_renderer
                ;;
            4)
                configure_network
                configure_firewall
                setup_systemd_service
                ;;
            5)
                if [ "$OS" = "fedora" ]; then
                    optimize_fedora_aggressive
                else
                    print_error "Invalid option"
                fi
                ;;
            q|Q)
                print_info "Exiting..."
                exit 0
                ;;
            *)
                print_error "Invalid option: $choice"
                ;;
        esac
    done
}

# Run main
main "$@"
