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
        $(info Compiler: Zen4 microarchitecture optimization enabled)

    # AVX-512 (x86-64-v4): Intel/AMD with AVX-512
    else ifneq (,$(findstring v4,$(FULL_VARIANT)))
        CXXFLAGS += -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq
        CFLAGS += -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq
        $(info Compiler: x86-64-v4 (AVX-512) optimization enabled)

    # AVX2 (x86-64-v3): Most modern x64 CPUs
    else ifneq (,$(findstring v3,$(FULL_VARIANT)))
        CXXFLAGS += -march=x86-64-v3 -mavx2 -mfma
        CFLAGS += -march=x86-64-v3 -mavx2 -mfma
        $(info Compiler: x86-64-v3 (AVX2) optimization enabled)

    # Baseline x64 (v2)
    else
        CXXFLAGS += -march=x86-64-v2
        CFLAGS += -march=x86-64-v2
        $(info Compiler: x86-64-v2 (baseline) optimization enabled)
    endif

# ARM64: Use native tuning for best results
else ifeq ($(DIRETTA_ARCH),aarch64)
    CXXFLAGS += -mcpu=native
    CFLAGS += -mcpu=native
    $(info Compiler: ARM64 native CPU optimization enabled)
endif

ifdef NOLOG
    NOLOG_SUFFIX = -nolog
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
# SDK Detection
# ============================================

ifdef DIRETTA_SDK_PATH
    SDK_PATH = $(DIRETTA_SDK_PATH)
else
    # Search for SDK in common locations (newest version first)
    SDK_SEARCH_PATHS = \
        ../DirettaHostSDK_147_19 \
        ../DirettaHostSDK_147 \
        ../DirettaHostSDK_148 \
        ./DirettaHostSDK_147_19 \
        ./DirettaHostSDK_147 \
        ./DirettaHostSDK_148 \
        $(HOME)/DirettaHostSDK_147_19 \
        $(HOME)/DirettaHostSDK_147 \
        $(HOME)/DirettaHostSDK_148 \
        /opt/DirettaHostSDK_147_19 \
        /opt/DirettaHostSDK_147 \
        /opt/DirettaHostSDK_148

    SDK_PATH = $(firstword $(foreach path,$(SDK_SEARCH_PATHS),$(wildcard $(path))))

    ifeq ($(SDK_PATH),)
        $(error Diretta SDK not found! Set DIRETTA_SDK_PATH or place SDK in one of: $(SDK_SEARCH_PATHS))
    endif
endif

SDK_LIB_DIRETTA = $(SDK_PATH)/lib/$(DIRETTA_LIB_NAME)

ifeq (,$(wildcard $(SDK_LIB_DIRETTA)))
    $(error Required library not found: $(DIRETTA_LIB_NAME))
endif

$(info SDK: $(SDK_PATH))
$(info )

# ============================================
# Paths and Libraries
# ============================================

# FFmpeg path override (for ABI compatibility with target system)
# Usage: make FFMPEG_PATH=/path/to/ffmpeg-headers
#
# This is critical for avoiding crashes when compile-time headers
# don't match runtime library version (e.g., compiling against
# FFmpeg 7.x headers but running against FFmpeg 5.x libraries)

# Auto-detect local FFmpeg headers (downloaded by install.sh)
FFMPEG_HEADERS_LOCAL = $(wildcard ./ffmpeg-headers/.version)

ifdef FFMPEG_PATH
    # Explicit path provided
    FFMPEG_INCLUDES = -I$(FFMPEG_PATH)
    FFMPEG_LDFLAGS =
    $(info FFmpeg headers: $(FFMPEG_PATH) (explicit))
else ifneq ($(FFMPEG_HEADERS_LOCAL),)
    # Local ffmpeg-headers directory exists (from install.sh)
    FFMPEG_PATH = ./ffmpeg-headers
    FFMPEG_INCLUDES = -I$(FFMPEG_PATH)
    FFMPEG_LDFLAGS =
    FFMPEG_LOCAL_VER := $(shell cat ./ffmpeg-headers/.version 2>/dev/null)
    $(info FFmpeg headers: ./ffmpeg-headers (v$(FFMPEG_LOCAL_VER)))
else
    # Fall back to system headers
    FFMPEG_INCLUDES = -I/usr/include/ffmpeg -I/usr/include
    FFMPEG_LDFLAGS =
    $(info FFmpeg headers: system (/usr/include))
    $(info )
    $(info ╔══════════════════════════════════════════════════════════════════╗)
    $(info ║ NOTE: Using system FFmpeg headers. If you experience crashes,    ║)
    $(info ║ ensure headers match your runtime FFmpeg version, or run:        ║)
    $(info ║   make FFMPEG_PATH=/path/to/ffmpeg-source                        ║)
    $(info ║ Or use install.sh which auto-downloads matching headers.         ║)
    $(info ╚══════════════════════════════════════════════════════════════════╝)
    $(info )
endif

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

# C sources (AVX optimized memcpy - x86 only)
ifeq ($(BASE_ARCH),x64)
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
