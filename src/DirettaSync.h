/**
 * @file DirettaSync.h
 * @brief Unified Diretta sync adapter for UPnP Renderer
 *
 * Merged from DirettaSyncAdapter and DirettaOutput for cleaner architecture.
 * Based on MPD Diretta Output Plugin v0.4.0
 */

#ifndef DIRETTA_SYNC_H
#define DIRETTA_SYNC_H

#include "DirettaRingBuffer.h"

#include <Sync.hpp>
#include <Find.hpp>
#include <Stream.hpp>
#include <Format.hpp>
#include <Profile.hpp>
#include <ACQUA/IPAddress.hpp>
#include <ACQUA/Clock.hpp>

#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>
#include <cmath>

//=============================================================================
// Debug Logging
//=============================================================================

extern bool g_verbose;

#define DIRETTA_LOG(msg) do { \
    if (g_verbose) { \
        std::cout << "[DirettaSync] " << msg << std::endl; \
    } \
} while(0)

//=============================================================================
// Audio Format
//=============================================================================

struct AudioFormat {
    uint32_t sampleRate = 44100;
    uint32_t bitDepth = 16;
    uint32_t channels = 2;
    bool isDSD = false;
    bool isCompressed = false;

    enum class DSDFormat { DSF, DFF };
    DSDFormat dsdFormat = DSDFormat::DSF;

    AudioFormat() = default;

    AudioFormat(uint32_t rate, uint32_t bits, uint32_t ch)
        : sampleRate(rate), bitDepth(bits), channels(ch),
          isDSD(false), isCompressed(false), dsdFormat(DSDFormat::DSF) {}

    bool operator==(const AudioFormat& other) const {
        return sampleRate == other.sampleRate &&
               bitDepth == other.bitDepth &&
               channels == other.channels &&
               isDSD == other.isDSD;
    }

    bool operator!=(const AudioFormat& other) const { return !(*this == other); }
};

//=============================================================================
// Buffer Configuration
//=============================================================================

namespace DirettaBuffer {
    constexpr float DSD_BUFFER_SECONDS = 0.8f;
    constexpr float PCM_BUFFER_SECONDS = 1.0f;

    constexpr size_t DSD_PREFILL_MS = 200;
    constexpr size_t PCM_PREFILL_MS = 50;
    constexpr size_t PCM_LOWRATE_PREFILL_MS = 100;

    constexpr unsigned int DAC_STABILIZATION_MS = 100;
    constexpr unsigned int ONLINE_WAIT_MS = 2000;
    constexpr unsigned int FORMAT_SWITCH_DELAY_MS = 800;
    constexpr unsigned int POST_ONLINE_SILENCE_BUFFERS = 50;

    // UPnP push model needs larger buffers than MPD's pull model
    constexpr size_t MIN_BUFFER_BYTES = 3072000;  // ~2 seconds at 192kHz
    constexpr size_t MAX_BUFFER_BYTES = 16777216;
    constexpr size_t MIN_PREFILL_BYTES = 1024;

    inline size_t calculateBufferSize(size_t bytesPerSecond, float seconds) {
        size_t size = static_cast<size_t>(bytesPerSecond * seconds);
        size = std::max(size, MIN_BUFFER_BYTES);
        size = std::min(size, MAX_BUFFER_BYTES);
        return size;
    }

    inline size_t calculatePrefill(size_t bytesPerSecond, bool isDsd, bool isLowBitrate) {
        size_t prefillMs = isDsd ? DSD_PREFILL_MS :
                           isLowBitrate ? PCM_LOWRATE_PREFILL_MS : PCM_PREFILL_MS;
        size_t result = (bytesPerSecond * prefillMs) / 1000;
        return std::max(result, MIN_PREFILL_BYTES);
    }
}

//=============================================================================
// Cycle Calculator
//=============================================================================

class DirettaCycleCalculator {
public:
    static constexpr int OVERHEAD = 24;

    explicit DirettaCycleCalculator(uint32_t mtu = 1500)
        : m_mtu(mtu), m_efficientMTU(mtu - OVERHEAD) {}

    unsigned int calculate(uint32_t sampleRate, int channels, int bitsPerSample) const {
        double bytesPerSecond = static_cast<double>(sampleRate) * channels * bitsPerSample / 8.0;
        double cycleTimeUs = (static_cast<double>(m_efficientMTU) / bytesPerSecond) * 1000000.0;
        unsigned int result = static_cast<unsigned int>(std::round(cycleTimeUs));
        return std::max(100u, std::min(result, 50000u));
    }

private:
    uint32_t m_mtu;
    int m_efficientMTU;
};

//=============================================================================
// Transfer Mode
//=============================================================================

enum class DirettaTransferMode { FIX_AUTO, VAR_AUTO, VAR_MAX, AUTO };

//=============================================================================
// Configuration
//=============================================================================

struct DirettaConfig {
    unsigned int cycleTime = 2620;
    bool cycleTimeAuto = true;
    DirettaTransferMode transferMode = DirettaTransferMode::AUTO;
    int threadMode = 1;
    unsigned int mtu = 0;  // 0 = auto-detect
    unsigned int mtuFallback = 1500;
    unsigned int dacStabilizationMs = DirettaBuffer::DAC_STABILIZATION_MS;
    unsigned int onlineWaitMs = DirettaBuffer::ONLINE_WAIT_MS;
    unsigned int formatSwitchDelayMs = DirettaBuffer::FORMAT_SWITCH_DELAY_MS;
};

//=============================================================================
// DirettaSync - Main Class
//=============================================================================

class DirettaSync : public DIRETTA::Sync {
public:
    DirettaSync();
    ~DirettaSync();

    // Non-copyable
    DirettaSync(const DirettaSync&) = delete;
    DirettaSync& operator=(const DirettaSync&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    /**
     * @brief Initialize and discover Diretta target (like MPD's Enable())
     */
    bool enable(const DirettaConfig& config = DirettaConfig());

    /**
     * @brief Shutdown (like MPD's Disable())
     */
    void disable();

    bool isEnabled() const { return m_enabled; }

    //=========================================================================
    // Connection (like MPD's Open/Close)
    //=========================================================================

    /**
     * @brief Open connection with specified format
     */
    bool open(const AudioFormat& format);

    /**
     * @brief Close connection
     */
    void close();

    bool isOpen() const { return m_open; }
    bool isOnline() { return is_online(); }

    //=========================================================================
    // Playback Control
    //=========================================================================

    bool startPlayback();
    void stopPlayback(bool immediate = false);
    void pausePlayback();
    void resumePlayback();

    bool isPlaying() const { return m_playing; }
    bool isPaused() const { return m_paused; }

    //=========================================================================
    // Audio Data
    //=========================================================================

    /**
     * @brief Send audio data (push model)
     * @param data Audio buffer
     * @param numSamples Number of samples (for PCM) or special encoding for DSD
     * @return Bytes consumed
     */
    size_t sendAudio(const uint8_t* data, size_t numSamples);

    float getBufferLevel() const;
    const AudioFormat& getFormat() const { return m_currentFormat; }

    //=========================================================================
    // Target Management
    //=========================================================================

    void setTargetIndex(int index) { m_targetIndex = index; }
    void setMTU(uint32_t mtu) { m_mtuOverride = mtu; }
    bool verifyTargetAvailable();
    static void listTargets();

protected:
    //=========================================================================
    // DIRETTA::Sync Overrides
    //=========================================================================

    bool getNewStream(DIRETTA::Stream& stream) override;
    bool getNewStreamCmp() override { return true; }
    bool startSyncWorker() override;
    void statusUpdate() override {}

private:
    //=========================================================================
    // Internal Methods
    //=========================================================================

    bool discoverTarget();
    bool measureMTU();
    bool openSyncConnection();
    bool reopenForFormatChange();
    void fullReset();
    void shutdownWorker();

    void configureSinkPCM(int rate, int channels, int inputBits, int& acceptedBits);
    void configureSinkDSD(uint32_t dsdBitRate, int channels, const AudioFormat& format);
    void configureRingPCM(int rate, int channels, int direttaBps, int inputBps);
    void configureRingDSD(uint32_t byteRate, int channels);

    void applyTransferMode(DirettaTransferMode mode, ACQUA::Clock cycleTime);
    unsigned int calculateCycleTime(uint32_t sampleRate, int channels, int bitsPerSample);
    void requestShutdownSilence(int buffers);
    bool waitForOnline(unsigned int timeoutMs);
    void logSinkCapabilities();

    //=========================================================================
    // State
    //=========================================================================

    DirettaConfig m_config;
    std::unique_ptr<DirettaCycleCalculator> m_calculator;

    // Target
    ACQUA::IPAddress m_targetAddress;
    int m_targetIndex = -1;
    uint32_t m_mtuOverride = 0;
    uint32_t m_effectiveMTU = 1500;

    // Connection state
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};

    // Format tracking
    AudioFormat m_currentFormat;
    AudioFormat m_previousFormat;
    bool m_hasPreviousFormat = false;

    // Worker thread
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_draining{false};
    std::atomic<bool> m_workerActive{false};
    std::thread m_workerThread;
    std::mutex m_workerMutex;
    std::mutex m_configMutex;
    std::mutex m_pushMutex;

    // Ring buffer
    DirettaRingBuffer m_ringBuffer;

    // Format parameters (protected by m_configMutex)
    int m_sampleRate = 44100;
    int m_channels = 2;
    int m_bytesPerSample = 2;
    int m_inputBytesPerSample = 2;
    int m_bytesPerBuffer = 176;
    bool m_need24BitPack = false;
    bool m_need16To32Upsample = false;
    bool m_isDsdMode = false;
    bool m_needDsdBitReversal = false;
    bool m_needDsdByteSwap = false;  // For LITTLE endian targets
    bool m_isLowBitrate = false;

    // Prefill and stabilization
    size_t m_prefillTarget = 0;
    std::atomic<bool> m_prefillComplete{false};
    std::atomic<bool> m_postOnlineDelayDone{false};
    std::atomic<int> m_silenceBuffersRemaining{0};
    std::atomic<int> m_stabilizationCount{0};

    // Statistics
    std::atomic<int> m_streamCount{0};
    std::atomic<int> m_pushCount{0};
};

#endif // DIRETTA_SYNC_H
