#!/bin/bash
# check-sdk.sh - Diretta SDK Installation Checker for DirettaRendererUPnP
# Author: Dominique COMET
# Version: 1.0.0

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo ""
echo "════════════════════════════════════════════════════════"
echo "  DirettaRendererUPnP - SDK Installation Checker"
echo "════════════════════════════════════════════════════════"
echo ""

# ============================================
# Function: Check if SDK exists in a path
# ============================================
check_sdk_path() {
    local path=$1
    if [ -d "$path" ]; then
        echo -e "${GREEN}✓${NC} Found: $path"
        return 0
    fi
    return 1
}

# ============================================
# Step 1: Search for SDK
# ============================================
echo "🔍 Searching for Diretta SDK..."
echo ""

SDK_SEARCH_PATHS=(
    "$HOME/DirettaHostSDK_147"
    "./DirettaHostSDK_147"
    "../DirettaHostSDK_147"
    "/opt/DirettaHostSDK_147"
    "$HOME/audio/DirettaHostSDK_147"
    "/usr/local/DirettaHostSDK_147"
)

SDK_PATH=""
for path in "${SDK_SEARCH_PATHS[@]}"; do
    if check_sdk_path "$path"; then
        SDK_PATH="$path"
        break
    fi
done

if [ -z "$SDK_PATH" ]; then
    echo -e "${RED}❌ SDK not found!${NC}"
    echo ""
    echo "Searched in:"
    for path in "${SDK_SEARCH_PATHS[@]}"; do
        echo "  - $path"
    done
    echo ""
    echo -e "${YELLOW}📥 How to install:${NC}"
    echo ""
    echo "1. Download SDK from:"
    echo "   https://www.diretta.link/hostsdk.html"
    echo ""
    echo "2. Extract the downloaded file:"
    echo "   tar -xzf DirettaHostSDK_vX.X.X_Linux_*.tar.gz"
    echo ""
    echo "3. Move/rename to one of the searched locations:"
    echo "   mv DirettaHostSDK ~/DirettaHostSDK_147"
    echo ""
    echo "   OR set environment variable:"
    echo "   export DIRETTA_SDK_PATH=/path/to/your/sdk"
    echo ""
    echo "4. Run this script again to verify"
    echo ""
    exit 1
fi

# ============================================
# Step 2: Verify SDK Structure
# ============================================
echo ""
echo "🔍 Verifying SDK structure..."
echo ""

# Check headers
if [ ! -d "$SDK_PATH/Host/Diretta" ]; then
    echo -e "${RED}❌ Headers not found${NC} at: $SDK_PATH/Host/Diretta"
    echo ""
    echo "Your SDK installation appears incomplete."
    echo "Please re-download and extract the SDK."
    exit 1
else
    echo -e "${GREEN}✓${NC} Headers found"
fi

# Check lib directory
if [ ! -d "$SDK_PATH/lib" ]; then
    echo -e "${RED}❌ Library directory not found${NC} at: $SDK_PATH/lib"
    exit 1
else
    echo -e "${GREEN}✓${NC} Library directory found"
fi

# ============================================
# Step 3: List Available Libraries
# ============================================
echo ""
echo "📚 Available library variants:"
echo ""

DIRETTA_LIBS=($(ls "$SDK_PATH/lib/libDirettaHost_"*.a 2>/dev/null || true))

if [ ${#DIRETTA_LIBS[@]} -eq 0 ]; then
    echo -e "${RED}❌ No Diretta libraries found!${NC}"
    echo ""
    echo "Your SDK lib directory is empty or corrupted."
    exit 1
fi

VARIANTS=()
for lib in "${DIRETTA_LIBS[@]}"; do
    # Extract variant name from library filename
    variant=$(basename "$lib" | sed 's/libDirettaHost_//' | sed 's/\.a$//')
    VARIANTS+=("$variant")
    echo -e "  ${GREEN}✓${NC} $variant"
done

# Check for ACQUA libraries
echo ""
echo "📚 ACQUA libraries (optional):"
echo ""

ACQUA_LIBS=($(ls "$SDK_PATH/lib/libACQUA_"*.a 2>/dev/null || true))
if [ ${#ACQUA_LIBS[@]} -eq 0 ]; then
    echo -e "  ${YELLOW}⚠${NC}  No ACQUA libraries found (this is OK, they're optional)"
else
    for lib in "${ACQUA_LIBS[@]}"; do
        variant=$(basename "$lib" | sed 's/libACQUA_//' | sed 's/\.a$//')
        echo -e "  ${GREEN}✓${NC} $variant"
    done
fi

# ============================================
# Step 4: Detect System Architecture
# ============================================
echo ""
echo "🖥️  Detecting your system..."
echo ""

UNAME_M=$(uname -m)
echo "Architecture: $UNAME_M"

# Detect base arch
case "$UNAME_M" in
    x86_64)
        BASE_ARCH="x64"
        echo -e "Base: ${GREEN}x64${NC} (Intel/AMD 64-bit)"
        
        # Detect CPU capabilities
        if grep -q avx512 /proc/cpuinfo 2>/dev/null; then
            RECOMMENDED="x64-linux-15v4"
            echo -e "CPU Features: ${GREEN}AVX512${NC} detected (x86-64-v4)"
        elif grep -q avx2 /proc/cpuinfo 2>/dev/null; then
            RECOMMENDED="x64-linux-15v3"
            echo -e "CPU Features: ${GREEN}AVX2${NC} detected (x86-64-v3)"
        else
            RECOMMENDED="x64-linux-15v2"
            echo -e "CPU Features: ${YELLOW}Basic x64${NC} (x86-64-v2)"
        fi
        
        # Check for AMD Zen 4
        if lscpu 2>/dev/null | grep -qi "AMD.*Zen 4"; then
            RECOMMENDED="x64-linux-15zen4"
            echo -e "CPU Model: ${GREEN}AMD Zen 4${NC} detected"
        fi
        ;;
        
    aarch64|arm64)
        BASE_ARCH="aarch64"
        echo -e "Base: ${GREEN}aarch64${NC} (ARM 64-bit)"
        
        # Check kernel version
        KERNEL_VER=$(uname -r | cut -d. -f1-2)
        KERNEL_MAJOR=$(echo $KERNEL_VER | cut -d. -f1)
        KERNEL_MINOR=$(echo $KERNEL_VER | cut -d. -f2)
        
        if [ "$KERNEL_MAJOR" -gt 4 ] || [ "$KERNEL_MAJOR" -eq 4 -a "$KERNEL_MINOR" -ge 16 ]; then
            RECOMMENDED="aarch64-linux-15k16"
            echo -e "Kernel: ${GREEN}$KERNEL_VER${NC} (k16 variant recommended)"
        else
            RECOMMENDED="aarch64-linux-15"
            echo -e "Kernel: ${YELLOW}$KERNEL_VER${NC} (standard variant)"
        fi
        ;;
        
    riscv64)
        BASE_ARCH="riscv64"
        RECOMMENDED="riscv64-linux-15"
        echo -e "Base: ${GREEN}riscv64${NC} (RISC-V 64-bit)"
        ;;
        
    *)
        BASE_ARCH="unknown"
        RECOMMENDED=""
        echo -e "Base: ${RED}Unknown${NC} ($UNAME_M)"
        ;;
esac

# ============================================
# Step 5: Check if Recommended Library Exists
# ============================================
echo ""
echo "💡 Recommendation:"
echo ""

if [ -n "$RECOMMENDED" ]; then
    # Check if recommended variant exists
    FOUND=false
    for variant in "${VARIANTS[@]}"; do
        if [ "$variant" = "$RECOMMENDED" ]; then
            FOUND=true
            break
        fi
    done
    
    if $FOUND; then
        echo -e "  ${GREEN}✓${NC} Recommended variant available: ${GREEN}$RECOMMENDED${NC}"
        echo ""
        echo "  Use: ${BLUE}make ARCH_NAME=$RECOMMENDED${NC}"
    else
        echo -e "  ${YELLOW}⚠${NC}  Recommended variant not found: ${YELLOW}$RECOMMENDED${NC}"
        echo ""
        echo "  Available alternatives for your system:"
        for variant in "${VARIANTS[@]}"; do
            if [[ "$variant" == "$BASE_ARCH"* ]]; then
                echo -e "    ${GREEN}✓${NC} $variant"
                echo "       make ARCH_NAME=$variant"
            fi
        done
    fi
else
    echo -e "  ${YELLOW}⚠${NC}  Unable to auto-detect. Please choose manually:"
    for variant in "${VARIANTS[@]}"; do
        echo "    - $variant"
    done
fi

# ============================================
# Step 6: Check for Required Dependencies
# ============================================
echo ""
echo "🔍 Checking for build dependencies..."
echo ""

MISSING_DEPS=()

# Check for g++
if ! command -v g++ &> /dev/null; then
    echo -e "${RED}❌${NC} g++ not found"
    MISSING_DEPS+=("g++")
else
    echo -e "${GREEN}✓${NC} g++ found"
fi

# Check for make
if ! command -v make &> /dev/null; then
    echo -e "${RED}❌${NC} make not found"
    MISSING_DEPS+=("make")
else
    echo -e "${GREEN}✓${NC} make found"
fi

# Check for UPnP library
if ! ldconfig -p | grep -q libupnp 2>/dev/null; then
    echo -e "${YELLOW}⚠${NC}  libupnp not found (may cause build failure)"
    MISSING_DEPS+=("libupnp")
else
    echo -e "${GREEN}✓${NC} libupnp found"
fi

# Check for FFmpeg
if ! pkg-config --exists libavformat libavcodec 2>/dev/null; then
    echo -e "${YELLOW}⚠${NC}  FFmpeg libraries not found (may cause build failure)"
    MISSING_DEPS+=("ffmpeg-devel")
else
    echo -e "${GREEN}✓${NC} FFmpeg libraries found"
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}⚠ Some dependencies are missing${NC}"
    echo ""
    echo "Install them with:"
    echo ""
    
    # Detect distro
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu|debian)
                echo "  sudo apt-get install build-essential libupnp-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev"
                ;;
            fedora)
                echo "  sudo dnf install gcc-c++ make libupnp-devel ffmpeg-devel"
                ;;
            arch|archlinux|manjaro)
                echo "  sudo pacman -S base-devel libupnp ffmpeg"
                ;;
            gentoo)
                echo "  emerge --ask sys-devel/gcc dev-util/make net-libs/libupnp media-video/ffmpeg"
                ;;
            *)
                echo "  (install build tools, libupnp, and ffmpeg using your package manager)"
                ;;
        esac
    fi
fi

# ============================================
# Step 7: Summary and Next Steps
# ============================================
echo ""
echo "════════════════════════════════════════════════════════"
echo "  Summary"
echo "════════════════════════════════════════════════════════"
echo ""

if [ -n "$RECOMMENDED" ]; then
    # Check if recommended exists
    FOUND=false
    for variant in "${VARIANTS[@]}"; do
        if [ "$variant" = "$RECOMMENDED" ]; then
            FOUND=true
            break
        fi
    done
    
    if $FOUND; then
        echo -e "${GREEN}✓ Everything looks good!${NC}"
        echo ""
        echo "Ready to build DirettaRendererUPnP with:"
        echo ""
        echo -e "  ${BLUE}make ARCH_NAME=$RECOMMENDED${NC}"
        echo ""
        echo "Or let make auto-detect:"
        echo ""
        echo -e "  ${BLUE}make${NC}"
        echo ""
        echo "After building, run:"
        echo ""
        echo "  ./bin/DirettaRendererUPnP --list-targets"
        echo ""
    else
        echo -e "${YELLOW}⚠ SDK found but recommended variant missing${NC}"
        echo ""
        echo "Choose one of the available variants and build with:"
        echo ""
        echo "  make ARCH_NAME=<variant>"
        echo ""
    fi
else
    echo -e "${YELLOW}⚠ Unable to auto-detect best variant${NC}"
    echo ""
    echo "Please choose manually from available variants above."
    echo ""
fi

echo "════════════════════════════════════════════════════════"
echo ""
