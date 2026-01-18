/**
 * @file DirettaRenderer.cpp
 * @brief Simplified Diretta UPnP Renderer implementation
 *
 * Connection and format management delegated to DirettaSync.
 */

#include "DirettaRenderer.h"
#include "DirettaSync.h"
#include "UPnPDevice.hpp"
#include "AudioEngine.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <functional>
#include <unistd.h>
#include <cstring>

extern bool g_verbose;
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }

//=============================================================================
// Hybrid Flow Control Constants
//=============================================================================

namespace FlowControl {
    constexpr int MICROSLEEP_US = 500;                                    // 500µs micro-sleep (was 10ms)
    constexpr int MAX_WAIT_MS = 20;                                       // Max total wait time
    constexpr int MAX_RETRIES = MAX_WAIT_MS * 1000 / MICROSLEEP_US;       // 40 retries
    constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;                        // Early-return below 10%
}

//=============================================================================
// UUID Generation
//=============================================================================

static std::string generateUUID() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strcpy(hostname, "diretta-renderer");
    }

    std::hash<std::string> hasher;
    size_t hash = hasher(std::string(hostname));

    std::stringstream ss;
    ss << "uuid:diretta-renderer-" << std::hex << hash;
    return ss.str();
}

//=============================================================================
// Time String Parsing
//=============================================================================

static double parseTimeString(const std::string& timeStr) {
    double hours = 0, minutes = 0, seconds = 0;

    if (sscanf(timeStr.c_str(), "%lf:%lf:%lf", &hours, &minutes, &seconds) >= 2) {
        return hours * 3600 + minutes * 60 + seconds;
    }

    try {
        return std::stod(timeStr);
    } catch (...) {
        return 0.0;
    }
}

//=============================================================================
// Config
//=============================================================================

DirettaRenderer::Config::Config() {
    uuid = generateUUID();
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

DirettaRenderer::DirettaRenderer(const Config& config)
    : m_config(config)
{
    DEBUG_LOG("[DirettaRenderer] Created");
}

DirettaRenderer::~DirettaRenderer() {
    stop();
    DEBUG_LOG("[DirettaRenderer] Destroyed");
}

void DirettaRenderer::waitForCallbackComplete() {
    m_shutdownRequested.store(true, std::memory_order_release);

    auto start = std::chrono::steady_clock::now();
    while (m_callbackRunning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) {
            std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
            break;
        }
    }

    m_shutdownRequested.store(false, std::memory_order_release);
}

//=============================================================================
// Start
//=============================================================================

bool DirettaRenderer::start() {
    if (m_running) {
        std::cerr << "[DirettaRenderer] Already running" << std::endl;
        return false;
    }

    DEBUG_LOG("[DirettaRenderer] Starting...");

    try {
        // Create and enable DirettaSync
        std::cout << "[DirettaRenderer] Checking Diretta Target..." << std::endl;

        m_direttaSync = std::make_unique<DirettaSync>();
        m_direttaSync->setTargetIndex(m_config.targetIndex);

        if (!m_direttaSync->verifyTargetAvailable()) {
            std::cerr << "[DirettaRenderer] No Diretta Target found!" << std::endl;
            std::cerr << "[DirettaRenderer] Run: ./bin/DirettaRendererUPnP --list-targets" << std::endl;
            return false;
        }

        DirettaConfig syncConfig;
        if (!m_direttaSync->enable(syncConfig)) {
            std::cerr << "[DirettaRenderer] Failed to enable DirettaSync" << std::endl;
            return false;
        }

        std::cout << "[DirettaRenderer] Diretta Target ready" << std::endl;

        // Create UPnP device
        UPnPDevice::Config upnpConfig;
        upnpConfig.friendlyName = m_config.name;
        upnpConfig.manufacturer = "DIY Audio";
        upnpConfig.modelName = "Diretta UPnP Renderer";
        upnpConfig.uuid = m_config.uuid;
        upnpConfig.port = m_config.port;
        upnpConfig.networkInterface = m_config.networkInterface;

        m_upnp = std::make_unique<UPnPDevice>(upnpConfig);

        // Create AudioEngine
        m_audioEngine = std::make_unique<AudioEngine>();

        //=====================================================================
        // Audio Callback - Simplified
        //=====================================================================

        m_audioEngine->setAudioCallback(
            [this](const AudioBuffer& buffer, size_t samples,
                   uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

                // Check if shutdown requested (avoid work during teardown)
                if (m_shutdownRequested.load(std::memory_order_acquire)) {
                    return false;
                }

                // Lightweight atomic flag (no syscalls in hot path)
                m_callbackRunning.store(true, std::memory_order_release);
                struct Guard {
                    std::atomic<bool>& flag;
                    ~Guard() { flag.store(false, std::memory_order_release); }
                } guard{m_callbackRunning};

                const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();

                // Build format
                AudioFormat format(sampleRate, bitDepth, channels);
                format.isDSD = trackInfo.isDSD;
                format.isCompressed = trackInfo.isCompressed;

                if (trackInfo.isDSD) {
                    format.bitDepth = 1;
                    // Use detected source format (from file extension or codec)
                    if (trackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DSF) {
                        format.dsdFormat = AudioFormat::DSDFormat::DSF;
                        DEBUG_LOG("[Callback] DSD format: DSF (LSB first)");
                    } else if (trackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DFF) {
                        format.dsdFormat = AudioFormat::DSDFormat::DFF;
                        DEBUG_LOG("[Callback] DSD format: DFF (MSB first)");
                    } else {
                        // Fallback to codec string if detection failed
                        format.dsdFormat = (trackInfo.codec.find("lsb") != std::string::npos)
                            ? AudioFormat::DSDFormat::DSF
                            : AudioFormat::DSDFormat::DFF;
                        DEBUG_LOG("[Callback] DSD format: "
                                  << (format.dsdFormat == AudioFormat::DSDFormat::DSF ? "DSF" : "DFF")
                                  << " (from codec fallback)");
                    }
                }

                // Open/resume connection if needed
                // Check isPlaying() not isOpen() - after stopPlayback(), isOpen() is true
                // but we still need to call open() to trigger quick resume
                //
                // CRITICAL FIX: Also check for format changes!
                // When transitioning DSD→PCM (or vice versa), DirettaSync may still be
                // "playing" but with the wrong format. We must call open() to reconfigure.
                bool needsOpen = !m_direttaSync->isPlaying();

                if (!needsOpen && m_direttaSync->isOpen()) {
                    // Check if format has changed
                    const AudioFormat& currentSyncFormat = m_direttaSync->getFormat();
                    bool formatChanged = (currentSyncFormat.sampleRate != format.sampleRate ||
                                         currentSyncFormat.bitDepth != format.bitDepth ||
                                         currentSyncFormat.channels != format.channels ||
                                         currentSyncFormat.isDSD != format.isDSD);
                    if (formatChanged) {
                        std::cout << "[Callback] FORMAT CHANGE DETECTED!" << std::endl;
                        std::cout << "[Callback]   Old: " << currentSyncFormat.sampleRate << "Hz/"
                                  << currentSyncFormat.bitDepth << "bit "
                                  << (currentSyncFormat.isDSD ? "DSD" : "PCM") << std::endl;
                        std::cout << "[Callback]   New: " << format.sampleRate << "Hz/"
                                  << format.bitDepth << "bit "
                                  << (format.isDSD ? "DSD" : "PCM") << std::endl;

                        // Send silence BEFORE stopping to flush Diretta pipeline
                        // This prevents crackling on format transitions (gapless case)
                        m_direttaSync->sendPreTransitionSilence();

                        // Stop current playback to trigger full reopen
                        m_direttaSync->stopPlayback(true);
                        needsOpen = true;
                    }
                }

                if (needsOpen) {
                    if (!m_direttaSync->open(format)) {
                        std::cerr << "[Callback] Failed to open DirettaSync" << std::endl;
                        return false;
                    }

                    // Propagate S24 alignment hint to ring buffer for 24-bit PCM
                    // This helps detection when track starts with silence
                    if (!format.isDSD && bitDepth == 24 &&
                        trackInfo.s24Alignment != TrackInfo::S24Alignment::Unknown) {
                        DirettaRingBuffer::S24PackMode hint =
                            (trackInfo.s24Alignment == TrackInfo::S24Alignment::LsbAligned)
                                ? DirettaRingBuffer::S24PackMode::LsbAligned
                                : DirettaRingBuffer::S24PackMode::MsbAligned;
                        m_direttaSync->setS24PackModeHint(hint);
                        DEBUG_LOG("[Callback] Set S24 hint: "
                                  << (hint == DirettaRingBuffer::S24PackMode::LsbAligned ? "LSB" : "MSB"));
                    }
                }

                // Send audio (DirettaSync handles all format conversions)
                if (trackInfo.isDSD) {
                    // DSD: Atomic send with retry
                    int retryCount = 0;
                    const int maxRetries = 100;
                    size_t sent = 0;

                    while (sent == 0 && retryCount < maxRetries) {
                        sent = m_direttaSync->sendAudio(buffer.data(), samples);

                        if (sent == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            retryCount++;
                        }
                    }

                    if (sent == 0) {
                        std::cerr << "[Callback] DSD timeout" << std::endl;
                    }
                } else {
                    // PCM: Incremental send with hybrid flow control
                    const uint8_t* audioData = buffer.data();
                    size_t remainingSamples = samples;
                    size_t bytesPerSample = (bitDepth == 24 || bitDepth == 32)
                        ? 4 * channels : (bitDepth / 8) * channels;

                    // Hybrid flow control: micro-sleep normally, early-return if buffer critical
                    float bufferLevel = m_direttaSync->getBufferLevel();
                    bool criticalMode = (bufferLevel < FlowControl::CRITICAL_BUFFER_LEVEL);

                    int retryCount = 0;

                    while (remainingSamples > 0 && retryCount < FlowControl::MAX_RETRIES) {
                        size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

                        if (sent > 0) {
                            size_t samplesConsumed = sent / bytesPerSample;
                            remainingSamples -= samplesConsumed;
                            audioData += sent;
                            retryCount = 0;
                        } else {
                            if (criticalMode) {
                                // Buffer critically low - return immediately to prioritize refill
                                DEBUG_LOG("[Audio] Early-return, buffer critical: " << bufferLevel);
                                break;
                            }
                            // Normal backpressure: 500µs micro-sleep (was 10ms)
                            std::this_thread::sleep_for(std::chrono::microseconds(FlowControl::MICROSLEEP_US));
                            retryCount++;
                        }
                    }
                }

                return true;
            }
        );

        //=====================================================================
        // Track Change Callback
        //=====================================================================

        m_audioEngine->setTrackChangeCallback(
            [this](int trackNumber, const TrackInfo& info, const std::string& uri, const std::string& metadata) {
                if (g_verbose) {
                    std::cout << "[DirettaRenderer] Track " << trackNumber << ": " << info.codec;
                    if (info.isDSD) {
                        std::cout << " DSD" << info.dsdRate << " (" << info.sampleRate << "Hz)";
                    } else {
                        std::cout << " " << info.sampleRate << "Hz/" << info.bitDepth << "bit";
                    }
                    std::cout << "/" << info.channels << "ch" << std::endl;
                }

                m_upnp->setCurrentURI(uri);
                m_upnp->setCurrentMetadata(metadata);
                m_upnp->notifyTrackChange(uri, metadata);
                m_upnp->notifyStateChange("PLAYING");
            }
        );

        m_audioEngine->setTrackEndCallback([this]() {
            std::cout << "[DirettaRenderer] Track ended naturally" << std::endl;

            // Fully release the Diretta target on playlist end
            // This closes the SDK connection so the target can accept other sources
            // Using release() instead of close() ensures complete disconnection
            if (m_direttaSync) {
                m_direttaSync->release();
            }

            // Notify control point that track finished
            // This is required for sequential playlist advancement
            // The control point will poll GetTransportInfo, see STOPPED,
            // and send SetAVTransportURI + Play for the next track
            m_upnp->notifyStateChange("STOPPED");
        });

        //=====================================================================
        // UPnP Callbacks
        //=====================================================================

        UPnPDevice::Callbacks callbacks;

        callbacks.onSetURI = [this](const std::string& uri, const std::string& metadata) {
            DEBUG_LOG("[DirettaRenderer] SetURI: " << uri);

            AudioEngine::State currentState;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                currentState = m_audioEngine->getState();
            }

            // Auto-stop if playing
            if (currentState == AudioEngine::State::PLAYING ||
                currentState == AudioEngine::State::PAUSED) {

                std::cout << "[DirettaRenderer] Auto-STOP before URI change" << std::endl;

                m_audioEngine->stop();
                waitForCallbackComplete();

                // Don't close DirettaSync - keep connection alive for quick track transitions
                // Format changes are handled in DirettaSync::open()
                if (m_direttaSync && m_direttaSync->isOpen()) {
                    // EXPERIMENTAL: Force full reopen for user-initiated track changes
                    // This ensures clean transition even for same-format tracks
                    // (vs gapless sequential playback which uses quick path)
                    m_direttaSync->setForceFullReopen(true);

                    // Send silence BEFORE stopping to flush Diretta pipeline
                    // This prevents crackling on DSD→PCM or DSD rate change transitions
                    m_direttaSync->sendPreTransitionSilence();
                    m_direttaSync->stopPlayback(true);
                    // m_direttaSync->close();  // Removed
                }

                m_upnp->notifyStateChange("STOPPED");
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_currentURI = uri;
                m_currentMetadata = metadata;
                m_audioEngine->setCurrentURI(uri, metadata);
            }
        };

        callbacks.onSetNextURI = [this](const std::string& uri, const std::string& metadata) {
            std::lock_guard<std::mutex> lock(m_mutex);
            DEBUG_LOG("[DirettaRenderer] SetNextURI for gapless");
            m_audioEngine->setNextURI(uri, metadata);
        };

        callbacks.onPlay = [this]() {
            std::cout << "[DirettaRenderer] Play" << std::endl;
            std::lock_guard<std::mutex> lock(m_mutex);

            // Resume from pause?
            if (m_direttaSync && m_direttaSync->isOpen() && m_direttaSync->isPaused()) {
                DEBUG_LOG("[DirettaRenderer] Resuming from pause");
                m_direttaSync->resumePlayback();
                m_audioEngine->play();
                m_upnp->notifyStateChange("PLAYING");
                return;
            }

            // Reopen track if needed
            if (m_direttaSync && !m_direttaSync->isOpen() && !m_currentURI.empty()) {
                DEBUG_LOG("[DirettaRenderer] Reopening track");
                m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
            }

            // DAC stabilization delay
            auto now = std::chrono::steady_clock::now();
            auto timeSinceStop = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStopTime);
            if (timeSinceStop.count() < 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            m_audioEngine->play();
            m_upnp->notifyStateChange("PLAYING");
        };

        callbacks.onPause = [this]() {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::cout << "[DirettaRenderer] Pause" << std::endl;

            if (m_audioEngine) {
                m_audioEngine->pause();
            }
            if (m_direttaSync && m_direttaSync->isPlaying()) {
                m_direttaSync->pausePlayback();
            }
            m_upnp->notifyStateChange("PAUSED_PLAYBACK");
        };

        callbacks.onStop = [this]() {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::cout << "[DirettaRenderer] Stop" << std::endl;

            m_lastStopTime = std::chrono::steady_clock::now();

            m_audioEngine->stop();
            waitForCallbackComplete();

            if (!m_currentURI.empty()) {
                m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
            }

            // Fully release Diretta target on Stop for proper resource cleanup
            // This ensures:
            // - Clean handoff when switching to a different renderer
            // - Proper resource release on the Diretta target
            // - Control point gets expected clean disconnection
            // Trade-off: subsequent Play will need to reopen SDK (~300ms)
            if (m_direttaSync) {
                m_direttaSync->release();
            }

            m_upnp->notifyStateChange("STOPPED");
        };

        callbacks.onSeek = [this](const std::string& target) {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::cout << "[DirettaRenderer] Seek: " << target << std::endl;

            double seconds = parseTimeString(target);
            if (m_audioEngine) {
                m_audioEngine->seek(seconds);
            }
        };

        m_upnp->setCallbacks(callbacks);

        // Start UPnP server
        if (!m_upnp->start()) {
            std::cerr << "[DirettaRenderer] Failed to start UPnP server" << std::endl;
            return false;
        }

        DEBUG_LOG("[DirettaRenderer] UPnP: " << m_upnp->getDeviceURL());

        // Start threads
        m_running = true;
        m_upnpThread = std::thread(&DirettaRenderer::upnpThreadFunc, this);
        m_audioThread = std::thread(&DirettaRenderer::audioThreadFunc, this);
        m_positionThread = std::thread(&DirettaRenderer::positionThreadFunc, this);

        std::cout << "[DirettaRenderer] Started" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DirettaRenderer] Exception: " << e.what() << std::endl;
        stop();
        return false;
    }
}

//=============================================================================
// Stop
//=============================================================================

void DirettaRenderer::stop() {
    if (!m_running) return;

    DEBUG_LOG("[DirettaRenderer] Stopping...");

    m_running = false;

    if (m_audioEngine) {
        m_audioEngine->stop();
    }

    if (m_direttaSync) {
        m_direttaSync->disable();
    }

    if (m_upnp) {
        m_upnp->stop();
    }

    if (m_upnpThread.joinable()) m_upnpThread.join();
    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_positionThread.joinable()) m_positionThread.join();

    DEBUG_LOG("[DirettaRenderer] Stopped");
}

//=============================================================================
// Thread Functions
//=============================================================================

void DirettaRenderer::upnpThreadFunc() {
    DEBUG_LOG("[UPnP Thread] Started");

    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    DEBUG_LOG("[UPnP Thread] Stopped");
}

void DirettaRenderer::audioThreadFunc() {
    DEBUG_LOG("[Audio Thread] Started");

    // Buffer-level flow control thresholds (like MPD's Delay() approach)
    constexpr float BUFFER_HIGH_THRESHOLD = 0.5f;  // Throttle when >50% full
    constexpr float BUFFER_LOW_THRESHOLD = 0.25f;  // Warn when <25% full

    uint32_t lastSampleRate = 0;
    size_t currentSamplesPerCall = 8192;

    while (m_running) {
        if (!m_audioEngine) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto state = m_audioEngine->getState();

        if (state == AudioEngine::State::PLAYING) {
            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            uint32_t sampleRate = trackInfo.sampleRate;
            bool isDSD = trackInfo.isDSD;

            if (sampleRate == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Adjust samples per call based on format
            // PCM: 2048 samples = ~46ms at 44.1kHz
            // DSD: Rate-adaptive for consistent ~12ms chunks
            size_t samplesPerCall;
            if (isDSD) {
                samplesPerCall = DirettaBuffer::calculateDsdSamplesPerCall(sampleRate);
            } else {
                samplesPerCall = 2048;
            }

            if (sampleRate != lastSampleRate || samplesPerCall != currentSamplesPerCall) {
                currentSamplesPerCall = samplesPerCall;
                lastSampleRate = sampleRate;
                DEBUG_LOG("[Audio Thread] Format: " << sampleRate << "Hz "
                          << (isDSD ? "DSD" : "PCM") << ", samples/call="
                          << currentSamplesPerCall);
            }

            // Buffer-level flow control (MPD-style)
            // Only throttle if DirettaSync is actively playing
            // If not playing (after stop), bufferLevel stays 0 so we call process()
            // which triggers the open/quick-resume path in the audio callback
            float bufferLevel = 0.0f;
            if (m_direttaSync && m_direttaSync->isPlaying()) {
                bufferLevel = m_direttaSync->getBufferLevel();
            }

            if (bufferLevel > BUFFER_HIGH_THRESHOLD) {
                // Buffer is healthy - throttle to avoid wasting CPU
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                // Buffer needs filling - process immediately
                bool success = m_audioEngine->process(currentSamplesPerCall);

                if (!success) {
                    // No data available from decoder, brief pause
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else if (bufferLevel < BUFFER_LOW_THRESHOLD && bufferLevel > 0.0f) {
                    // Buffer is getting low - process again immediately (catch up)
                    m_audioEngine->process(currentSamplesPerCall);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lastSampleRate = 0;
        }
    }

    DEBUG_LOG("[Audio Thread] Stopped");
}

void DirettaRenderer::positionThreadFunc() {
    DEBUG_LOG("[Position Thread] Started");

    while (m_running) {
        if (!m_audioEngine || !m_upnp) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        auto state = m_audioEngine->getState();

        if (state == AudioEngine::State::PLAYING) {
            double positionSeconds = m_audioEngine->getPosition();
            int position = static_cast<int>(positionSeconds);

            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            int duration = 0;
            if (trackInfo.sampleRate > 0) {
                duration = trackInfo.duration / trackInfo.sampleRate;
            }

            m_upnp->setCurrentPosition(position);
            m_upnp->setTrackDuration(duration);
            m_upnp->notifyPositionChange(position, duration);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    DEBUG_LOG("[Position Thread] Stopped");
}
