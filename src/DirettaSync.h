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
// Retry Configuration
//=============================================================================

namespace DirettaRetry {
    // Connection establishment (DIRETTA::Sync::open)
    constexpr int OPEN_RETRIES = 3;
    constexpr int OPEN_DELAY_MS = 500;

    // setSink configuration
    constexpr int SETSINK_RETRIES_FULL = 20;      // After disconnect
    constexpr int SETSINK_RETRIES_QUICK = 15;     // Quick reconfigure
    constexpr int SETSINK_DELAY_FULL_MS = 500;
    constexpr int SETSINK_DELAY_QUICK_MS = 300;

    // connect() call
    constexpr int CONNECT_RETRIES = 3;
    constexpr int CONNECT_DELAY_MS = 500;

    // Format change reopen
    constexpr int REOPEN_SINK_RETRIES = 10;
    constexpr int REOPEN_SINK_DELAY_MS = 500;
}

//=============================================================================
// Buffer Configuration
//=============================================================================

namespace DirettaBuffer {
    constexpr float DSD_BUFFER_SECONDS = 0.8f;
    constexpr float PCM_BUFFER_SECONDS = 0.3f;  // Was 1.0f - low latency

    constexpr size_t DSD_PREFILL_MS = 200;
    constexpr size_t PCM_PREFILL_MS = 30;       // Was 50 - faster start
    constexpr size_t PCM_LOWRATE_PREFILL_MS = 100;

    constexpr unsigned int DAC_STABILIZATION_MS = 100;
    constexpr unsigned int ONLINE_WAIT_MS = 2000;
    constexpr unsigned int FORMAT_SWITCH_DELAY_MS = 800;
    constexpr unsigned int POST_ONLINE_SILENCE_BUFFERS = 50;

    // UPnP push model needs larger buffers than MPD's pull model
    // 64KB = ~370ms floor at 44.1kHz/16-bit, negligible at higher rates
    constexpr size_t MIN_BUFFER_BYTES = 65536;  // Was 3072000
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

    // Calculate DSD samples per call based on rate
    // Target: ~10-12ms chunks for consistent scheduling granularity
    // Returns DSD samples (1-bit), which convert to bytes via: bytes = samples * channels / 8
    inline size_t calculateDsdSamplesPerCall(uint32_t dsdSampleRate) {
        // Target chunk duration in milliseconds
        constexpr double TARGET_CHUNK_MS = 12.0;

        // Limits
        constexpr size_t MIN_DSD_SAMPLES = 8192;   // ~3ms at DSD64
        constexpr size_t MAX_DSD_SAMPLES = 131072; // ~46ms at DSD64, ~3ms at DSD1024

        // Calculate samples for target duration
        // DSD sample rate is the 1-bit rate (e.g., 2822400 for DSD64)
        size_t samplesPerCall = static_cast<size_t>(dsdSampleRate * TARGET_CHUNK_MS / 1000.0);

        // Round to multiple of 256 for alignment (32 bytes per channel minimum)
        samplesPerCall = ((samplesPerCall + 255) / 256) * 256;

        // Clamp to reasonable range (match existing std::max/std::min pattern)
        samplesPerCall = std::max(samplesPerCall, MIN_DSD_SAMPLES);
        samplesPerCall = std::min(samplesPerCall, MAX_DSD_SAMPLES);

        return samplesPerCall;
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
     * @brief Close connection (keeps SDK ready for quick resume)
     */
    void close();

    /**
     * @brief Release target completely (closes SDK connection)
     *
     * Use this when playback has ended and we want to fully release
     * the target so it can accept connections from other sources.
     * The next open() will automatically reopen the SDK.
     */
    void release();

    bool isOpen() const { return m_open; }
    bool isOnline() { return is_online(); }

    //=========================================================================
    // Playback Control
    //=========================================================================

    bool startPlayback();
    void stopPlayback(bool immediate = false);
    void pausePlayback();
    void resumePlayback();

    /**
     * @brief Send silence buffers before format transition
     *
     * Call this BEFORE stopPlayback() when changing formats (DSD→PCM, DSD rate change).
     * Silence buffers flush the Diretta pipeline to prevent crackling on transitions.
     * Scales silence duration with DSD rate (higher rates need more buffers).
     */
    void sendPreTransitionSilence();

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

    /**
     * @brief Set S24 pack mode hint for 24-bit audio
     *
     * Propagates alignment hint from TrackInfo to ring buffer for better
     * 24-bit sample detection when track starts with silence.
     */
    void setS24PackModeHint(DirettaRingBuffer::S24PackMode hint) {
        m_ringBuffer.setS24PackModeHint(hint);
    }

    // EXPERIMENTAL: Force full reopen on next open() call
    // When set, bypasses quick-reconnect even for same-format tracks.
    // Use case: User-initiated track changes (vs gapless sequential playback)
    // Set this flag before stopping playback on user interaction.
    void setForceFullReopen(bool force) { m_forceFullReopen = force; }

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

    bool getNewStream(diretta_stream& stream) override;
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
    void beginReconfigure();
    void endReconfigure();

    void applyTransferMode(DirettaTransferMode mode, ACQUA::Clock cycleTime);
    unsigned int calculateCycleTime(uint32_t sampleRate, int channels, int bitsPerSample);
    void requestShutdownSilence(int buffers);
    bool waitForOnline(unsigned int timeoutMs);
    void logSinkCapabilities();

    class ReconfigureGuard {
    public:
        explicit ReconfigureGuard(DirettaSync& sync) : sync_(sync) { sync_.beginReconfigure(); }
        ~ReconfigureGuard() { sync_.endReconfigure(); }
        ReconfigureGuard(const ReconfigureGuard&) = delete;
        ReconfigureGuard& operator=(const ReconfigureGuard&) = delete;

    private:
        DirettaSync& sync_;
    };

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
    std::atomic<bool> m_enabled{false};      // Target discovered, ready to use
    std::atomic<bool> m_sdkOpen{false};      // SDK-level connection open
    std::atomic<bool> m_open{false};         // Connected to target for playback
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

    // EXPERIMENTAL: Force full reopen on user-initiated track changes
    // Cleared after use in open()
    bool m_forceFullReopen{false};
    std::atomic<bool> m_workerActive{false};
    std::thread m_workerThread;
    std::mutex m_workerMutex;
    std::mutex m_configMutex;
    std::atomic<bool> m_reconfiguring{false};
    mutable std::atomic<int> m_ringUsers{0};

    // Ring buffer
    DirettaRingBuffer m_ringBuffer;

    // Format parameters (atomic snapshot for audio thread)
    std::atomic<int> m_sampleRate{44100};
    std::atomic<int> m_channels{2};
    std::atomic<int> m_bytesPerSample{2};
    std::atomic<int> m_inputBytesPerSample{2};
    std::atomic<int> m_bytesPerBuffer{176};
    std::atomic<int> m_bytesPerFrame{0};
    std::atomic<uint32_t> m_framesPerBufferRemainder{0};
    std::atomic<uint32_t> m_framesPerBufferAccumulator{0};
    std::atomic<bool> m_need24BitPack{false};
    std::atomic<bool> m_need16To32Upsample{false};
    std::atomic<bool> m_isDsdMode{false};
    std::atomic<bool> m_needDsdBitReversal{false};
    std::atomic<bool> m_needDsdByteSwap{false};  // For LITTLE endian targets
    std::atomic<bool> m_isLowBitrate{false};

    // Cached DSD conversion mode - set at track open, eliminates per-iteration branch checks
    DirettaRingBuffer::DSDConversionMode m_dsdConversionMode{DirettaRingBuffer::DSDConversionMode::Passthrough};

    // Format generation counter - incremented on ANY format change
    // Allows sendAudio to skip reloading atomics when format hasn't changed
    std::atomic<uint32_t> m_formatGeneration{0};

    // Cached format values for sendAudio fast path (updated when generation changes)
    // Protected by generation counter check - no race with configureRingXXX
    uint32_t m_cachedFormatGen{0};
    bool m_cachedDsdMode{false};
    bool m_cachedPack24bit{false};
    bool m_cachedUpsample16to32{false};
    int m_cachedChannels{2};
    int m_cachedBytesPerSample{2};
    DirettaRingBuffer::DSDConversionMode m_cachedDsdConversionMode{DirettaRingBuffer::DSDConversionMode::Passthrough};

    // C1: Consumer generation counter for getNewStream fast path
    // Incremented alongside m_formatGeneration in configureRingXXX
    std::atomic<uint32_t> m_consumerStateGen{0};

    // Cached consumer state (only accessed by worker thread)
    uint32_t m_cachedConsumerGen{0};
    int m_cachedBytesPerBuffer{176};
    uint8_t m_cachedSilenceByte{0};
    bool m_cachedConsumerIsDsd{false};
    int m_cachedConsumerSampleRate{44100};
    int m_cachedBytesPerFrame{0};
    uint32_t m_cachedFramesPerBufferRemainder{0};

    // Prefill and stabilization
    size_t m_prefillTarget = 0;
    std::atomic<bool> m_prefillComplete{false};
    std::atomic<bool> m_postOnlineDelayDone{false};
    std::atomic<int> m_silenceBuffersRemaining{0};
    std::atomic<int> m_stabilizationCount{0};

    // Statistics
    std::atomic<int> m_streamCount{0};
    std::atomic<int> m_pushCount{0};
    std::atomic<uint32_t> m_underrunCount{0};
};

#endif // DIRETTA_SYNC_H
