#!/bin/bash
# Diretta UPnP Renderer - Startup Wrapper Script
# This script reads configuration and starts the renderer with appropriate options

set -e

# Default values (can be overridden by config file)
TARGET="${TARGET:-1}"
PORT="${PORT:-4005}"
GAPLESS="${GAPLESS:-}"
VERBOSE="${VERBOSE:-}"
NETWORK_INTERFACE="${NETWORK_INTERFACE:-}"
THREAD_MODE="${THREAD_MODE:-}"
CYCLE_TIME="${CYCLE_TIME:-}"
CYCLE_MIN_TIME="${CYCLE_MIN_TIME:-}"
INFO_CYCLE="${INFO_CYCLE:-}"
TRANSFER_MODE="${TRANSFER_MODE:-}"
TARGET_PROFILE_LIMIT="${TARGET_PROFILE_LIMIT:-}"
MTU_OVERRIDE="${MTU_OVERRIDE:-}"

# Process priority defaults
NICE_LEVEL="${NICE_LEVEL:--10}"
IO_SCHED_CLASS="${IO_SCHED_CLASS:-realtime}"
IO_SCHED_PRIORITY="${IO_SCHED_PRIORITY:-0}"

RENDERER_BIN="/opt/diretta-renderer-upnp/DirettaRendererUPnP"

# Build command with options
CMD="$RENDERER_BIN"

# Basic options
CMD="$CMD --target $TARGET"

# UPnP port (if specified)
if [ -n "$PORT" ]; then
    CMD="$CMD --port $PORT"
fi

# Network interface option (CRITICAL for multi-homed systems)
if [ -n "$NETWORK_INTERFACE" ]; then
    # Check if it looks like an IP address or interface name
    if [[ "$NETWORK_INTERFACE" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Binding to IP address: $NETWORK_INTERFACE"
        CMD="$CMD --bind-ip $NETWORK_INTERFACE"
    else
        echo "Binding to network interface: $NETWORK_INTERFACE"
        CMD="$CMD --interface $NETWORK_INTERFACE"
    fi
fi

# Gapless
if [ -n "$GAPLESS" ]; then
    CMD="$CMD $GAPLESS"
fi

# Log verbosity (--verbose or --quiet)
if [ -n "$VERBOSE" ]; then
    CMD="$CMD $VERBOSE"
fi

# Advanced Diretta settings (only if specified)
if [ -n "$THREAD_MODE" ]; then
    CMD="$CMD --thread-mode $THREAD_MODE"
fi

if [ -n "$CYCLE_TIME" ]; then
    CMD="$CMD --cycle-time $CYCLE_TIME"
fi

if [ -n "$CYCLE_MIN_TIME" ]; then
    CMD="$CMD --cycle-min-time $CYCLE_MIN_TIME"
fi

if [ -n "$INFO_CYCLE" ]; then
    CMD="$CMD --info-cycle $INFO_CYCLE"
fi

if [ -n "$TRANSFER_MODE" ]; then
    CMD="$CMD --transfer-mode $TRANSFER_MODE"
fi

if [ -n "$TARGET_PROFILE_LIMIT" ]; then
    CMD="$CMD --target-profile-limit $TARGET_PROFILE_LIMIT"
fi

if [ -n "$MTU_OVERRIDE" ]; then
    CMD="$CMD --mtu $MTU_OVERRIDE"
fi

# Build exec prefix for process priority
EXEC_PREFIX=""

# Apply nice level
if [ -n "$NICE_LEVEL" ] && [ "$NICE_LEVEL" != "0" ]; then
    EXEC_PREFIX="nice -n $NICE_LEVEL"
fi

# Apply I/O scheduling
if [ -n "$IO_SCHED_CLASS" ]; then
    # Map class name to ionice class number
    case "$IO_SCHED_CLASS" in
        realtime|1)  IONICE_CLASS=1 ;;
        best-effort|2) IONICE_CLASS=2 ;;
        idle|3)      IONICE_CLASS=3 ;;
        *)           IONICE_CLASS="" ;;
    esac

    if [ -n "$IONICE_CLASS" ]; then
        if [ "$IONICE_CLASS" = "3" ]; then
            # idle class has no priority level
            EXEC_PREFIX="ionice -c $IONICE_CLASS $EXEC_PREFIX"
        else
            EXEC_PREFIX="ionice -c $IONICE_CLASS -n ${IO_SCHED_PRIORITY:-0} $EXEC_PREFIX"
        fi
    fi
fi

# Log the command being executed
echo "════════════════════════════════════════════════════════"
echo "  Starting Diretta UPnP Renderer"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  Target:            $TARGET"
echo "  Network Interface: ${NETWORK_INTERFACE:-auto-detect}"
echo "  Nice level:        $NICE_LEVEL"
echo "  I/O scheduling:    $IO_SCHED_CLASS (priority $IO_SCHED_PRIORITY)"
echo ""
echo "Command:"
echo "  $EXEC_PREFIX $CMD"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute with priority settings
exec $EXEC_PREFIX $CMD
