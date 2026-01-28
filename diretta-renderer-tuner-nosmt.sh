#!/bin/bash
#
# diretta-renderer-tuner-nosmt.sh
# CPU isolation and real-time tuning for diretta-renderer.service
# VERSION: NO-SMT (Hyper-Threading/SMT disabled)
#
# Auto-detects CPU topology for AMD and Intel processors.
# Disables SMT for maximum per-core performance.
#
# Benefits of nosmt for audio:
#   - No SMT resource contention (ALUs, cache, etc.)
#   - More predictable latency per core
#   - Simpler core topology
#
# Trade-offs:
#   - Lose half the logical CPUs
#   - Less parallelism available
#
# Usage: sudo ./diretta-renderer-tuner-nosmt.sh [apply|revert|status|detect]

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

    # Total logical CPUs (current)
    TOTAL_CPUS=$(nproc)

    # Physical cores
    PHYSICAL_CORES=$(grep "^cpu cores" /proc/cpuinfo | head -1 | awk '{print $4}')
    if [[ -z "$PHYSICAL_CORES" ]]; then
        PHYSICAL_CORES=$(cat /proc/cpuinfo | grep -E "^(physical id|core id)" | paste - - | sort -u | wc -l)
    fi

    # Detect if SMT is currently enabled
    if [[ $TOTAL_CPUS -gt $PHYSICAL_CORES ]]; then
        SMT_CURRENTLY_ENABLED=true
        THREADS_PER_CORE=$((TOTAL_CPUS / PHYSICAL_CORES))
    else
        SMT_CURRENTLY_ENABLED=false
        THREADS_PER_CORE=1
    fi

    # After nosmt, we'll have only physical cores
    CPUS_AFTER_NOSMT=$PHYSICAL_CORES

    echo "   Vendor:           $CPU_VENDOR"
    echo "   Model:            $CPU_MODEL"
    echo "   Physical cores:   $PHYSICAL_CORES"
    echo "   Current CPUs:     $TOTAL_CPUS"
    echo "   SMT currently:    $SMT_CURRENTLY_ENABLED"
    echo "   CPUs after nosmt: $CPUS_AFTER_NOSMT (physical cores only)"
}

# Calculate optimal CPU allocation for nosmt mode
# With nosmt, we only have physical cores (0 to PHYSICAL_CORES-1)
calculate_cpu_allocation() {
    echo "INFO: Calculating optimal CPU allocation (nosmt mode)..."

    # With nosmt: use CPU 0 for housekeeping, rest for renderer
    HOUSEKEEPING_CPUS="0"

    if [[ $PHYSICAL_CORES -gt 1 ]]; then
        RENDERER_CPUS="1-$((PHYSICAL_CORES - 1))"
    else
        echo "ERROR: Only 1 physical core detected. Cannot isolate CPUs."
        exit 1
    fi

    local renderer_count=$((PHYSICAL_CORES - 1))

    echo "   Housekeeping:     CPU 0 (1 physical core)"
    echo "   Renderer:         CPUs $RENDERER_CPUS ($renderer_count physical cores)"
}

# =============================================================================
# MANUAL OVERRIDE (Optional - edit if auto-detection doesn't suit your needs)
# =============================================================================

# Uncomment and edit these to override auto-detection:
# HOUSEKEEPING_CPUS="0"
# RENDERER_CPUS="1-11"

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

# Helper scripts/services (different names to avoid conflicts with SMT version)
GOVERNOR_SERVICE="cpu-performance-diretta-nosmt.service"
IRQ_SCRIPT_NAME="set-irq-affinity-diretta-nosmt.sh"
IRQ_SCRIPT_PATH="${LOCAL_BIN_DIR}/${IRQ_SCRIPT_NAME}"
THREAD_DIST_SCRIPT_NAME="distribute-diretta-threads-nosmt.sh"
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
Diretta Renderer CPU Tuner (NO-SMT Auto-Detection)
===================================================

Usage: sudo $0 [apply|revert|status|detect]

Commands:
  apply        - Apply CPU isolation with SMT disabled
  revert       - Remove all tuning configurations
  status       - Check current tuning status
  detect       - Show detected CPU topology (no changes)

This version DISABLES SMT (Hyper-Threading), leaving only physical cores.
This provides more predictable latency at the cost of fewer total CPUs.

Supported CPUs:
  - AMD Ryzen (all generations)
  - Intel Core (all generations)
  - Any x86_64 CPU with SMT/Hyper-Threading

Example results:
  - Ryzen 9 5900X: 12 cores (instead of 24 logical CPUs)
  - Ryzen 7 7700X: 8 cores (instead of 16 logical CPUs)
  - Intel i9-13900K: 24 cores (instead of 32 logical CPUs)
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
    echo "INFO: Applying GRUB kernel parameters (with nosmt)..."

    # Remove any previous instances of these parameters
    sed -i -E 's/ (isolcpus|nohz|nohz_full|rcu_nocbs|irqaffinity|nosmt)=[^"]*//g' "${GRUB_FILE}"
    # Also remove standalone nosmt
    sed -i -E 's/ nosmt([" ])/ \1/g' "${GRUB_FILE}"

    # Build new kernel parameters
    # nosmt disables SMT/Hyper-Threading at boot
    local grub_cmdline="nosmt isolcpus=${RENDERER_CPUS} nohz=on nohz_full=${RENDERER_CPUS} rcu_nocbs=${RENDERER_CPUS} irqaffinity=${HOUSEKEEPING_CPUS}"

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

    echo "SUCCESS: GRUB configuration updated (nosmt enabled)."
    echo "         After reboot, only physical cores 0-$((PHYSICAL_CORES - 1)) will be available."
}

apply_systemd_slice() {
    echo "INFO: Creating systemd slice for CPU pinning..."

    cat << EOF > "${SYSTEMD_DIR}/${SLICE_NAME}"
[Unit]
Description=Slice for Diretta Renderer audio service (nosmt)
Before=slices.target

[Slice]
# Pin to isolated audio cores (physical cores only, no SMT)
AllowedCPUs=${RENDERER_CPUS}
# Allow full CPU usage
CPUQuota=100%
EOF

    echo "SUCCESS: Systemd slice created: ${SLICE_NAME}"
}

apply_thread_distribution() {
    echo "INFO: Creating thread distribution script (nosmt version)..."

    local expanded_cpus
    expanded_cpus=$(expand_cpu_list "${RENDERER_CPUS}")

    cat << 'SCRIPT_HEADER' > "${THREAD_DIST_SCRIPT_PATH}"
#!/bin/bash
#
# distribute-diretta-threads-nosmt.sh
# Distributes DirettaRenderer threads across physical cores (no SMT)
#
# With nosmt, we have fewer cores but each is a full physical core
# with no resource sharing. This should give more consistent latency.
#

set -euo pipefail

MAIN_PID="${1:-}"
LOG_FILE="/var/log/diretta-thread-distribution-nosmt.log"

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
# Available renderer CPUs - physical cores only (auto-detected, nosmt)
RENDERER_CPUS_ARRAY=(${expanded_cpus})
NUM_CPUS=\${#RENDERER_CPUS_ARRAY[@]}

SCRIPT_CPUS

    cat << 'SCRIPT_BODY' >> "${THREAD_DIST_SCRIPT_PATH}"
log "Starting thread distribution for PID $MAIN_PID (nosmt mode)"
log "Available physical cores: ${RENDERER_CPUS_ARRAY[*]} ($NUM_CPUS cores)"

# Get all thread IDs for this process
TIDS=$(ps -T -o tid= -p "$MAIN_PID" 2>/dev/null | tr -d ' ')

if [[ -z "$TIDS" ]]; then
    log "WARNING: No threads found for PID $MAIN_PID"
    exit 0
fi

THREAD_COUNT=$(echo "$TIDS" | wc -l)
log "Found $THREAD_COUNT threads to distribute across $NUM_CPUS physical cores"

if [[ $THREAD_COUNT -gt $NUM_CPUS ]]; then
    log "NOTE: More threads ($THREAD_COUNT) than cores ($NUM_CPUS) - some cores will run multiple threads"
fi

# Distribute threads round-robin
i=0
while read -r tid; do
    if [[ -n "$tid" ]]; then
        cpu_index=$(( i % NUM_CPUS ))
        target_cpu=${RENDERER_CPUS_ARRAY[$cpu_index]}

        if taskset -pc "$target_cpu" "$tid" > /dev/null 2>&1; then
            log "  Thread $tid -> Core $target_cpu (physical)"
        else
            log "  Thread $tid -> Core $target_cpu (failed)"
        fi

        i=$(( i + 1 ))
    fi
done <<< "$TIDS"

log "Thread distribution complete: $i threads across $NUM_CPUS physical cores"

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

# Distribute threads across physical cores after startup
ExecStartPost=${THREAD_DIST_SCRIPT_PATH} \$MAINPID
EOF

    echo "SUCCESS: Service override created: ${override_dir}/10-isolation.conf"
}

apply_irq_config() {
    echo "INFO: Creating IRQ affinity script..."

    cat << EOF > "${IRQ_SCRIPT_PATH}"
#!/bin/bash
# Set all IRQs to housekeeping core (nosmt version)
# Auto-generated for: ${CPU_MODEL:-Unknown CPU}
# With nosmt, we only have physical core 0 for housekeeping

HOUSEKEEPING_CPUS="${HOUSEKEEPING_CPUS}"
LOG_FILE="/var/log/irq-affinity-diretta-nosmt.log"

echo "\$(date): Starting IRQ affinity setup (nosmt mode)" | tee "\$LOG_FILE"
echo "Housekeeping core: \$HOUSEKEEPING_CPUS" | tee -a "\$LOG_FILE"

# Set default affinity for new IRQs
echo "\$HOUSEKEEPING_CPUS" > /proc/irq/default_smp_affinity_list 2>> "\$LOG_FILE" || true

# Move all existing IRQs to housekeeping core
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
    cat << EOF > "${SYSTEMD_DIR}/set-irq-affinity-diretta-nosmt.service"
[Unit]
Description=Set IRQ affinity for Diretta Renderer (nosmt)
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
Description=Set CPU governor to performance (nosmt, physical cores only)
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

# =============================================================================
# REVERT FUNCTIONS
# =============================================================================

revert_grub_config() {
    echo "INFO: Reverting GRUB kernel parameters..."

    sed -i -E 's/ (isolcpus|nohz|nohz_full|rcu_nocbs|irqaffinity|nosmt)=[^"]*//g' "${GRUB_FILE}"
    sed -i -E 's/ nosmt([" ])/ \1/g' "${GRUB_FILE}"

    if command -v update-grub &> /dev/null; then
        update-grub
    elif command -v grub2-mkconfig &> /dev/null; then
        grub2-mkconfig -o /boot/grub2/grub.cfg
    fi

    echo "SUCCESS: GRUB configuration reverted (nosmt removed)."
}

revert_systemd_config() {
    echo "INFO: Removing systemd configurations..."

    rm -f "${SYSTEMD_DIR}/${SLICE_NAME}"
    rm -rf "${SYSTEMD_DIR}/${SERVICE_NAME}.d"
    rm -f "${SYSTEMD_DIR}/set-irq-affinity-diretta-nosmt.service"
    rm -f "${IRQ_SCRIPT_PATH}"
    rm -f "${SYSTEMD_DIR}/${GOVERNOR_SERVICE}"
    rm -f "${THREAD_DIST_SCRIPT_PATH}"

    echo "SUCCESS: Systemd configurations removed."
}

# =============================================================================
# STATUS FUNCTION
# =============================================================================

check_status() {
    echo "=== Diretta Renderer Tuner Status (NO-SMT VERSION) ==="
    echo ""

    detect_cpu_topology
    echo ""

    local has_error=0

    # Check if nosmt is active
    echo -n "0. SMT Status: "
    if grep -q "nosmt" /proc/cmdline; then
        echo "DISABLED (nosmt active)"
        local online_cpus
        online_cpus=$(nproc 2>/dev/null || grep -c ^processor /proc/cpuinfo)
        echo "   Online CPUs: $online_cpus (physical cores only)"
    else
        echo "ENABLED (nosmt not in cmdline - reboot required)"
        has_error=1
    fi

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
    if [[ -f "${SYSTEMD_DIR}/set-irq-affinity-diretta-nosmt.service" ]]; then
        local irq_status
        irq_status=$(systemctl is-active set-irq-affinity-diretta-nosmt.service 2>/dev/null || echo "inactive")
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
            echo "  Threads per physical core:"
            ps -T -o psr= -p "$main_pid" 2>/dev/null | sort | uniq -c | while read -r count cpu; do
                printf "    Core %s: %s threads\n" "$cpu" "$count"
            done
        fi
    else
        echo "Service: NOT RUNNING"
    fi

    echo ""

    if [[ $has_error -eq 0 ]]; then
        echo "=== All configurations in place (nosmt mode) ==="
        if ! grep -q "nosmt" /proc/cmdline; then
            echo "NOTE: Reboot required for nosmt to take effect."
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
            echo "=== Applying Diretta Renderer CPU Tuning (NO-SMT) ==="
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
            systemctl enable set-irq-affinity-diretta-nosmt.service "${GOVERNOR_SERVICE}" 2>/dev/null || true

            echo ""
            echo "=== Configuration Applied (NO-SMT) ==="
            echo ""
            echo "CPU Allocation (after reboot):"
            echo "  Mode:         NO-SMT (Hyper-Threading disabled)"
            echo "  Housekeeping: CPU ${HOUSEKEEPING_CPUS} (1 physical core)"
            echo "  Renderer:     CPUs ${RENDERER_CPUS} ($((PHYSICAL_CORES - 1)) physical cores)"
            echo ""
            echo "IMPORTANT: A REBOOT is required for nosmt and CPU isolation."
            echo ""
            echo "After reboot:"
            echo "  - SMT will be disabled ($PHYSICAL_CORES physical cores only)"
            echo "  - Restart the service: sudo systemctl restart ${SERVICE_NAME}"
            echo "  - Check status: sudo $0 status"
            echo ""
            ;;

        revert)
            echo "=== Reverting Diretta Renderer CPU Tuning (NO-SMT) ==="
            echo ""

            systemctl disable set-irq-affinity-diretta-nosmt.service "${GOVERNOR_SERVICE}" 2>/dev/null || true

            revert_grub_config
            revert_systemd_config

            echo ""
            echo "INFO: Reloading systemd daemon..."
            systemctl daemon-reload

            echo ""
            echo "=== Configuration Reverted ==="
            echo ""
            echo "IMPORTANT: A REBOOT is required to re-enable SMT."
            echo ""
            ;;

        status)
            check_status
            ;;

        detect)
            echo "=== CPU Topology Detection (NO-SMT mode) ==="
            echo ""
            detect_cpu_topology
            calculate_cpu_allocation
            echo ""
            echo "To apply this configuration, run: sudo $0 apply"
            ;;

        redistribute)
            echo "=== Manual Thread Redistribution (NO-SMT) ==="
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

            # Check current SMT status
            if grep -q "nosmt" /proc/cmdline; then
                echo "SMT Status: DISABLED (nosmt active)"
            else
                echo "SMT Status: ENABLED (nosmt not active yet)"
                echo "NOTE: Full nosmt benefits require reboot"
            fi
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
