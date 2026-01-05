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
    std::unique_lock<std::mutex> lk(m_callbackMutex);
    bool completed = m_callbackCV.wait_for(lk, std::chrono::seconds(5),
        [this]{ return !m_callbackRunning; });
    if (!completed) {
        std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
        m_callbackRunning = false;
    }
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

                // RAII guard
                {
                    std::lock_guard<std::mutex> lk(m_callbackMutex);
                    m_callbackRunning = true;
                }
                struct Guard {
                    DirettaRenderer* self;
                    ~Guard() {
                        {
                            std::lock_guard<std::mutex> lk(self->m_callbackMutex);
                            self->m_callbackRunning = false;
                        }
                        self->m_callbackCV.notify_all();
                    }
                } guard{this};

                const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();

                // Build format
                AudioFormat format(sampleRate, bitDepth, channels);
                format.isDSD = trackInfo.isDSD;
                format.isCompressed = trackInfo.isCompressed;

                if (trackInfo.isDSD) {
                    format.bitDepth = 1;
                    format.dsdFormat = (trackInfo.codec.find("lsb") != std::string::npos)
                        ? AudioFormat::DSDFormat::DSF
                        : AudioFormat::DSDFormat::DFF;
                }

                // Open/resume connection if needed
                // Check isPlaying() not isOpen() - after stopPlayback(), isOpen() is true
                // but we still need to call open() to trigger quick resume
                if (!m_direttaSync->isPlaying()) {
                    if (!m_direttaSync->open(format)) {
                        std::cerr << "[Callback] Failed to open DirettaSync" << std::endl;
                        return false;
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
                    // PCM: Incremental send
                    const uint8_t* audioData = buffer.data();
                    size_t remainingSamples = samples;
                    size_t bytesPerSample = (bitDepth == 24 || bitDepth == 32)
                        ? 4 * channels : (bitDepth / 8) * channels;

                    int retryCount = 0;
                    const int maxRetries = 50;

                    while (remainingSamples > 0 && retryCount < maxRetries) {
                        size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

                        if (sent > 0) {
                            size_t samplesConsumed = sent / bytesPerSample;
                            remainingSamples -= samplesConsumed;
                            audioData += sent;
                            retryCount = 0;
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
            std::cout << "[DirettaRenderer] 🏁 Track ended naturally" << std::endl;
            // Notify control point that track finished
            // This is required for sequential playlist advancement
            // The control point will poll GetTransportInfo, see STOPPED,
            // and send SetAVTransportURI + Play for the next track
            m_upnp->notifyStateChange("STOPPED");
            std::cout << "[DirettaRenderer] 🏁 Notified STOPPED to control point" << std::endl;
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
                currentState == AudioEngine::State::PAUSED ||
                currentState == AudioEngine::State::TRANSITIONING) {

                std::cout << "[DirettaRenderer] Auto-STOP before URI change" << std::endl;

                {
                    std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                    m_audioEngine->stop();
                }
                waitForCallbackComplete();

                // Don't close DirettaSync - keep connection alive for quick track transitions
                // Format changes are handled in DirettaSync::open()
                if (m_direttaSync && m_direttaSync->isOpen()) {
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

            {
                std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                m_audioEngine->stop();
            }
            waitForCallbackComplete();

            if (!m_currentURI.empty()) {
                m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
            }

            // Don't close DirettaSync here - keep connection alive for quick track transitions
            // DirettaSync will only close on:
            // - Format family change (PCM↔DSD) - handled in audio callback
            // - App shutdown - handled in DirettaRenderer::stop()
            if (m_direttaSync) {
                m_direttaSync->stopPlayback(true);
                // m_direttaSync->close();  // Removed - keep connection open
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
            size_t samplesPerCall = isDSD ? 32768 : 8192;

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
