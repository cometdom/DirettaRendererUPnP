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

//=============================================================================
// Bit reversal lookup table for DSD MSB<->LSB conversion
//=============================================================================

static const uint8_t bitReverseTable[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};

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
    for (int attempt = 0; attempt < 3 && !opened; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("open() retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

    std::cout << "Available Diretta Targets (" << results.size() << " found):" << std::endl;

    int index = 1;
    for (const auto& target : results) {
        std::cout << "[" << index << "] " << target.second.targetName << std::endl;
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

        if (sameFormat) {
            std::cout << "[DirettaSync] Same format - quick resume (no setSink)" << std::endl;
            // Light reset - just clear buffer and reset flags, don't stop workers
            m_ringBuffer.clear();
            m_prefillComplete = false;
            m_stopRequested = false;
            play();
            m_playing = true;
            m_paused = false;
            std::cout << "[DirettaSync] ========== OPEN COMPLETE (quick) ==========" << std::endl;
            return true;
        } else {
            // Format change - need full reopen for reliable Target reconfiguration
            // Some Targets need DIRETTA::Sync to be fully closed and reopened
            std::cout << "[DirettaSync] Format change - full reopen" << std::endl;
            if (!reopenForFormatChange()) {
                std::cerr << "[DirettaSync] Failed to reopen for format change" << std::endl;
                return false;
            }
            needFullConnect = true;
        }
    }

    // Full reset for first open or after format change reopen
    if (needFullConnect) {
        fullReset();
    }
    m_isDsdMode = newIsDsd;

    uint32_t effectiveSampleRate;
    int effectiveChannels = format.channels;
    int bitsPerSample;

    if (m_isDsdMode) {
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
    int maxAttempts = needFullConnect ? 20 : 15;
    int retryDelayMs = needFullConnect ? 500 : 300;
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
        for (int attempt = 0; attempt < 3 && !connected; attempt++) {
            if (attempt > 0) {
                DIRETTA_LOG("connect retry #" << attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    requestShutdownSilence(m_isDsdMode ? 50 : 20);

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

bool DirettaSync::reopenForFormatChange() {
    DIRETTA_LOG("reopenForFormatChange: sending silence before format switch...");

    // Send silence buffers to let DAC mute gracefully before format change
    // More silence for DSD to prevent loud cracks
    int silenceBuffers = m_isDsdMode ? 100 : 30;
    requestShutdownSilence(silenceBuffers);

    // Wait for silence to be sent
    auto start = std::chrono::steady_clock::now();
    while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(300)) {
            DIRETTA_LOG("Silence timeout in reopenForFormatChange");
            break;
        }
        std::this_thread::yield();
    }

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
    for (int attempt = 0; attempt < 10 && !sinkFound; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("setSink retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
        std::lock_guard<std::mutex> pushLock(m_pushMutex);

        m_prefillComplete = false;
        m_postOnlineDelayDone = false;
        m_silenceBuffersRemaining = 0;
        m_stabilizationCount = 0;
        m_streamCount = 0;
        m_pushCount = 0;
        m_isDsdMode = false;
        m_needDsdBitReversal = false;
        m_needDsdByteSwap = false;
        m_isLowBitrate = false;
        m_need24BitPack = false;
        m_need16To32Upsample = false;

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
        m_needDsdBitReversal = !sourceIsLSB;  // Reverse if source is MSB (DFF)
        m_needDsdByteSwap = false;  // BIG endian = no swap
        DIRETTA_LOG("Sink DSD: LSB | BIG" << (m_needDsdBitReversal ? " (bit reversal)" : ""));
        return;
    }

    // Try MSB | BIG
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_MSB |
                  DIRETTA::FormatID::FMT_DSD_BIG);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal = sourceIsLSB;  // Reverse if source is LSB (DSF)
        m_needDsdByteSwap = false;  // BIG endian = no swap
        DIRETTA_LOG("Sink DSD: MSB | BIG" << (m_needDsdBitReversal ? " (bit reversal)" : ""));
        return;
    }

    // Try LSB | LITTLE
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_LSB |
                  DIRETTA::FormatID::FMT_DSD_LITTLE);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal = !sourceIsLSB;
        m_needDsdByteSwap = true;  // LITTLE endian = swap bytes
        DIRETTA_LOG("Sink DSD: LSB | LITTLE" << (m_needDsdBitReversal ? " (bit reversal)" : "") << " (byte swap)");
        return;
    }

    // Try MSB | LITTLE
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_MSB |
                  DIRETTA::FormatID::FMT_DSD_LITTLE);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal = sourceIsLSB;
        m_needDsdByteSwap = true;  // LITTLE endian = swap bytes
        DIRETTA_LOG("Sink DSD: MSB | LITTLE" << (m_needDsdBitReversal ? " (bit reversal)" : "") << " (byte swap)");
        return;
    }

    // Last resort - assume LSB | BIG target
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        m_needDsdBitReversal = !sourceIsLSB;
        m_needDsdByteSwap = false;
        DIRETTA_LOG("Sink DSD: FMT_DSD1 only" << (m_needDsdBitReversal ? " (bit reversal)" : ""));
        return;
    }

    throw std::runtime_error("No supported DSD format found");
}

//=============================================================================
// Ring Buffer Configuration
//=============================================================================

void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> pushLock(m_pushMutex);

    m_sampleRate = rate;
    m_channels = channels;
    m_bytesPerSample = direttaBps;
    m_inputBytesPerSample = inputBps;
    m_need24BitPack = (direttaBps == 3 && inputBps == 4);
    m_need16To32Upsample = (direttaBps == 4 && inputBps == 2);
    m_isDsdMode = false;
    m_needDsdBitReversal = false;
    m_needDsdByteSwap = false;
    m_isLowBitrate = (direttaBps <= 2 && rate <= 48000);

    size_t bytesPerSecond = static_cast<size_t>(rate) * channels * direttaBps;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::PCM_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x00);

    m_bytesPerBuffer = ((rate + 999) / 1000) * channels * direttaBps;

    m_prefillTarget = DirettaBuffer::calculatePrefill(bytesPerSecond, false, m_isLowBitrate);
    m_prefillTarget = std::min(m_prefillTarget, ringSize / 4);
    m_prefillComplete = false;

    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", prefill=" << m_prefillTarget);
}

void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::lock_guard<std::mutex> pushLock(m_pushMutex);

    m_isDsdMode = true;
    m_need24BitPack = false;
    m_need16To32Upsample = false;
    m_channels = channels;
    m_isLowBitrate = false;

    uint32_t bytesPerSecond = byteRate * channels;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::DSD_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x69);  // DSD silence

    uint32_t inputBytesPerMs = (byteRate / 1000) * channels;
    m_bytesPerBuffer = inputBytesPerMs;
    m_bytesPerBuffer = ((m_bytesPerBuffer + (4 * channels - 1)) / (4 * channels)) * (4 * channels);
    if (m_bytesPerBuffer < 64) m_bytesPerBuffer = 64;

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
    if (!m_playing) return;

    if (!immediate) {
        requestShutdownSilence(m_isDsdMode ? 50 : 20);

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

    requestShutdownSilence(m_isDsdMode ? 30 : 10);

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

    play();
    m_paused = false;
    m_playing = true;
}

//=============================================================================
// Audio Data (Push Interface)
//=============================================================================

size_t DirettaSync::sendAudio(const uint8_t* data, size_t numSamples) {
    if (m_draining.load(std::memory_order_acquire)) return 0;
    if (m_stopRequested.load(std::memory_order_acquire)) return 0;
    if (!is_online()) return 0;

    std::lock_guard<std::mutex> lock(m_pushMutex);

    // Snapshot config state
    bool dsdMode, pack24bit, upsample16to32, needBitReversal, needByteSwap;
    int numChannels;
    {
        std::lock_guard<std::mutex> configLock(m_configMutex);
        dsdMode = m_isDsdMode;
        pack24bit = m_need24BitPack;
        upsample16to32 = m_need16To32Upsample;
        needBitReversal = m_needDsdBitReversal;
        needByteSwap = m_needDsdByteSwap;
        numChannels = m_channels;
    }

    size_t written = 0;
    size_t totalBytes;
    const char* formatLabel;

    if (dsdMode) {
        // DSD: numSamples encoding from AudioEngine
        // numSamples = (totalBytes * 8) / channels
        // Reverse: totalBytes = numSamples * channels / 8
        totalBytes = (numSamples * numChannels) / 8;

        written = m_ringBuffer.pushDSDPlanar(
            data, totalBytes, numChannels,
            needBitReversal ? bitReverseTable : nullptr,
            needByteSwap);
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
        size_t bytesPerFrame = (m_bytesPerSample) * numChannels;
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

        int count = m_pushCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 3 || count % 500 == 0) {
            DIRETTA_LOG("sendAudio #" << count << " in=" << totalBytes
                        << " out=" << written << " avail=" << m_ringBuffer.getAvailable()
                        << " [" << formatLabel << "]");
        }
    }

    return written;
}

float DirettaSync::getBufferLevel() const {
    size_t size = m_ringBuffer.size();
    if (size == 0) return 0.0f;
    return static_cast<float>(m_ringBuffer.getAvailable()) / static_cast<float>(size);
}

//=============================================================================
// DIRETTA::Sync Overrides
//=============================================================================

bool DirettaSync::getNewStream(DIRETTA::Stream& stream) {
    m_workerActive = true;

    // Snapshot config under mutex
    int currentBytesPerBuffer;
    uint8_t currentSilenceByte;
    bool currentIsDsd;
    size_t currentRingSize;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        currentBytesPerBuffer = m_bytesPerBuffer;
        currentSilenceByte = m_ringBuffer.silenceByte();
        currentIsDsd = m_isDsdMode;
        currentRingSize = m_ringBuffer.size();
    }

    if (stream.size() != static_cast<size_t>(currentBytesPerBuffer)) {
        stream.resize(currentBytesPerBuffer);
    }

    uint8_t* dest = reinterpret_cast<uint8_t*>(stream.get_16());

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
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int count = m_stabilizationCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS)) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount = 0;
            DIRETTA_LOG("Post-online stabilization complete");
        }
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    int count = m_streamCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    size_t avail = m_ringBuffer.getAvailable();

    if (count <= 5 || count % 5000 == 0) {
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        DIRETTA_LOG("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                    << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                    << fillPct << "%) " << (currentIsDsd ? "[DSD]" : "[PCM]"));
    }

    // Underrun
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        std::cerr << "[DirettaSync] UNDERRUN #" << count
                  << " avail=" << avail << " need=" << currentBytesPerBuffer << std::endl;
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
        if (m_isLowBitrate || m_isDsdMode) {
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
