#ifndef DIRETTA_OUTPUT_H
#define DIRETTA_OUTPUT_H

#include <Diretta/SyncBuffer>
#include <Diretta/Find>
#include <ACQUA/UDPV6>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

/**
 * @brief Audio format specification
 */
struct AudioFormat {
    uint32_t sampleRate;
    uint32_t bitDepth;
    uint32_t channels;
    bool isDSD;              // ⭐ DSD flag
    bool isCompressed;       // ⭐ True for FLAC/ALAC, false for WAV/AIFF
    
    enum class DSDFormat {   // ⭐ DSD format type
        DSF,  // LSB first, Little Endian
        DFF   // MSB first, Big Endian
    };
    
    DSDFormat dsdFormat;     // ⭐ DSD format
    
    AudioFormat() 
        : sampleRate(44100), bitDepth(16), channels(2)
        , isDSD(false), isCompressed(true), dsdFormat(DSDFormat::DSF) {}
    
    AudioFormat(uint32_t rate, uint32_t bits, uint32_t ch) 
        : sampleRate(rate), bitDepth(bits), channels(ch)
        , isDSD(false), isCompressed(true), dsdFormat(DSDFormat::DSF) {}
    
    bool operator==(const AudioFormat& other) const {
        if (isDSD != other.isDSD) return false;
        if (isDSD && dsdFormat != other.dsdFormat) return false;
        return sampleRate == other.sampleRate && 
               bitDepth == other.bitDepth && 
               channels == other.channels;
    }
    
    bool operator!=(const AudioFormat& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Diretta output handler
 * 
 * Manages connection to Diretta DAC and handles audio streaming
 * using SyncBuffer for gapless playback.
 */
class DirettaOutput {
public:
    /**
     * @brief Constructor
     */
    DirettaOutput();
    
    /**
     * @brief Destructor
     */
    ~DirettaOutput();
    
    /**
     * @brief Initialize and connect to Diretta target
     * @param format Initial audio format
     * @param bufferSeconds Buffer size in seconds
     * @return true if successful, false otherwise
     */
    bool open(const AudioFormat& format, float bufferSeconds = 2.0f);
    
    /**
     * @brief Close connection
     */
    void close();
    
    /**
     * @brief Check if connected
     * @return true if connected, false otherwise
     */
    bool isConnected() const { return m_connected; }
    
    /**
     * @brief Start playback
     * @return true if successful, false otherwise
     */
    bool play();
    
    /**
     * @brief Stop playback
     * @param immediate If true, stop immediately; if false, drain buffer first
     */
    void stop(bool immediate = false);
    
    /**
     * @brief Change audio format (for format transitions)
     * @param newFormat New audio format
     * @return true if successful, false otherwise
     */
    bool changeFormat(const AudioFormat& newFormat);
    
    /**
     * @brief Get current audio format
     * @return Current format
     */
    const AudioFormat& getFormat() const { return m_currentFormat; }
    
    /**
     * @brief Send audio data to DAC
     * @param data Audio buffer
     * @param numSamples Number of samples (frames)
     * @return true if successful, false otherwise
     */
    bool sendAudio(const uint8_t* data, size_t numSamples); 
    
    /**
     * @brief Get buffer level (for monitoring)
     * @return Buffer fill level (0.0 to 1.0)
     */
    float getBufferLevel() const;
    
    /**
     * @brief Set MTU (Maximum Transmission Unit) for network packets
     * 
     * Common values:
     * - 1500: Standard Ethernet (default)
     * - 9000: Jumbo frames (requires network support)
     * - 16000: Super jumbo frames (for high-performance audio)
     * 
     * ⚠️  Must be called BEFORE open()
     * ⚠️  Network infrastructure must support the chosen MTU
     * 
     * @param mtu MTU value in bytes
     */
    void setMTU(uint32_t mtu);
    
    /**
     * @brief Get current MTU setting
     * @return MTU value in bytes
     */
    uint32_t getMTU() const { return m_mtu; }
    
    /**
     * @brief Set target index for selection
     * @param index Target index (-1 = interactive, >= 0 = specific target)
     */
    void setTargetIndex(int index) { m_targetIndex = index; }
    
    /**
     * @brief Verify that a Diretta target is available on the network
     * @return true if at least one target is available, false otherwise
     */
    bool verifyTargetAvailable();
    
    /**
     * @brief List all available Diretta targets on the network
     */
    void listAvailableTargets();
    
    // ═══════════════════════════════════════════════════════════════
    // Playback control
    // ═══════════════════════════════════════════════════════════════
    
    void pause();
    void resume();
    bool isPaused() const { return m_isPaused; }
    bool isPlaying() const { return m_playing; } 
    bool seek(int64_t samplePosition);
    
    // ═══════════════════════════════════════════════════════════════
    // ⭐ v1.2.0: Gapless Pro - Native SDK gapless support
    // ═══════════════════════════════════════════════════════════════
    
    /**
     * @brief Prepare next track for gapless playback
     * 
     * Uses native Diretta SDK gapless methods (writeStreamStart + addStream)
     * for truly seamless track transitions.
     * 
     * @param data Audio buffer of next track
     * @param numSamples Number of samples (frames)
     * @param format Audio format of next track
     * @return true if preparation successful, false otherwise
     */
    bool prepareNextTrack(const uint8_t* data, 
                          size_t numSamples,
                          const AudioFormat& format);
    
    /**
     * @brief Check if next track is ready for gapless transition
     * @return true if next track prepared and ready
     */
    bool isNextTrackReady() const;
    
    /**
     * @brief Cancel prepared next track
     * 
     * Call this if user skips or changes playlist before next track plays
     */
    void cancelNextTrack();
    
    /**
     * @brief Enable or disable gapless mode
     * @param enabled true to enable gapless, false to disable
     */
    void setGaplessMode(bool enabled);
    
    /**
     * @brief Check if gapless mode is enabled
     * @return true if gapless enabled
     */
    bool isGaplessMode() const { return m_gaplessEnabled; }
    
    /**
     * @brief Check if buffer is empty (for format change drain)
     * @return true if buffer empty or not connected
     */
    bool isBufferEmpty() const;
    
    // ═══════════════════════════════════════════════════════════════
    // Advanced SDK configuration
    // ═══════════════════════════════════════════════════════════════
    
    void setThredMode(int mode) { m_thredMode = mode; }
    void setCycleTime(int time) { m_cycleTime = time; }
    void setCycleMinTime(int time) { m_cycleMinTime = time; }
    void setInfoCycle(int time) { m_infoCycle = time; }
    
private:
    // Network
    std::unique_ptr<ACQUA::UDPV6> m_udp;
    std::unique_ptr<ACQUA::UDPV6> m_raw;
    ACQUA::IPAddress m_targetAddress;
    uint32_t m_mtu;
    
    // Diretta
    std::unique_ptr<DIRETTA::SyncBuffer> m_syncBuffer;
    AudioFormat m_currentFormat;
    float m_bufferSeconds;
    
    // State
    std::atomic<bool> m_connected;
    std::atomic<bool> m_playing;
    std::atomic<bool> m_isPaused;
    int m_targetIndex;
    int64_t m_totalSamplesSent;
    int64_t m_pausedPosition;
    
    // ⭐ v1.2.0: Gapless Pro state
    bool m_gaplessEnabled;
    bool m_nextTrackPrepared;
    AudioFormat m_nextTrackFormat;
    mutable std::mutex m_gaplessMutex;  // Protect gapless state
    
    // Advanced SDK parameters
    int m_thredMode;
    int m_cycleTime;
    int m_cycleMinTime;
    int m_infoCycle;
    
    // Helper functions
    bool findTarget();
    bool findAndSelectTarget(int targetIndex = -1);
    bool configureDiretta(const AudioFormat& format);
    
    // ⭐ v1.2.0 Stable: Network optimization
    void optimizeNetworkConfig(const AudioFormat& format);
    
    // ⭐ v1.2.0: Gapless Pro helper
    DIRETTA::Stream createStreamFromAudio(const uint8_t* data, 
                                          size_t numSamples,
                                          const AudioFormat& format);
    
    // Prevent copying
    DirettaOutput(const DirettaOutput&) = delete;
    DirettaOutput& operator=(const DirettaOutput&) = delete;
};

#endif // DIRETTA_OUTPUT_H
