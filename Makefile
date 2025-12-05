# Diretta UPnP Renderer - Makefile with Auto-Detection
# Automatically detects SDK location AND system architecture

# ============================================
# Compiler Settings
# ============================================

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

# ============================================
# Architecture Detection
# ============================================

UNAME_M := $(shell uname -m)

# Map uname output to Diretta library architecture
ifeq ($(UNAME_M),x86_64)
    DIRETTA_ARCH = x64
    ARCH_DESC = x86_64 (Intel/AMD 64-bit)
else ifeq ($(UNAME_M),aarch64)
    DIRETTA_ARCH = arm64
    ARCH_DESC = ARM64 (Raspberry Pi 4, etc.)
else ifeq ($(UNAME_M),armv7l)
    DIRETTA_ARCH = arm
    ARCH_DESC = ARM 32-bit (Raspberry Pi 3, etc.)
else ifeq ($(UNAME_M),arm64)
    DIRETTA_ARCH = arm64
    ARCH_DESC = ARM64 (Apple Silicon, etc.)
else
    DIRETTA_ARCH = unknown
    ARCH_DESC = Unknown architecture: $(UNAME_M)
endif

$(info ✓ Detected architecture: $(ARCH_DESC))

# Check if architecture is supported
ifeq ($(DIRETTA_ARCH),unknown)
    $(error ❌ Unsupported architecture: $(UNAME_M). Supported: x86_64, aarch64, arm64, armv7l)
endif

# ============================================
# Diretta SDK Auto-Detection
# ============================================

# Method 1: Check environment variable first
ifdef DIRETTA_SDK_PATH
    SDK_PATH = $(DIRETTA_SDK_PATH)
    $(info ✓ Using SDK from environment: $(SDK_PATH))
else
    # Method 2: Search common locations
    SDK_SEARCH_PATHS = \
        $(HOME)/DirettaHostSDK_147 \
        ./DirettaHostSDK_147 \
        ../DirettaHostSDK_147 \
        /opt/DirettaHostSDK_147 \
        $(HOME)/audio/DirettaHostSDK_147 \
        /usr/local/DirettaHostSDK_147

    # Find first existing path
    SDK_PATH = $(firstword $(foreach path,$(SDK_SEARCH_PATHS),$(wildcard $(path))))

    # Check if SDK was found
    ifeq ($(SDK_PATH),)
        $(error ❌ Diretta SDK not found! Searched in: $(SDK_SEARCH_PATHS). \
                Please download from https://www.diretta.link/hostsdk.html or set DIRETTA_SDK_PATH environment variable)
    else
        $(info ✓ SDK auto-detected: $(SDK_PATH))
    endif
endif

# ============================================
# Architecture-Specific Library Selection
# ============================================

# Diretta library naming pattern: libDirettaHost_<arch>-linux-15v3.so
DIRETTA_LIB_NAME = libDirettaHost_$(DIRETTA_ARCH)-linux-15v3.a
ACQUA_LIB_NAME = libACQUA_$(DIRETTA_ARCH)-linux-15v3.a

# Full paths to libraries
SDK_LIB_DIRETTA = $(SDK_PATH)/lib/$(DIRETTA_LIB_NAME)
SDK_LIB_ACQUA = $(SDK_PATH)/lib/$(ACQUA_LIB_NAME)

$(info ✓ Looking for: $(DIRETTA_LIB_NAME))

# ============================================
# Verify SDK Installation
# ============================================

# Check if Diretta library exists
ifeq (,$(wildcard $(SDK_LIB_DIRETTA)))
    $(info )
    $(info ❌ Diretta library not found!)
    $(info    Expected: $(SDK_LIB_DIRETTA))
    $(info )
    $(info 📝 Available libraries in $(SDK_PATH)/lib/:)
    $(info $(shell ls -1 $(SDK_PATH)/lib/libDirettaHost*.so 2>/dev/null || echo "    No libraries found"))
    $(info )
    $(error Please ensure you have the correct Diretta SDK for $(ARCH_DESC))
endif

# Check if ACQUA library exists
ifeq (,$(wildcard $(SDK_LIB_ACQUA)))
    $(warning ⚠️  ACQUA library not found at: $(SDK_LIB_ACQUA))
    $(warning    This may be normal if your SDK version doesn't include ACQUA)
endif

# Check if SDK headers exist
SDK_HEADER = $(SDK_PATH)/Host/Diretta/SyncBuffer
ifeq (,$(wildcard $(SDK_HEADER)))
    $(error ❌ SDK headers not found at: $(SDK_PATH)/Host/. Please check SDK installation)
endif

$(info ✓ SDK validation passed for $(ARCH_DESC))

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

# Architecture-specific library linking
LIBS = \
    -lupnp \
    -lixml \
    -lpthread \
    -lDirettaHost_$(DIRETTA_ARCH)-linux-15v3 \
    -lavformat \
    -lavcodec \
    -lavutil \
    -lswresample

# Add ACQUA library if it exists
ifneq (,$(wildcard $(SDK_LIB_ACQUA)))
    LIBS += -lACQUA_$(DIRETTA_ARCH)-linux-15v3
    $(info ✓ ACQUA library will be linked)
endif

# ============================================
# Source Files
# ============================================

SRCDIR = src
OBJDIR = obj
BINDIR = bin

# Source files (adjust paths if your sources are in src/)
SOURCES = \
    $(SRCDIR)/main.cpp \
    $(SRCDIR)/DirettaRenderer.cpp \
    $(SRCDIR)/AudioEngine.cpp \
    $(SRCDIR)/DirettaOutput.cpp \
    $(SRCDIR)/UPnPDevice.cpp

# If sources are in root directory instead, uncomment:
# SOURCES = main.cpp DirettaRenderer.cpp AudioEngine.cpp DirettaOutput.cpp UPnPDevice.cpp

OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DEPENDS = $(OBJECTS:.o=.d)

TARGET = $(BINDIR)/DirettaRendererUPnP

# ============================================
# Build Rules
# ============================================

.PHONY: all clean info help install arch-info

# Default target
all: $(TARGET)
	@echo "✓ Build complete: $(TARGET)"
	@echo "✓ Built for: $(ARCH_DESC)"

# Link
$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $(TARGET) for $(DIRETTA_ARCH)..."
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

# Compile
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Create directories
$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(BINDIR):
	@mkdir -p $(BINDIR)

# Clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "✓ Clean complete"

# Show configuration
info:
	@echo "============================================"
	@echo " Diretta UPnP Renderer - Build Configuration"
	@echo "============================================"
	@echo "System Architecture:  $(UNAME_M)"
	@echo "Diretta Architecture: $(DIRETTA_ARCH)"
	@echo "Architecture Desc:    $(ARCH_DESC)"
	@echo ""
	@echo "SDK Path:             $(SDK_PATH)"
	@echo "Diretta Library:      $(DIRETTA_LIB_NAME)"
	@echo "Library Path:         $(SDK_LIB_DIRETTA)"
	@echo "Library Exists:       $(if $(wildcard $(SDK_LIB_DIRETTA)),✓ Yes,❌ No)"
	@echo ""
	@echo "ACQUA Library:        $(ACQUA_LIB_NAME)"
	@echo "ACQUA Path:           $(SDK_LIB_ACQUA)"
	@echo "ACQUA Exists:         $(if $(wildcard $(SDK_LIB_ACQUA)),✓ Yes,⚠️  No (optional))"
	@echo ""
	@echo "SDK Headers:          $(SDK_HEADER)"
	@echo "Headers Exist:        $(if $(wildcard $(SDK_HEADER)),✓ Yes,❌ No)"
	@echo ""
	@echo "Compiler:             $(CXX)"
	@echo "Flags:                $(CXXFLAGS)"
	@echo "Sources:              $(SOURCES)"
	@echo "Target:               $(TARGET)"
	@echo "============================================"

# Show architecture detection info
arch-info:
	@echo "============================================"
	@echo " Architecture Detection"
	@echo "============================================"
	@echo "uname -m output:      $(UNAME_M)"
	@echo "Mapped to:            $(DIRETTA_ARCH)"
	@echo "Description:          $(ARCH_DESC)"
	@echo ""
	@echo "Supported architectures:"
	@echo "  - x86_64  → x64   (Intel/AMD 64-bit)"
	@echo "  - aarch64 → arm64 (ARM 64-bit, Raspberry Pi 4+)"
	@echo "  - arm64   → arm64 (Apple Silicon, etc.)"
	@echo "  - armv7l  → arm   (ARM 32-bit, Raspberry Pi 3)"
	@echo ""
	@echo "Expected library: $(DIRETTA_LIB_NAME)"
	@echo "============================================"

# Help
help:
	@echo "Diretta UPnP Renderer - Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make           Build the renderer (auto-detects architecture)"
	@echo "  make clean     Remove build artifacts"
	@echo "  make info      Show detailed build configuration"
	@echo "  make arch-info Show architecture detection details"
	@echo "  make help      Show this help message"
	@echo ""
	@echo "Architecture Detection:"
	@echo "  The Makefile automatically detects your system architecture and"
	@echo "  selects the appropriate Diretta library:"
	@echo "    - x86_64  → libDirettaHost_x64-linux-15v3.so"
	@echo "    - aarch64 → libDirettaHost_arm64-linux-15v3.so"
	@echo "    - armv7l  → libDirettaHost_arm-linux-15v3.so"
	@echo ""
	@echo "SDK Detection:"
	@echo "  The Makefile automatically searches for DirettaHostSDK_147 in:"
	@echo "    - ~/DirettaHostSDK_147"
	@echo "    - ./DirettaHostSDK_147"
	@echo "    - ../DirettaHostSDK_147"
	@echo "    - /opt/DirettaHostSDK_147"
	@echo ""
	@echo "  To specify a custom SDK location:"
	@echo "    export DIRETTA_SDK_PATH=/path/to/sdk"
	@echo "    make"
	@echo ""
	@echo "  Or use it inline:"
	@echo "    make DIRETTA_SDK_PATH=/path/to/sdk"
	@echo ""
	@echo "Troubleshooting:"
	@echo "  If library not found, run: make arch-info"
	@echo "  This shows what library is expected for your architecture."
	@echo ""

# Include dependencies
-include $(DEPENDS)
