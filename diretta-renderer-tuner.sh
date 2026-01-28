#!/bin/bash
#
# diretta-renderer-tuner.sh
# CPU isolation and real-time tuning for diretta-renderer.service
#
# Auto-detects CPU topology for AMD and Intel processors.
# Supports any number of cores with or without SMT/Hyper-Threading.
#
# Features:
#   - Automatic CPU topology detection (AMD Ryzen, Intel Core, etc.)
#   - CPU isolation via kernel parameters (isolcpus, nohz_full, rcu_nocbs)
#   - Systemd slice for CPU pinning
#   - Real-time FIFO scheduling for the audio hot path
#   - IRQ affinity to housekeeping cores
#   - CPU governor set to performance
#
# Usage: sudo ./diretta-renderer-tuner.sh [apply|revert|status|detect]

# --- Bash Best Practices ---
set -euo pipefail

# =============================================================================
# CPU TOPOLOGY DETECTION
# =============================================================================

detect_cpu_topology() {
    echo "INFO: Detecting CPU topology..."

    # Get CPU info
    CPU_VENDOR=$(grep -m1 "vendor_id" /proc/cpuinfo | awk '{print $3}')
    CPU_MODEL=$(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')

    # Total logical CPUs
    TOTAL_CPUS=$(nproc)

    # Physical cores (unique core ids)
    PHYSICAL_CORES=$(grep "^cpu cores" /proc/cpuinfo | head -1 | awk '{print $4}')
    if [[ -z "$PHYSICAL_CORES" ]]; then
        # Fallback: count unique physical id + core id combinations
        PHYSICAL_CORES=$(cat /proc/cpuinfo | grep -E "^(physical id|core id)" | paste - - | sort -u | wc -l)
    fi

    # Detect SMT (Hyper-Threading)
    if [[ $TOTAL_CPUS -gt $PHYSICAL_CORES ]]; then
        SMT_ENABLED=true
        THREADS_PER_CORE=$((TOTAL_CPUS / PHYSICAL_CORES))
    else
        SMT_ENABLED=false
        THREADS_PER_CORE=1
    fi

    echo "   Vendor:          $CPU_VENDOR"
    echo "   Model:           $CPU_MODEL"
    echo "   Physical cores:  $PHYSICAL_CORES"
    echo "   Logical CPUs:    $TOTAL_CPUS"
    echo "   SMT/HT:          $SMT_ENABLED (${THREADS_PER_CORE} threads/core)"
}

# Build SMT sibling map
# Returns pairs like "0,16 1,17 2,18..." for a 16-core/32-thread CPU
build_smt_sibling_map() {
    local -a sibling_map=()

    for cpu in $(seq 0 $((TOTAL_CPUS - 1))); do
        local sibling_file="/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list"
        if [[ -f "$sibling_file" ]]; then
            local siblings=$(cat "$sibling_file")
            # Only add if this is the first CPU in the pair
            local first_cpu=$(echo "$siblings" | cut -d',' -f1 | cut -d'-' -f1)
            if [[ "$first_cpu" == "$cpu" ]]; then
                sibling_map+=("$siblings")
            fi
        fi
    done

    echo "${sibling_map[@]}"
}

# Calculate optimal CPU allocation
# Uses 1 physical core (+ SMT sibling) for housekeeping, rest for renderer
calculate_cpu_allocation() {
    echo "INFO: Calculating optimal CPU allocation..."

    if [[ "$SMT_ENABLED" == true ]]; then
        # With SMT: use first physical core + its sibling for housekeeping
        # Find sibling of CPU 0
        local sibling_file="/sys/devices/system/cpu/cpu0/topology/thread_siblings_list"
        if [[ -f "$sibling_file" ]]; then
            HOUSEKEEPING_CPUS=$(cat "$sibling_file")
        else
            # Fallback: assume sibling is at PHYSICAL_CORES offset
            HOUSEKEEPING_CPUS="0,$PHYSICAL_CORES"
        fi

        # Renderer gets all other CPUs
        local all_cpus=""
        for cpu in $(seq 0 $((TOTAL_CPUS - 1))); do
            # Skip housekeeping CPUs
            if [[ ! ",$HOUSEKEEPING_CPUS," == *",$cpu,"* ]]; then
                if [[ -z "$all_cpus" ]]; then
                    all_cpus="$cpu"
                else
                    all_cpus="$all_cpus,$cpu"
                fi
            fi
        done
        RENDERER_CPUS=$(compact_cpu_list "$all_cpus")
    else
        # Without SMT: use CPU 0 for housekeeping, rest for renderer
        HOUSEKEEPING_CPUS="0"
        RENDERER_CPUS="1-$((TOTAL_CPUS - 1))"
    fi

    echo "   Housekeeping:    CPUs $HOUSEKEEPING_CPUS"
    echo "   Renderer:        CPUs $RENDERER_CPUS"
}

# Compact a comma-separated CPU list into range notation
# e.g., "1,2,3,4,13,14,15,16" -> "1-4,13-16"
compact_cpu_list() {
    local input="$1"
    local -a cpus=(${input//,/ })
    local result=""
    local start=-1
    local prev=-1

    # Sort numerically
    IFS=$'\n' sorted=($(sort -n <<<"${cpus[*]}")); unset IFS

    for cpu in "${sorted[@]}"; do
        if [[ $start -eq -1 ]]; then
            start=$cpu
            prev=$cpu
        elif [[ $cpu -eq $((prev + 1)) ]]; then
            prev=$cpu
        else
            # End of range
            if [[ $start -eq $prev ]]; then
                result="${result:+$result,}$start"
            else
                result="${result:+$result,}$start-$prev"
            fi
            start=$cpu
            prev=$cpu
        fi
    done

    # Final range
    if [[ $start -ne -1 ]]; then
        if [[ $start -eq $prev ]]; then
            result="${result:+$result,}$start"
        else
            result="${result:+$result,}$start-$prev"
        fi
    fi

    echo "$result"
}

# =============================================================================
# MANUAL OVERRIDE (Optional - edit if auto-detection doesn't suit your needs)
# =============================================================================

# Uncomment and edit these to override auto-detection:
# HOUSEKEEPING_CPUS="0,12"
# RENDERER_CPUS="1-11,13-23"

# =============================================================================
# DERIVED VARIABLES
# =============================================================================

# System paths
GRUB_FILE="/etc/default/grub"
SYSTEMD_DIR="/etc/systemd/system"
LOCAL_BIN_DIR="/usr/local/bin"

# Service configuration
SERVICE_NAME="diretta-renderer.service"
SLICE_NAME="diretta-renderer.slice"

# Helper scripts/services
GOVERNOR_SERVICE="cpu-performance-diretta.service"
IRQ_SCRIPT_NAME="set-irq-affinity-diretta.sh"
IRQ_SCRIPT_PATH="${LOCAL_BIN_DIR}/${IRQ_SCRIPT_NAME}"
THREAD_DIST_SCRIPT_NAME="distribute-diretta-threads.sh"
THREAD_DIST_SCRIPT_PATH="${LOCAL_BIN_DIR}/${THREAD_DIST_SCRIPT_NAME}"

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

check_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "ERROR: This script must be run as root. Please use 'sudo'." >&2
        exit 1
    fi
}

usage() {
    cat <<EOF
Diretta Renderer CPU Tuner (Auto-Detection)
============================================

Usage: sudo $0 [apply|revert|status|detect]

Commands:
  apply        - Apply CPU isolation and real-time tuning
  revert       - Remove all tuning configurations
  status       - Check current tuning status
  detect       - Show detected CPU topology (no changes)

This script automatically detects your CPU topology (AMD or Intel)
and configures optimal CPU isolation for audio processing.

Supported CPUs:
  - AMD Ryzen (all generations)
  - Intel Core (all generations)
  - Any x86_64 CPU with or without SMT/Hyper-Threading
EOF
}

# Expand CPU range notation (e.g., "1-3,8" -> "1 2 3 8")
expand_cpu_list() {
    local input="$1"
    local result=""

    for part in ${input//,/ }; do
        if [[ "$part" == *-* ]]; then
            local start="${part%-*}"
            local end="${part#*-}"
            for ((i=start; i<=end; i++)); do
                result+="$i "
            done
        else
            result+="$part "
        fi
    done

    echo "$result"
}

# =============================================================================
# APPLY FUNCTIONS
# =============================================================================

apply_grub_config() {
    echo "INFO: Applying GRUB kernel parameters for CPU isolation..."

    # Remove any previous instances of these parameters
    sed -i -E 's/ (isolcpus|nohz|nohz_full|rcu_nocbs|irqaffinity)=[^"]*//g' "${GRUB_FILE}"

    # Build new kernel parameters
    local grub_cmdline="isolcpus=${RENDERER_CPUS} nohz=on nohz_full=${RENDERER_CPUS} rcu_nocbs=${RENDERER_CPUS} irqaffinity=${HOUSEKEEPING_CPUS}"

    # Append to GRUB_CMDLINE_LINUX
    sed -i "s|^\(GRUB_CMDLINE_LINUX=\".*\)\"|\1 ${grub_cmdline}\"|" "${GRUB_FILE}"

    # Update GRUB
    if command -v update-grub &> /dev/null; then
        update-grub
    elif command -v grub2-mkconfig &> /dev/null; then
        grub2-mkconfig -o /boot/grub2/grub.cfg
    else
        echo "WARNING: Could not find update-grub or grub2-mkconfig."
        echo "         Please update GRUB manually."
    fi

    echo "SUCCESS: GRUB configuration updated."
}

apply_systemd_slice() {
    echo "INFO: Creating systemd slice for CPU pinning..."

    cat << EOF > "${SYSTEMD_DIR}/${SLICE_NAME}"
[Unit]
Description=Slice for Diretta Renderer audio service
Before=slices.target

[Slice]
# Pin to isolated audio cores
AllowedCPUs=${RENDERER_CPUS}
# Allow full CPU usage
CPUQuota=100%
EOF

    echo "SUCCESS: Systemd slice created: ${SLICE_NAME}"
}

apply_service_override() {
    echo "INFO: Creating service drop-in for real-time scheduling..."

    local override_dir="${SYSTEMD_DIR}/${SERVICE_NAME}.d"
    mkdir -p "${override_dir}"

    cat << EOF > "${override_dir}/10-isolation.conf"
[Service]
# Use dedicated CPU slice
Slice=${SLICE_NAME}

# Real-time scheduling for audio hot path
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=90

# High process priority
Nice=-19

# I/O scheduling - realtime class
IOSchedulingClass=realtime
IOSchedulingPriority=0

# Memory locking for Diretta SDK buffers
LimitMEMLOCK=infinity

# Real-time priority limit
LimitRTPRIO=99

# Distribute threads across cores after startup
ExecStartPost=${THREAD_DIST_SCRIPT_PATH} \$MAINPID
EOF

    echo "SUCCESS: Service override created: ${override_dir}/10-isolation.conf"
}

apply_irq_config() {
    echo "INFO: Creating IRQ affinity script..."

    cat << EOF > "${IRQ_SCRIPT_PATH}"
#!/bin/bash
# Set all IRQs to housekeeping cores to avoid interrupting audio processing
# Auto-generated for: ${CPU_MODEL:-Unknown CPU}

HOUSEKEEPING_CPUS="${HOUSEKEEPING_CPUS}"
LOG_FILE="/var/log/irq-affinity-diretta.log"

echo "\$(date): Starting IRQ affinity setup for Diretta Renderer" | tee "\$LOG_FILE"
echo "Housekeeping CPUs: \$HOUSEKEEPING_CPUS" | tee -a "\$LOG_FILE"

# Set default affinity for new IRQs
echo "\$HOUSEKEEPING_CPUS" > /proc/irq/default_smp_affinity_list 2>> "\$LOG_FILE" || true

# Move all existing IRQs to housekeeping cores
for irq_dir in /proc/irq/*; do
    if [ -f "\$irq_dir/smp_affinity_list" ]; then
        irq=\$(basename "\$irq_dir")
        echo "\$HOUSEKEEPING_CPUS" > "\$irq_dir/smp_affinity_list" 2>> "\$LOG_FILE" || true
    fi
done

echo "\$(date): IRQ affinity setup complete" | tee -a "\$LOG_FILE"
EOF

    chmod +x "${IRQ_SCRIPT_PATH}"

    # Create systemd service
    cat << EOF > "${SYSTEMD_DIR}/set-irq-affinity-diretta.service"
[Unit]
Description=Set IRQ affinity for Diretta Renderer audio isolation
After=network.target

[Service]
Type=oneshot
ExecStart=${IRQ_SCRIPT_PATH}
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    echo "SUCCESS: IRQ affinity configuration created."
}

apply_governor_config() {
    echo "INFO: Creating CPU governor service..."

    local expanded_cpus
    expanded_cpus=$(expand_cpu_list "${RENDERER_CPUS}")

    cat << EOF > "${SYSTEMD_DIR}/${GOVERNOR_SERVICE}"
[Unit]
Description=Set CPU governor to performance for Diretta Renderer cores
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/bash -c 'for cpu in ${expanded_cpus}; do \
    if [ -f /sys/devices/system/cpu/cpu\$cpu/cpufreq/scaling_governor ]; then \
        echo performance > /sys/devices/system/cpu/cpu\$cpu/cpufreq/scaling_governor 2>/dev/null || \
        cpufreq-set -c \$cpu -g performance 2>/dev/null || true; \
    fi; \
done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    echo "SUCCESS: CPU governor service created."
}

apply_thread_distribution() {
    echo "INFO: Creating thread distribution script..."

    local expanded_cpus
    expanded_cpus=$(expand_cpu_list "${RENDERER_CPUS}")

    cat << 'SCRIPT_HEADER' > "${THREAD_DIST_SCRIPT_PATH}"
#!/bin/bash
#
# distribute-diretta-threads.sh
# Distributes DirettaRenderer threads across available cores round-robin
#
# Called by systemd ExecStartPost after the service starts.
#

set -euo pipefail

MAIN_PID="${1:-}"
LOG_FILE="/var/log/diretta-thread-distribution.log"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S'): $*" | tee -a "$LOG_FILE"
}

if [[ -z "$MAIN_PID" ]]; then
    log "ERROR: No PID provided"
    exit 1
fi

# Wait for threads to spawn
sleep 1.0

# Check if process still exists
if ! ps -p "$MAIN_PID" > /dev/null 2>&1; then
    log "WARNING: Process $MAIN_PID no longer exists, skipping"
    exit 0
fi

SCRIPT_HEADER

    cat << SCRIPT_CPUS >> "${THREAD_DIST_SCRIPT_PATH}"
# Available renderer CPUs (auto-detected)
RENDERER_CPUS_ARRAY=(${expanded_cpus})
NUM_CPUS=\${#RENDERER_CPUS_ARRAY[@]}

SCRIPT_CPUS

    cat << 'SCRIPT_BODY' >> "${THREAD_DIST_SCRIPT_PATH}"
log "Starting thread distribution for PID $MAIN_PID"
log "Available CPUs: ${RENDERER_CPUS_ARRAY[*]} ($NUM_CPUS cores)"

# Get all thread IDs for this process
TIDS=$(ps -T -o tid= -p "$MAIN_PID" 2>/dev/null | tr -d ' ')

if [[ -z "$TIDS" ]]; then
    log "WARNING: No threads found for PID $MAIN_PID"
    exit 0
fi

THREAD_COUNT=$(echo "$TIDS" | wc -l)
log "Found $THREAD_COUNT threads to distribute"

# Distribute threads round-robin
i=0
while read -r tid; do
    if [[ -n "$tid" ]]; then
        cpu_index=$(( i % NUM_CPUS ))
        target_cpu=${RENDERER_CPUS_ARRAY[$cpu_index]}

        if taskset -pc "$target_cpu" "$tid" > /dev/null 2>&1; then
            log "  Thread $tid -> CPU $target_cpu"
        else
            log "  Thread $tid -> CPU $target_cpu (failed)"
        fi

        i=$(( i + 1 ))
    fi
done <<< "$TIDS"

log "Thread distribution complete: $i threads across $NUM_CPUS CPUs"

# Show final distribution
log "Final thread layout:"
ps -T -o tid=,psr=,comm= -p "$MAIN_PID" 2>/dev/null | while read -r line; do
    log "  $line"
done

exit 0
SCRIPT_BODY

    chmod +x "${THREAD_DIST_SCRIPT_PATH}"
    echo "SUCCESS: Thread distribution script created: ${THREAD_DIST_SCRIPT_PATH}"
}

# =============================================================================
# REVERT FUNCTIONS
# =============================================================================

revert_grub_config() {
    echo "INFO: Reverting GRUB kernel parameters..."

    sed -i -E 's/ (isolcpus|nohz|nohz_full|rcu_nocbs|irqaffinity)=[^"]*//g' "${GRUB_FILE}"

    if command -v update-grub &> /dev/null; then
        update-grub
    elif command -v grub2-mkconfig &> /dev/null; then
        grub2-mkconfig -o /boot/grub2/grub.cfg
    fi

    echo "SUCCESS: GRUB configuration reverted."
}

revert_systemd_config() {
    echo "INFO: Removing systemd configurations..."

    rm -f "${SYSTEMD_DIR}/${SLICE_NAME}"
    rm -rf "${SYSTEMD_DIR}/${SERVICE_NAME}.d"
    rm -f "${SYSTEMD_DIR}/set-irq-affinity-diretta.service"
    rm -f "${IRQ_SCRIPT_PATH}"
    rm -f "${SYSTEMD_DIR}/${GOVERNOR_SERVICE}"
    rm -f "${THREAD_DIST_SCRIPT_PATH}"

    echo "SUCCESS: Systemd configurations removed."
}

# =============================================================================
# STATUS FUNCTION
# =============================================================================

check_status() {
    echo "=== Diretta Renderer Tuner Status ==="
    echo ""

    # Show detected CPU
    detect_cpu_topology
    echo ""

    local has_error=0

    # 1. GRUB parameters
    echo -n "1. GRUB CPU isolation: "
    if grep -q "isolcpus=" /proc/cmdline; then
        echo "ACTIVE"
        echo "   Current: $(grep -oE 'isolcpus=[^ ]+' /proc/cmdline)"
    else
        echo "NOT ACTIVE (requires reboot after apply)"
        has_error=1
    fi

    # 2. Systemd slice
    echo -n "2. Systemd slice (${SLICE_NAME}): "
    if [[ -f "${SYSTEMD_DIR}/${SLICE_NAME}" ]]; then
        echo "EXISTS"
    else
        echo "MISSING"
        has_error=1
    fi

    # 3. Service override
    echo -n "3. Service override: "
    if [[ -f "${SYSTEMD_DIR}/${SERVICE_NAME}.d/10-isolation.conf" ]]; then
        echo "EXISTS"
    else
        echo "MISSING"
        has_error=1
    fi

    # 4. IRQ affinity
    echo -n "4. IRQ affinity service: "
    if [[ -f "${SYSTEMD_DIR}/set-irq-affinity-diretta.service" ]]; then
        local irq_status
        irq_status=$(systemctl is-active set-irq-affinity-diretta.service 2>/dev/null || echo "inactive")
        echo "EXISTS (${irq_status})"
    else
        echo "MISSING"
        has_error=1
    fi

    # 5. Governor service
    echo -n "5. CPU governor service: "
    if [[ -f "${SYSTEMD_DIR}/${GOVERNOR_SERVICE}" ]]; then
        local gov_status
        gov_status=$(systemctl is-active "${GOVERNOR_SERVICE}" 2>/dev/null || echo "inactive")
        echo "EXISTS (${gov_status})"
    else
        echo "MISSING"
        has_error=1
    fi

    # 6. Thread distribution script
    echo -n "6. Thread distribution script: "
    if [[ -f "${THREAD_DIST_SCRIPT_PATH}" ]]; then
        echo "EXISTS"
    else
        echo "MISSING"
        has_error=1
    fi

    echo ""

    # Service status
    echo "=== Service Status ==="
    echo ""
    if systemctl is-active --quiet "${SERVICE_NAME}" 2>/dev/null; then
        echo "Service: RUNNING"
        systemctl show "${SERVICE_NAME}" -p Slice,CPUSchedulingPolicy,Nice 2>/dev/null | sed 's/^/  /'

        local main_pid
        main_pid=$(systemctl show "${SERVICE_NAME}" -p MainPID --value 2>/dev/null)
        if [[ -n "$main_pid" && "$main_pid" != "0" ]]; then
            echo ""
            echo "  Process affinity (allowed CPUs):"
            taskset -pc "$main_pid" 2>/dev/null | sed 's/^/    /' || echo "    (unable to read)"

            echo ""
            echo "  Thread distribution (current):"
            echo "    TID      CPU  COMMAND"
            ps -T -o tid=,psr=,comm= -p "$main_pid" 2>/dev/null | while read -r tid psr comm; do
                printf "    %-8s %-4s %s\n" "$tid" "$psr" "$comm"
            done

            echo ""
            echo "  Threads per CPU:"
            ps -T -o psr= -p "$main_pid" 2>/dev/null | sort | uniq -c | while read -r count cpu; do
                printf "    CPU %s: %s threads\n" "$cpu" "$count"
            done
        fi
    else
        echo "Service: NOT RUNNING"
    fi

    echo ""

    if [[ $has_error -eq 0 ]]; then
        echo "=== All configurations in place ==="
        if ! grep -q "isolcpus=" /proc/cmdline; then
            echo "NOTE: Reboot required for kernel parameters to take effect."
        fi
    else
        echo "=== Some configurations missing - run 'apply' ==="
    fi
}

# =============================================================================
# MAIN
# =============================================================================

main() {
    check_root

    case "${1:-}" in
        apply)
            echo "=== Applying Diretta Renderer CPU Tuning ==="
            echo ""

            detect_cpu_topology
            calculate_cpu_allocation
            echo ""

            apply_grub_config
            apply_systemd_slice
            apply_thread_distribution
            apply_service_override
            apply_irq_config
            apply_governor_config

            echo ""
            echo "INFO: Reloading systemd daemon..."
            systemctl daemon-reload

            echo "INFO: Enabling helper services..."
            systemctl enable set-irq-affinity-diretta.service "${GOVERNOR_SERVICE}" 2>/dev/null || true

            echo ""
            echo "=== Configuration Applied ==="
            echo ""
            echo "CPU Allocation:"
            echo "  Housekeeping: CPUs ${HOUSEKEEPING_CPUS}"
            echo "  Renderer:     CPUs ${RENDERER_CPUS}"
            echo ""
            echo "IMPORTANT: A REBOOT is required for CPU isolation to take effect."
            echo ""
            echo "After reboot:"
            echo "  - Restart the service: sudo systemctl restart ${SERVICE_NAME}"
            echo "  - Check status: sudo $0 status"
            echo ""
            ;;

        revert)
            echo "=== Reverting Diretta Renderer CPU Tuning ==="
            echo ""

            systemctl disable set-irq-affinity-diretta.service "${GOVERNOR_SERVICE}" 2>/dev/null || true

            revert_grub_config
            revert_systemd_config

            echo ""
            echo "INFO: Reloading systemd daemon..."
            systemctl daemon-reload

            echo ""
            echo "=== Configuration Reverted ==="
            echo ""
            echo "IMPORTANT: A REBOOT is required for kernel parameter changes."
            echo ""
            ;;

        status)
            check_status
            ;;

        detect)
            echo "=== CPU Topology Detection ==="
            echo ""
            detect_cpu_topology
            calculate_cpu_allocation
            echo ""
            echo "To apply this configuration, run: sudo $0 apply"
            ;;

        redistribute)
            echo "=== Manual Thread Redistribution ==="
            echo ""

            if ! systemctl is-active --quiet "${SERVICE_NAME}" 2>/dev/null; then
                echo "ERROR: ${SERVICE_NAME} is not running"
                exit 1
            fi

            local main_pid
            main_pid=$(systemctl show "${SERVICE_NAME}" -p MainPID --value 2>/dev/null)
            if [[ -z "$main_pid" || "$main_pid" == "0" ]]; then
                echo "ERROR: Could not get PID for ${SERVICE_NAME}"
                exit 1
            fi

            echo "Service PID: $main_pid"
            echo ""

            if [[ -f "${THREAD_DIST_SCRIPT_PATH}" ]]; then
                echo "Running thread distribution script..."
                "${THREAD_DIST_SCRIPT_PATH}" "$main_pid"
            else
                echo "ERROR: Thread distribution script not found. Run 'apply' first."
                exit 1
            fi

            echo ""
            echo "=== Current Thread Layout ==="
            echo "TID      CPU  COMMAND"
            ps -T -o tid=,psr=,comm= -p "$main_pid" 2>/dev/null | while read -r tid psr comm; do
                printf "%-8s %-4s %s\n" "$tid" "$psr" "$comm"
            done
            ;;

        *)
            usage
            ;;
    esac
}

main "$@"
