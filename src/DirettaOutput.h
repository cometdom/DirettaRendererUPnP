#ifndef DIRETTA_OUTPUT_H
#define DIRETTA_OUTPUT_H

#include <Diretta/SyncBuffer>
#include <Diretta/Find>
#include <ACQUA/UDPV6>
#include <string>
#include <memory>
#include <atomic>

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
    
    void setMTU(uint32_t mtu);
    
    /**
     * @brief Get current MTU setting
     * @return MTU value in bytes
     */
    uint32_t getMTU() const { return m_mtu; }
    void pause();                           // ⭐ NOUVEAU
    void resume();                          // ⭐ NOUVEAU
    bool isPaused() const { return m_isPaused; }  // ⭐ NOUVEAU
    bool isPlaying() const { return m_playing; } 
    bool seek(int64_t samplePosition);  // ⭐ NOUVEAU
    // ⭐ NEW: Advanced Diretta SDK configuration
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
    bool m_mtuManuallySet;  // ⭐ Track if MTU was manually configured
    
    // Diretta
    std::unique_ptr<DIRETTA::SyncBuffer> m_syncBuffer;
    AudioFormat m_currentFormat;
    float m_bufferSeconds;  // Changed from int to float (v1.0.9)
    
    // State
    std::atomic<bool> m_connected;
    std::atomic<bool> m_playing;
    int m_targetIndex = -1;  // Target selection index
    
    // Helper functions
    bool findTarget();
    bool findAndSelectTarget(int targetIndex = -1);  // -1 = interactive selection
    bool configureDiretta(const AudioFormat& format);
    
    // Prevent copying
    DirettaOutput(const DirettaOutput&) = delete;
    DirettaOutput& operator=(const DirettaOutput&) = delete;
    
    int64_t m_totalSamplesSent = 0;  // ⭐ Tracking pour pause/seek
    bool m_isPaused = false;               // ⭐ NOUVEAU
    int64_t m_pausedPosition = 0;          // ⭐ NOUVEAU
     // ⭐ NEW: Advanced SDK parameters (stored values)
    int m_thredMode = 1;
    int m_cycleTime = 10000;
    int m_cycleMinTime = 333;
    int m_infoCycle = 5000;   

};

#endif // DIRETTA_OUTPUT_H
