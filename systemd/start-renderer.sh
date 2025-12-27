#!/bin/bash
# Diretta UPnP Renderer - Startup Wrapper Script
# This script reads configuration and starts the renderer with appropriate options

set -e

# Default values (can be overridden by config file)
NAME="${NAME:-}"
TARGET="${TARGET:-1}"
PORT="${PORT:-4005}"
BUFFER="${BUFFER:-2.0}"
GAPLESS="${GAPLESS:-}"
VERBOSE="${VERBOSE:-}"
NETWORK_INTERFACE="${NETWORK_INTERFACE:-}"
THREAD_MODE="${THREAD_MODE:-}"
CYCLE_TIME="${CYCLE_TIME:-}"
CYCLE_MIN_TIME="${CYCLE_MIN_TIME:-}"
INFO_CYCLE="${INFO_CYCLE:-}"
MTU_OVERRIDE="${MTU_OVERRIDE:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RENDERER_BIN="${SCRIPT_DIR}/DirettaRendererUPnP"

# Build command with options
CMD=("$RENDERER_BIN")

# Renderer name
if [ -n "$NAME" ]; then
    NAME="$(eval echo "$NAME")"
    CMD+=( --name "$NAME" )
fi

# Basic options
CMD+=( --target "$TARGET" )
CMD+=( --port "$PORT" )
CMD+=( --buffer "$BUFFER" )

# Network interface option (CRITICAL for multi-homed systems)
if [ -n "$NETWORK_INTERFACE" ]; then
    # Check if it looks like an IP address or interface name
    if [[ "$NETWORK_INTERFACE" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Binding to IP address: $NETWORK_INTERFACE"
        CMD+=( --bind-ip "$NETWORK_INTERFACE" )
    else
        echo "Binding to network interface: $NETWORK_INTERFACE"
        CMD+=( --interface "$NETWORK_INTERFACE" )
    fi
fi

# Gapless
if [ -n "$GAPLESS" ]; then
    CMD+=( "$GAPLESS" )
fi

# Verbose
if [ -n "$VERBOSE" ]; then
    CMD+=( "$VERBOSE" )
fi

# Advanced Diretta settings (only if specified)
if [ -n "$THREAD_MODE" ]; then
    CMD+=( --thread-mode "$THREAD_MODE" )
fi

if [ -n "$CYCLE_TIME" ]; then
    CMD+=( --cycle-time "$CYCLE_TIME" )
fi

if [ -n "$CYCLE_MIN_TIME" ]; then
    CMD+=( --cycle-min-time "$CYCLE_MIN_TIME" )
fi

if [ -n "$INFO_CYCLE" ]; then
    CMD+=( --info-cycle "$INFO_CYCLE" )
fi

if [ -n "$MTU_OVERRIDE" ]; then
    CMD+=( --mtu "$MTU_OVERRIDE" )
fi

# Log the command being executed
echo "════════════════════════════════════════════════════════"
echo "  Starting Diretta UPnP Renderer"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  Target:           $TARGET"
echo "  Port:             $PORT"
echo "  Buffer:           $BUFFER seconds"
echo "  Network Interface: ${NETWORK_INTERFACE:-auto-detect}"
echo ""
echo "Command:"
echo "  ${CMD[@]}"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute
exec "${CMD[@]}"
