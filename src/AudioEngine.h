#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <string>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

/**
 * @brief Audio track information
 */
struct TrackInfo {
    std::string uri;
    std::string metadata;
    uint32_t sampleRate;
    uint32_t bitDepth;
    uint32_t channels;
    std::string codec;
    uint64_t duration; // in samples
    bool isDSD;        // true if DSD format
    int dsdRate;       // DSD rate (64, 128, 256, 512, 1024)
    bool isCompressed; // true if format requires decoding (FLAC/ALAC), false for WAV/AIFF
    
    TrackInfo() : sampleRate(0), bitDepth(0), channels(2), duration(0), isDSD(false), dsdRate(0), isCompressed(true) {}
};

/**
 * @brief Audio buffer for streaming
 */
class AudioBuffer {
public:
    AudioBuffer(size_t size = 0);
    ~AudioBuffer();
    
    void resize(size_t size);
    size_t size() const { return m_size; }
    uint8_t* data() { return m_data; }
    const uint8_t* data() const { return m_data; }
    
private:
    uint8_t* m_data;
    size_t m_size;
};

/**
 * @brief Audio decoder for a single track
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    
    /**
     * @brief Open and decode a URL
     * @param url Audio file URL
     * @return true if successful, false otherwise
     */
    bool open(const std::string& url);
    
    /**
     * @brief Close the decoder
     */
    void close();
    
    /**
     * @brief Get track information
     * @return Track info
     */
    const TrackInfo& getTrackInfo() const { return m_trackInfo; }
    
    /**
     * @brief Read and decode audio samples
     * @param buffer Output buffer
     * @param numSamples Number of samples to read
     * @param outputRate Target sample rate
     * @param outputBits Target bit depth
     * @return Number of samples actually read (0 = EOF)
     */
    size_t readSamples(AudioBuffer& buffer, size_t numSamples, 
                      uint32_t outputRate, uint32_t outputBits);
    
    /**
     * @brief Check if EOF reached
     * @return true if at end of file
     */
    bool isEOF() const { return m_eof; }
    
    /**
     * @brief Seek to a specific position in the audio file
     * @param seconds Position in seconds
     * @return true if successful, false otherwise
     */
    bool seek(double seconds);
    
private:
    AVFormatContext* m_formatContext;
    AVCodecContext* m_codecContext;
    SwrContext* m_swrContext;
    int m_audioStreamIndex;
    TrackInfo m_trackInfo;
    bool m_eof;
    
    // ⭐ DSD Native Mode
    bool m_rawDSD;           // True if reading raw DSD packets (no decoding)
    AVPacket* m_packet;      // For raw packet reading
    
    // CRITICAL: Buffer interne pour les samples excédentaires
    // Quand une frame décodée contient plus de samples que demandé,
    // on garde l'excédent ici pour le prochain appel
    AudioBuffer m_remainingSamples;
    size_t m_remainingCount;
        // ⭐⭐⭐ NEW: Debug/diagnostic counters (instance variables, NOT static!)
    // These were previously static variables causing race conditions when
    // multiple AudioDecoder instances run concurrently (e.g., gapless preload)
    int m_readCallCount = 0;              // readSamples() call counter
    int m_packetCount = 0;                // DSD packet counter
    bool m_dsdWarningShown = false;       // DSD packet size warning (once per instance)
    bool m_interleavingLoggedDOP = false; // DSD-over-PCM interleaving logged
    bool m_interleavingLoggedNative = false; // Native DSD interleaving logged
    bool m_dumpedFirstPacket = false;     // First packet hex dump flag
    bool m_bitReversalLogged = false;     // Bit reversal logged (PCM mode)
    bool m_resamplingLogged = false;      // Resampling logged (PCM mode)
    bool m_resamplerInitLogged = false;   // Resampler init logged (open())
    bool initResampler(uint32_t outputRate, uint32_t outputBits);
    
};

/**
 * @brief Audio Engine with gapless playback support
 * 
 * Manages audio decoding, buffering, and gapless transitions.
 * Supports pre-loading next track for seamless playback.
 */
class AudioEngine {
public:
    /**
     * @brief Playback state
     */
    enum class State {
        STOPPED,
        PLAYING,
        PAUSED,
        TRANSITIONING
    };
    
    /**
     * @brief Callback for audio data ready
     * @param buffer Audio buffer
     * @param samples Number of samples
     * @param sampleRate Sample rate
     * @param bitDepth Bit depth
     * @param channels Number of channels
     * @return true to continue, false to stop
     */
    using AudioCallback = std::function<bool(const AudioBuffer&, size_t, 
                                            uint32_t, uint32_t, uint32_t)>;
    
    /**
     * @brief Callback for track change
     * @param trackNumber New track number
     * @param trackInfo Track information
     * @param uri Track URI
     * @param metadata Track metadata
     */
    using TrackChangeCallback = std::function<void(int, const TrackInfo&, const std::string&, const std::string&)>;

    /**
     * @brief Callback for track end
     */
    using TrackEndCallback = std::function<void()>;   
    
    /**
     * @brief Constructor
     */
    AudioEngine();
    
    /**
     * @brief Destructor
     */
    ~AudioEngine();
    
    /**
     * @brief Set audio callback
     * @param callback Callback function
     */
    void setAudioCallback(const AudioCallback& callback);
    
    /**
     * @brief Set track change callback
     * @param callback Callback function
     */
    void setTrackChangeCallback(const TrackChangeCallback& callback);

    /**
     * @brief Set track end callback
     * @param callback Callback function
     */
    void setTrackEndCallback(const TrackEndCallback& callback); 
    
    /**
     * @brief Set current track URI
     * @param uri Track URI
     * @param metadata Track metadata (optional)
     */
     void setCurrentURI(const std::string& uri, const std::string& metadata, bool forceReopen = false);  // ⭐ Ajouter forceReopen
    
    /**
     * @brief Set next track URI for gapless playback
     * @param uri Track URI
     * @param metadata Track metadata (optional)
     */
    void setNextURI(const std::string& uri, const std::string& metadata = "");
    
    /**
     * @brief Start playback
     * @return true if successful, false otherwise
     */
    bool play();
    
    /**
     * @brief Stop playback
     */
    void stop();
    
    /**
     * @brief Pause playback
     */
    void pause();
    
    /**
     * @brief Get current state
     * @return Current playback state
     */
    State getState() const { return m_state; }
    
    /**
     * @brief Get current track number
     * @return Track number (1-based)
     */
    int getTrackNumber() const { return m_trackNumber; }
    
    /**
     * @brief Get current track info
     * @return Track information
     */
    const TrackInfo& getCurrentTrackInfo() const { return m_currentTrackInfo; }
    
    /**
     * @brief Get playback position in seconds
     * @return Position in seconds
     */
    double getPosition() const;
    
    /**
     * @brief Seek to a specific position (in seconds)
     * @param seconds Position in seconds
     * @return true if successful, false otherwise
     */
    bool seek(double seconds);
    
    /**
     * @brief Seek to a specific position (time string format)
     * @param timeStr Time string in format "HH:MM:SS", "MM:SS", or seconds as string
     * @return true if successful, false otherwise
     */
    bool seek(const std::string& timeStr);
 
    /**
     * @brief Get current sample rate
     */
     uint32_t getCurrentSampleRate() const;  // ⭐ AJOUTER ICI

    
    /**
     * @brief Main processing loop (called from audio thread)
     * @param samplesNeeded Number of samples needed
     * @return true if data produced, false if stopped
     */
    bool process(size_t samplesNeeded);
    
private:
    std::atomic<State> m_state;
    std::atomic<int> m_trackNumber;
    std::atomic<bool> m_pauseRequested{false};
    
    // Current and next track
    std::string m_currentURI;
    std::string m_currentMetadata;
    std::string m_nextURI;
    std::string m_nextMetadata;
    TrackInfo m_currentTrackInfo;
    TrackEndCallback m_trackEndCallback;
    
    // Decoders
    std::unique_ptr<AudioDecoder> m_currentDecoder;
    std::unique_ptr<AudioDecoder> m_nextDecoder;
    
    // Callbacks
    AudioCallback m_audioCallback;
    TrackChangeCallback m_trackChangeCallback;
    
    // Synchronization
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    
    // Buffer
    AudioBuffer m_buffer;
    
    // Playback tracking
    uint64_t m_samplesPlayed;
    int m_silenceCount;  // Pour drainage du buffer Diretta
    bool m_isDraining;   // Flag pour éviter de re-logger "Track finished"
    
    // Helper functions
    bool openCurrentTrack();
    bool preloadNextTrack();
    void transitionToNextTrack();

    // Thread-safe pending next track mechanism
    mutable std::mutex m_pendingMutex;
    std::atomic<bool> m_pendingNextTrack{false};
    std::string m_pendingNextURI;
    std::string m_pendingNextMetadata;

   // Preload thread management (replaces detached thread)
    std::thread m_preloadThread;
    std::atomic<bool> m_preloadRunning{false};
    void waitForPreloadThread();

    // ⭐⭐⭐ NEW: Async seek mechanism to avoid deadlock
    // The UPnP thread sets these flags, the audio thread processes the seek
    std::atomic<bool> m_seekRequested{false};
    std::atomic<double> m_seekTarget{0.0};

    // Prevent copying
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;};

#endif // AUDIO_ENGINE_H
