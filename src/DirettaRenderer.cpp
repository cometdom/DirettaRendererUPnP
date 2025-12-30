/**
 * @file DirettaRenderer.cpp
 * @brief Main Diretta Renderer implementation - TIMING CORRECTED
 * 
 * CORRECTION MAJEURE:
 * - Ajout de contrôle de débit précis dans audioThreadFunc()
 * - Le timing est basé sur le sample rate du fichier en cours
 * - Utilise sleep_until() pour un timing précis au microseconde près
 */

#include "DirettaRenderer.h"
#include "UPnPDevice.hpp"
#include "AudioEngine.h"
#include "DirettaOutput.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <functional>  // For std::hash
#include <unistd.h>    // For gethostname
#include <cstring>     // For strcpy
#include <mutex>       // For stop/play synchronization

// ============================================================================
// Logging system - Variable globale définie dans main.cpp
// ============================================================================
extern bool g_verbose;
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }


// Generate stable UUID based on hostname
// This ensures the same UUID across restarts, so UPnP control points
// recognize the renderer as the same device
static std::string generateUUID() {
    // Get hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strcpy(hostname, "diretta-renderer");
    }
    
    // Create a simple hash of hostname for UUID
    std::hash<std::string> hasher;
    size_t hash = hasher(std::string(hostname));
    
    std::stringstream ss;
    ss << "uuid:diretta-renderer-" << std::hex << hash;
    return ss.str();
}

// ============================================================================
// DirettaRenderer::Config
// ============================================================================

DirettaRenderer::Config::Config() {
    uuid = generateUUID();
    targetIndex = -1;  // Default: interactive selection
    networkInterface = "";  // (vide = auto-detect)
}

// ============================================================================
// DirettaRenderer
// ============================================================================

DirettaRenderer::DirettaRenderer(const Config& config)
    : m_config(config)
    , m_running(false)
{
    DEBUG_LOG("[DirettaRenderer] Created");
}

DirettaRenderer::~DirettaRenderer() {
    stop();
    DEBUG_LOG("[DirettaRenderer] Destroyed");
}

// Helper function to parse UPnP time strings (HH:MM:SS or HH:MM:SS.mmm)
static double parseTimeString(const std::string& timeStr) {
    double hours = 0, minutes = 0, seconds = 0;
    
    // Format: "HH:MM:SS" ou "HH:MM:SS.mmm"
    if (sscanf(timeStr.c_str(), "%lf:%lf:%lf", &hours, &minutes, &seconds) >= 2) {
        return hours * 3600 + minutes * 60 + seconds;
    }
    
    // Fallback: try to parse as seconds directly
    try {
        return std::stod(timeStr);
    } catch (...) {
        std::cerr << "[parseTimeString] ⚠️  Failed to parse time: " << timeStr << std::endl;
        return 0.0;
    }
}


bool DirettaRenderer::start() {
    if (m_running) {
        std::cerr << "[DirettaRenderer] Already running" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[DirettaRenderer] Initializing components...");
    
    try {
        // ⭐ CRITICAL: Verify Diretta Target availability BEFORE starting UPnP
        // This prevents the renderer from accepting connections when no DAC is available
        DEBUG_LOG("[DirettaRenderer] ");
        std::cout << "[DirettaRenderer] ══════════════════════════════════════════════════════" << std::endl;
        std::cout << "[DirettaRenderer] ⚠️  IMPORTANT: Checking Diretta Target availability..." << std::endl;
        std::cout << "[DirettaRenderer] ══════════════════════════════════════════════════════" << std::endl;
        DEBUG_LOG("[DirettaRenderer] ");
        
        // Create DirettaOutput first to verify target
        m_direttaOutput = std::make_unique<DirettaOutput>();
        m_direttaOutput->setTargetIndex(m_config.targetIndex);
        
        // ⭐ Verify target is available by attempting discovery
        if (!m_direttaOutput->verifyTargetAvailable()) {
            std::cerr << "[DirettaRenderer] " << std::endl;
            std::cerr << "[DirettaRenderer] ══════════════════════════════════════════════════════" << std::endl;
            std::cerr << "[DirettaRenderer] ❌ FATAL: No Diretta Target available!" << std::endl;
            std::cerr << "[DirettaRenderer] ══════════════════════════════════════════════════════" << std::endl;
            std::cerr << "[DirettaRenderer] " << std::endl;
            std::cerr << "[DirettaRenderer] The renderer cannot start without a Diretta Target." << std::endl;
            std::cerr << "[DirettaRenderer] " << std::endl;
            std::cerr << "[DirettaRenderer] Please:" << std::endl;
            std::cerr << "[DirettaRenderer]   1. Power on your Diretta Target device" << std::endl;
            std::cerr << "[DirettaRenderer]   2. Ensure it's connected to the same network" << std::endl;
            std::cerr << "[DirettaRenderer]   3. Check firewall settings" << std::endl;
            std::cerr << "[DirettaRenderer]   4. Run: ./bin/DirettaRendererUPnP --list-targets" << std::endl;
            std::cerr << "[DirettaRenderer] " << std::endl;
            return false;
        }
        
        std::cout << "[DirettaRenderer] ✓ Diretta Target verified and ready" << std::endl;
        DEBUG_LOG("[DirettaRenderer] ");
        
        // Configure MTU
        if (m_networkMTU != 1500) {
            m_direttaOutput->setMTU(m_networkMTU);
        }
        
        // ⭐ v1.2.0: Configure Gapless Pro mode
        m_direttaOutput->setGaplessMode(m_config.gaplessEnabled);
        DEBUG_LOG("[DirettaRenderer] ✓ Gapless mode: " 
                  << (m_config.gaplessEnabled ? "ENABLED" : "DISABLED"));
        
        // Create other components
        UPnPDevice::Config upnpConfig;
        upnpConfig.friendlyName = m_config.name;
        upnpConfig.manufacturer = "DIY Audio";
        upnpConfig.modelName = "Diretta UPnP Renderer";
        upnpConfig.uuid = m_config.uuid;
        upnpConfig.port = m_config.port;
        upnpConfig.networkInterface = m_config.networkInterface;
        
        m_upnp = std::make_unique<UPnPDevice>(upnpConfig);        
        
        m_audioEngine = std::make_unique<AudioEngine>();

        
        
m_audioEngine->setAudioCallback(
    [this](const AudioBuffer& buffer, size_t samples,
           uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

        {
            std::lock_guard<std::mutex> lk(m_callbackMutex);
            m_callbackRunning = true;
        }

        // RAII guard - clears flag on any exit path
        struct CallbackGuard {
            DirettaRenderer* self;
            bool manuallyReleased = false;  // ⭐ v1.2.0 Stable: Support manual release
            
            ~CallbackGuard() {
                if (!manuallyReleased) {  // ⭐ Only release if not done manually
                    {
                        std::lock_guard<std::mutex> lk(self->m_callbackMutex);
                        self->m_callbackRunning = false;
                    }
                    self->m_callbackCV.notify_all();
                }
            }
        } guard{this};

        DEBUG_LOG("[Callback] Sending " << samples << " samples");
        
        // Get track info to check for DSD
        const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();
        
        // ═══════════════════════════════════════════════════════════════
        // ⭐⭐⭐ CRITICAL FIX: Persistent format tracking ⭐⭐⭐
        // ═══════════════════════════════════════════════════════════════
        
        // Static variable to remember LAST format even after close()
        // This is the KEY to detecting format changes after JPLAY's AUTO-STOP
        static AudioFormat lastFormat(0, 0, 0);
        static bool hasLastFormat = false;
        bool needReopen = false;
        bool formatChanged = false;

        // Build current format from callback parameters
        AudioFormat currentFormat(sampleRate, bitDepth, channels);
        currentFormat.isDSD = trackInfo.isDSD;
        currentFormat.isCompressed = trackInfo.isCompressed;

        if (trackInfo.isDSD) {
            currentFormat.bitDepth = 1;  // DSD = 1 bit
            std::string codec = trackInfo.codec;
            if (codec.find("lsb") != std::string::npos) {
                currentFormat.dsdFormat = AudioFormat::DSDFormat::DSF;
                DEBUG_LOG("[Callback] DSD format: DSF (LSB)");
            } else {
                currentFormat.dsdFormat = AudioFormat::DSDFormat::DFF;
                DEBUG_LOG("[Callback] DSD format: DFF (MSB)");
            }
        }
        
        // ═══════════════════════════════════════════════════════════════
        // ⭐ Format change detection (works EVEN after close())
        // ═══════════════════════════════════════════════════════════════
        
        
        if (m_direttaOutput->isConnected()) {
            // Case 1: Already connected - check against current connection
            const AudioFormat& connectedFormat = m_direttaOutput->getFormat();
            
            if (connectedFormat != currentFormat) {
                formatChanged = true;
                
                std::cout << "════════════════════════════════════════" << std::endl;
                std::cout << "[Callback] ⚠️  FORMAT CHANGE DETECTED (connected)!" << std::endl;
                std::cout << "[Callback] Old: " << connectedFormat.sampleRate << "Hz/" 
                          << connectedFormat.bitDepth << "bit/" << connectedFormat.channels << "ch"
                          << (connectedFormat.isDSD ? " DSD" : " PCM") << std::endl;
                std::cout << "[Callback] New: " << currentFormat.sampleRate << "Hz/" 
                          << currentFormat.bitDepth << "bit/" << currentFormat.channels << "ch"
                          << (currentFormat.isDSD ? " DSD" : " PCM") << std::endl;
                std::cout << "════════════════════════════════════════" << std::endl;
                
                // ⭐ v1.2.0 Stable: Release callback flag BEFORE long operations
                {
                    std::lock_guard<std::mutex> lk(m_callbackMutex);
                    m_callbackRunning = false;
                    guard.manuallyReleased = true;  // Prevent double release
                }
                m_callbackCV.notify_all();
                DEBUG_LOG("[Callback] ✓ Callback flag released early (anti-deadlock)");
                
                // ⭐⭐⭐ USE changeFormat() FOR PROPER TRANSITION ⭐⭐⭐
                std::cout << "[Callback] 🔄 Executing format change sequence..." << std::endl;
                
                // ⭐ v1.2.0 Stable: Explicit buffer draining before stop
                if (m_direttaOutput->isConnected()) {
                    std::cout << "[Callback]    0. Draining buffer before format change..." << std::endl;
                    
                    int timeout = 50;  // 500ms max
                    int drained = 0;
                    
                    while (!m_direttaOutput->isBufferEmpty() && timeout-- > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        drained += 10;
                    }
                    
                    if (timeout <= 0) {
                        std::cout << "[Callback]    ⚠️  Buffer drain timeout after 500ms" << std::endl;
                    } else {
                        std::cout << "[Callback]    ✓ Buffer drained in " << drained << "ms" << std::endl;
                    }
                }
                
                // STEP 1: Stop playback (graceful drain)
                std::cout << "[Callback]    1. Stopping..." << std::endl;
                m_direttaOutput->stop(false);  // ✅ false = graceful drain
                
                // STEP 2: Change format
                std::cout << "[Callback]    2. Changing format..." << std::endl;
                if (!m_direttaOutput->changeFormat(currentFormat)) {
                    std::cerr << "[Callback] ❌ Format change failed!" << std::endl;
                    m_direttaOutput->close();
                    return false;
                }
                
                // STEP 3: Restart playback
                std::cout << "[Callback]    3. Restarting playback..." << std::endl;
                if (!m_direttaOutput->play()) {
                    std::cerr << "[Callback] ❌ Failed to restart!" << std::endl;
                    return false;
                }
                
                // STEP 4: Wait for DAC lock
                std::cout << "[Callback]    4. Waiting for DAC lock (300ms)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                
                std::cout << "[Callback] ✅ Format change completed successfully" << std::endl;
                std::cout << "════════════════════════════════════════" << std::endl;
            }
            
        } else if (hasLastFormat) {
            // Case 2: NOT connected but we have a previous format
            // This is the CRITICAL case for JPLAY's AUTO-STOP behavior!
            
            if (lastFormat != currentFormat) {
                formatChanged = true;
                
                std::cout << "════════════════════════════════════════" << std::endl;
                std::cout << "[Callback] ⚠️  FORMAT CHANGE DETECTED (after close)!" << std::endl;
                std::cout << "[Callback] Previous: " << lastFormat.sampleRate << "Hz/" 
                          << lastFormat.bitDepth << "bit/" << lastFormat.channels << "ch"
                          << (lastFormat.isDSD ? " DSD" : " PCM") << std::endl;
                std::cout << "[Callback] New: " << currentFormat.sampleRate << "Hz/" 
                          << currentFormat.bitDepth << "bit/" << currentFormat.channels << "ch"
                          << (currentFormat.isDSD ? " DSD" : " PCM") << std::endl;
                std::cout << "[Callback] 💡 Will open with new format after AUTO-STOP..." << std::endl;
                std::cout << "════════════════════════════════════════" << std::endl;

                needReopen = true;
            }
        }
        
        // ═══════════════════════════════════════════════════════════════
        // ⭐ Open connection if needed
        // ═══════════════════════════════════════════════════════════════
        
        if (!m_direttaOutput->isConnected() || needReopen) {
            auto initStart = std::chrono::steady_clock::now();
            
            // ⭐⭐⭐ CRITICAL FIX: Determine if we need to wait for Target
            bool wasConnected = hasLastFormat;  // If we had a previous format, we were connected before
            bool needsTargetReset = wasConnected && !m_direttaOutput->isConnected();
            
            if (formatChanged) {
                std::cout << "[Callback] 🔌 Opening Diretta with NEW format after change..." << std::endl;
                std::cout << "[Callback]    Old: " << lastFormat.sampleRate << "Hz/" 
                          << lastFormat.bitDepth << "bit/" << lastFormat.channels << "ch" << std::endl;
                std::cout << "[Callback]    New: " << sampleRate << "Hz/" 
                          << bitDepth << "bit/" << channels << "ch" << std::endl;
                
                // Wait for Target to reinitialize after format change
                std::cout << "[Callback] ⏳ Waiting for Target reinitialization (500ms)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                std::cout << "[Callback] ✓ Target ready for new format" << std::endl;
                
            } else if (needsTargetReset) {
                // ⭐⭐⭐ NEW: Also wait when reopening with SAME format
                // After close(), the Target needs time to reset even if format unchanged
                std::cout << "[Callback] 🔌 Reopening Diretta connection (same format: " 
                          << sampleRate << "Hz/" << bitDepth << "bit/" << channels << "ch)" << std::endl;
                std::cout << "[Callback] ⏳ Waiting for Target reset (300ms)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                std::cout << "[Callback] ✓ Target ready for reconnection" << std::endl;
                
            } else {
                // First connection ever
                DEBUG_LOG("[Callback] 🔌 First audio buffer received, initializing Diretta...");
            }
            
            DEBUG_LOG("[Callback]    Format: " << sampleRate << "Hz/" << bitDepth << "bit/" << channels << "ch");
            
            // Open Diretta connection
            AudioFormat format(sampleRate, bitDepth, channels);
            
            // ⭐ Propagate compression info for buffer optimization
            format.isCompressed = trackInfo.isCompressed;
            
            // ⭐ Configure DSD if needed
                if (trackInfo.isDSD) {
                format.isDSD = true;
                format.bitDepth = 1;  // DSD = 1 bit
                format.sampleRate = sampleRate;
                
                // Determine DSD format from codec
                std::string codec = trackInfo.codec;
                if (codec.find("lsb") != std::string::npos) {
                format.dsdFormat = AudioFormat::DSDFormat::DSF;
                DEBUG_LOG("[DirettaRenderer] 🎵 DSD format: DSF (LSB)");
             } else {
                format.dsdFormat = AudioFormat::DSDFormat::DFF;
            DEBUG_LOG("[DirettaRenderer] 🎵 DSD format: DFF (MSB)");
          }
      }
            
            if (g_verbose) {
                std::cout << "[DirettaRenderer] 🔌 Opening Diretta connection: ";
                if (format.isDSD) {
                    std::cout << "DSD" << trackInfo.dsdRate << " (" << sampleRate << " Hz)";
                } else {
                    std::cout << sampleRate << "Hz/" << bitDepth << "bit";
                }
                std::cout << "/" << channels << "ch" << std::endl;
            }
            
            if (!m_direttaOutput->open(format, m_config.bufferSeconds)) {
                std::cerr << "[DirettaRenderer] ❌ Failed to open Diretta output" << std::endl;
                return false;
            }
            
            auto connectTime = std::chrono::steady_clock::now();
            auto connectDuration = std::chrono::duration_cast<std::chrono::milliseconds>(connectTime - initStart);
            DEBUG_LOG("[DirettaRenderer] ✓ Connection established in " << connectDuration.count() << "ms");
            
            if (!m_direttaOutput->play()) {
                std::cerr << "[DirettaRenderer] ❌ Failed to start Diretta playback" << std::endl;
                return false;
            }
            
            // ⭐ CRITICAL: Wait for DAC stabilization
            DEBUG_LOG("[DirettaRenderer] ⏳ Waiting for DAC stabilization (200ms)...");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            
            auto totalTime = std::chrono::steady_clock::now();
            auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalTime - initStart);
            std::cout << "[DirettaRenderer] ✅ Ready to stream (total init: " << totalDuration.count() << "ms)" << std::endl;
            
            if (formatChanged) {
                std::cout << "[Callback] ✅ Format change completed!" << std::endl;
                std::cout << "[Callback] 💡 DAC locked to " << sampleRate << "Hz" << std::endl;
            } else if (needsTargetReset) {
                std::cout << "[Callback] ✅ Reconnection completed!" << std::endl;
            }
            
            // ⭐ Save format for next comparison
            lastFormat = format;
            hasLastFormat = true;
        }
        
        // ═══════════════════════════════════════════════════════════════
        // ⭐ Send audio data
        // ═══════════════════════════════════════════════════════════════
        
        if (!m_direttaOutput->sendAudio(buffer.data(), samples)) {
            std::cerr << "[Callback] ❌ Failed to send audio" << std::endl;
            return false;
        }
        
        return true;  // Continue playback
    }
);

		m_audioEngine->setTrackChangeCallback(
            [this](int trackNumber, const TrackInfo& info, const std::string& uri, const std::string& metadata) {
                if (g_verbose) {
                    std::cout << "[DirettaRenderer] 🎵 Track " << trackNumber 
                              << ": " << info.codec << " ";
                    
                    if (info.isDSD) {
                        std::cout << "DSD" << info.dsdRate << " (" << info.sampleRate << "Hz)";
                    } else {
                        std::cout << info.sampleRate << "Hz/" << info.bitDepth << "bit";
                    }
                    
                    std::cout << "/" << info.channels << "ch" << std::endl;
                }
                
                // CRITICAL: Update UPnP with new URI and metadata
                DEBUG_LOG("[DirettaRenderer] 🔔 Notifying UPnP of track change");
                m_upnp->setCurrentURI(uri);
                m_upnp->setCurrentMetadata(metadata);
                m_upnp->notifyTrackChange(uri, metadata);
                m_upnp->notifyStateChange("PLAYING");
            }
        );

         m_audioEngine->setTrackEndCallback([this]() {
            DEBUG_LOG("[DirettaRenderer] ✓ Track ended, notifying UPnP controller");
            m_upnp->notifyStateChange("STOPPED");
        });                  

        // ═══════════════════════════════════════════════════════════════
        // ⭐ v1.2.0: Gapless Pro - Next track callback
        // ═══════════════════════════════════════════════════════════════
        
        m_audioEngine->setNextTrackCallback(
            [this](const uint8_t* data, size_t samples, const AudioFormat& format) {
                DEBUG_LOG("[DirettaRenderer] 🎵 Next track callback triggered");
                DEBUG_LOG("[DirettaRenderer]    Samples: " << samples 
                          << ", Format: " << format.sampleRate << "Hz/" 
                          << format.bitDepth << "bit/" << format.channels << "ch");
                
                if (m_direttaOutput && m_direttaOutput->isGaplessMode()) {
                    bool prepared = m_direttaOutput->prepareNextTrack(data, samples, format);
                    
                    if (prepared) {
                        DEBUG_LOG("[DirettaRenderer] ✅ Next track prepared for gapless");
                    } else {
                        DEBUG_LOG("[DirettaRenderer] ⚠️  Failed to prepare next track");
                    }
                } else {
                    if (!m_direttaOutput) {
                        DEBUG_LOG("[DirettaRenderer] ⚠️  DirettaOutput not available");
                    } else {
                        DEBUG_LOG("[DirettaRenderer] ℹ️  Gapless mode disabled");
                    }
                }
            }
        );
        
        // ═══════════════════════════════════════════════════════════════

        
        // Setup callbacks from UPnP to AudioEngine
  
        // Track last stop time for DAC stabilization delay
        static std::chrono::steady_clock::time_point lastStopTime;
  
UPnPDevice::Callbacks callbacks;

callbacks.onSetURI = [this](const std::string& uri, const std::string& metadata) {
    DEBUG_LOG("[DirettaRenderer] SetURI: " << uri);
    
    // ⭐ v1.2.0 FIX: Keep mutex locked (v1.0.9 structure) + timeout prevents deadlock
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto currentState = m_audioEngine->getState();
    
    // ⭐ Auto-STOP if playing (JPlay iOS compatibility - added in v1.0.8)
    if (currentState == AudioEngine::State::PLAYING || 
        currentState == AudioEngine::State::PAUSED ||
        currentState == AudioEngine::State::TRANSITIONING) {
        
        std::cout << "════════════════════════════════════════" << std::endl;
        std::cout << "[DirettaRenderer] ⚠️  SetURI while " 
                  << (currentState == AudioEngine::State::PLAYING ? "PLAYING" :
                      currentState == AudioEngine::State::PAUSED ? "PAUSED" : "TRANSITIONING")
                  << std::endl;
        std::cout << "[DirettaRenderer] 🛑 Auto-STOP before URI change (JPlay iOS compatibility)" << std::endl;
        std::cout << "════════════════════════════════════════" << std::endl;

        // Stop audio engine
        m_audioEngine->stop();
        
        // Wait for callback (has 5s timeout built-in, won't deadlock thanks to patch #10)
        waitForCallbackComplete();

        // Stop and close DirettaOutput
        if (m_direttaOutput) {
            if (m_direttaOutput->isPlaying()) {
                m_direttaOutput->stop(true);
            }
            if (m_direttaOutput->isConnected()) {
                m_direttaOutput->close();
            }
        }
        
        // Notify state change
        m_upnp->notifyStateChange("STOPPED");
        
        DEBUG_LOG("[DirettaRenderer] ✓ Auto-STOP completed");
    }
    
    // Update URI (still under mutex lock - safe!)
    this->m_currentURI = uri;
    this->m_currentMetadata = metadata;
    m_audioEngine->setCurrentURI(uri, metadata);
};

// CRITICAL: SetNextAVTransportURI pour le gapless
callbacks.onSetNextURI = [this](const std::string& uri, const std::string& metadata) {
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    DEBUG_LOG("[DirettaRenderer] ✓ SetNextAVTransportURI received for gapless");
    m_audioEngine->setNextURI(uri, metadata);
};

callbacks.onPlay = [&lastStopTime, this]() {
    std::cout << "[DirettaRenderer] ✓ Play command received" << std::endl;
    
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    
    // ⭐ CRITICAL: Check if connected FIRST, before checking pause state
    // After STOP, DirettaOutput is closed (not connected), so isPaused() is meaningless
    if (m_direttaOutput && m_direttaOutput->isConnected() && m_direttaOutput->isPaused()) {
        // TRUE RESUME: DirettaOutput is connected AND paused
        DEBUG_LOG("[DirettaRenderer] 🔄 Resuming from pause...");
        try {
            // Resume DirettaOutput first
            m_direttaOutput->resume();
            
            // Then AudioEngine
            if (m_audioEngine) {
                m_audioEngine->play();
            }
            
            m_upnp->notifyStateChange("PLAYING");
            DEBUG_LOG("[DirettaRenderer] ✓ Resumed from pause");
        } catch (const std::exception& e) {
            std::cerr << "❌ Exception resuming: " << e.what() << std::endl;
        }
        return;
    }
    
    // ⭐ Not connected or not paused → Need to open/reopen track
    if (!m_direttaOutput->isConnected() && !m_currentURI.empty()) {
        DEBUG_LOG("[DirettaRenderer] ⚠️  DirettaOutput not connected after STOP");
        DEBUG_LOG("[DirettaRenderer] Reopening track: " << m_currentURI);
        
        // Reopen the track in AudioEngine
        m_audioEngine->setCurrentURI(m_currentURI, m_currentMetadata, true);
        DEBUG_LOG("[DirettaRenderer] ✓ Track reopened");
    }
    
    // DAC stabilization delay after recent Stop
    {
        auto now = std::chrono::steady_clock::now();
        auto timeSinceStop = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStopTime);
        
        if (timeSinceStop.count() < 100) {
            DEBUG_LOG("[DirettaRenderer] ⚠️  Stop was " << timeSinceStop.count() 
                      << "ms ago, adding safety delay");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    m_audioEngine->play();
    m_upnp->notifyStateChange("PLAYING");
};

callbacks.onPause = [this]() {
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "[DirettaRenderer] ⏸️  PAUSE REQUESTED" << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl;
    
    try {
        // ⭐ IMPORTANT : Mettre AudioEngine en pause AVANT DirettaOutput
        if (m_audioEngine) {
            DEBUG_LOG("[DirettaRenderer] Pausing AudioEngine...");
            m_audioEngine->pause();  // ⭐ AJOUTER CETTE LIGNE
            DEBUG_LOG("[DirettaRenderer] ✓ AudioEngine paused");
        }
        
        if (m_direttaOutput && m_direttaOutput->isPlaying()) {
            DEBUG_LOG("[DirettaRenderer] Pausing DirettaOutput...");
            m_direttaOutput->pause();
            DEBUG_LOG("[DirettaRenderer] ✓ DirettaOutput paused");
        }
        
        m_upnp->notifyStateChange("PAUSED_PLAYBACK");
        DEBUG_LOG("[DirettaRenderer] ✓ Pause complete");
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception in Pause callback: " << e.what() << std::endl;
    }
};
callbacks.onStop = [&lastStopTime, this]() {
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "[DirettaRenderer] ⛔ STOP REQUESTED" << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl;
    
    // Record stop time for DAC stabilization delay
    lastStopTime = std::chrono::steady_clock::now();
    
    try {
        // SYNC: Stop with mutex held, then wait for callback
        {
            std::lock_guard<std::mutex> cbLock(m_callbackMutex);
            DEBUG_LOG("[DirettaRenderer] Calling AudioEngine::stop()...");
            m_audioEngine->stop();
        }
        waitForCallbackComplete();
        DEBUG_LOG("[DirettaRenderer] ✓ AudioEngine stopped");
        
       // ⭐ RESET position: Recharger l'URI pour revenir au début
             if (!this->m_currentURI.empty()) {
        DEBUG_LOG("[DirettaRenderer] Resetting position to beginning...");
        m_audioEngine->setCurrentURI(this->m_currentURI, this->m_currentMetadata, true);  // ⭐ AJOUTER true
        DEBUG_LOG("[DirettaRenderer] ✓ Position reset to 0");
    }			        
        DEBUG_LOG("[DirettaRenderer] Calling DirettaOutput::stop(immediate=true)...");
        m_direttaOutput->stop(true);
        DEBUG_LOG("[DirettaRenderer] ✓ DirettaOutput stopped");
        
        DEBUG_LOG("[DirettaRenderer] Calling DirettaOutput::close()...");
        m_direttaOutput->close();
        DEBUG_LOG("[DirettaRenderer] ✓ DirettaOutput closed");
        
        DEBUG_LOG("[DirettaRenderer] Notifying UPnP state change...");
        m_upnp->notifyStateChange("STOPPED");
        DEBUG_LOG("[DirettaRenderer] ✓ UPnP notified");
        
        DEBUG_LOG("[DirettaRenderer] ✓ Stop sequence completed BEFORE responding to JPLAY");
        
    } catch (const std::exception& e) {
        std::cerr << "❌❌❌ EXCEPTION in Stop callback: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "❌❌❌ UNKNOWN EXCEPTION in Stop callback!" << std::endl;
    }
};

callbacks.onSeek = [this](const std::string& target) {  // ⭐ Enlever unit
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "[DirettaRenderer] 🔍 SEEK REQUESTED" << std::endl;
    std::cout << "   Target: " << target << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl;
    
    try {
        // Parser le target (format: "HH:MM:SS" ou "HH:MM:SS.mmm")
        double seconds = parseTimeString(target);
        
        std::cout << "[DirettaRenderer] Parsed time: " << seconds << "s" << std::endl;
        // Seek dans AudioEngine
        if (m_audioEngine) {
            std::cout << "[DirettaRenderer] Seeking AudioEngine..." << std::endl;
            if (!m_audioEngine->seek(seconds)) {
                std::cerr << "[DirettaRenderer] ❌ AudioEngine seek failed" << std::endl;
                return;
            }
            DEBUG_LOG("[DirettaRenderer] ✓ Seek request sent to AudioEngine (async)");        }
        
            DEBUG_LOG("[DirettaRenderer] ✓ Seek complete");
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception in Seek callback: " << e.what() << std::endl;
    }
	};
        

m_upnp->setCallbacks(callbacks);       
      
       // Start UPnP server
        if (!m_upnp->start()) {
            std::cerr << "[DirettaRenderer] Failed to start UPnP server" << std::endl;
            return false;
        }
        
        DEBUG_LOG("[DirettaRenderer] UPnP Server: " << m_upnp->getDeviceURL());
        DEBUG_LOG("[DirettaRenderer] Device URL: " << m_upnp->getDeviceURL() << "/description.xml");
        
        // Start threads
        m_running = true;
        
        m_upnpThread = std::thread(&DirettaRenderer::upnpThreadFunc, this);
        m_audioThread = std::thread(&DirettaRenderer::audioThreadFunc, this);
        m_positionThread = std::thread(&DirettaRenderer::positionThreadFunc, this);
        
        DEBUG_LOG("[DirettaRenderer] ✓ All components started");
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[DirettaRenderer] Exception during start: " << e.what() << std::endl;
        stop();
        return false;
    }
}

void DirettaRenderer::stop() {
    if (!m_running) {
        return;
    }
    
    DEBUG_LOG("[DirettaRenderer] Stopping...");
    
    m_running = false;
    
    // Stop audio engine
    if (m_audioEngine) {
        m_audioEngine->stop();
        m_upnp->notifyStateChange("STOPPED");
    }
    
    // Stop Diretta output
    if (m_direttaOutput) {
        m_direttaOutput->close();
        m_upnp->notifyStateChange("STOPPED");
    }
    
    // Stop UPnP server
    if (m_upnp) {
      m_upnp->stop();
  }
    
    // Wait for threads
    if (m_upnpThread.joinable()) {
        m_upnpThread.join();
    }
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }
    if (m_positionThread.joinable()) {
        m_positionThread.join();
    }
    
    DEBUG_LOG("[DirettaRenderer] ✓ Stopped");
}



void DirettaRenderer::upnpThreadFunc() {
    std::cout << "[UPnP Thread] Started" << std::endl;
    
    // UPnP server runs in its own daemon threads (libmicrohttpd)
    // Just keep this thread alive
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << "[UPnP Thread] Stopped" << std::endl;
}

void DirettaRenderer::audioThreadFunc() {
    DEBUG_LOG("[Audio Thread] Started");
    DEBUG_LOG("[Audio Thread] ⏱️  Precise timing enabled")
    
    // ✅ CRITICAL: Packet size must be adapted to format!
    // DSD:  32768 samples (matches Diretta processing quantum, ~11.6ms)
    // PCM:  8192 samples (larger values cause track skipping in gapless)
    
    auto nextProcessTime = std::chrono::steady_clock::now();
    uint32_t lastSampleRate = 0;
    std::chrono::microseconds lastInterval(0);
    size_t currentSamplesPerCall = 8192;  // Default for PCM
    
    // Track for debug
    AudioEngine::State lastLoggedState = AudioEngine::State::STOPPED;
    
    while (m_running) {
        if (!m_audioEngine) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        auto state = m_audioEngine->getState();
        
        // Log state changes
        if (state != lastLoggedState) {
            std::cout << "[Audio Thread] ⚡ State changed: " 
                      << (int)lastLoggedState << " → " << (int)state << std::endl;
            lastLoggedState = state;
        }
        
        if (state == AudioEngine::State::PLAYING) {
            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            uint32_t sampleRate = trackInfo.sampleRate;
            bool isDSD = trackInfo.isDSD;
            
            if (sampleRate == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                nextProcessTime = std::chrono::steady_clock::now();
                continue;
            }
            
            // ✅ Adapt packet size based on format
            size_t samplesPerCall = isDSD ? 32768 : 8192;
            
            // Recalculate timing if format changed
            if (sampleRate != lastSampleRate || samplesPerCall != currentSamplesPerCall) {
                currentSamplesPerCall = samplesPerCall;
                lastSampleRate = sampleRate;
                
                auto interval = std::chrono::microseconds(
                    (currentSamplesPerCall * 1000000LL) / sampleRate
                );
                lastInterval = interval;
                
                auto intervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
                double callsPerSecond = 1000000.0 / interval.count();
                
                std::cout << "[Audio Thread] ⏱️  Timing reconfigured for " << sampleRate << "Hz "
                          << (isDSD ? "DSD" : "PCM") << ":" << std::endl;
                std::cout << "[Audio Thread]     - Samples/call: " << currentSamplesPerCall << std::endl;
                std::cout << "[Audio Thread]     - Interval: " << intervalMs.count() << " ms (" 
                          << interval.count() << " µs)" << std::endl;
                std::cout << "[Audio Thread]     - Calls/sec: " << std::fixed << std::setprecision(1) 
                          << callsPerSecond << std::endl;
            }
            
            std::this_thread::sleep_until(nextProcessTime);
            
            bool success = m_audioEngine->process(currentSamplesPerCall);
            
            nextProcessTime += lastInterval;
            
            if (!success) {
                // Compteur pour réduire le spam de logs
                static int failCount = 0;
                static int totalFails = 0;
                
                failCount++;
                totalFails++;
                
                // Logger seulement tous les 100 échecs (ou le premier)
                if (failCount == 1 || failCount % 100 == 0) {
                    std::cout << "[Audio Thread] ⚠️  process() returned false"
                              << " (" << totalFails << " total, " 
                              << failCount << " consecutive)" << std::endl;
                }
                
                // ⭐ CRITICAL FIX: Ajouter une pause pour éviter le spam CPU
                // Sans cette pause, la boucle repart immédiatement et spam
                // des milliers de fois par seconde !
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                // Reset le temps de prochain process
                nextProcessTime = std::chrono::steady_clock::now();
            } else {
                // Reset le compteur d'échecs consécutifs quand ça réussit
                static int failCount = 0;
                failCount = 0;
            }
                   
        } else {
            // ← AJOUTER : Log quand en attente
            static int waitCount = 0;
            if (waitCount++ == 0 || waitCount % 10 == 0) {
                DEBUG_LOG("[Audio Thread] ⏸️  Waiting (state=" << (int)state 
                          << ", count=" << waitCount << ")");
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            nextProcessTime = std::chrono::steady_clock::now();
            lastSampleRate = 0;
            
            // Reset le compteur quand on repasse en PLAYING
            if (state == AudioEngine::State::PLAYING) {
                waitCount = 0;
            }
        }
    }
    
    std::cout << "[Audio Thread] Stopped" << std::endl;
}

void DirettaRenderer::positionThreadFunc() {
    DEBUG_LOG("[Position Thread] Started - updating position for eventing");
    
    while (m_running) {
        if (!m_audioEngine || !m_upnp) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        
        auto state = m_audioEngine->getState();
        
        if (state == AudioEngine::State::PLAYING) {
            // Récupérer la position actuelle depuis AudioEngine
            double positionSeconds = m_audioEngine->getPosition();
            int position = static_cast<int>(positionSeconds);
            
            // Récupérer la durée de la piste
            const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
            // ⚠️ IMPORTANT: trackInfo.duration est en SAMPLES, convertir en secondes
            int duration = 0;
            if (trackInfo.sampleRate > 0) {
                duration = trackInfo.duration / trackInfo.sampleRate;
            }
            
            // Mettre à jour UPnP
            m_upnp->setCurrentPosition(position);
            m_upnp->setTrackDuration(duration);
            
            // Envoyer événement aux contrôleurs abonnés (mConnect, BubbleUPnP)
            m_upnp->notifyPositionChange(position, duration);
            
            // Log périodique (toutes les 10 secondes pour ne pas polluer)
            static int lastLoggedPosition = -10;
            if (position - lastLoggedPosition >= 10) {
                DEBUG_LOG("[Position Thread] 📍 Position: " << position << "s / " << duration << "s");
                lastLoggedPosition = position;
            }
        }
        
        // Mise à jour toutes les secondes (standard UPnP)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << "[Position Thread] Stopped" << std::endl;
}