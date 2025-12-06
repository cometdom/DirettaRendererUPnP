# Diretta UPnP Renderer - Makefile with Manual Architecture Override
# Supports: x64 (v2/v3/v4/zen4), aarch64, riscv64
# 
# Usage:
#   make                              # Auto-detect
#   make ARCH_NAME=x64-linux-15v3     # Manual override
#   make ARCH_NAME=aarch64-linux-15   # Raspberry Pi
#   make NOLOG=1                      # Use -nolog variant

# ============================================
# Compiler Settings
# ============================================

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

# ============================================
# Manual Override Option
# ============================================

UNAME_M := $(shell uname -m)

# Map uname output to Diretta library architecture
ifeq ($(UNAME_M),x86_64)
    DIRETTA_ARCH = x64
    ARCH_DESC = x86_64 (Intel/AMD 64-bit)
else ifeq ($(UNAME_M),aarch64)
    DIRETTA_ARCH = aarch64
    ARCH_DESC = ARM64 (Raspberry Pi 4, etc.)
else ifeq ($(UNAME_M),armv7l)
    DIRETTA_ARCH = arm
    ARCH_DESC = ARM 32-bit (Raspberry Pi 3, etc.)
else ifeq ($(UNAME_M),arm64)
    DIRETTA_ARCH = arm64
    ARCH_DESC = ARM64 (Apple Silicon, etc.)
else
    # ============================================
    # Automatic Architecture Detection
    # ============================================
    
    UNAME_M := $(shell uname -m)
    
    # Detect base architecture
    ifeq ($(UNAME_M),x86_64)
        BASE_ARCH = x64
        ARCH_DESC_BASE = x86_64 (Intel/AMD 64-bit)
    else ifeq ($(UNAME_M),aarch64)
        BASE_ARCH = aarch64
        ARCH_DESC_BASE = ARM64 (aarch64)
    else ifeq ($(UNAME_M),arm64)
        BASE_ARCH = aarch64
        ARCH_DESC_BASE = ARM64 (arm64 → aarch64)
    else ifeq ($(UNAME_M),armv7l)
        BASE_ARCH = arm
        ARCH_DESC_BASE = ARM 32-bit (armv7l)
        $(warning ARM 32-bit detected but not officially supported by Diretta SDK)
    else ifeq ($(UNAME_M),riscv64)
        BASE_ARCH = riscv64
        ARCH_DESC_BASE = RISC-V 64-bit
    else
        BASE_ARCH = unknown
        ARCH_DESC_BASE = Unknown: $(UNAME_M)
    endif
    
    # ============================================
    # Architecture-Specific Variant Detection
    # ============================================
    
    ifeq ($(BASE_ARCH),x64)
        # x64: Auto-detect CPU capabilities
        HAS_AVX2 := $(shell grep -q avx2 /proc/cpuinfo && echo 1 || echo 0)
        HAS_AVX512 := $(shell grep -q avx512 /proc/cpuinfo && echo 1 || echo 0)
        IS_ZEN4 := $(shell lscpu 2>/dev/null | grep -qi "AMD.*Zen 4" && echo 1 || echo 0)
        
        ifeq ($(IS_ZEN4),1)
            DEFAULT_VARIANT = x64-linux-15zen4
            CPU_DESC = AMD Zen 4 detected
        else ifeq ($(HAS_AVX512),1)
            DEFAULT_VARIANT = x64-linux-15v4
            CPU_DESC = AVX512 detected (x86-64-v4)
        else ifeq ($(HAS_AVX2),1)
            DEFAULT_VARIANT = x64-linux-15v3
            CPU_DESC = AVX2 detected (x86-64-v3)
        else
            DEFAULT_VARIANT = x64-linux-15v2
            CPU_DESC = Basic x64 (x86-64-v2)
        endif
        
    else ifeq ($(BASE_ARCH),aarch64)
        # aarch64: Check kernel version
        KERNEL_VER := $(shell uname -r | cut -d. -f1-2)
        KERNEL_MAJOR := $(shell echo $(KERNEL_VER) | cut -d. -f1)
        KERNEL_MINOR := $(shell echo $(KERNEL_VER) | cut -d. -f2)
        
        # Use k16 variant if kernel >= 4.16
        ifeq ($(shell [ $(KERNEL_MAJOR) -gt 4 ] || [ $(KERNEL_MAJOR) -eq 4 -a $(KERNEL_MINOR) -ge 16 ] && echo 1),1)
            DEFAULT_VARIANT = aarch64-linux-15k16
            CPU_DESC = Kernel $(KERNEL_VER) (using k16 variant)
        else
            DEFAULT_VARIANT = aarch64-linux-15
            CPU_DESC = Kernel $(KERNEL_VER) (using standard variant)
        endif
        
    else ifeq ($(BASE_ARCH),riscv64)
        DEFAULT_VARIANT = riscv64-linux-15
        CPU_DESC = RISC-V 64-bit
        
    else
        DEFAULT_VARIANT = unknown
        CPU_DESC = Unknown architecture
    endif
    
    # Allow user to override just the variant part
    ifndef VARIANT
        FULL_VARIANT = $(DEFAULT_VARIANT)
    else
        FULL_VARIANT = $(BASE_ARCH)-linux-$(VARIANT)
    endif
    
    ARCH_DESC = $(ARCH_DESC_BASE) - $(CPU_DESC)
endif

# ============================================
# Add -nolog suffix if requested
# ============================================

ifdef NOLOG
    NOLOG_SUFFIX = -nolog
else
    NOLOG_SUFFIX = 
endif

# ============================================
# Construct Library Names
# ============================================

DIRETTA_LIB_NAME = libDirettaHost_$(FULL_VARIANT)$(NOLOG_SUFFIX).a
ACQUA_LIB_NAME = libACQUA_$(FULL_VARIANT)$(NOLOG_SUFFIX).a

$(info )
$(info ═══════════════════════════════════════════════════════)
$(info   Diretta UPnP Renderer - Build Configuration)
$(info ═══════════════════════════════════════════════════════)
$(info Architecture:  $(ARCH_DESC))
$(info Variant:       $(FULL_VARIANT))
$(info Library:       $(DIRETTA_LIB_NAME))
$(info ═══════════════════════════════════════════════════════)
$(info )

# ============================================
# Diretta SDK Auto-Detection
# ============================================

ifdef DIRETTA_SDK_PATH
    SDK_PATH = $(DIRETTA_SDK_PATH)
    $(info ✓ Using SDK from environment: $(SDK_PATH))
else
    SDK_SEARCH_PATHS = \
        $(HOME)/DirettaHostSDK_147 \
        ./DirettaHostSDK_147 \
        ../DirettaHostSDK_147 \
        /opt/DirettaHostSDK_147 \
        $(HOME)/audio/DirettaHostSDK_147 \
        /usr/local/DirettaHostSDK_147

    SDK_PATH = $(firstword $(foreach path,$(SDK_SEARCH_PATHS),$(wildcard $(path))))

    ifeq ($(SDK_PATH),)
        $(error ❌ Diretta SDK not found! Searched in: $(SDK_SEARCH_PATHS))
    else
        $(info ✓ SDK auto-detected: $(SDK_PATH))
    endif
endif

# ============================================
# Verify SDK Installation
# ============================================

# Diretta library naming pattern: libDirettaHost_<arch>-linux-15v3.so
# Only x86_64 and aarch64 are supported
ifeq ($(DIRETTA_ARCH),x64)
    DIRETTA_LIB_NAME = libDirettaHost_$(DIRETTA_ARCH)-linux-15v3.a
    ACQUA_LIB_NAME = libACQUA_$(DIRETTA_ARCH)-linux-15v3.a
    DIRETTA_LIB_SUFFIX = 15v3
else ifeq ($(DIRETTA_ARCH),aarch64)
    DIRETTA_LIB_NAME = libDirettaHost_$(DIRETTA_ARCH)-linux-15.a
    ACQUA_LIB_NAME = libACQUA_$(DIRETTA_ARCH)-linux-15.a
    DIRETTA_LIB_SUFFIX = 15
else
    $(error ❌ Unsupported architecture for library linking: $(DIRETTA_ARCH). Only x64 (x86_64) and aarch64 are supported.)
endif

# Full paths to libraries
SDK_LIB_DIRETTA = $(SDK_PATH)/lib/$(DIRETTA_LIB_NAME)
SDK_LIB_ACQUA = $(SDK_PATH)/lib/$(ACQUA_LIB_NAME)

ifeq (,$(wildcard $(SDK_LIB_DIRETTA)))
    $(info )
    $(info ❌ Required library not found: $(DIRETTA_LIB_NAME))
    $(info    Path checked: $(SDK_LIB_DIRETTA))
    $(info )
    $(info 📝 Available libraries in SDK:)
    $(info $(shell ls -1 $(SDK_PATH)/lib/libDirettaHost_*.a 2>/dev/null | sed 's|.*/libDirettaHost_||' | sed 's|\.a||' || echo "    No libraries found"))
    $(info )
    $(info 💡 Common solutions:)
    $(info )
    $(info   For Raspberry Pi:)
    $(info     make ARCH_NAME=aarch64-linux-15)
    $(info )
    $(info   For x64 systems:)
    $(info     make ARCH_NAME=x64-linux-15v2       # Baseline)
    $(info     make ARCH_NAME=x64-linux-15v3       # AVX2 (most common))
    $(info     make ARCH_NAME=x64-linux-15v4       # AVX512)
    $(info     make ARCH_NAME=x64-linux-15zen4     # AMD Ryzen 7000+)
    $(info )
    $(info   For RISC-V:)
    $(info     make ARCH_NAME=riscv64-linux-15)
    $(info )
    $(info   Or run: make list-variants)
    $(info )
    $(error Build failed: library not found)
endif

ifeq (,$(wildcard $(SDK_LIB_ACQUA)))
    $(warning ⚠️  ACQUA library not found: $(ACQUA_LIB_NAME))
endif

SDK_HEADER = $(SDK_PATH)/Host/Diretta/SyncBuffer
ifeq (,$(wildcard $(SDK_HEADER)))
    $(error ❌ SDK headers not found at: $(SDK_PATH)/Host/)
endif

$(info ✓ SDK validation passed)
$(info )

# ============================================
# Include and Library Paths
# ============================================

INCLUDES = \
    -I/usr/include/ffmpeg \
    -I/usr/include/upnp \
    -I/usr/local/include \
    -I. \
    -I$(SDK_PATH)/Host

LDFLAGS += \
    -L/usr/local/lib \
    -L$(SDK_PATH)/lib

LIBS = \
    -lupnp \
    -lixml \
    -lpthread \
    -lDirettaHost_$(DIRETTA_ARCH)-linux-$(DIRETTA_LIB_SUFFIX) \
    -lavformat \
    -lavcodec \
    -lavutil \
    -lswresample

ifneq (,$(wildcard $(SDK_LIB_ACQUA)))
    LIBS += -lACQUA_$(DIRETTA_ARCH)-linux-$(DIRETTA_LIB_SUFFIX)
    $(info ✓ ACQUA library will be linked)
endif

# ============================================
# Source Files
# ============================================

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = \
    $(SRCDIR)/main.cpp \
    $(SRCDIR)/DirettaRenderer.cpp \
    $(SRCDIR)/AudioEngine.cpp \
    $(SRCDIR)/DirettaOutput.cpp \
    $(SRCDIR)/UPnPDevice.cpp

OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DEPENDS = $(OBJECTS:.o=.d)

TARGET = $(BINDIR)/DirettaRendererUPnP

# ============================================
# Build Rules
# ============================================

.PHONY: all clean info help list-variants examples

all: $(TARGET)
	@echo ""
	@echo "✓ Build complete: $(TARGET)"
	@echo "✓ Using: $(DIRETTA_LIB_NAME)"

$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(BINDIR):
	@mkdir -p $(BINDIR)

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "✓ Clean complete"

# ============================================
# Information Commands
# ============================================

info:
	@echo "════════════════════════════════════════════════════════"
	@echo " Diretta UPnP Renderer - Detailed Configuration"
	@echo "════════════════════════════════════════════════════════"
	@echo ""
	@echo "System:"
	@echo "  uname -m:     $(UNAME_M)"
	@echo "  Base Arch:    $(BASE_ARCH)"
	@echo ""
	@echo "Selected:"
	@echo "  Variant:      $(FULL_VARIANT)"
	@echo "  Library:      $(DIRETTA_LIB_NAME)"
	@echo "  No-Log:       $(if $(NOLOG),Yes,No)"
	@echo ""
	@echo "SDK:"
	@echo "  Path:         $(SDK_PATH)"
	@echo "  Diretta Lib:  $(SDK_LIB_DIRETTA)"
	@echo "  ACQUA Lib:    $(SDK_LIB_ACQUA)"
	@echo ""
	@echo "Build:"
	@echo "  Compiler:     $(CXX)"
	@echo "  Target:       $(TARGET)"
	@echo ""
	@echo "════════════════════════════════════════════════════════"

list-variants:
	@echo "════════════════════════════════════════════════════════"
	@echo " Available Libraries in SDK"
	@echo "════════════════════════════════════════════════════════"
	@echo ""
	@echo "All available variants:"
	@ls -1 $(SDK_PATH)/lib/libDirettaHost_*.a 2>/dev/null | sed 's|.*/libDirettaHost_||' | sed 's|\.a||' | sed 's|^|  ✓ |' || echo "  ❌ No libraries found"
	@echo ""
	@echo "════════════════════════════════════════════════════════"

examples:
	@echo "════════════════════════════════════════════════════════"
	@echo " Build Examples"
	@echo "════════════════════════════════════════════════════════"
	@echo ""
	@echo "Auto-detection (recommended):"
	@echo "  make"
	@echo ""
	@echo "Manual override for Raspberry Pi:"
	@echo "  make ARCH_NAME=aarch64-linux-15"
	@echo "  make ARCH_NAME=aarch64-linux-15k16"
	@echo ""
	@echo "Manual override for x64:"
	@echo "  make ARCH_NAME=x64-linux-15v2       # Baseline (SSE4)"
	@echo "  make ARCH_NAME=x64-linux-15v3       # AVX2 (most common)"
	@echo "  make ARCH_NAME=x64-linux-15v4       # AVX512"
	@echo "  make ARCH_NAME=x64-linux-15zen4     # AMD Zen 4"
	@echo ""
	@echo "Manual override for RISC-V:"
	@echo "  make ARCH_NAME=riscv64-linux-15"
	@echo ""
	@echo "Disable logging (any architecture):"
	@echo "  make ARCH_NAME=x64-linux-15v3 NOLOG=1"
	@echo "  make ARCH_NAME=aarch64-linux-15 NOLOG=1"
	@echo ""
	@echo "Musl libc variants (if needed):"
	@echo "  make ARCH_NAME=x64-linux-musl15zen4"
	@echo "  make ARCH_NAME=aarch64-linux-musl15"
	@echo ""
	@echo "════════════════════════════════════════════════════════"

help:
	@echo "Diretta UPnP Renderer - Makefile"
	@echo ""
	@echo "Commands:"
	@echo "  make              Auto-detect architecture and build"
	@echo "  make clean        Remove build artifacts"
	@echo "  make info         Show detailed configuration"
	@echo "  make list-variants List all SDK library variants"
	@echo "  make examples     Show build command examples"
	@echo "  make help         Show this help"
	@echo ""
	@echo "Options:"
	@echo "  ARCH_NAME=<variant>  Manually specify library variant"
	@echo "  NOLOG=1              Use -nolog version"
	@echo "  DIRETTA_SDK_PATH=<path>  Custom SDK location"
	@echo ""
	@echo "Common usage:"
	@echo ""
	@echo "  Raspberry Pi:"
	@echo "    make ARCH_NAME=aarch64-linux-15"
	@echo ""
	@echo "  x64 PC with AVX2:"
	@echo "    make ARCH_NAME=x64-linux-15v3"
	@echo ""
	@echo "  Auto-detect (tries to find best match):"
	@echo "    make"
	@echo ""
	@echo "For more examples: make examples"

-include $(DEPENDS)
