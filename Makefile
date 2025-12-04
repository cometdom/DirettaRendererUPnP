# Diretta UPnP Renderer - Makefile with Auto-Detection
# This Makefile automatically detects the Diretta SDK location

# ============================================
# Compiler Settings
# ============================================

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

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
                Please download from https://www.diretta.link or set DIRETTA_SDK_PATH environment variable)
    else
        $(info ✓ SDK auto-detected: $(SDK_PATH))
    endif
endif

# ============================================
# Verify SDK Installation
# ============================================

# Check if SDK library exists
SDK_LIB = $(SDK_PATH)/lib/libDirettaHost_x64-linux-15v3.a
ifeq (,$(wildcard $(SDK_LIB)))
    $(error ❌ SDK library not found at: $(SDK_LIB). Please check SDK installation)
endif

# Check if SDK headers exist
SDK_HEADER = $(SDK_PATH)/Host/Diretta/SyncBuffer
ifeq (,$(wildcard $(SDK_HEADER)))
    $(error ❌ SDK headers not found at: $(SDK_PATH)/Host/. Please check SDK installation)
endif

$(info ✓ SDK validation passed)

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
    -lDirettaHost_x64-linux-15v3 \
    -lACQUA_x64-linux-15v3 \
    -lavformat \
    -lavcodec \
    -lavutil \
    -lswresample

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

# If sources are in root directory instead:
# SOURCES = main.cpp DirettaRenderer.cpp AudioEngine.cpp DirettaOutput.cpp UPnPDevice.cpp

OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DEPENDS = $(OBJECTS:.o=.d)

TARGET = $(BINDIR)/DirettaRendererUPnP

# ============================================
# Build Rules
# ============================================

.PHONY: all clean info help install

# Default target
all: $(TARGET)
	@echo "✓ Build complete: $(TARGET)"

# Link
$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $(TARGET)..."
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
	@echo "SDK Path:     $(SDK_PATH)"
	@echo "SDK Library:  $(SDK_LIB)"
	@echo "SDK Headers:  $(SDK_HEADER)"
	@echo "Compiler:     $(CXX)"
	@echo "Flags:        $(CXXFLAGS)"
	@echo "Sources:      $(SOURCES)"
	@echo "Target:       $(TARGET)"
	@echo "============================================"

# Help
help:
	@echo "Diretta UPnP Renderer - Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make          Build the renderer"
	@echo "  make clean    Remove build artifacts"
	@echo "  make info     Show build configuration"
	@echo "  make help     Show this help message"
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

# Include dependencies
-include $(DEPENDS)
