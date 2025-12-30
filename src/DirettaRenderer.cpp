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
        
        // Create other components
        UPnPDevice::Config upnpConfig;
        upnpConfig.friendlyName = m_config.name;
        upnpConfig.manufacturer = "DIY Audio";
        upnpConfig.modelName = "Diretta UPnP Renderer";
        upnpConfig.uuid = m_config.uuid;
        upnpConfig.port = m_config.port;

        m_upnp = std::make_unique<UPnPDevice>(upnpConfig);        
        
        m_audioEngine = std::make_unique<AudioEngine>();

        
        
        // Setup callbacks from AudioEngine to DirettaOutput
        m_audioEngine->setAudioCallback(
    [this](const AudioBuffer& buffer, size_t samples, 
           uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {
        
        // ⚠️  DEBUG: Temporarily disabled state check to debug DSD playback
        // CRITICAL: Re-enable this after finding the root cause!
        /*
        if (m_audioEngine->getState() != AudioEngine::State::PLAYING) {
            DEBUG_LOG("[Callback] ⛔ Not PLAYING, stopping audio flow");
            return false;  // Arrêter immédiatement
        }
        */
        
        DEBUG_LOG("[Callback] Sending " << samples << " samples");
        
        // Get track info to check for DSD
        const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();
        
        if (!m_direttaOutput->isConnected()) {
            // ⭐ LOG: Premier buffer reçu, initialisation Diretta
            auto initStart = std::chrono::steady_clock::now();
            DEBUG_LOG("[Callback] 🔌 First audio buffer received, initializing Diretta...");
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
                // LSB = DSF, MSB = DFF
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
            
            // ⭐⭐⭐ CRITICAL FIX: Wait for DAC stabilization to prevent silent playback ⭐⭐⭐
            // The Diretta connection is established, but the DAC needs time to lock
            // onto the new format and be ready to receive audio samples.
            // Without this delay, the first buffers may be lost → silent playback
            DEBUG_LOG("[DirettaRenderer] ⏳ Waiting for DAC stabilization (200ms)...");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            auto totalTime = std::chrono::steady_clock::now();
            auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalTime - initStart);
            std::cout << "[DirettaRenderer] ✅ Ready to stream (total init: " << totalDuration.count() << "ms)" << std::endl;
        }
        
        // Check format change
        AudioFormat currentFormat = m_direttaOutput->getFormat();
        bool formatChanged = false;
        
        if (trackInfo.isDSD != currentFormat.isDSD) {
            formatChanged = true;
        } else if (currentFormat.sampleRate != sampleRate ||
                   currentFormat.bitDepth != bitDepth ||
                   currentFormat.channels != channels) {
            formatChanged = true;
        }
        
        if (formatChanged) {
            DEBUG_LOG("[DirettaRenderer] 🔄 Format change detected");
            
            AudioFormat newFormat(sampleRate, bitDepth, channels);
            
            // ⭐ Configure DSD if needed
            if (trackInfo.isDSD) {
                newFormat.isDSD = true;
                newFormat.bitDepth = 1;
                newFormat.sampleRate = sampleRate;
                
                std::string codec = trackInfo.codec;
                if (codec.find("lsb") != std::string::npos) {
                    newFormat.dsdFormat = AudioFormat::DSDFormat::DSF;
                } else {
                    newFormat.dsdFormat = AudioFormat::DSDFormat::DFF;
                }
            }
            
            if (!m_direttaOutput->changeFormat(newFormat)) {
                std::cerr << "[DirettaRenderer] ❌ Failed to change format" << std::endl;
                return false;
            }
        }
        
        // Send audio to Diretta
        return m_direttaOutput->sendAudio(buffer.data(), samples);
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

        
        // Setup callbacks from UPnP to AudioEngine
  
        // Track last stop time to handle Stop+Play race condition
        static std::chrono::steady_clock::time_point lastStopTime;
        static std::mutex stopTimeMutex;
  
UPnPDevice::Callbacks callbacks;

callbacks.onSetURI = [this](const std::string& uri, const std::string& metadata) {
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    DEBUG_LOG("[DirettaRenderer] SetURI: " << uri);
    
    // ⭐ Sauvegarder l'URI courante
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

callbacks.onPlay = [&lastStopTime, &stopTimeMutex, this]() {
    std::cout << "[DirettaRenderer] ✓ Play command received" << std::endl;
    
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    // ⭐ NOUVEAU : Gérer Resume si en pause
if (m_direttaOutput && m_direttaOutput->isPaused()) {
    DEBUG_LOG("[DirettaRenderer] 🔄 Resuming from pause...");
    try {
        // ⭐ Reprendre DirettaOutput d'abord
        m_direttaOutput->resume();
        
        // ⭐ Puis AudioEngine
        if (m_audioEngine) {
            m_audioEngine->play();  // ⭐ AJOUTER CETTE LIGNE
        }
        
        m_upnp->notifyStateChange("PLAYING");
        DEBUG_LOG("[DirettaRenderer] ✓ Resumed from pause");
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception resuming: " << e.what() << std::endl;
    }
    return;
}    
    // ⚠️  SAFETY: Conditional delay to avoid race condition with Stop
    // Only add delay if Stop was called very recently (< 100ms ago)
    // This prevents gapless issues while still protecting against Stop+Play races
    {
        std::lock_guard<std::mutex> lock(stopTimeMutex);
        auto now = std::chrono::steady_clock::now();
        auto timeSinceStop = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStopTime);
        
        if (timeSinceStop.count() < 100) {
            DEBUG_LOG("[DirettaRenderer] ⚠️  Stop was " << timeSinceStop.count() 
                      << "ms ago, adding safety delay");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
callbacks.onStop = [&lastStopTime, &stopTimeMutex, this]() {
    std::lock_guard<std::mutex> lock(m_mutex);  // Serialize UPnP actions
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "[DirettaRenderer] ⛔ STOP REQUESTED" << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl;
    
    // Record stop time for Play race condition detection
    {
        std::lock_guard<std::mutex> lock(stopTimeMutex);
        lastStopTime = std::chrono::steady_clock::now();
    }
    
    try {
        DEBUG_LOG("[DirettaRenderer] Calling AudioEngine::stop()...");
        m_audioEngine->stop();
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
            std::cout << "[DirettaRenderer] ✓ AudioEngine seeked" << std::endl;
        }
        
        // Seek dans DirettaOutput
        if (m_direttaOutput && m_audioEngine) {
    uint32_t sampleRate = m_audioEngine->getCurrentSampleRate();  // ⭐ Obtenir depuis AudioEngine
    if (sampleRate > 0) {
        int64_t samplePosition = static_cast<int64_t>(seconds * sampleRate);
            
            std::cout << "[DirettaRenderer] Seeking DirettaOutput to sample " << samplePosition << "..." << std::endl;
            if (!m_direttaOutput->seek(samplePosition)) {
                std::cerr << "[DirettaRenderer] ❌ DirettaOutput seek failed" << std::endl;
                return;
            }
            std::cout << "[DirettaRenderer] ✓ DirettaOutput seeked" << std::endl;
        }
	}
        std::cout << "[DirettaRenderer] ✓ Seek complete" << std::endl;
        
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
                // ← AJOUTER : Log quand process échoue
                std::cout << "[Audio Thread] ⚠️  process() returned false" << std::endl;
                nextProcessTime = std::chrono::steady_clock::now();
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