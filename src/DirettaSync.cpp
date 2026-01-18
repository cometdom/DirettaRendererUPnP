/**
 * @file DirettaSync.cpp
 * @brief Unified Diretta sync implementation
 *
 * Based on MPD Diretta Output Plugin v0.4.0
 * Preserves DSD planar handling from original UPnP renderer
 */

#include "DirettaSync.h"
#include <stdexcept>
#include <iomanip>

namespace {
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) {
            return;
        }
        // C2: acquire ensures increment visible to beginReconfigure() before ring ops
        users_.fetch_add(1, std::memory_order_acquire);
        if (reconfiguring.load(std::memory_order_acquire)) {
            // C2: bail-out - never entered guarded section, relaxed is safe
            users_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        active_ = true;
    }

    ~RingAccessGuard() {
        if (active_) {
            // C2: release ensures all ring ops complete before decrement
            users_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool active() const { return active_; }

private:
    std::atomic<int>& users_;
    bool active_;
};
} // namespace

//=============================================================================
// Constructor / Destructor
//=============================================================================

DirettaSync::DirettaSync() {
    m_ringBuffer.resize(44100 * 2 * 4, 0x00);
    DIRETTA_LOG("Created");
}

DirettaSync::~DirettaSync() {
    disable();
    DIRETTA_LOG("Destroyed");
}

//=============================================================================
// Initialization (Enable/Disable like MPD)
//=============================================================================

bool DirettaSync::enable(const DirettaConfig& config) {
    if (m_enabled) {
        DIRETTA_LOG("Already enabled");
        return true;
    }

    m_config = config;
    DIRETTA_LOG("Enabling...");

    if (!discoverTarget()) {
        DIRETTA_LOG("Failed to discover target");
        return false;
    }

    if (!measureMTU()) {
        DIRETTA_LOG("MTU measurement failed, using fallback");
    }

    m_calculator = std::make_unique<DirettaCycleCalculator>(m_effectiveMTU);

    if (!openSyncConnection()) {
        DIRETTA_LOG("Failed to open sync connection");
        return false;
    }

    m_enabled = true;
    DIRETTA_LOG("Enabled, MTU=" << m_effectiveMTU);
    return true;
}

void DirettaSync::disable() {
    DIRETTA_LOG("Disabling...");

    if (m_open) {
        close();
    }

    if (m_enabled) {
        shutdownWorker();
        DIRETTA::Sync::close();
        m_sdkOpen = false;
        m_calculator.reset();
        m_enabled = false;
    }

    m_hasPreviousFormat = false;
    DIRETTA_LOG("Disabled");
}

bool DirettaSync::openSyncConnection() {
    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);

    DIRETTA_LOG("Opening DIRETTA::Sync with threadMode=" << m_config.threadMode);

    bool opened = false;
    for (int attempt = 0; attempt < DirettaRetry::OPEN_RETRIES && !opened; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("open() retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(DirettaRetry::OPEN_DELAY_MS));
        }
        opened = DIRETTA::Sync::open(
            DIRETTA::Sync::THRED_MODE(m_config.threadMode),
            cycleTime, 0, "DirettaRenderer", 0x44525400,
            -1, -1, 0, DIRETTA::Sync::MSMODE_MS3);
    }

    if (!opened) {
        DIRETTA_LOG("DIRETTA::Sync::open failed after 3 attempts");
        return false;
    }

    m_sdkOpen = true;
    inquirySupportFormat(m_targetAddress);

    if (g_verbose) {
        logSinkCapabilities();
    }

    return true;
}

//=============================================================================
// Target Discovery
//=============================================================================

bool DirettaSync::discoverTarget() {
    DIRETTA_LOG("Discovering Diretta target...");

    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;
    findSettings.Name = "DirettaRenderer";
    findSettings.MyID = 0x44525400;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        DIRETTA_LOG("Failed to open finder");
        return false;
    }

    DIRETTA::Find::PortResalts results;
    if (!find.findOutput(results) || results.empty()) {
        find.close();
        DIRETTA_LOG("No Diretta targets found");
        return false;
    }

    DIRETTA_LOG("Found " << results.size() << " target(s)");

    if (results.size() == 1 || m_targetIndex == 0) {
        auto it = results.begin();
        m_targetAddress = it->first;
        DIRETTA_LOG("Selected: " << it->second.targetName);
    } else if (m_targetIndex > 0 && m_targetIndex < static_cast<int>(results.size())) {
        auto it = results.begin();
        std::advance(it, m_targetIndex);
        m_targetAddress = it->first;
        DIRETTA_LOG("Selected target #" << (m_targetIndex + 1));
    } else {
        auto it = results.begin();
        m_targetAddress = it->first;
        DIRETTA_LOG("Selected first target: " << it->second.targetName);
    }

    find.close();
    return true;
}

bool DirettaSync::measureMTU() {
    if (m_mtuOverride > 0) {
        m_effectiveMTU = m_mtuOverride;
        DIRETTA_LOG("Using configured MTU=" << m_effectiveMTU);
        return true;
    }

    if (m_config.mtu > 0) {
        m_effectiveMTU = m_config.mtu;
        DIRETTA_LOG("Using config MTU=" << m_effectiveMTU);
        return true;
    }

    DIRETTA_LOG("Measuring MTU...");

    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        m_effectiveMTU = m_config.mtuFallback;
        return false;
    }

    uint32_t measuredMTU = 0;
    bool ok = find.measSendMTU(m_targetAddress, measuredMTU);
    find.close();

    if (ok && measuredMTU > 0) {
        m_effectiveMTU = measuredMTU;
        DIRETTA_LOG("Measured MTU=" << m_effectiveMTU);
        return true;
    }

    m_effectiveMTU = m_config.mtuFallback;
    DIRETTA_LOG("MTU measurement failed, using fallback=" << m_effectiveMTU);
    return false;
}

bool DirettaSync::verifyTargetAvailable() {
    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) return false;

    DIRETTA::Find::PortResalts results;
    bool found = find.findOutput(results) && !results.empty();
    find.close();

    return found;
}

void DirettaSync::listTargets() {
    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        std::cerr << "Failed to open Diretta finder" << std::endl;
        return;
    }

    DIRETTA::Find::PortResalts results;
    if (!find.findOutput(results) || results.empty()) {
        std::cout << "No Diretta targets found" << std::endl;
        find.close();
        return;
    }

    std::cout << "\nAvailable Diretta Targets (" << results.size() << " found):\n" << std::endl;

    int index = 1;
    for (const auto& target : results) {
        const auto& info = target.second;
        std::cout << "[" << index << "] " << info.targetName << std::endl;

        // Show output/port name if available (differentiates I2S vs USB, etc.)
        if (!info.outputName.empty()) {
            std::cout << "    Output: " << info.outputName << std::endl;
        }

        // Show port numbers
        std::cout << "    Port: IN=" << info.PI << " OUT=" << info.PO;
        if (info.multiport) {
            std::cout << " (multiport)";
        }
        std::cout << std::endl;

        // Show configuration URL if available
        if (!info.config.empty()) {
            std::cout << "    Config: " << info.config << std::endl;
        }

        // Show SDK version
        std::cout << "    Version: " << info.version << std::endl;

        // Show Product ID
        std::cout << "    ProductID: 0x" << std::hex << info.productID << std::dec << std::endl;

        std::cout << std::endl;
        index++;
    }

    find.close();
}

void DirettaSync::logSinkCapabilities() {
    const auto& info = getSinkInfo();
    std::cout << "[DirettaSync] Sink capabilities:" << std::endl;
    std::cout << "[DirettaSync]   PCM: " << (info.checkSinkSupportPCM() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD: " << (info.checkSinkSupportDSD() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD LSB: " << (info.checkSinkSupportDSDlsb() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD MSB: " << (info.checkSinkSupportDSDmsb() ? "YES" : "NO") << std::endl;
}

//=============================================================================
// Open/Close (Connection Management)
//=============================================================================

bool DirettaSync::open(const AudioFormat& format) {

    std::cout << "[DirettaSync] ========== OPEN ==========" << std::endl;
    std::cout << "[DirettaSync] Format: " << format.sampleRate << "Hz/"
              << format.bitDepth << "bit/" << format.channels << "ch "
              << (format.isDSD ? "DSD" : "PCM") << std::endl;

    if (!m_enabled) {
        std::cerr << "[DirettaSync] ERROR: Not enabled" << std::endl;
        return false;
    }

    // Reopen SDK if it was released (e.g., after playlist end)
    if (!m_sdkOpen) {
        std::cout << "[DirettaSync] SDK was released, reopening..." << std::endl;
        if (!openSyncConnection()) {
            std::cerr << "[DirettaSync] ERROR: Failed to reopen SDK" << std::endl;
            return false;
        }
        std::cout << "[DirettaSync] SDK reopened successfully" << std::endl;
    }

    bool newIsDsd = format.isDSD;
    bool needFullConnect = true;  // Whether we need connectPrepare/connect/connectWait

    // Fast path: Already open with same format - just reset buffer and resume
    // This avoids the expensive setSink/connect sequence for same-format track transitions
    if (m_open && m_hasPreviousFormat) {
        bool sameFormat = (m_previousFormat.sampleRate == format.sampleRate &&
                          m_previousFormat.bitDepth == format.bitDepth &&
                          m_previousFormat.channels == format.channels &&
                          m_previousFormat.isDSD == format.isDSD);

        std::cout << "[DirettaSync]   Previous: " << m_previousFormat.sampleRate << "Hz/"
                  << m_previousFormat.bitDepth << "bit/" << m_previousFormat.channels << "ch"
                  << (m_previousFormat.isDSD ? " DSD" : " PCM") << std::endl;
        std::cout << "[DirettaSync]   Current:  " << format.sampleRate << "Hz/"
                  << format.bitDepth << "bit/" << format.channels << "ch"
                  << (format.isDSD ? " DSD" : " PCM") << std::endl;

        // EXPERIMENTAL: Check force full reopen flag (user-initiated track change)
        // When set, bypass quick path even for same format
        if (m_forceFullReopen) {
            std::cout << "[DirettaSync] EXPERIMENTAL: Force full reopen requested (user interaction)" << std::endl;
            m_forceFullReopen = false;  // Clear flag after use

            // Use standard reopen sequence for clean transition
            if (!reopenForFormatChange()) {
                std::cerr << "[DirettaSync] Failed to reopen for user-initiated change" << std::endl;
                return false;
            }
            needFullConnect = true;
            // Skip to full connect path (after the if-else block)
        } else if (sameFormat) {
            std::cout << "[DirettaSync] Same format - quick resume (no setSink)" << std::endl;

            // Send silence before transition to flush Diretta pipeline
            if (m_isDsdMode.load(std::memory_order_acquire)) {
                requestShutdownSilence(30);
                auto start = std::chrono::steady_clock::now();
                while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) break;
                    std::this_thread::yield();
                }
            }

            // Clear buffer and reset flags
            m_ringBuffer.clear();
            m_prefillComplete = false;
            m_postOnlineDelayDone = false;
            m_stabilizationCount = 0;
            m_stopRequested = false;
            m_draining = false;
            m_silenceBuffersRemaining = 0;
            play();
            m_playing = true;
            m_paused = false;
            std::cout << "[DirettaSync] ========== OPEN COMPLETE (quick) ==========" << std::endl;
            return true;
        } else {
            // Format change detected
            bool wasDSD = m_previousFormat.isDSD;
            bool nowDSD = format.isDSD;
            bool nowPCM = !format.isDSD;

            // Detect rate changes (DSD or PCM)
            // DSD512×44.1 (22,579,200 Hz) ↔ DSD512×48 (24,576,000 Hz) requires clock domain change
            bool isDsdRateChange = wasDSD && nowDSD &&
                                   (m_previousFormat.sampleRate != format.sampleRate);
            bool isPcmRateChange = !wasDSD && nowPCM &&
                                   (m_previousFormat.sampleRate != format.sampleRate);

            if (wasDSD && (nowPCM || isDsdRateChange)) {
                // DSD→PCM or any DSD rate change: Full close/reopen for clean transition
                // I2S targets are timing-sensitive and need a clean break
                // Rate changes cause noise if target's internal buffers aren't fully flushed
                // Clock domain changes (44.1kHz ↔ 48kHz family) also require full reset
                // Note: We can't send silence here because playback is already stopped
                // (auto-stop happens before URI change), so getNewStream() isn't being called
                if (nowPCM) {
                    std::cout << "[DirettaSync] DSD->PCM transition - full close/reopen" << std::endl;
                } else {
                    int prevMultiplier = m_previousFormat.sampleRate / 2822400;
                    int newMultiplier = format.sampleRate / 2822400;
                    std::cout << "[DirettaSync] DSD" << (prevMultiplier * 64) << "->DSD"
                              << (newMultiplier * 64) << " rate change - full close/reopen" << std::endl;
                }

                int dsdMultiplier = m_previousFormat.sampleRate / 44100;
                std::cout << "[DirettaSync] Previous format was DSD" << dsdMultiplier << std::endl;

                // Clear any pending silence requests (playback is stopped, can't send anyway)
                m_silenceBuffersRemaining = 0;

                // Stop playback and disconnect
                stop();
                disconnect(true);

                // Close DIRETTA::Sync completely (critical for buffer flush)
                DIRETTA::Sync::close();

                // Shutdown worker thread
                m_running = false;
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }

                m_open = false;
                m_playing = false;
                m_paused = false;

                // Extended delay for target to fully reset
                // DSD→PCM needs delay for clock domain switch (TEST: reduced from 800 to 400)
                // DSD rate downgrade needs 400ms to flush internal buffers
                int resetDelayMs = nowPCM ? 400 : 400;
                std::cout << "[DirettaSync] Waiting " << resetDelayMs
                          << "ms for target to reset..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(resetDelayMs));

                // Reopen DIRETTA::Sync fresh
                ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);
                if (!DIRETTA::Sync::open(
                        DIRETTA::Sync::THRED_MODE(m_config.threadMode),
                        cycleTime, 0, "DirettaRenderer", 0x44525400,
                        -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
                    std::cerr << "[DirettaSync] Failed to re-open DIRETTA::Sync" << std::endl;
                    return false;
                }
                std::cout << "[DirettaSync] DIRETTA::Sync reopened" << std::endl;

                // Fall through to full open path (needFullConnect is already true)
            } else if (isPcmRateChange) {
                // PCM rate change: Full close/reopen for clean transition
                // Same issue as DSD - stale samples at old rate cause transition noise
                std::cout << "[DirettaSync] PCM " << m_previousFormat.sampleRate << "Hz->"
                          << format.sampleRate << "Hz rate change - full close/reopen" << std::endl;

                // Clear any pending silence requests
                m_silenceBuffersRemaining = 0;

                // Stop playback and disconnect
                stop();
                disconnect(true);

                // Close DIRETTA::Sync completely (critical for buffer flush)
                DIRETTA::Sync::close();

                // Shutdown worker thread
                m_running = false;
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }

                m_open = false;
                m_playing = false;
                m_paused = false;

                // Shorter delay for PCM rate change (TEST: reduced from 200 to 100)
                int resetDelayMs = 100;
                std::cout << "[DirettaSync] Waiting " << resetDelayMs
                          << "ms for target to reset..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(resetDelayMs));

                // Reopen DIRETTA::Sync fresh
                ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);
                if (!DIRETTA::Sync::open(
                        DIRETTA::Sync::THRED_MODE(m_config.threadMode),
                        cycleTime, 0, "DirettaRenderer", 0x44525400,
                        -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
                    std::cerr << "[DirettaSync] Failed to re-open DIRETTA::Sync" << std::endl;
                    return false;
                }
                std::cout << "[DirettaSync] DIRETTA::Sync reopened" << std::endl;

                // Fall through to full open path
            } else {
                // Other format changes (PCM→DSD, bit depth change):
                // use existing reopenForFormatChange()
                std::cout << "[DirettaSync] Format change - reopen" << std::endl;
                if (!reopenForFormatChange()) {
                    std::cerr << "[DirettaSync] Failed to reopen for format change" << std::endl;
                    return false;
                }
            }
            needFullConnect = true;
        }
    }

    // Full reset for first open or after format change reopen
    if (needFullConnect) {
        fullReset();
    }
    m_isDsdMode.store(newIsDsd, std::memory_order_release);

    uint32_t effectiveSampleRate;
    int effectiveChannels = format.channels;
    int bitsPerSample;

    if (m_isDsdMode.load(std::memory_order_acquire)) {
        uint32_t dsdBitRate = format.sampleRate;
        uint32_t byteRate = dsdBitRate / 8;
        effectiveSampleRate = dsdBitRate;
        bitsPerSample = 1;

        DIRETTA_LOG("DSD: bitRate=" << dsdBitRate << " byteRate=" << byteRate);

        configureSinkDSD(dsdBitRate, format.channels, format);
        configureRingDSD(byteRate, format.channels);
    } else {
        effectiveSampleRate = format.sampleRate;

        int acceptedBits;
        configureSinkPCM(format.sampleRate, format.channels, format.bitDepth, acceptedBits);
        bitsPerSample = acceptedBits;

        int direttaBps = (acceptedBits == 32) ? 4 : (acceptedBits == 24) ? 3 : 2;
        int inputBps = (format.bitDepth == 32 || format.bitDepth == 24) ? 4 : 2;

        configureRingPCM(format.sampleRate, format.channels, direttaBps, inputBps);
    }

    unsigned int cycleTimeUs = calculateCycleTime(effectiveSampleRate, effectiveChannels, bitsPerSample);
    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(cycleTimeUs);

    // Initial delay - Target needs time to prepare for new format
    // Longer delay for first open/reconnect, shorter for reconfigure
    int initialDelayMs = needFullConnect ? 500 : 200;
    std::this_thread::sleep_for(std::chrono::milliseconds(initialDelayMs));

    // setSink reconfiguration
    bool sinkSet = false;
    int maxAttempts = needFullConnect ? DirettaRetry::SETSINK_RETRIES_FULL : DirettaRetry::SETSINK_RETRIES_QUICK;
    int retryDelayMs = needFullConnect ? DirettaRetry::SETSINK_DELAY_FULL_MS : DirettaRetry::SETSINK_DELAY_QUICK_MS;
    for (int attempt = 0; attempt < maxAttempts && !sinkSet; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("setSink retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
        sinkSet = setSink(m_targetAddress, cycleTime, false, m_effectiveMTU);
    }

    if (!sinkSet) {
        std::cerr << "[DirettaSync] Failed to set sink after " << maxAttempts << " attempts" << std::endl;
        return false;
    }

    applyTransferMode(m_config.transferMode, cycleTime);

    // Connect sequence - only needed after disconnect
    if (needFullConnect) {
        if (!connectPrepare()) {
            std::cerr << "[DirettaSync] connectPrepare failed" << std::endl;
            return false;
        }

        bool connected = false;
        for (int attempt = 0; attempt < DirettaRetry::CONNECT_RETRIES && !connected; attempt++) {
            if (attempt > 0) {
                DIRETTA_LOG("connect retry #" << attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(DirettaRetry::CONNECT_DELAY_MS));
            }
            connected = connect(0);
        }

        if (!connected) {
            std::cerr << "[DirettaSync] connect failed" << std::endl;
            return false;
        }

        if (!connectWait()) {
            std::cerr << "[DirettaSync] connectWait failed" << std::endl;
            disconnect();
            return false;
        }
    } else {
        DIRETTA_LOG("Skipping connect sequence (still connected)");
    }

    // Clear buffer and start playback
    m_ringBuffer.clear();
    m_prefillComplete = false;
    m_postOnlineDelayDone = false;

    play();

    if (!waitForOnline(m_config.onlineWaitMs)) {
        DIRETTA_LOG("WARNING: Did not come online within timeout");
    }

    m_postOnlineDelayDone = false;
    m_stabilizationCount = 0;

    // Save format state
    m_previousFormat = format;
    m_hasPreviousFormat = true;
    m_currentFormat = format;

    m_open = true;
    m_playing = true;
    m_paused = false;

    std::cout << "[DirettaSync] ========== OPEN COMPLETE ==========" << std::endl;
    return true;
}

void DirettaSync::close() {
    std::cout << "[DirettaSync] Close()" << std::endl;

    if (!m_open) {
        DIRETTA_LOG("Not open");
        return;
    }

    // Request shutdown silence
    requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 50 : 20);

    auto start = std::chrono::steady_clock::now();
    while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(150)) {
            DIRETTA_LOG("Silence timeout");
            break;
        }
        std::this_thread::yield();
    }

    m_stopRequested = true;

    stop();
    disconnect(true);  // Wait for proper disconnection before returning

    int waitCount = 0;
    while (m_workerActive.load() && waitCount < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    m_open = false;
    m_playing = false;
    m_paused = false;

    DIRETTA_LOG("Close() done");
}

void DirettaSync::release() {
    std::cout << "[DirettaSync] Release() - fully releasing target" << std::endl;

    // First do a normal close if still open
    if (m_open) {
        close();
    }

    // Now fully close the SDK connection so target is released
    if (m_sdkOpen) {
        DIRETTA_LOG("Closing SDK connection...");

        // Shutdown worker thread
        m_running = false;
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_workerThread.joinable()) {
                m_workerThread.join();
            }
        }

        // Close SDK-level connection
        DIRETTA::Sync::close();
        m_sdkOpen = false;

        // Brief delay to ensure target processes the disconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "[DirettaSync] Target released" << std::endl;
    }

    // Clear format state so next open() starts fresh
    m_hasPreviousFormat = false;
}

bool DirettaSync::reopenForFormatChange() {
    DIRETTA_LOG("reopenForFormatChange: stopping...");

    stop();
    disconnect(true);
    DIRETTA::Sync::close();

    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }

    DIRETTA_LOG("Waiting " << m_config.formatSwitchDelayMs << "ms...");
    std::this_thread::sleep_for(std::chrono::milliseconds(m_config.formatSwitchDelayMs));

    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);

    if (!DIRETTA::Sync::open(
            DIRETTA::Sync::THRED_MODE(m_config.threadMode),
            cycleTime, 0, "DirettaRenderer", 0x44525400,
            -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
        std::cerr << "[DirettaSync] Failed to re-open sync" << std::endl;
        return false;
    }

    // Re-discover sink with retry
    bool sinkFound = false;
    for (int attempt = 0; attempt < DirettaRetry::REOPEN_SINK_RETRIES && !sinkFound; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("setSink retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(DirettaRetry::REOPEN_SINK_DELAY_MS));
        }
        sinkFound = setSink(m_targetAddress, cycleTime, false, m_effectiveMTU);
    }

    if (!sinkFound) {
        std::cerr << "[DirettaSync] Failed to re-discover sink" << std::endl;
        return false;
    }

    inquirySupportFormat(m_targetAddress);

    DIRETTA_LOG("reopenForFormatChange complete");
    return true;
}

void DirettaSync::fullReset() {
    DIRETTA_LOG("fullReset()");

    m_stopRequested = true;
    m_draining = false;

    int waitCount = 0;
    while (m_workerActive.load(std::memory_order_acquire) && waitCount < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        ReconfigureGuard guard(*this);

        m_prefillComplete = false;
        m_postOnlineDelayDone = false;
        m_silenceBuffersRemaining = 0;
        m_stabilizationCount = 0;
        m_streamCount = 0;
        m_pushCount = 0;
        m_isDsdMode.store(false, std::memory_order_release);
        m_needDsdBitReversal.store(false, std::memory_order_release);
        m_needDsdByteSwap.store(false, std::memory_order_release);
        m_isLowBitrate.store(false, std::memory_order_release);
        m_need24BitPack.store(false, std::memory_order_release);
        m_need16To32Upsample.store(false, std::memory_order_release);
        m_bytesPerFrame.store(0, std::memory_order_release);
        m_framesPerBufferRemainder.store(0, std::memory_order_release);
        m_framesPerBufferAccumulator.store(0, std::memory_order_release);

        m_ringBuffer.clear();
    }

    m_stopRequested = false;
}

//=============================================================================
// Sink Configuration
//=============================================================================

void DirettaSync::configureSinkPCM(int rate, int channels, int inputBits, int& acceptedBits) {
    (void)inputBits;
    std::lock_guard<std::mutex> lock(m_configMutex);

    DIRETTA::FormatConfigure fmt;
    fmt.setSpeed(rate);
    fmt.setChannel(channels);

    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_32);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 32;
        DIRETTA_LOG("Sink PCM: " << rate << "Hz " << channels << "ch 32-bit");
        return;
    }

    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_24);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 24;
        DIRETTA_LOG("Sink PCM: " << rate << "Hz " << channels << "ch 24-bit");
        return;
    }

    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_16);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 16;
        DIRETTA_LOG("Sink PCM: " << rate << "Hz " << channels << "ch 16-bit");
        return;
    }

    throw std::runtime_error("No supported PCM format found");
}

void DirettaSync::configureSinkDSD(uint32_t dsdBitRate, int channels, const AudioFormat& format) {
    std::lock_guard<std::mutex> lock(m_configMutex);

    DIRETTA_LOG("DSD: bitRate=" << dsdBitRate << " ch=" << channels);

    // Source format: DSF=LSB, DFF=MSB
    bool sourceIsLSB = (format.dsdFormat == AudioFormat::DSDFormat::DSF);
    DIRETTA_LOG("Source DSD format: " << (sourceIsLSB ? "LSB (DSF)" : "MSB (DFF)"));

    const auto& info = getSinkInfo();
    DIRETTA_LOG("Sink DSD support: " << (info.checkSinkSupportDSD() ? "YES" : "NO"));
    DIRETTA_LOG("Sink DSD LSB: " << (info.checkSinkSupportDSDlsb() ? "YES" : "NO"));
    DIRETTA_LOG("Sink DSD MSB: " << (info.checkSinkSupportDSDmsb() ? "YES" : "NO"));

    DIRETTA::FormatConfigure fmt;
    fmt.setSpeed(dsdBitRate);
    fmt.setChannel(channels);

    // Try LSB | BIG first (most common for DSF files)
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_LSB |
                  DIRETTA::FormatID::FMT_DSD_BIG);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal.store(!sourceIsLSB, std::memory_order_release);  // Reverse if source is MSB (DFF)
        m_needDsdByteSwap.store(false, std::memory_order_release);  // BIG endian = no swap
        // Set cached conversion mode: no swap, maybe bit reverse
        m_dsdConversionMode = m_needDsdBitReversal.load(std::memory_order_acquire)
            ? DirettaRingBuffer::DSDConversionMode::BitReverseOnly
            : DirettaRingBuffer::DSDConversionMode::Passthrough;
        DIRETTA_LOG("Sink DSD: LSB | BIG"
                    << (m_needDsdBitReversal.load(std::memory_order_acquire) ? " (bit reversal)" : "")
                    << " mode=" << static_cast<int>(m_dsdConversionMode));
        return;
    }

    // Try MSB | BIG
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_MSB |
                  DIRETTA::FormatID::FMT_DSD_BIG);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal.store(sourceIsLSB, std::memory_order_release);  // Reverse if source is LSB (DSF)
        m_needDsdByteSwap.store(false, std::memory_order_release);  // BIG endian = no swap
        // Set cached conversion mode: no swap, maybe bit reverse
        m_dsdConversionMode = m_needDsdBitReversal.load(std::memory_order_acquire)
            ? DirettaRingBuffer::DSDConversionMode::BitReverseOnly
            : DirettaRingBuffer::DSDConversionMode::Passthrough;
        DIRETTA_LOG("Sink DSD: MSB | BIG"
                    << (m_needDsdBitReversal.load(std::memory_order_acquire) ? " (bit reversal)" : "")
                    << " mode=" << static_cast<int>(m_dsdConversionMode));
        return;
    }

    // Try LSB | LITTLE
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_LSB |
                  DIRETTA::FormatID::FMT_DSD_LITTLE);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal.store(!sourceIsLSB, std::memory_order_release);
        m_needDsdByteSwap.store(true, std::memory_order_release);  // LITTLE endian = swap bytes
        // Set cached conversion mode: always swap, maybe bit reverse
        m_dsdConversionMode = m_needDsdBitReversal.load(std::memory_order_acquire)
            ? DirettaRingBuffer::DSDConversionMode::BitReverseAndSwap
            : DirettaRingBuffer::DSDConversionMode::ByteSwapOnly;
        DIRETTA_LOG("Sink DSD: LSB | LITTLE"
                    << (m_needDsdBitReversal.load(std::memory_order_acquire) ? " (bit reversal)" : "")
                    << " (byte swap) mode=" << static_cast<int>(m_dsdConversionMode));
        return;
    }

    // Try MSB | LITTLE
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_MSB |
                  DIRETTA::FormatID::FMT_DSD_LITTLE);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal.store(sourceIsLSB, std::memory_order_release);
        m_needDsdByteSwap.store(true, std::memory_order_release);  // LITTLE endian = swap bytes
        // Set cached conversion mode: always swap, maybe bit reverse
        m_dsdConversionMode = m_needDsdBitReversal.load(std::memory_order_acquire)
            ? DirettaRingBuffer::DSDConversionMode::BitReverseAndSwap
            : DirettaRingBuffer::DSDConversionMode::ByteSwapOnly;
        DIRETTA_LOG("Sink DSD: MSB | LITTLE"
                    << (m_needDsdBitReversal.load(std::memory_order_acquire) ? " (bit reversal)" : "")
                    << " (byte swap) mode=" << static_cast<int>(m_dsdConversionMode));
        return;
    }

    // Last resort - assume LSB | BIG target
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal.store(!sourceIsLSB, std::memory_order_release);
        m_needDsdByteSwap.store(false, std::memory_order_release);
        DIRETTA_LOG("Sink DSD: FMT_DSD1 only"
                    << (m_needDsdBitReversal.load(std::memory_order_acquire) ? " (bit reversal)" : ""));

        // Set cached conversion mode for optimized DSD path
        bool needReverse = m_needDsdBitReversal.load(std::memory_order_acquire);
        bool needSwap = m_needDsdByteSwap.load(std::memory_order_acquire);
        if (needReverse && needSwap) {
            m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::BitReverseAndSwap;
        } else if (needReverse) {
            m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::BitReverseOnly;
        } else if (needSwap) {
            m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::ByteSwapOnly;
        } else {
            m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::Passthrough;
        }
        DIRETTA_LOG("DSD conversion mode: " << static_cast<int>(m_dsdConversionMode));
        return;
    }

    throw std::runtime_error("No supported DSD format found");
}

//=============================================================================
// Ring Buffer Configuration
//=============================================================================

void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    m_sampleRate.store(rate, std::memory_order_release);
    m_channels.store(channels, std::memory_order_release);
    m_bytesPerSample.store(direttaBps, std::memory_order_release);
    m_inputBytesPerSample.store(inputBps, std::memory_order_release);
    m_need24BitPack.store(direttaBps == 3 && inputBps == 4, std::memory_order_release);
    m_need16To32Upsample.store(direttaBps == 4 && inputBps == 2, std::memory_order_release);
    m_isDsdMode.store(false, std::memory_order_release);
    m_needDsdBitReversal.store(false, std::memory_order_release);
    m_needDsdByteSwap.store(false, std::memory_order_release);
    m_isLowBitrate.store(direttaBps <= 2 && rate <= 48000, std::memory_order_release);
    m_dsdConversionMode = DirettaRingBuffer::DSDConversionMode::Passthrough;

    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);

    size_t bytesPerSecond = static_cast<size_t>(rate) * channels * direttaBps;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::PCM_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x00);
    ringSize = m_ringBuffer.size();

    int bytesPerFrame = channels * direttaBps;
    int framesBase = rate / 1000;
    int framesRemainder = rate % 1000;
    m_bytesPerFrame.store(bytesPerFrame, std::memory_order_release);
    m_framesPerBufferRemainder.store(static_cast<uint32_t>(framesRemainder), std::memory_order_release);
    m_framesPerBufferAccumulator.store(0, std::memory_order_release);
    m_bytesPerBuffer.store(framesBase * bytesPerFrame, std::memory_order_release);

    m_prefillTarget = DirettaBuffer::calculatePrefill(bytesPerSecond, false,
        m_isLowBitrate.load(std::memory_order_acquire));
    m_prefillTarget = std::min(m_prefillTarget, ringSize / 4);
    m_prefillComplete = false;

    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", prefill=" << m_prefillTarget);
}

void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    m_isDsdMode.store(true, std::memory_order_release);
    m_need24BitPack.store(false, std::memory_order_release);
    m_need16To32Upsample.store(false, std::memory_order_release);
    m_channels.store(channels, std::memory_order_release);
    m_isLowBitrate.store(false, std::memory_order_release);

    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);

    uint32_t bytesPerSecond = byteRate * channels;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::DSD_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x69);  // DSD silence
    ringSize = m_ringBuffer.size();

    uint32_t inputBytesPerMs = (byteRate / 1000) * channels;
    size_t bytesPerBuffer = inputBytesPerMs;
    bytesPerBuffer = ((bytesPerBuffer + (4 * channels - 1)) / (4 * channels)) * (4 * channels);
    if (bytesPerBuffer < 64) bytesPerBuffer = 64;
    m_bytesPerBuffer.store(static_cast<int>(bytesPerBuffer), std::memory_order_release);
    m_bytesPerFrame.store(0, std::memory_order_release);
    m_framesPerBufferRemainder.store(0, std::memory_order_release);
    m_framesPerBufferAccumulator.store(0, std::memory_order_release);

    m_prefillTarget = DirettaBuffer::calculatePrefill(bytesPerSecond, true, false);
    m_prefillTarget = std::min(m_prefillTarget, ringSize / 4);
    m_prefillComplete = false;

    DIRETTA_LOG("Ring DSD: byteRate=" << byteRate << " ch=" << channels
                << " buffer=" << ringSize << " prefill=" << m_prefillTarget);
}

//=============================================================================
// Playback Control
//=============================================================================

bool DirettaSync::startPlayback() {
    if (!m_open) return false;
    if (m_playing && !m_paused) return true;

    if (m_paused) {
        resumePlayback();
        return true;
    }

    play();
    m_playing = true;
    m_paused = false;
    return true;
}

void DirettaSync::stopPlayback(bool immediate) {
    // Log accumulated underruns at session end
    uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
    if (underruns > 0) {
        std::cerr << "[DirettaSync] Session had " << underruns << " underrun(s)" << std::endl;
    }

    if (!m_playing) return;

    if (!immediate) {
        requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 50 : 20);

        auto start = std::chrono::steady_clock::now();
        while (m_silenceBuffersRemaining.load() > 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(150)) break;
            std::this_thread::yield();
        }
    }

    stop();
    m_playing = false;
    m_paused = false;
}

void DirettaSync::pausePlayback() {
    if (!m_playing || m_paused) return;

    requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 30 : 10);

    auto start = std::chrono::steady_clock::now();
    while (m_silenceBuffersRemaining.load() > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(80)) break;
        std::this_thread::yield();
    }

    stop();
    m_paused = true;
}

void DirettaSync::resumePlayback() {
    if (!m_paused) return;

    DIRETTA_LOG("Resuming from pause...");

    // Reset flags set during pausePlayback()
    m_draining = false;
    m_stopRequested = false;
    m_silenceBuffersRemaining = 0;

    // Clear stale buffer data and require fresh prefill
    m_ringBuffer.clear();
    m_prefillComplete = false;

    play();
    m_paused = false;
    m_playing = true;

    DIRETTA_LOG("Resumed - buffer cleared, waiting for prefill");
}

void DirettaSync::sendPreTransitionSilence() {
    // Pre-transition silence disabled - was causing issues during format switching
    // The stopPlayback() silence mechanism handles this case adequately
}

//=============================================================================
// Audio Data (Push Interface)
//=============================================================================

size_t DirettaSync::sendAudio(const uint8_t* data, size_t numSamples) {
    if (m_draining.load(std::memory_order_acquire)) return 0;
    if (m_stopRequested.load(std::memory_order_acquire)) return 0;
    if (!is_online()) return 0;

    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) return 0;

    // Generation counter optimization: single atomic load vs 5-6 loads
    // Only reload format atomics when format has actually changed
    uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
    if (gen != m_cachedFormatGen) {
        m_cachedDsdMode = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedPack24bit = m_need24BitPack.load(std::memory_order_acquire);
        m_cachedUpsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
        m_cachedChannels = m_channels.load(std::memory_order_acquire);
        m_cachedBytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
        m_cachedDsdConversionMode = m_dsdConversionMode;
        m_cachedFormatGen = gen;
    }

    // Use cached values (no atomic loads in hot path)
    bool dsdMode = m_cachedDsdMode;
    bool pack24bit = m_cachedPack24bit;
    bool upsample16to32 = m_cachedUpsample16to32;
    int numChannels = m_cachedChannels;
    int bytesPerSample = m_cachedBytesPerSample;

    size_t written = 0;
    size_t totalBytes;
    const char* formatLabel;

    if (dsdMode) {
        // DSD: numSamples encoding from AudioEngine
        // numSamples = (totalBytes * 8) / channels
        // Reverse: totalBytes = numSamples * channels / 8
        totalBytes = (numSamples * numChannels) / 8;

        // Use optimized path with cached conversion mode (no per-iteration branching)
        written = m_ringBuffer.pushDSDPlanarOptimized(
            data, totalBytes, numChannels, m_cachedDsdConversionMode);
        formatLabel = "DSD";

    } else if (pack24bit) {
        // PCM 24-bit: numSamples is sample count
        size_t bytesPerFrame = 4 * numChannels;  // S24_P32
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push24BitPacked(data, totalBytes);
        formatLabel = "PCM24";

    } else if (upsample16to32) {
        // PCM 16->32
        size_t bytesPerFrame = 2 * numChannels;
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push16To32(data, totalBytes);
        formatLabel = "PCM16->32";

    } else {
        // PCM direct copy
        size_t bytesPerFrame = static_cast<size_t>(bytesPerSample) * numChannels;
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push(data, totalBytes);
        formatLabel = "PCM";
    }

    // Check prefill completion
    if (written > 0) {
        if (!m_prefillComplete.load(std::memory_order_acquire)) {
            if (m_ringBuffer.getAvailable() >= m_prefillTarget) {
                m_prefillComplete = true;
                DIRETTA_LOG(formatLabel << " prefill complete: " << m_ringBuffer.getAvailable() << " bytes");
            }
        }

        if (g_verbose) {
            int count = m_pushCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 3 || count % 500 == 0) {
                DIRETTA_LOG("sendAudio #" << count << " in=" << totalBytes
                            << " out=" << written << " avail=" << m_ringBuffer.getAvailable()
                            << " [" << formatLabel << "]");
            }
        }
    }

    return written;
}

float DirettaSync::getBufferLevel() const {
    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) return 0.0f;
    size_t size = m_ringBuffer.size();
    if (size == 0) return 0.0f;
    return static_cast<float>(m_ringBuffer.getAvailable()) / static_cast<float>(size);
}

//=============================================================================
// DIRETTA::Sync Overrides
//=============================================================================

bool DirettaSync::getNewStream(diretta_stream& baseStream) {
    // SDK 148+ uses diretta_stream& but passes DIRETTA::Stream objects
    DIRETTA::Stream& stream = static_cast<DIRETTA::Stream&>(baseStream);

    m_workerActive = true;

    // C1: Generation counter optimization for stable state
    // Single atomic load in common case (format rarely changes during playback)
    uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
    if (gen != m_cachedConsumerGen) {
        // Cold path: reload stable state values
        m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
        m_cachedSilenceByte = m_ringBuffer.silenceByte();
        m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedConsumerSampleRate = m_sampleRate.load(std::memory_order_acquire);
        // PCM buffer rounding drift fix values (stable per-track)
        m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        m_cachedFramesPerBufferRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
        m_cachedConsumerGen = gen;
    }

    // Hot path: use cached values
    int currentBytesPerBuffer = m_cachedBytesPerBuffer;
    uint8_t currentSilenceByte = m_cachedSilenceByte;

    // PCM buffer rounding drift fix: accumulator adjusts buffer size for 44.1k family
    // Uses cached remainder/bytesPerFrame, only accumulator is per-call
    if (m_cachedFramesPerBufferRemainder != 0) {
        uint32_t acc = m_framesPerBufferAccumulator.load(std::memory_order_relaxed);
        acc += m_cachedFramesPerBufferRemainder;
        if (acc >= 1000) {
            acc -= 1000;
            currentBytesPerBuffer += m_cachedBytesPerFrame;
        }
        m_framesPerBufferAccumulator.store(acc, std::memory_order_relaxed);
    }

    if (stream.size() != static_cast<size_t>(currentBytesPerBuffer)) {
        stream.resize(currentBytesPerBuffer);
    }

    uint8_t* dest = reinterpret_cast<uint8_t*>(stream.get_16());

    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    bool currentIsDsd = m_cachedConsumerIsDsd;
    size_t currentRingSize = m_ringBuffer.size();

    // Shutdown silence
    int silenceRemaining = m_silenceBuffersRemaining.load(std::memory_order_acquire);
    if (silenceRemaining > 0) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_silenceBuffersRemaining.fetch_sub(1, std::memory_order_acq_rel);
        m_workerActive = false;
        return true;
    }

    // Stop requested
    if (m_stopRequested.load(std::memory_order_acquire)) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    // Prefill not complete
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    // Post-online stabilization
    // Scale stabilization to achieve consistent WARMUP TIME regardless of MTU
    // With small MTU (1500), getNewStream() is called more frequently (shorter cycle time)
    // With large MTU (9000+), calls are less frequent (longer cycle time)
    // We need to scale buffer count to achieve target warmup duration
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int stabilizationTarget = static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS);

        if (currentIsDsd) {
            // Target warmup time scales with DSD rate:
            // DSD64: 50ms, DSD128: 100ms, DSD256: 200ms, DSD512: 400ms
            // C1: Use cached sample rate from generation counter
            int currentSampleRate = m_cachedConsumerSampleRate;
            int dsdMultiplier = currentSampleRate / 2822400;  // DSD64 = 1
            int targetWarmupMs = 50 * std::max(1, dsdMultiplier);  // 50ms baseline

            // Calculate cycle time based on MTU and data rate
            // cycleTime = (efficientMTU / bytesPerSecond) in microseconds
            int efficientMTU = static_cast<int>(m_effectiveMTU) - 24;  // Subtract overhead
            double bytesPerSecond = static_cast<double>(currentSampleRate) * 2 / 8.0;  // 2ch, 1bit
            double cycleTimeUs = (static_cast<double>(efficientMTU) / bytesPerSecond) * 1000000.0;

            // Calculate buffers needed for target warmup time
            // targetWarmupMs * 1000 = warmup in microseconds
            double buffersNeeded = (targetWarmupMs * 1000.0) / cycleTimeUs;
            stabilizationTarget = static_cast<int>(std::ceil(buffersNeeded));

            // Clamp to reasonable range
            stabilizationTarget = std::max(50, std::min(stabilizationTarget, 3000));
        }

        int count = m_stabilizationCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= stabilizationTarget) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount = 0;
            DIRETTA_LOG("Post-online stabilization complete (" << count << " buffers)");
        }
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    int count = m_streamCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t avail = m_ringBuffer.getAvailable();

    if (g_verbose && (count <= 5 || count % 5000 == 0)) {
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        DIRETTA_LOG("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                    << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                    << fillPct << "%) " << (currentIsDsd ? "[DSD]" : "[PCM]"));
    }

    // Underrun - count silently, log at session end
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    m_workerActive = false;
    return true;
}

bool DirettaSync::startSyncWorker() {
    std::lock_guard<std::mutex> lock(m_workerMutex);

    DIRETTA_LOG("startSyncWorker (running=" << m_running.load() << ")");

    if (m_running.load() && m_workerThread.joinable()) {
        DIRETTA_LOG("Worker already running");
        return true;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_running = true;
    m_stopRequested = false;

    m_workerThread = std::thread([this]() {
        while (m_running.load(std::memory_order_acquire)) {
            if (!syncWorker()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    });

    return true;
}

//=============================================================================
// Internal Helpers
//=============================================================================

void DirettaSync::beginReconfigure() {
    m_reconfiguring.store(true, std::memory_order_release);
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void DirettaSync::endReconfigure() {
    m_reconfiguring.store(false, std::memory_order_release);
}

void DirettaSync::shutdownWorker() {
    m_stopRequested = true;
    m_running = false;

    int waitCount = 0;
    while (m_workerActive.load(std::memory_order_acquire) && waitCount < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void DirettaSync::requestShutdownSilence(int buffers) {
    m_silenceBuffersRemaining = buffers;
    m_draining = true;
    DIRETTA_LOG("Requested " << buffers << " shutdown silence buffers");
}

bool DirettaSync::waitForOnline(unsigned int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeoutMs);

    while (!is_online()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            DIRETTA_LOG("Online timeout");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    DIRETTA_LOG("Online after " << elapsed << "ms");
    return true;
}

void DirettaSync::applyTransferMode(DirettaTransferMode mode, ACQUA::Clock cycleTime) {
    if (mode == DirettaTransferMode::AUTO) {
        if (m_isLowBitrate.load(std::memory_order_acquire) ||
            m_isDsdMode.load(std::memory_order_acquire)) {
            DIRETTA_LOG("Using VarAuto");
            configTransferVarAuto(cycleTime);
        } else {
            DIRETTA_LOG("Using VarMax");
            configTransferVarMax(cycleTime);
        }
        return;
    }

    switch (mode) {
        case DirettaTransferMode::FIX_AUTO:
            configTransferFixAuto(cycleTime);
            break;
        case DirettaTransferMode::VAR_AUTO:
            configTransferVarAuto(cycleTime);
            break;
        case DirettaTransferMode::VAR_MAX:
        default:
            configTransferVarMax(cycleTime);
            break;
    }
}

unsigned int DirettaSync::calculateCycleTime(uint32_t sampleRate, int channels, int bitsPerSample) {
    if (!m_config.cycleTimeAuto || !m_calculator) {
        return m_config.cycleTime;
    }
    return m_calculator->calculate(sampleRate, channels, bitsPerSample);
}
