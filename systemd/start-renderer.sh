#!/bin/bash
# Diretta UPnP Renderer - Startup Wrapper Script
# This script reads configuration and starts the renderer with appropriate options

set -e

# Default values (can be overridden by config file)
# v2.1.10: Aligned variable names with CLI (KEY → --key mapping)
# Old names (RENDERER_NAME, NETWORK_INTERFACE, MTU_OVERRIDE) still supported as fallback
TARGET="${TARGET:-1}"
PORT="${PORT:-4005}"
NAME="${NAME:-${RENDERER_NAME:-}}"
GAPLESS="${GAPLESS:-}"
VERBOSE="${VERBOSE:-}"
MINIMAL_UPNP="${MINIMAL_UPNP:-}"
INTERFACE="${INTERFACE:-${NETWORK_INTERFACE:-}}"
THREAD_MODE="${THREAD_MODE:-}"
CYCLE_TIME="${CYCLE_TIME:-}"
CYCLE_MIN_TIME="${CYCLE_MIN_TIME:-}"
INFO_CYCLE="${INFO_CYCLE:-}"
TRANSFER_MODE="${TRANSFER_MODE:-}"
TARGET_PROFILE_LIMIT="${TARGET_PROFILE_LIMIT:-}"
MTU="${MTU:-${MTU_OVERRIDE:-}}"

# CPU affinity (no pinning by default). Accepts single core or comma-separated list.
# Examples: CPU_AUDIO=3  or  CPU_AUDIO="3,4,5"
CPU_AUDIO="${CPU_AUDIO:-}"
CPU_OTHER="${CPU_OTHER:-}"

# Buffer configuration (leave empty to use defaults)
PCM_BUFFER_SECONDS="${PCM_BUFFER_SECONDS:-}"
PCM_REMOTE_BUFFER_SECONDS="${PCM_REMOTE_BUFFER_SECONDS:-}"
DSD_BUFFER_SECONDS="${DSD_BUFFER_SECONDS:-}"
PCM_PREFILL_MS="${PCM_PREFILL_MS:-}"
PCM_REMOTE_PREFILL_MS="${PCM_REMOTE_PREFILL_MS:-}"
DSD_PREFILL_MS="${DSD_PREFILL_MS:-}"

# Process priority defaults
NICE_LEVEL="${NICE_LEVEL:--10}"
IO_SCHED_CLASS="${IO_SCHED_CLASS:-realtime}"
IO_SCHED_PRIORITY="${IO_SCHED_PRIORITY:-0}"
RT_PRIORITY="${RT_PRIORITY:-50}"

# Advanced network config
TARGET_INTERFACE="${TARGET_INTERFACE:-}"
TARGET_SPEED="${TARGET_SPEED:-100}"
TARGET_DUPLEX="${TARGET_DUPLEX:-full}"

# IRQ affinity for the target NIC (away from --cpu-audio core)
IRQ_INTERFACE="${IRQ_INTERFACE:-}"
IRQ_CPUS="${IRQ_CPUS:-}"

# SMT control: on / off / forceoff / empty (no change)
SMT="${SMT:-}"

RENDERER_BIN="/opt/diretta-renderer-upnp/DirettaRendererUPnP"

# Advanced network interface settings
if [ -n "$TARGET_INTERFACE" ]; then
    if command -v ethtool >/dev/null 2>&1; then
        echo "Set advanced target network settings: $TARGET_INTERFACE -> ${TARGET_SPEED}Mbit/${TARGET_DUPLEX}-duplex"
        ethtool -s "$TARGET_INTERFACE" speed "$TARGET_SPEED" duplex "$TARGET_DUPLEX"
        sleep 1
    else
        echo "WARNING: TARGET_INTERFACE set but ethtool is not installed — skipping link tuning." >&2
    fi
fi

# IRQ affinity: pin all IRQs whose name contains any of the interfaces listed
# in $IRQ_INTERFACE (comma-separated, e.g. "enp1s0,enp2s0") to the CPU list
# $IRQ_CPUS. Useful to keep network interrupts off the audio worker core,
# including setups with separate NICs for the upstream source and the Diretta
# target. Some IRQs (managed/MSI-X) are read-only — those are counted as
# "skipped".
if [ -n "$IRQ_INTERFACE" ] && [ -n "$IRQ_CPUS" ]; then
    pinned=0
    skipped=0
    IFS=',' read -ra IRQ_IFACE_LIST <<< "$IRQ_INTERFACE"
    for iface in "${IRQ_IFACE_LIST[@]}"; do
        iface=$(echo "$iface" | tr -d ' ')
        [ -z "$iface" ] && continue
        while IFS= read -r line; do
            irq=$(echo "$line" | awk -F: '{print $1}' | tr -d ' ')
            if [ -n "$irq" ] && [ -e "/proc/irq/$irq/smp_affinity_list" ]; then
                if echo "$IRQ_CPUS" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null; then
                    pinned=$((pinned + 1))
                else
                    skipped=$((skipped + 1))
                fi
            fi
        done < <(grep -F "$iface" /proc/interrupts)
    done
    echo "IRQ affinity for $IRQ_INTERFACE -> CPU(s) $IRQ_CPUS: $pinned pinned, $skipped skipped (managed/read-only)"
fi

# SMT (Hyper-Threading) toggle. System-wide setting — must be applied BEFORE
# launching DRUP so any subsequent CPU_AUDIO/CPU_OTHER pinning sees the right
# topology. Non-persistent across reboots; the kernel resets to the BIOS
# default unless 'nosmt' is also added to the GRUB cmdline.
if [ -n "$SMT" ]; then
    SMT_CTRL="/sys/devices/system/cpu/smt/control"
    case "$SMT" in
        on|off|forceoff)
            if [ -w "$SMT_CTRL" ]; then
                current=$(cat "$SMT_CTRL" 2>/dev/null || echo "?")
                if [ "$current" != "$SMT" ]; then
                    if echo "$SMT" > "$SMT_CTRL" 2>/dev/null; then
                        echo "SMT: $current -> $SMT"
                    else
                        echo "WARNING: SMT change to '$SMT' refused (BIOS lock or kernel-restricted)" >&2
                    fi
                else
                    echo "SMT already $current — no change"
                fi
            else
                echo "WARNING: SMT control not available at $SMT_CTRL" >&2
            fi
            ;;
        *)
            echo "WARNING: invalid SMT value '$SMT' — use on/off/forceoff or leave empty" >&2
            ;;
    esac
fi

# Build command as array (preserves arguments with spaces)
CMD=("$RENDERER_BIN")

# Basic options
CMD+=("--target" "$TARGET")

# Renderer name (supports spaces, e.g., "Devialet Target")
if [ -n "$NAME" ]; then
    CMD+=("--name" "$NAME")
fi

# UPnP port (if specified)
if [ -n "$PORT" ]; then
    CMD+=("--port" "$PORT")
fi

# Network interface option (CRITICAL for multi-homed systems)
# --interface accepts both interface names (eth0) and IP addresses (192.168.1.32)
if [ -n "$INTERFACE" ]; then
    echo "Binding to network interface: $INTERFACE"
    CMD+=("--interface" "$INTERFACE")
fi

# Gapless
if [ -n "$GAPLESS" ]; then
    CMD+=($GAPLESS)
fi

# Log verbosity (--verbose or --quiet)
if [ -n "$VERBOSE" ]; then
    CMD+=($VERBOSE)
fi

# Minimal UPnP mode (no position polling, no events)
if [ -n "$MINIMAL_UPNP" ] && [ "$MINIMAL_UPNP" = "1" ]; then
    CMD+=("--minimal-upnp")
fi

# Advanced Diretta settings (only if specified)
if [ -n "$THREAD_MODE" ]; then
    CMD+=("--thread-mode" "$THREAD_MODE")
fi

if [ -n "$CYCLE_TIME" ]; then
    CMD+=("--cycle-time" "$CYCLE_TIME")
fi

if [ -n "$CYCLE_MIN_TIME" ]; then
    CMD+=("--cycle-min-time" "$CYCLE_MIN_TIME")
fi

if [ -n "$INFO_CYCLE" ]; then
    CMD+=("--info-cycle" "$INFO_CYCLE")
fi

if [ -n "$TRANSFER_MODE" ]; then
    CMD+=("--transfer-mode" "$TRANSFER_MODE")
fi

if [ -n "$TARGET_PROFILE_LIMIT" ]; then
    CMD+=("--target-profile-limit" "$TARGET_PROFILE_LIMIT")
fi

if [ -n "$MTU" ]; then
    CMD+=("--mtu" "$MTU")
fi

if [ -n "$RT_PRIORITY" ] && [ "$RT_PRIORITY" != "50" ]; then
    CMD+=("--rt-priority" "$RT_PRIORITY")
fi

# CPU affinity
if [ -n "$CPU_AUDIO" ]; then
    CMD+=("--cpu-audio" "$CPU_AUDIO")
fi

if [ -n "$CPU_OTHER" ]; then
    CMD+=("--cpu-other" "$CPU_OTHER")
fi

# Buffer configuration
if [ -n "$PCM_BUFFER_SECONDS" ]; then
    CMD+=("--pcm-buffer-seconds" "$PCM_BUFFER_SECONDS")
fi
if [ -n "$PCM_REMOTE_BUFFER_SECONDS" ]; then
    CMD+=("--pcm-remote-buffer-seconds" "$PCM_REMOTE_BUFFER_SECONDS")
fi
if [ -n "$DSD_BUFFER_SECONDS" ]; then
    CMD+=("--dsd-buffer-seconds" "$DSD_BUFFER_SECONDS")
fi
if [ -n "$PCM_PREFILL_MS" ]; then
    CMD+=("--pcm-prefill-ms" "$PCM_PREFILL_MS")
fi
if [ -n "$PCM_REMOTE_PREFILL_MS" ]; then
    CMD+=("--pcm-remote-prefill-ms" "$PCM_REMOTE_PREFILL_MS")
fi
if [ -n "$DSD_PREFILL_MS" ]; then
    CMD+=("--dsd-prefill-ms" "$DSD_PREFILL_MS")
fi

# Build exec prefix as array for process priority
EXEC_PREFIX=()

# Apply nice level
if [ -n "$NICE_LEVEL" ] && [ "$NICE_LEVEL" != "0" ]; then
    EXEC_PREFIX=("nice" "-n" "$NICE_LEVEL")
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
            EXEC_PREFIX=("ionice" "-c" "$IONICE_CLASS" "${EXEC_PREFIX[@]}")
        else
            EXEC_PREFIX=("ionice" "-c" "$IONICE_CLASS" "-n" "${IO_SCHED_PRIORITY:-0}" "${EXEC_PREFIX[@]}")
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
echo "  Name:              ${NAME:-Diretta Renderer (default)}"
echo "  Network Interface: ${INTERFACE:-auto-detect}"
echo "  Nice level:        $NICE_LEVEL"
echo "  I/O scheduling:    $IO_SCHED_CLASS (priority $IO_SCHED_PRIORITY)"
echo "  RT priority:       $RT_PRIORITY (SCHED_FIFO)"
echo ""
echo "Command:"
echo "  ${EXEC_PREFIX[*]} ${CMD[*]}"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute with priority settings
exec "${EXEC_PREFIX[@]}" "${CMD[@]}"
