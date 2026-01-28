# Diretta UPnP Renderer - Makefile (Simplified Architecture)
# Uses unified DirettaSync class (merged from DirettaSyncAdapter + DirettaOutput)
# Based on MPD Diretta Output Plugin v0.4.0
#
# Usage:
#   make                              # Build with auto-detect
#   make ARCH_NAME=x64-linux-15v3     # Manual architecture

# ============================================
# Compiler Settings
# ============================================

CXX = g++
CC = gcc
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
CFLAGS = -O3 -Wall
LDFLAGS = -pthread

# ============================================
# Architecture Detection (unchanged from original)
# ============================================

UNAME_M := $(shell uname -m)

ifeq ($(UNAME_M),x86_64)
    BASE_ARCH = x64
else ifeq ($(UNAME_M),aarch64)
    BASE_ARCH = aarch64
else ifeq ($(UNAME_M),arm64)
    BASE_ARCH = aarch64
else ifeq ($(UNAME_M),riscv64)
    BASE_ARCH = riscv64
else
    BASE_ARCH = unknown
endif

ifeq ($(BASE_ARCH),x64)
    HAS_AVX2   := $(shell grep -q avx2 /proc/cpuinfo 2>/dev/null && echo 1 || echo 0)
    HAS_AVX512 := $(shell grep -q avx512 /proc/cpuinfo 2>/dev/null && echo 1 || echo 0)

    # Zen4 detection: Ryzen 7000/8000/9000 series, EPYC 9004, Threadripper 7000
    # Also check for "znver4" in gcc's output (more reliable)
    IS_ZEN4    := $(shell grep -m1 "model name" /proc/cpuinfo 2>/dev/null | grep -qiE "(Ryzen.*(5|7|9)[- ]*(7[0-9]{3}|8[0-9]{3}|9[0-9]{3})|EPYC.*90[0-9]{2}|Threadripper.*7[0-9]{3})" && echo 1 || echo 0)

    # Fallback: Check if compiler supports znver4 and CPU has AVX-512 + specific Zen4 features
    ifeq ($(IS_ZEN4),0)
        IS_ZEN4 := $(shell grep -q "avx512vbmi2" /proc/cpuinfo 2>/dev/null && grep -q "vaes" /proc/cpuinfo 2>/dev/null && echo 1 || echo 0)
    endif

    ifeq ($(IS_ZEN4),1)
        DEFAULT_VARIANT = x64-linux-15zen4
    else ifeq ($(HAS_AVX512),1)
        DEFAULT_VARIANT = x64-linux-15v4
    else ifeq ($(HAS_AVX2),1)
        DEFAULT_VARIANT = x64-linux-15v3
    else
        DEFAULT_VARIANT = x64-linux-15v2
    endif

else ifeq ($(BASE_ARCH),aarch64)
    PAGE_SIZE := $(shell getconf PAGESIZE 2>/dev/null || echo 4096)
    IS_RPI5 := $(shell [ -r /proc/device-tree/model ] && grep -q "Raspberry Pi 5" /proc/device-tree/model 2>/dev/null && echo 1 || echo 0)

    ifeq ($(IS_RPI5),1)
        DEFAULT_VARIANT = aarch64-linux-15k16
    else ifeq ($(PAGE_SIZE),16384)
        DEFAULT_VARIANT = aarch64-linux-15k16
    else
        DEFAULT_VARIANT = aarch64-linux-15
    endif

else ifeq ($(BASE_ARCH),riscv64)
    DEFAULT_VARIANT = riscv64-linux-15
else
    DEFAULT_VARIANT = unknown
endif

ifdef ARCH_NAME
    FULL_VARIANT = $(ARCH_NAME)
else
    FULL_VARIANT = $(DEFAULT_VARIANT)
endif

# ============================================
# Architecture-specific compiler flags
# ============================================

DIRETTA_ARCH = $(word 1,$(subst -, ,$(FULL_VARIANT)))

ifeq ($(DIRETTA_ARCH),x64)
    # Zen4: Full microarchitecture optimization (-march=znver4)
    # Includes: AVX-512, optimized scheduling, cache hints, branch prediction
    ifneq (,$(findstring zen4,$(FULL_VARIANT)))
        CXXFLAGS += -march=znver4 -mtune=znver4
        CFLAGS += -march=znver4 -mtune=znver4
        COMPILE_AVX2 = 1
        $(info Compiler: Zen4 microarchitecture optimization enabled)

    # AVX-512 (x86-64-v4): Intel/AMD with AVX-512
    else ifneq (,$(findstring v4,$(FULL_VARIANT)))
        CXXFLAGS += -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq
        CFLAGS += -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq
        COMPILE_AVX2 = 1
        $(info Compiler: x86-64-v4 (AVX-512) optimization enabled)

    # AVX2 (x86-64-v3): Most modern x64 CPUs
    else ifneq (,$(findstring v3,$(FULL_VARIANT)))
        CXXFLAGS += -march=x86-64-v3 -mavx2 -mfma
        CFLAGS += -march=x86-64-v3 -mavx2 -mfma
        COMPILE_AVX2 = 1
        $(info Compiler: x86-64-v3 (AVX2) optimization enabled)

    # Baseline x64 (v2) - no AVX2
    else
        CXXFLAGS += -march=x86-64-v2
        CFLAGS += -march=x86-64-v2
        COMPILE_AVX2 = 0
        $(info Compiler: x86-64-v2 (baseline, no AVX2) optimization enabled)
    endif

# ARM64: Use native tuning for best results
else ifeq ($(DIRETTA_ARCH),aarch64)
    CXXFLAGS += -mcpu=native
    CFLAGS += -mcpu=native
    $(info Compiler: ARM64 native CPU optimization enabled)
endif

ifdef NOLOG
    NOLOG_SUFFIX = -nolog
    CXXFLAGS += -DNOLOG
    $(info Logging: DISABLED (production build))
else
    NOLOG_SUFFIX =
endif

# Optional DSD diagnostics (heavy logging for DSD debugging)
# Usage: make DSD_DIAG=1
ifdef DSD_DIAG
    CXXFLAGS += -DDIRETTA_DSD_DIAGNOSTICS
    $(info DSD diagnostics: ENABLED)
endif

DIRETTA_LIB_NAME = libDirettaHost_$(FULL_VARIANT)$(NOLOG_SUFFIX).a
ACQUA_LIB_NAME   = libACQUA_$(FULL_VARIANT)$(NOLOG_SUFFIX).a

$(info )
$(info ═══════════════════════════════════════════════════════)
$(info   Diretta UPnP Renderer - SIMPLIFIED ARCHITECTURE)
$(info   DirettaSync: Unified adapter (DirettaSyncAdapter+DirettaOutput))
$(info ═══════════════════════════════════════════════════════)
$(info Variant:       $(FULL_VARIANT))
$(info Library:       $(DIRETTA_LIB_NAME))
$(info ═══════════════════════════════════════════════════════)
$(info )

# ============================================
# Diretta SDK Auto-Detection - SIMPLIFIED VERSION
# ============================================

ifdef DIRETTA_SDK_PATH
    SDK_PATH = $(DIRETTA_SDK_PATH)
    SDK_VERSION = custom
    $(info ✓ Using SDK from environment: $(SDK_PATH))
else
    # Search for DirettaHostSDK_* in common locations
    SDK_CANDIDATES := $(shell find $(HOME) . .. /opt $(HOME)/audio /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -V | tail -1)
    
    ifneq ($(SDK_CANDIDATES),)
        SDK_PATH = $(SDK_CANDIDATES)
        SDK_VERSION = $(subst DirettaHostSDK_,,$(notdir $(SDK_PATH)))
        $(info ✓ SDK auto-detected: $(SDK_PATH))
        $(info ✓ SDK version: $(SDK_VERSION))
    else
        $(info )
        $(info ❌ Diretta SDK not found!)
        $(info )
        $(info 📁 Searched for DirettaHostSDK_* in:)
        $(info    $(HOME)/)
        $(info    ./)
        $(info    ../)
        $(info    /opt/)
        $(info    $(HOME)/audio/)
        $(info    /usr/local/)
        $(info )
        $(info 💡 Solutions:)
        $(info    1. Download SDK to one of the locations above)
        $(info    2. Or set: make DIRETTA_SDK_PATH=/your/path/DirettaHostSDK_XXX)
        $(info )
        $(error SDK not found)
    endif
endif

# ============================================
# Verify SDK Installation
# ============================================

# Full paths to libraries
SDK_LIB_DIRETTA = $(SDK_PATH)/lib/$(DIRETTA_LIB_NAME)
SDK_LIB_ACQUA   = $(SDK_PATH)/lib/$(ACQUA_LIB_NAME)

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
# Paths and Libraries
# ============================================

# FFmpeg path override (for ABI compatibility with target system)
# Usage: make FFMPEG_PATH=/path/to/ffmpeg-headers
#        make FFMPEG_LIB_PATH=/path/to/ffmpeg-libs
#
# CRITICAL: FFmpeg ABI mismatch causes segmentation faults!
# Compile-time headers MUST match runtime library version.

# Auto-detect local FFmpeg headers (downloaded by install.sh)
FFMPEG_HEADERS_LOCAL = $(wildcard ./ffmpeg-headers/.version)

ifdef FFMPEG_PATH
    # Explicit path provided
    FFMPEG_INCLUDES = -I$(FFMPEG_PATH)
    FFMPEG_HEADER_PATH = $(FFMPEG_PATH)
else ifneq ($(FFMPEG_HEADERS_LOCAL),)
    # Local ffmpeg-headers directory exists (from install.sh)
    FFMPEG_PATH = ./ffmpeg-headers
    FFMPEG_INCLUDES = -I$(FFMPEG_PATH)
    FFMPEG_HEADER_PATH = $(FFMPEG_PATH)
else
    # Fall back to system headers
    FFMPEG_INCLUDES = -I/usr/include/ffmpeg -I/usr/include
    FFMPEG_HEADER_PATH = /usr/include
endif

# FFmpeg library path (for linking)
ifdef FFMPEG_LIB_PATH
    FFMPEG_LDFLAGS = -L$(FFMPEG_LIB_PATH) -Wl,-rpath,$(FFMPEG_LIB_PATH)
else
    FFMPEG_LDFLAGS =
endif

# ============================================
# FFmpeg Version Detection and Validation
# ============================================

# Detect header version from libavformat/version.h or version_major.h
FFMPEG_HEADER_VERSION := $(shell \
    if [ -f "$(FFMPEG_HEADER_PATH)/libavformat/version_major.h" ]; then \
        grep -h 'LIBAVFORMAT_VERSION_MAJOR' $(FFMPEG_HEADER_PATH)/libavformat/version_major.h 2>/dev/null | grep -oE '[0-9]+' | head -1; \
    elif [ -f "$(FFMPEG_HEADER_PATH)/libavformat/version.h" ]; then \
        grep -h 'LIBAVFORMAT_VERSION_MAJOR' $(FFMPEG_HEADER_PATH)/libavformat/version.h 2>/dev/null | grep -oE '[0-9]+' | head -1; \
    elif [ -f "/usr/include/ffmpeg/libavformat/version_major.h" ]; then \
        grep -h 'LIBAVFORMAT_VERSION_MAJOR' /usr/include/ffmpeg/libavformat/version_major.h 2>/dev/null | grep -oE '[0-9]+' | head -1; \
    elif [ -f "/usr/include/ffmpeg/libavformat/version.h" ]; then \
        grep -h 'LIBAVFORMAT_VERSION_MAJOR' /usr/include/ffmpeg/libavformat/version.h 2>/dev/null | grep -oE '[0-9]+' | head -1; \
    else \
        echo "unknown"; \
    fi)

# Map libavformat major version to FFmpeg version
# libavformat 62 = FFmpeg 8.x, 61 = FFmpeg 7.x, 60 = FFmpeg 6.x, 59 = FFmpeg 5.x, 58 = FFmpeg 4.x
FFMPEG_HEADER_FFVERSION := $(shell \
    v="$(FFMPEG_HEADER_VERSION)"; \
    if [ "$$v" = "62" ]; then echo "8.x"; \
    elif [ "$$v" = "61" ]; then echo "7.x"; \
    elif [ "$$v" = "60" ]; then echo "6.x"; \
    elif [ "$$v" = "59" ]; then echo "5.x"; \
    elif [ "$$v" = "58" ]; then echo "4.x"; \
    else echo "unknown"; fi)

# Detect runtime library version
FFMPEG_LIB_VERSION := $(shell \
    pkg-config --modversion libavformat 2>/dev/null | cut -d. -f1 || \
    (ldconfig -p 2>/dev/null | grep libavformat | head -1 | grep -oE '[0-9]+\.[0-9]+' | cut -d. -f1) || \
    echo "unknown")

# Map runtime library to FFmpeg version
FFMPEG_LIB_FFVERSION := $(shell \
    v="$(FFMPEG_LIB_VERSION)"; \
    if [ "$$v" = "62" ] || [ "$$v" = "8" ]; then echo "8.x"; \
    elif [ "$$v" = "61" ] || [ "$$v" = "7" ]; then echo "7.x"; \
    elif [ "$$v" = "60" ] || [ "$$v" = "6" ]; then echo "6.x"; \
    elif [ "$$v" = "59" ] || [ "$$v" = "5" ]; then echo "5.x"; \
    elif [ "$$v" = "58" ] || [ "$$v" = "4" ]; then echo "4.x"; \
    else echo "unknown"; fi)

# Display FFmpeg configuration
$(info )
$(info ═══════════════════════════════════════════════════════)
$(info   FFmpeg Configuration)
$(info ═══════════════════════════════════════════════════════)
$(info Headers path:     $(FFMPEG_HEADER_PATH))
$(info Headers version:  libavformat $(FFMPEG_HEADER_VERSION) (FFmpeg $(FFMPEG_HEADER_FFVERSION)))
$(info Library version:  libavformat $(FFMPEG_LIB_VERSION) (FFmpeg $(FFMPEG_LIB_FFVERSION)))
ifdef FFMPEG_LIB_PATH
$(info Library path:     $(FFMPEG_LIB_PATH))
endif
$(info ═══════════════════════════════════════════════════════)

# Version mismatch detection
ifneq ($(FFMPEG_HEADER_VERSION),$(FFMPEG_LIB_VERSION))
    ifneq ($(FFMPEG_HEADER_VERSION),unknown)
        ifneq ($(FFMPEG_LIB_VERSION),unknown)
$(info )
$(info ╔══════════════════════════════════════════════════════════════════╗)
$(info ║  ⚠️  WARNING: FFmpeg VERSION MISMATCH DETECTED!                   ║)
$(info ║                                                                  ║)
$(info ║  Headers:  libavformat $(FFMPEG_HEADER_VERSION) (FFmpeg $(FFMPEG_HEADER_FFVERSION))                           ║)
$(info ║  Library:  libavformat $(FFMPEG_LIB_VERSION) (FFmpeg $(FFMPEG_LIB_FFVERSION))                           ║)
$(info ║                                                                  ║)
$(info ║  This WILL cause segmentation faults at runtime!                 ║)
$(info ║                                                                  ║)
$(info ║  Solutions:                                                      ║)
$(info ║  1. Download matching headers:                                   ║)
$(info ║     wget https://ffmpeg.org/releases/ffmpeg-5.1.2.tar.xz         ║)
$(info ║     tar xf ffmpeg-5.1.2.tar.xz && mv ffmpeg-5.1.2 ffmpeg-headers ║)
$(info ║     make clean && make                                           ║)
$(info ║                                                                  ║)
$(info ║  2. Or specify matching library path:                            ║)
$(info ║     make FFMPEG_LIB_PATH=/path/to/ffmpeg-7.x/lib                 ║)
$(info ║                                                                  ║)
$(info ║  3. Or use install.sh which handles this automatically           ║)
$(info ╚══════════════════════════════════════════════════════════════════╝)
$(info )
ifndef FFMPEG_IGNORE_MISMATCH
$(error FFmpeg version mismatch! Set FFMPEG_IGNORE_MISMATCH=1 to override)
endif
        endif
    endif
endif
$(info )

INCLUDES = \
    $(FFMPEG_INCLUDES) \
    -I/usr/include/upnp \
    -I/usr/local/include \
    -I. \
    -Isrc \
    -I$(SDK_PATH)/Host

LDFLAGS += \
    $(FFMPEG_LDFLAGS) \
    -L/usr/local/lib \
    -L$(SDK_PATH)/lib

LIBS = \
    -lupnp \
    -lixml \
    -lpthread \
    -lDirettaHost_$(FULL_VARIANT)$(NOLOG_SUFFIX) \
    -lavformat \
    -lavcodec \
    -lavutil \
    -lswresample

SDK_LIB_ACQUA = $(SDK_PATH)/lib/$(ACQUA_LIB_NAME)
ifneq (,$(wildcard $(SDK_LIB_ACQUA)))
    LIBS += -lACQUA_$(FULL_VARIANT)$(NOLOG_SUFFIX)
endif

# ============================================
# Source Files - SIMPLIFIED ARCHITECTURE
# ============================================

SRCDIR = src
OBJDIR = obj
BINDIR = bin

# Simplified architecture source files:
# - DirettaSync.cpp replaces DirettaSyncAdapter.cpp + DirettaOutput.cpp
SOURCES = \
    $(SRCDIR)/main.cpp \
    $(SRCDIR)/DirettaRenderer.cpp \
    $(SRCDIR)/AudioEngine.cpp \
    $(SRCDIR)/DirettaSync.cpp \
    $(SRCDIR)/UPnPDevice.cpp

# C sources (AVX optimized memcpy - x86 with AVX2 only)
# Skip for baseline x86-64-v2 which doesn't have AVX2
ifeq ($(COMPILE_AVX2),1)
    C_SOURCES = $(SRCDIR)/fastmemcpy-avx.c
else
    C_SOURCES =
endif

OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
C_OBJECTS = $(C_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
C_DEPENDS = $(C_OBJECTS:.o=.d)
DEPENDS = $(OBJECTS:.o=.d) $(C_DEPENDS)

TARGET = $(BINDIR)/DirettaRendererUPnP

# ============================================
# Build Rules
# ============================================

.PHONY: all clean info show-arch list-variants

all: $(TARGET)
	@echo ""
	@echo "Build complete: $(TARGET)"
	@echo "Architecture: Simplified (DirettaSync unified)"

$(TARGET): $(OBJECTS) $(C_OBJECTS) | $(BINDIR)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) $(C_OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# C compilation rule (AVX/AVX-512 optimized)
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@echo "Compiling $< (C/AVX)..."
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(BINDIR):
	@mkdir -p $(BINDIR)

clean:
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "Clean complete"

info:
	@echo "Source files (simplified architecture):"
	@for src in $(SOURCES); do echo "  $$src"; done
	@echo ""
	@echo "Key files:"
	@echo "  DirettaRingBuffer.h  - Extracted ring buffer class"
	@echo "  DirettaSync.h/cpp    - Unified adapter (replaces DirettaSyncAdapter + DirettaOutput)"
	@echo "  DirettaRenderer.h/cpp - Simplified renderer"

# ============================================
# Test Target
# ============================================

TEST_TARGET = $(BINDIR)/test_audio_memory
TEST_SOURCES = $(SRCDIR)/test_audio_memory.cpp
TEST_OBJECTS = $(TEST_SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

test: $(TEST_TARGET)
	@echo "Running tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS) | $(BINDIR)
	@echo "Linking $(TEST_TARGET)..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_OBJECTS) -o $(TEST_TARGET)

# ============================================
# Architecture Information
# ============================================

show-arch:
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  Architecture Detection Results"
	@echo "═══════════════════════════════════════════════════════"
	@echo "Machine:        $(UNAME_M)"
	@echo "Base arch:      $(BASE_ARCH)"
	@echo "SDK variant:    $(FULL_VARIANT)"
	@echo "SDK library:    $(DIRETTA_LIB_NAME)"
	@echo "SDK path:       $(SDK_PATH)"
	@echo ""
	@echo "Detection flags:"
ifeq ($(BASE_ARCH),x64)
	@echo "  HAS_AVX2:     $(HAS_AVX2)"
	@echo "  HAS_AVX512:   $(HAS_AVX512)"
	@echo "  IS_ZEN4:      $(IS_ZEN4)"
endif
ifeq ($(BASE_ARCH),aarch64)
	@echo "  PAGE_SIZE:    $(PAGE_SIZE)"
	@echo "  IS_RPI5:      $(IS_RPI5)"
endif
	@echo ""
	@echo "Compiler flags:"
	@echo "  CXXFLAGS:     $(CXXFLAGS)"
	@echo "  CFLAGS:       $(CFLAGS)"
	@echo "═══════════════════════════════════════════════════════"
	@echo ""

list-variants:
	@echo ""
	@echo "Available SDK library variants in $(SDK_PATH)/lib/:"
	@ls -1 $(SDK_PATH)/lib/libDirettaHost_*.a 2>/dev/null | sed 's/.*libDirettaHost_/  /' | sed 's/\.a$$//' || echo "  (none found)"
	@echo ""
	@echo "Usage: make ARCH_NAME=<variant>"
	@echo "Example: make ARCH_NAME=x64-linux-15zen4"
	@echo ""

-include $(DEPENDS)
