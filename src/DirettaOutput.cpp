/**
 * @file DirettaOutput.cpp
 * @brief Diretta Output implementation
 */

#include "DirettaOutput.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

extern bool g_verbose;
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }


DirettaOutput::DirettaOutput()
    : m_mtu(1500)
    , m_bufferSeconds(2)
    , m_connected(false)
    , m_playing(false)
    , m_isPaused(false)
    , m_targetIndex(-1)
    , m_totalSamplesSent(0)
    , m_pausedPosition(0)
    , m_gaplessEnabled(true)       // ⭐ v1.2.0: Gapless enabled by default
    , m_nextTrackPrepared(false)   // ⭐ v1.2.0
    , m_thredMode(1)
    , m_cycleTime(10000)
    , m_cycleMinTime(333)
    , m_infoCycle(100000)
{
    DEBUG_LOG("[DirettaOutput] Created");
    DEBUG_LOG("[DirettaOutput] ✓ Gapless Pro mode: " 
              << (m_gaplessEnabled ? "ENABLED" : "DISABLED"));
}

DirettaOutput::~DirettaOutput() {
    close();
}

void DirettaOutput::setMTU(uint32_t mtu) {
    if (m_connected) {
        std::cerr << "[DirettaOutput] ⚠️  Cannot change MTU while connected" << std::endl;
        return;
    }
    
    m_mtu = mtu;
    
    DEBUG_LOG("[DirettaOutput] ✓ MTU configured: " << m_mtu << " bytes");
    
    if (mtu > 1500) {
        std::cout << " (jumbo frames)";
    }
    
    std::cout << std::endl;
}



bool DirettaOutput::open(const AudioFormat& format, float bufferSeconds) {
    DEBUG_LOG("[DirettaOutput] Opening: " 
              << format.sampleRate << "Hz/" 
              << format.bitDepth << "bit/" 
              << format.channels << "ch");
    
    m_currentFormat = format;
    m_totalSamplesSent = 0;
    DEBUG_LOG("[DirettaOutput] ⭐ m_totalSamplesSent RESET to 0"); 
    
    // ✅ INTELLIGENT BUFFER ADAPTATION based on processing complexity
    //
    // Processing Pipeline Complexity:
    // 1. DSD (DSF/DFF):        Raw bitstream read      → 0.8s  (instant)
    // 2. WAV/AIFF:             Direct PCM read         → 1.0s  (very fast)
    // 3. FLAC/ALAC/APE:        Lossless decompression  → 2.0s  (moderate)
    //
    // This matches Dominique's insight: Diretta can handle uncompressed 
    // formats as efficiently as DSD, since both skip the decode step!
    
    float effectiveBuffer;
    
    if (format.isDSD) {
        // DSD: Raw bitstream, zero decode overhead
        effectiveBuffer = std::min(bufferSeconds, 0.02f);
        DEBUG_LOG("[DirettaOutput] 🎵 DSD: raw bitstream path");
        
    } else if (!format.isCompressed) {
        // WAV/AIFF: Uncompressed PCM - intelligent buffer sizing
        
        // ⚠️  LOOPBACK DETECTION (v1.0.10)
        // Check if this is local playback (same-machine streaming)
        // In loopback mode, data arrives in bursts without network buffering
        bool isLoopback = false;
        // Heuristic: If MTU is default (not jumbo), likely loopback wasn't configured
        // Real network would use jumbo frames (16128)
        // This is a simple heuristic - not perfect but works in most cases
        if (m_mtu <= 1500) {
            isLoopback = true;
        }
        
        if (format.bitDepth >= 24 && format.sampleRate >= 88200) {
            // Hi-Res audio handling
            if (isLoopback && format.sampleRate <= 96000) {
                // Loopback + Hi-Res ≤96kHz: needs larger buffer
                // Reason: Data arrives in bursts, need extra buffer to prevent underruns
                effectiveBuffer = std::max(std::min(bufferSeconds, 1.5f), 1.2f);
                DEBUG_LOG("[DirettaOutput] ⚠️  Loopback Hi-Res detected (" << format.bitDepth 
                          << "bit/" << format.sampleRate << "Hz)");
                DEBUG_LOG("[DirettaOutput]   Using 2-2.5s buffer (burst protection)");
                DEBUG_LOG("[DirettaOutput]   💡 TIP: For lower latency, use remote player");
                DEBUG_LOG("[DirettaOutput]        or enable oversampling in your player");
            } else {
                // Network or high sample rate: normal buffer
                effectiveBuffer = std::max(std::min(bufferSeconds, 1.5f), 1.2f);
                DEBUG_LOG("[DirettaOutput] ✓ Hi-Res PCM (" << format.bitDepth 
                          << "bit/" << format.sampleRate << "Hz): enhanced buffer");
                DEBUG_LOG("[DirettaOutput]   Buffer: " << effectiveBuffer 
                          << "s (DAC stabilization)");
            }
        } else {
            // Standard PCM: low latency
            effectiveBuffer = std::min(bufferSeconds, 1.0f);
            DEBUG_LOG("[DirettaOutput] ✓ Uncompressed PCM: low-latency path");
            DEBUG_LOG("[DirettaOutput]   Buffer: " << effectiveBuffer << "s");
        }
        
    } else {
        // FLAC/ALAC/etc: Compressed, needs decoding buffer
        effectiveBuffer = std::max(bufferSeconds, 0.8f);
        DEBUG_LOG("[DirettaOutput] ℹ️  Compressed PCM (FLAC/ALAC): decoding required");
        
        if (bufferSeconds < 2) {
            DEBUG_LOG("[DirettaOutput]   Using 2s minimum for decode stability");
        }
    }
    
    m_bufferSeconds = effectiveBuffer;
    DEBUG_LOG("[DirettaOutput] → Effective buffer: " << m_bufferSeconds << "s");
    
    // Find Diretta target
    DEBUG_LOG("[DirettaOutput] Finding Diretta target...");
    if (!findAndSelectTarget(m_targetIndex)) {  // Use configured target index
        std::cerr << "[DirettaOutput] ❌ Failed to find or select Diretta target" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[DirettaOutput] ✓ Found Diretta target");
    
// Configure and connect (with retry for slow DACs)
const int CONFIG_MAX_RETRIES = 3;
bool configured = false;
int attempt = 1;

for (attempt = 1; attempt <= CONFIG_MAX_RETRIES && !configured; attempt++) {
    if (attempt > 1) {
        std::cout << "[DirettaOutput] ⚠️  Configuration attempt " << attempt 
                  << "/" << CONFIG_MAX_RETRIES << " (DAC may be initializing...)" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    configured = configureDiretta(format);
    
    if (!configured && attempt < CONFIG_MAX_RETRIES) {
        DEBUG_LOG("[DirettaOutput] Configuration failed, retrying...");
    }
}

if (!configured) {
    std::cerr << "[DirettaOutput] ❌ Failed to configure Diretta after " 
              << CONFIG_MAX_RETRIES << " attempts" << std::endl;
    std::cerr << "[DirettaOutput] 💡 Possible causes:" << std::endl;
    std::cerr << "[DirettaOutput]    - DAC not fully initialized (wait 30s after power-on)" << std::endl;
    std::cerr << "[DirettaOutput]    - Unsupported audio format" << std::endl;
    std::cerr << "[DirettaOutput]    - DAC firmware issue" << std::endl;
    return false;
}

if (attempt > 2) {  // Si on a réussi après plus d'une tentative
    std::cout << "[DirettaOutput] ✅ Configuration succeeded on attempt " << (attempt - 1) << std::endl;
}
    
    m_connected = true;
    std::cout << "[DirettaOutput] ✓ Connected and configured" << std::endl;
    
    return true;
}

void DirettaOutput::close() {
    // ⭐ v1.2.0 Stable: Protection contre double close
    if (!m_connected) {
        DEBUG_LOG("[DirettaOutput] Already closed, skipping");
        return;
    }
    
    DEBUG_LOG("[DirettaOutput] Closing connection...");
    
    // Marquer comme déconnecté IMMÉDIATEMENT pour éviter ré-entrée
    m_connected = false;
    m_playing = false;
    
    if (m_syncBuffer) {
        DEBUG_LOG("[DirettaOutput] 1. Disconnecting SyncBuffer...");
        
        try {
            m_syncBuffer->pre_disconnect(true);  // Immediate
        } catch (const std::exception& e) {
            std::cerr << "[DirettaOutput] ⚠️  Exception during disconnect: " 
                      << e.what() << std::endl;
        }
        
        DEBUG_LOG("[DirettaOutput] 2. Releasing SyncBuffer...");
        m_syncBuffer.reset();
    }
    
    DEBUG_LOG("[DirettaOutput] 3. Resetting UDP sockets...");
    m_udp.reset();
    m_raw.reset();
    
    DEBUG_LOG("[DirettaOutput] ✓ Connection closed");
}

bool DirettaOutput::play() {
    if (!m_connected) {
        std::cerr << "[DirettaOutput] ❌ Not connected" << std::endl;
        return false;
    }
    
    if (m_playing) {
        return true; // Already playing
    }
    
    DEBUG_LOG("[DirettaOutput] Starting playback...");
    
    if (!m_syncBuffer) {
        std::cerr << "[DirettaOutput] ❌ SyncBuffer not initialized" << std::endl;
        return false;
    }
    
    m_syncBuffer->play();
    m_playing = true;
    
    std::cout << "[DirettaOutput] ✓ Playing" << std::endl;
    
    return true;
}

void DirettaOutput::stop(bool immediate) {
    if (!m_playing) {
        DEBUG_LOG("[DirettaOutput] ⚠️  stop() called but not playing");
        return;
    }
    
    DEBUG_LOG("[DirettaOutput] 🛑 Stopping (immediate=" << immediate << ")...");
    
    if (m_syncBuffer) {
        if (!immediate) {
            // ⭐ DRAIN buffers before stopping (graceful stop)
            DEBUG_LOG("[DirettaOutput] Draining buffers before stop...");
            int drain_timeout_ms = 5000;
            int drain_waited_ms = 0;
            
            while (drain_waited_ms < drain_timeout_ms) {
                if (m_syncBuffer->buffer_empty()) {
                    DEBUG_LOG("[DirettaOutput] ✓ Buffers drained");
                    break;
                }
                
                if (drain_waited_ms % 200 == 0) {
                    size_t buffered = m_syncBuffer->getLastBufferCount();
                    DEBUG_LOG("[DirettaOutput]    Waiting... (" << buffered << " samples buffered)");
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                drain_waited_ms += 50;
            }
            
            if (drain_waited_ms >= drain_timeout_ms) {
                std::cerr << "[DirettaOutput] ⚠️  Drain timeout, forcing immediate stop" << std::endl;
                immediate = true;  // Force immediate if timeout
            }
        }
        
        DEBUG_LOG("[DirettaOutput] Calling pre_disconnect(" << immediate << ")...");
        auto start = std::chrono::steady_clock::now();
        
        m_syncBuffer->pre_disconnect(immediate);
        
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        DEBUG_LOG("[DirettaOutput] ✓ pre_disconnect completed in " << duration.count() << "ms");
        DEBUG_LOG("[DirettaOutput] Calling seek_front() to reset buffer...");
        m_syncBuffer->seek_front();
        DEBUG_LOG("[DirettaOutput] ✓ Buffer reset to front");
    } else {
        std::cout << "[DirettaOutput] ⚠️  No SyncBuffer to disconnect" << std::endl;
    }
    
    m_playing = false;
    m_isPaused = false;      // Reset état pause
    m_pausedPosition = 0;    // Reset position sauvegardée
    m_totalSamplesSent = 0;
    
    std::cout << "[DirettaOutput] ✓ Stopped" << std::endl;
}

void DirettaOutput::pause() {
    if (!m_playing || m_isPaused) {
        return;
    }
    
    DEBUG_LOG("[DirettaOutput] ⏸️  Pausing...");
    
    // Sauvegarder la position actuelle
    m_pausedPosition = m_totalSamplesSent;
    
    // Arrêter la lecture
    if (m_syncBuffer) {
        m_syncBuffer->stop();
    }
    
    m_isPaused = true;
    m_playing = false;
    
    DEBUG_LOG("[DirettaOutput] ✓ Paused at sample " << m_pausedPosition);
}

void DirettaOutput::resume() {
    if (!m_isPaused) {
        return;
    }
    
    DEBUG_LOG("[DirettaOutput] ▶️  Resuming from sample " << m_pausedPosition << "...");
    
    if (m_syncBuffer) {
        // Seek à la position sauvegardée
        m_syncBuffer->seek(m_pausedPosition);
        
        // Redémarrer la lecture
        m_syncBuffer->play();
    }
    
    m_isPaused = false;
    m_playing = true;
    
    std::cout << "[DirettaOutput] ✓ Resumed" << std::endl;
}





bool DirettaOutput::changeFormat(const AudioFormat& newFormat) {
    std::cout << "[DirettaOutput] Format change request: "
              << m_currentFormat.sampleRate << "Hz/" << m_currentFormat.bitDepth << "bit"
              << " → " << newFormat.sampleRate << "Hz/" << newFormat.bitDepth << "bit" << std::endl;
    
    if (newFormat == m_currentFormat) {
        std::cout << "[DirettaOutput] ✓ Same format, no change needed" << std::endl;
        return true;
    }
    
    std::cout << "[DirettaOutput] ⚠️  Format change during playback - CRITICAL DRAIN REQUIRED" << std::endl;
    
    if (m_syncBuffer) {
        // ⭐ STEP 1: STOP SENDING NEW DATA (caller must stop feeding audio)
        std::cout << "[DirettaOutput] 1. Stop sending new audio data..." << std::endl;
        
        // ⭐ STEP 2: WAIT FOR ALL QUEUED BUFFERS TO BE PLAYED
        std::cout << "[DirettaOutput] 2. Draining queued buffers..." << std::endl;
        int drain_timeout_ms = 10000;  // 10 seconds max for drain
        int drain_waited_ms = 0;
        
        // Get initial buffer count
        size_t initialBuffered = m_syncBuffer->getLastBufferCount();
        std::cout << "[DirettaOutput]    Initial buffered samples: " << initialBuffered << std::endl;
        
        // Wait until buffer is empty
        while (drain_waited_ms < drain_timeout_ms) {
            if (m_syncBuffer->buffer_empty()) {
                std::cout << "[DirettaOutput]    ✓ All buffers drained!" << std::endl;
                break;
            }
            
            // Log progress every 200ms
            if (drain_waited_ms % 200 == 0) {
                size_t buffered = m_syncBuffer->getLastBufferCount();
                DEBUG_LOG("[DirettaOutput]    Waiting... (" << buffered << " samples remaining)");
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            drain_waited_ms += 50;
        }
        
        if (drain_waited_ms >= drain_timeout_ms) {
            size_t remainingBuffered = m_syncBuffer->getLastBufferCount();
            std::cerr << "[DirettaOutput]    ⚠️  DRAIN TIMEOUT! " << remainingBuffered 
                      << " samples still buffered after " << drain_timeout_ms << "ms" << std::endl;
            std::cerr << "[DirettaOutput]    Forcing immediate disconnect..." << std::endl;
            // Force immediate disconnect to avoid pink noise
            m_syncBuffer->pre_disconnect(true);
        } else {
            std::cout << "[DirettaOutput]    ✓ Buffer drained in " << drain_waited_ms << "ms" << std::endl;
            
            // ⭐ STEP 3: GRACEFUL DISCONNECT
            std::cout << "[DirettaOutput] 3. Graceful disconnect..." << std::endl;
            m_syncBuffer->pre_disconnect(false);
        }
        
        // ⭐ STEP 4: WAIT FOR HARDWARE STABILIZATION
        std::cout << "[DirettaOutput] 4. Waiting for hardware stabilization (200ms)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // ⭐ STEP 5: DESTROY SYNCBUFFER (force recreation in configureDiretta)
        // After pre_disconnect(), SyncBuffer cannot be reused for a different format
        std::cout << "[DirettaOutput] 5. Destroying SyncBuffer for clean recreation..." << std::endl;
        m_syncBuffer.reset();
    }

    // ⭐ STEP 6: RECONFIGURE WITH NEW FORMAT
    std::cout << "[DirettaOutput] 6. Configuring new format..." << std::endl;
    if (!configureDiretta(newFormat)) {
        std::cerr << "[DirettaOutput] ❌ Failed to reconfigure" << std::endl;
        return false;
    }
    
    // ⭐ v1.2.0 Stable: Optimize network config for new format
    optimizeNetworkConfig(newFormat);
    
    // ⭐ STEP 7: RESTART PLAYBACK IF NEEDED
    if (m_playing) {
        std::cout << "[DirettaOutput] 7. Restarting playback..." << std::endl;
        m_syncBuffer->play();
        
        // Wait for DAC to lock onto new format
        std::cout << "[DirettaOutput]    Waiting for DAC lock (200ms)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    m_currentFormat = newFormat;
    m_totalSamplesSent = 0;  // Reset counter for new format
    
    std::cout << "[DirettaOutput] ✅ Format changed successfully" << std::endl;
    
    return true;
}
bool DirettaOutput::sendAudio(const uint8_t* data, size_t numSamples) {
    if (!m_connected || !m_playing) {
        return false;
    }
    
    if (!m_syncBuffer) {
        return false;
    }
    
    // CRITICAL: Different calculation for DSD vs PCM
    size_t dataSize;
    
    if (m_currentFormat.isDSD) {
        // For DSD: numSamples is already in audio frames (not bits)
        // DSD64 stereo: 1 audio frame = 2 channels × 1 bit = 2 bits = 0.25 bytes
        // But AudioEngine gives us samples in terms of bits per channel
        // So: numSamples = bits per channel, dataSize = total bytes
        //
        // Example: 32768 samples = 32768 bits per channel
        //          For stereo: 32768 bits × 2 channels = 65536 bits = 8192 bytes
        dataSize = (numSamples * m_currentFormat.channels) / 8;
        
        static int debugCount = 0;
        if (debugCount++ < 3) {
            DEBUG_LOG("[DirettaOutput::sendAudio] DSD: " << numSamples 
                      << " samples → " << dataSize << " bytes");
        }
    } else {
        // ✅ PCM: Calculate based on ACTUAL format (not what we'll send)
        // For 24-bit: input is S32 (4 bytes), output will be S24 (3 bytes)
        uint32_t inputBytesPerSample = (m_currentFormat.bitDepth == 24) ? 4 : (m_currentFormat.bitDepth / 8);
        inputBytesPerSample *= m_currentFormat.channels;
        
        // Output size (what we'll actually send to Diretta)
        uint32_t outputBytesPerSample = (m_currentFormat.bitDepth / 8) * m_currentFormat.channels;
        dataSize = numSamples * outputBytesPerSample;
    }
    
    DIRETTA::Stream stream;
    stream.resize(dataSize);
    
// ✅ CRITICAL FIX: Convert S32 → S24 if needed
if (!m_currentFormat.isDSD && m_currentFormat.bitDepth == 24) {
    // Input: S32 (4 bytes per sample)
    // Output: S24 (3 bytes per sample)
    const int32_t* input32 = reinterpret_cast<const int32_t*>(data);
    uint8_t* output24 = stream.get();
    
    size_t totalSamples = numSamples * m_currentFormat.channels;
    
    for (size_t i = 0; i < totalSamples; i++) {
        int32_t sample32 = input32[i];
        
        // Extract 24-bit from 32-bit (MSB aligned)
        // Little-endian byte order for S24LE
        output24[i*3 + 0] = (sample32 >> 8) & 0xFF;   // LSB
        output24[i*3 + 1] = (sample32 >> 16) & 0xFF;  // Mid
        output24[i*3 + 2] = (sample32 >> 24) & 0xFF;  // MSB
    }
    
    static int convCount = 0;
    if (convCount++ < 3 || convCount % 100 == 0) {
        DEBUG_LOG("[sendAudio] S32→S24: " << numSamples << " samples, " 
                  << totalSamples << " total, " << dataSize << " bytes");
    }
    } else {
        // For other formats (16-bit, 32-bit, DSD): direct copy
        memcpy(stream.get(), data, dataSize);
    }
    m_syncBuffer->setStream(stream);
    m_totalSamplesSent += numSamples;

    static int callCount = 0;
    if (++callCount % 500 == 0) {
        double seconds = static_cast<double>(m_totalSamplesSent) / m_currentFormat.sampleRate;
        DEBUG_LOG("[DirettaOutput] Position: " << seconds << "s (" 
                  << m_totalSamplesSent << " samples)");
    }

        
    return true;
}

float DirettaOutput::getBufferLevel() const {
    return 0.5f;
}

bool DirettaOutput::findTarget() {
    m_udp = std::make_unique<ACQUA::UDPV6>();
    m_raw = std::make_unique<ACQUA::UDPV6>();
    
    DIRETTA::Find::Setting findSetting;
    findSetting.Loopback = false;
    findSetting.ProductID = 0;
    
    DIRETTA::Find find(findSetting);
    
    if (!find.open()) {
        std::cerr << "[DirettaOutput] ❌ Failed to open Find" << std::endl;
        return false;
    }
    
    DIRETTA::Find::PortResalts targets;
    if (!find.findOutput(targets)) {
        std::cerr << "[DirettaOutput] ❌ Failed to find outputs" << std::endl;
        return false;
    }
    
    if (targets.empty()) {
        std::cerr << "[DirettaOutput] ❌ No Diretta targets found" << std::endl;
        return false;
    }
    
    std::cout << "[DirettaOutput] ✓ Found " << targets.size() << " target(s)" << std::endl;
    
    m_targetAddress = targets.begin()->first;
    
// ⭐ TOUJOURS mesurer le MTU physique
    uint32_t measuredMTU = 1500;
    if (find.measSendMTU(m_targetAddress, measuredMTU)) {
        DEBUG_LOG("[DirettaOutput] 📊 Physical MTU measured: " << measuredMTU << " bytes");
    } else {
        std::cerr << "[DirettaOutput] ⚠️  Failed to measure MTU" << std::endl;
    }
    
    m_mtu = measuredMTU;  // Utiliser le MTU mesuré
    std::cout << "[DirettaOutput] ✓ MTU: " << m_mtu << " bytes" << std::endl;

    return true;
}

bool DirettaOutput::findAndSelectTarget(int targetIndex) {
    m_udp = std::make_unique<ACQUA::UDPV6>();
    m_raw = std::make_unique<ACQUA::UDPV6>();
    
    DIRETTA::Find::Setting findSetting;
    findSetting.Loopback = false;
    findSetting.ProductID = 0;
    
    DIRETTA::Find find(findSetting);
    
    if (!find.open()) {
        std::cerr << "[DirettaOutput] ❌ Failed to open Find" << std::endl;
        return false;
    }
    
    DIRETTA::Find::PortResalts targets;
    if (!find.findOutput(targets)) {
        std::cerr << "[DirettaOutput] ❌ Failed to find outputs" << std::endl;
        return false;
    }
    
    if (targets.empty()) {
        std::cerr << "[DirettaOutput] ❌ No Diretta targets found" << std::endl;
        std::cerr << "[DirettaOutput] Please check:" << std::endl;
        std::cerr << "[DirettaOutput]   1. Diretta Target is powered on" << std::endl;
        std::cerr << "[DirettaOutput]   2. Target is connected to the same network" << std::endl;
        std::cerr << "[DirettaOutput]   3. Network firewall allows Diretta protocol" << std::endl;
        return false;
    }
    
    std::cout << "[DirettaOutput] ✓ Found " << targets.size() << " target(s)" << std::endl;
    std::cout << std::endl;
    
    // If only one target, use it automatically
    if (targets.size() == 1) {
        m_targetAddress = targets.begin()->first;
        DEBUG_LOG("[DirettaOutput] ✓ Auto-selected only available target");
    }
    // Multiple targets: interactive selection
    else {
        std::cout << "══════════════════════════════════════════════════════" << std::endl;
        std::cout << "  📡 Multiple Diretta Targets Detected" << std::endl;
        std::cout << "══════════════════════════════════════════════════════" << std::endl;
        std::cout << std::endl;
        
        // List all targets with index
        int index = 1;
        std::vector<ACQUA::IPAddress> targetList;
        
        for (const auto& target : targets) {
            targetList.push_back(target.first);
            
            std::cout << "[" << index << "] Target #" << index << std::endl;
            std::cout << "    Address: " << target.first.get_str() << std::endl;
            std::cout << std::endl;
            index++;
        }
        
        std::cout << "══════════════════════════════════════════════════════" << std::endl;
        
        int selection = 0;
        
        // If targetIndex provided (from command line), use it
        if (targetIndex >= 0 && targetIndex < static_cast<int>(targetList.size())) {
            selection = targetIndex;
            std::cout << "Using target #" << (selection + 1) << " (from command line)" << std::endl;
        } else {
            // Interactive selection
            std::cout << "\nPlease select a target (1-" << targetList.size() << "): ";
            std::cout.flush();
            
            std::string input;
            std::getline(std::cin, input);
            
            try {
                selection = std::stoi(input) - 1;  // Convert to 0-based index
                
                if (selection < 0 || selection >= static_cast<int>(targetList.size())) {
                    std::cerr << "[DirettaOutput] ❌ Invalid selection: " << (selection + 1) << std::endl;
                    std::cerr << "[DirettaOutput] Please select a number between 1 and " << targetList.size() << std::endl;
                    return false;
                }
            } catch (...) {
                std::cerr << "[DirettaOutput] ❌ Invalid input. Please enter a number." << std::endl;
                return false;
            }
        }
        
        m_targetAddress = targetList[selection];
        std::cout << "\n[DirettaOutput] ✓ Selected target #" << (selection + 1) << ": " 
                  << m_targetAddress.get_str() << std::endl;
        std::cout << std::endl;
    }
    
    // Measure MTU for selected target
    uint32_t measuredMTU = 1500;
    DEBUG_LOG("[DirettaOutput] Measuring network MTU...");
    
    if (find.measSendMTU(m_targetAddress, measuredMTU)) {
        DEBUG_LOG("[DirettaOutput] 📊 Physical MTU measured: " << measuredMTU << " bytes");
        
        if (measuredMTU >= 9000) {
            std::cout << " (Jumbo frames enabled! ✓)";
        } else if (measuredMTU > 1500) {
            std::cout << " (Extended frames)";
        } else {
            std::cout << " (Standard Ethernet)";
        }
        std::cout << std::endl;
    } else {
        std::cerr << "[DirettaOutput] ⚠️  Failed to measure MTU, using default: " 
                  << measuredMTU << " bytes" << std::endl;
    }
    
    m_mtu = measuredMTU;
    DEBUG_LOG("[DirettaOutput] ✓ MTU configured: " << m_mtu << " bytes");
    std::cout << std::endl;

    return true;
}

void DirettaOutput::listAvailableTargets() {
    DIRETTA::Find::Setting findSetting;
    findSetting.Loopback = false;
    findSetting.ProductID = 0;
    
    DIRETTA::Find find(findSetting);
    
    std::cout << "Opening Diretta Find..." << std::endl;
    if (!find.open()) {
        std::cerr << "Failed to initialize Diretta Find" << std::endl;
        std::cerr << "Make sure you run this with sudo/root privileges" << std::endl;
        return;
    }
    
    std::cout << "Scanning network for Diretta targets (waiting 3 seconds)..." << std::endl;
    DIRETTA::Find::PortResalts targets;
    if (!find.findOutput(targets)) {
        std::cerr << "Failed to scan for targets (findOutput returned false)" << std::endl;
        return;
    }
    
    if (targets.empty()) {
        std::cout << "No Diretta targets found on the network." << std::endl;
        return;
    }
    
    std::cout << "\n══════════════════════════════════════════════════════" << std::endl;
    std::cout << "  Available Diretta Targets (" << targets.size() << " found)" << std::endl;
    std::cout << "══════════════════════════════════════════════════════" << std::endl;
    
    int index = 1;
    for (const auto& target : targets) {
        std::cout << "\n[" << index << "] Target #" << index << std::endl;
        std::cout << "    IP Address: " << target.first.get_str() << std::endl;
        
        // Try to measure MTU for this target
        uint32_t mtu = 1500;
        if (find.measSendMTU(target.first, mtu)) {
            std::cout << "    MTU: " << mtu << " bytes";
            if (mtu >= 9000) {
                std::cout << " (Jumbo frames)";
            }
            std::cout << std::endl;
        }
        
        // Friendly device info from Diretta SDK
        const auto& info = target.second;
        if (!info.targetName.empty()) {
            std::cout << "    Device: " << info.targetName << std::endl;
        }
        if (!info.outputName.empty()) {
            std::cout << "    Output: " << info.outputName << std::endl;
        }
        if (!info.config.empty()) {
            std::cout << "    Config: " << info.config << std::endl;
        }
        if (info.productID != 0) {
            std::cout << "    ProductID: 0x" << std::hex << info.productID << std::dec << std::endl;
        }
        if (info.version != 0) {
            std::cout << "    Protocol: v" << info.version << std::endl;
        }
        if (info.multiport) {
            std::cout << "    Multiport: enabled" << std::endl;
        }
        if (info.Sync.isEnable()) {
            std::cout << "    Sync: hash=" << info.Sync.Hash
                      << " total=" << info.Sync.Total
                      << " all=" << info.Sync.All
                      << " self=" << info.Sync.Self << std::endl;
        }
        
        index++;
    }
    
    std::cout << "\n══════════════════════════════════════════════════════" << std::endl;
}

bool DirettaOutput::verifyTargetAvailable() {
    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_SECONDS = 5;
    
    std::cout << "[DirettaOutput] " << std::endl;
    DEBUG_LOG("[DirettaOutput] Scanning for Diretta targets...");
    DEBUG_LOG("[DirettaOutput] This may take several seconds per attempt");
    std::cout << "[DirettaOutput] " << std::endl;
    
    DIRETTA::Find::Setting findSetting;
    findSetting.Loopback = false;
    findSetting.ProductID = 0;
    
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        if (attempt > 1) {
            std::cout << "[DirettaOutput] " << std::endl;
            std::cout << "[DirettaOutput] 🔄 Retry " << attempt << "/" << MAX_RETRIES << "..." << std::endl;
        }
        
        // Create new Find object for each attempt (important!)
        DIRETTA::Find find(findSetting);
        
        DEBUG_LOG("[DirettaOutput] Opening Diretta Find on all network interfaces");
        std::cout.flush();
        
        if (!find.open()) {
            std::cerr << " ❌" << std::endl;
            std::cerr << "[DirettaOutput] Failed to initialize Diretta Find" << std::endl;
            
            if (attempt >= MAX_RETRIES) {
                std::cerr << "[DirettaOutput] " << std::endl;
                std::cerr << "[DirettaOutput] This usually means:" << std::endl;
                std::cerr << "[DirettaOutput]   1. Insufficient permissions (need root/sudo)" << std::endl;
                std::cerr << "[DirettaOutput]   2. Network interface is down" << std::endl;
                std::cerr << "[DirettaOutput]   3. Firewall blocking UDP multicast" << std::endl;
                return false;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SECONDS));
            continue;
        }
        
        std::cout << " ✓" << std::endl;
        DEBUG_LOG("[DirettaOutput] Scanning network";
        std::cout.flush());
        
        // Visual feedback during scan (SDK blocks here)
        auto scanStart = std::chrono::steady_clock::now();
        
        DIRETTA::Find::PortResalts targets;
        bool scanSuccess = find.findOutput(targets);
        
        auto scanEnd = std::chrono::steady_clock::now();
        auto scanDuration = std::chrono::duration_cast<std::chrono::milliseconds>(scanEnd - scanStart);
        
        std::cout << " (took " << scanDuration.count() << "ms)";
        
        if (!scanSuccess) {
            std::cout << " ⚠️" << std::endl;
            std::cerr << "[DirettaOutput] findOutput() returned false" << std::endl;
            
            if (attempt >= MAX_RETRIES) {
                std::cerr << "[DirettaOutput] " << std::endl;
                std::cerr << "[DirettaOutput] This could mean:" << std::endl;
                std::cerr << "[DirettaOutput]   1. No response from any targets (timeout)" << std::endl;
                std::cerr << "[DirettaOutput]   2. Targets are on a different subnet" << std::endl;
                std::cerr << "[DirettaOutput]   3. Network discovery is blocked" << std::endl;
                return false;
            }
            
            std::cout << "[DirettaOutput] No response, retrying..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SECONDS));
            continue;
        }
        
        if (targets.empty()) {
            std::cout << " ⚠️" << std::endl;
            std::cerr << "[DirettaOutput] Scan succeeded but no targets found" << std::endl;
            
            if (attempt >= MAX_RETRIES) {
                std::cerr << "[DirettaOutput] " << std::endl;
                std::cerr << "[DirettaOutput] ❌ No Diretta targets found after " 
                          << MAX_RETRIES << " attempts" << std::endl;
                std::cerr << "[DirettaOutput] Please ensure:" << std::endl;
                std::cerr << "[DirettaOutput]   1. Diretta Target is powered on and running" << std::endl;
                std::cerr << "[DirettaOutput]   2. Target is on the same network/VLAN" << std::endl;
                std::cerr << "[DirettaOutput]   3. Network allows multicast/broadcast" << std::endl;
                return false;
            }
            
            std::cout << "[DirettaOutput] Target may still be initializing, retrying..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SECONDS));
            continue;
        }
        
        // SUCCESS!
        std::cout << " ✓" << std::endl;
        std::cout << "[DirettaOutput] " << std::endl;
        DEBUG_LOG("[DirettaOutput] ✅ Found " << targets.size() << " Diretta target(s)");
        if (attempt > 1) {
            std::cout << " (after " << attempt << " attempt(s))";
        }
        std::cout << std::endl;
        std::cout << "[DirettaOutput] " << std::endl;
        
        // ⭐ CORRECTION: Itérer sur la map avec un itérateur
        int targetNum = 1;
        for (const auto& targetPair : targets) {
            const auto& targetInfo = targetPair.second;
            DEBUG_LOG("[DirettaOutput] Target #" << targetNum << ": " 
          << targetInfo.targetName);
            targetNum++;
        }
        std::cout << "[DirettaOutput] " << std::endl;
        
        // If specific target index is requested, verify it's in range
        if (m_targetIndex >= 0) {
            if (m_targetIndex >= static_cast<int>(targets.size())) {
                std::cerr << "[DirettaOutput] ❌ Target index " << (m_targetIndex + 1) 
                          << " is out of range (only " << targets.size() << " target(s) found)" << std::endl;
                std::cerr << "[DirettaOutput] Please run --list-targets to see available targets" << std::endl;
                return false;
            }
            
            // ⭐ CORRECTION: Trouver la target à l'index demandé
            auto it = targets.begin();
            std::advance(it, m_targetIndex);
            const auto& targetInfo = it->second;
            
            DEBUG_LOG("[DirettaOutput] ✓ Will use target #" << (m_targetIndex + 1) 
          << " (" << targetInfo.targetName << ")" );
            std::cout << "[DirettaOutput] " << std::endl;
        } else if (targets.size() > 1) {
            std::cout << "[DirettaOutput] 💡 Multiple targets detected. Interactive selection will be used." << std::endl;
            std::cout << "[DirettaOutput] " << std::endl;
        }
        
        return true;
    }
    
    // Should never reach here (all retry paths return above)
    return false;
}

bool DirettaOutput::configureDiretta(const AudioFormat& format) {
    DEBUG_LOG("[DirettaOutput] Configuring SyncBuffer...");
    
    if (!m_syncBuffer) {
        DEBUG_LOG("[DirettaOutput] Creating SyncBuffer...");
        m_syncBuffer = std::make_unique<DIRETTA::SyncBuffer>();
    }
  
    // ===== BUILD FORMAT =====
    DIRETTA::FormatID formatID;
    
    // CRITICAL: DSD FORMAT
    if (format.isDSD) {
        DEBUG_LOG("[DirettaOutput] 🎵 DSD NATIVE MODE");
        
        // ✅ Base DSD format - always use FMT_DSD1 and FMT_DSD_SIZ_32
        formatID = DIRETTA::FormatID::FMT_DSD1 | DIRETTA::FormatID::FMT_DSD_SIZ_32;
        
        // ✅ CRITICAL FIX: Detect DSF vs DFF format correctly
formatID |= DIRETTA::FormatID::FMT_DSD_LSB;
formatID |= DIRETTA::FormatID::FMT_DSD_LITTLE;

if (format.dsdFormat == AudioFormat::DSDFormat::DFF) {
    DEBUG_LOG("[DirettaOutput]    Format: DSF (LSB + LITTLE) [converted from DFF]");
} else {
    DEBUG_LOG("[DirettaOutput]    Format: DSF (LSB + LITTLE)");
}
        
        DEBUG_LOG("[DirettaOutput]    Word size: 32-bit container");
        DEBUG_LOG("[DirettaOutput]    DSD Rate: ");
        
        // Determine DSD rate (DSD64, DSD128, etc.)
        // DSD rates are based on 44.1kHz × 64/128/256/512
        if (format.sampleRate == 2822400) {
            std::cout << "DSD64 (2822400 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP64;
            DEBUG_LOG("[DirettaOutput]    ✅ DSD64 configured");
        } else if (format.sampleRate == 5644800) {
            std::cout << "DSD128 (5644800 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP128;
            DEBUG_LOG("[DirettaOutput]    ✅ DSD128 configured");
        } else if (format.sampleRate == 11289600) {
            std::cout << "DSD256 (11289600 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP256;
            DEBUG_LOG("[DirettaOutput]    ✅ DSD256 configured");
                 } else if (format.sampleRate == 22579200) {
            std::cout << "DSD512 (22579200 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP512;
            DEBUG_LOG("[DirettaOutput]    ✅ DSD512 configured");
        } else if (format.sampleRate == 45158400) {
            std::cout << "DSD1024 (45158400 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP1024;
            DEBUG_LOG("[DirettaOutput]    ✅ DSD1024 configured");   
        } else {
            std::cerr << "[DirettaOutput]    ⚠️  Unknown DSD rate: " << format.sampleRate << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP64;
        }
    } else {
        // PCM FORMAT (existing code - unchanged)
        switch (format.bitDepth) {
            case 16: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_16; break;
            case 24: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_24; break;
            case 32: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_32; break;
            default: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_32; break;
        }
        
        uint32_t baseRate;
        uint32_t multiplier;
        
        if (format.sampleRate % 44100 == 0) {
            baseRate = 44100;
            multiplier = format.sampleRate / 44100;
            formatID |= DIRETTA::FormatID::RAT_44100;
        } else if (format.sampleRate % 48000 == 0) {
            baseRate = 48000;
            multiplier = format.sampleRate / 48000;
            formatID |= DIRETTA::FormatID::RAT_48000;
        } else {
            baseRate = 44100;
            multiplier = 1;
            formatID |= DIRETTA::FormatID::RAT_44100;
        }
        
        std::cout << "[DirettaOutput] " << format.sampleRate << "Hz = " 
                  << baseRate << "Hz × " << multiplier << std::endl;
        
        if (multiplier == 1) {
            formatID |= DIRETTA::FormatID::RAT_MP1;
            std::cout << "[DirettaOutput] Multiplier: x1 (RAT_MP1)" << std::endl;
        } else if (multiplier == 2) {
            formatID |= DIRETTA::FormatID::RAT_MP2;
            std::cout << "[DirettaOutput] Multiplier: x2 (RAT_MP2)" << std::endl;
        } else if (multiplier == 4) {
            formatID |= DIRETTA::FormatID::RAT_MP4;
            std::cout << "[DirettaOutput] Multiplier: x4 (RAT_MP4 ONLY)" << std::endl;
        } else if (multiplier == 8) {
            formatID |= DIRETTA::FormatID::RAT_MP8;
            std::cout << "[DirettaOutput] Multiplier: x8 (RAT_MP8 ONLY)" << std::endl;
        } else if (multiplier >= 16) {
            formatID |= DIRETTA::FormatID::RAT_MP16;
            std::cout << "[DirettaOutput] Multiplier: x16 (RAT_MP16 ONLY)" << std::endl;
        }
    }
    
    // Add channels (common to both PCM and DSD)
    switch (format.channels) {
        case 1: formatID |= DIRETTA::FormatID::CHA_1; break;
        case 2: formatID |= DIRETTA::FormatID::CHA_2; break;
        case 4: formatID |= DIRETTA::FormatID::CHA_4; break;
        case 6: formatID |= DIRETTA::FormatID::CHA_6; break;
        case 8: formatID |= DIRETTA::FormatID::CHA_8; break;
        default: formatID |= DIRETTA::FormatID::CHA_2; break;
    }    
     // ===== SYNCBUFFER SETUP (SinHost order) =====
    DEBUG_LOG("[DirettaOutput] 1. Opening...");
    m_syncBuffer->open(
        DIRETTA::Sync::THRED_MODE(1),
        ACQUA::Clock::MilliSeconds(100),
        0, "DirettaRenderer", 0, 0, 0, 0,
        DIRETTA::Sync::MSMODE_AUTO
    );
    
    DEBUG_LOG("[DirettaOutput] 2. Setting sink...");
    m_syncBuffer->setSink(
        m_targetAddress,
        ACQUA::Clock::MilliSeconds(100),
        false,
        m_mtu
    );
    
    // ===== FORMAT NEGOTIATION (Yu Harada recommendation) =====
    // "Diretta only transfers RAW data - no decoding or bit expansion"
    // "Need to comply with Target's requirements"
    // Use checkSinkSupport or getSinkConfigure to verify compatibility
    
    DEBUG_LOG("[DirettaOutput] 3. Format negotiation with Target...");
    
    // Try to configure the requested format
    DEBUG_LOG("[DirettaOutput]    Requesting format: ");
    if (format.isDSD) {
        std::cout << "DSD" << (format.sampleRate / 44100) << " (" << format.sampleRate << "Hz)";
    } else {
        std::cout << "PCM " << format.bitDepth << "-bit " << format.sampleRate << "Hz";
    }
    std::cout << " " << format.channels << "ch" << std::endl;
    
    m_syncBuffer->setSinkConfigure(formatID);
    
    // Verify the configured format with Target
    DIRETTA::FormatID configuredFormat = m_syncBuffer->getSinkConfigure();
    
    if (configuredFormat == formatID) {
        DEBUG_LOG("[DirettaOutput]    ✅ Target accepted requested format");
    } else {
        std::cout << "[DirettaOutput]    ⚠️  Target modified format!" << std::endl;
        std::cout << "[DirettaOutput]       Requested: 0x" << std::hex << static_cast<uint32_t>(formatID) << std::dec << std::endl;
        std::cout << "[DirettaOutput]       Accepted:  0x" << std::hex << static_cast<uint32_t>(configuredFormat) << std::dec << std::endl;
        
        // Check if it's a bit depth issue (common for SPDIF targets)
        if (!format.isDSD) {
            // Extract bit depth from configured format  
            if ((configuredFormat & DIRETTA::FormatID::FMT_PCM_SIGNED_16) == DIRETTA::FormatID::FMT_PCM_SIGNED_16) {
                std::cout << "[DirettaOutput]       Target forced 16-bit (SPDIF limitation)" << std::endl;
                m_currentFormat.bitDepth = 16;  // Update our format tracking
            } else if ((configuredFormat & DIRETTA::FormatID::FMT_PCM_SIGNED_24) == DIRETTA::FormatID::FMT_PCM_SIGNED_24) {
                std::cout << "[DirettaOutput]       Target forced 24-bit" << std::endl;
                m_currentFormat.bitDepth = 24;  // Update our format tracking
            } else if ((configuredFormat & DIRETTA::FormatID::FMT_PCM_SIGNED_32) == DIRETTA::FormatID::FMT_PCM_SIGNED_32) {
                std::cout << "[DirettaOutput]       Target forced 32-bit" << std::endl;
                m_currentFormat.bitDepth = 32;  // Update our format tracking
            }
        }
        
        // Use the format accepted by Target
        formatID = configuredFormat;
    }
    
    DEBUG_LOG("[DirettaOutput] 3. Setting format...");
    // Format already configured during negotiation above
    
// 4. Configuring transfer...
DEBUG_LOG("[DirettaOutput] 4. Configuring transfer...");

// Setup buffer (network config will be optimized below)
const int fs1sec = format.sampleRate;
m_syncBuffer->setupBuffer(fs1sec * m_bufferSeconds, 4, false);
    
    // ⭐ v1.2.0 Stable: Optimize network config for format
    optimizeNetworkConfig(format);
    
    DEBUG_LOG("[DirettaOutput] 6. Connecting...");
    m_syncBuffer->connect(0, 0);
    // m_syncBuffer->connectWait();

// Wait with timeout
     int timeoutMs = 10000;
   int waitedMs = 0;
    while (!m_syncBuffer->is_connect() && waitedMs < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waitedMs += 100;
    }



    
    if (!m_syncBuffer->is_connect()) {
        std::cerr << "[DirettaOutput] ❌ Connection failed" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[DirettaOutput] ✓ Connected: " << format.sampleRate 
              << "Hz/" << format.bitDepth << "bit/" << format.channels << "ch");
    
    return true;
}

// ═══════════════════════════════════════════════════════════════
// ⭐ v1.2.0 Stable: Network optimization by format
// ═══════════════════════════════════════════════════════════════

void DirettaOutput::optimizeNetworkConfig(const AudioFormat& format) {
    if (!m_syncBuffer) {
        return;
    }
    
    DEBUG_LOG("[DirettaOutput] 🔧 Configuring network: VarMax (maximum throughput)");
    
    // ⭐ v1.2.0: Use VarMax for all formats (best performance with jumbo frames)
    ACQUA::Clock cycle(m_cycleTime);
    m_syncBuffer->configTransferVarMax(cycle);
    
    DEBUG_LOG("[DirettaOutput] ✓ Network configured: VarMax mode");
}

bool DirettaOutput::seek(int64_t samplePosition) {
    DEBUG_LOG("[DirettaOutput] 🔍 Seeking to sample " << samplePosition);

    if (!m_syncBuffer) {
        std::cerr << "[DirettaOutput] ⚠️  No SyncBuffer available for seek" << std::endl;
        return false;
     }

    bool wasPlaying = m_playing;
    
    // Pause if playing
    if (wasPlaying && m_syncBuffer) {
        m_syncBuffer->stop();
    }
    
    // Perform seek
    m_syncBuffer->seek(samplePosition);
    m_totalSamplesSent = samplePosition;
    
    // Resume if was playing
    if (wasPlaying && m_syncBuffer) {
        m_syncBuffer->play();
    }
   
    DEBUG_LOG("[DirettaOutput] ✓ Seeked to sample " << samplePosition);
    return true;
   }
// ═══════════════════════════════════════════════════════════════
// ⭐ v1.2.0: Gapless Pro - Implementation
// ═══════════════════════════════════════════════════════════════

bool DirettaOutput::prepareNextTrack(const uint8_t* data, 
                                     size_t numSamples,
                                     const AudioFormat& format) {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    
    if (!m_connected || !m_syncBuffer) {
        DEBUG_LOG("[DirettaOutput] ❌ Cannot prepare next track: not connected");
        return false;
    }
    
    if (!m_gaplessEnabled) {
        DEBUG_LOG("[DirettaOutput] ⚠️  Gapless disabled, skipping preparation");
        return false;
    }
    
    DEBUG_LOG("[DirettaOutput] 🎵 Preparing next track for gapless...");
    DEBUG_LOG("[DirettaOutput]    Format: " << format.sampleRate << "Hz/"
              << format.bitDepth << "bit/" << format.channels << "ch");
    
    try {
        // Check for format change
        bool formatChange = (format.sampleRate != m_currentFormat.sampleRate ||
                            format.bitDepth != m_currentFormat.bitDepth ||
                            format.channels != m_currentFormat.channels ||
                            format.isDSD != m_currentFormat.isDSD);
        
        if (formatChange) {
            DEBUG_LOG("[DirettaOutput] ⚠️  Format change detected!");
            DEBUG_LOG("[DirettaOutput]    Current: " << m_currentFormat.sampleRate 
                      << "Hz/" << m_currentFormat.bitDepth << "bit");
            DEBUG_LOG("[DirettaOutput]    Next: " << format.sampleRate 
                      << "Hz/" << format.bitDepth << "bit");
            DEBUG_LOG("[DirettaOutput]    → Gapless will trigger format change");
        }
        
        // Get stream for writing
        bool canWrite = false;
        m_syncBuffer->writeStreamStart(canWrite);  // Check if can write
        
        if (!canWrite) {
            DEBUG_LOG("[DirettaOutput] ⚠️  Buffer full, cannot prepare next track yet");
            return false;
        }
        
        DEBUG_LOG("[DirettaOutput] ✓ Got write stream, preparing " << numSamples << " samples");
        
        // Create audio stream
        DIRETTA::Stream audioStream = createStreamFromAudio(data, numSamples, format);
        
        // Add to SDK gapless queue
        m_syncBuffer->addStream(audioStream);
        
        // Mark as prepared
        m_nextTrackPrepared = true;
        m_nextTrackFormat = format;
        
        DEBUG_LOG("[DirettaOutput] ✅ Next track prepared for gapless transition");
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[DirettaOutput] ❌ Exception preparing next track: " 
                  << e.what() << std::endl;
        m_nextTrackPrepared = false;
        return false;
    }
}

bool DirettaOutput::isNextTrackReady() const {
    if (!m_syncBuffer || !m_gaplessEnabled) {
        return false;
    }
    
    // Check with SDK if stream ready
    bool ready = m_syncBuffer->checkStreamStart();
    
    if (ready && !m_nextTrackPrepared) {
        DEBUG_LOG("[DirettaOutput] 💡 SDK reports stream ready but not marked locally");
    }
    
    return ready && m_nextTrackPrepared;
}

// ⭐ v1.2.0 Stable: Buffer status check
bool DirettaOutput::isBufferEmpty() const {
    if (!m_syncBuffer || !m_connected) {
        return true;  // Considérer vide si pas connecté
    }
    
    return m_syncBuffer->buffer_empty();
}

void DirettaOutput::cancelNextTrack() {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    
    if (m_nextTrackPrepared) {
        DEBUG_LOG("[DirettaOutput] 🚫 Cancelling prepared next track");
        m_nextTrackPrepared = false;
        // Note: SDK automatically cleans up unused streams
    }
}

void DirettaOutput::setGaplessMode(bool enabled) {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    
    if (m_gaplessEnabled != enabled) {
        DEBUG_LOG("[DirettaOutput] " << (enabled ? "🎵 Enabling" : "🚫 Disabling") 
                  << " gapless mode");
        m_gaplessEnabled = enabled;
        
        if (!enabled) {
            cancelNextTrack();
        }
    }
}

DIRETTA::Stream DirettaOutput::createStreamFromAudio(const uint8_t* data, 
                                                      size_t numSamples,
                                                      const AudioFormat& format) {
    // Calculate data size
    size_t dataSize;
    
    if (format.isDSD) {
        // DSD: numSamples in bits, convert to bytes
        // Using 32-bit containers (FMT_DSD_SIZ_32)
        dataSize = numSamples * format.channels * 4;  // 4 bytes per 32-bit container
    } else {
        // PCM: numSamples in frames
        uint32_t bytesPerSample = (format.bitDepth / 8) * format.channels;
        dataSize = numSamples * bytesPerSample;
    }
    
    DEBUG_LOG("[DirettaOutput::createStreamFromAudio] Creating stream: " 
              << dataSize << " bytes for " << numSamples << " samples");
    
    // Create stream
    DIRETTA::Stream stream;
    stream.resize(dataSize);
    
    // Copy data
    if (!format.isDSD && format.bitDepth == 24) {
        // S32 → S24 conversion if needed
        const int32_t* src = reinterpret_cast<const int32_t*>(data);
        uint8_t* dst = stream.get();
        
        for (size_t i = 0; i < numSamples * format.channels; i++) {
            int32_t sample = src[i];
            
            // S32 → S24 (keep the 24 most significant bits)
            dst[i * 3 + 0] = (sample >> 8) & 0xFF;   // LSB
            dst[i * 3 + 1] = (sample >> 16) & 0xFF;  // Mid
            dst[i * 3 + 2] = (sample >> 24) & 0xFF;  // MSB
        }
        
        DEBUG_LOG("[DirettaOutput::createStreamFromAudio] ✓ Converted S32→S24");
    } else {
        // Direct copy for other formats
        memcpy(stream.get(), data, dataSize);
        DEBUG_LOG("[DirettaOutput::createStreamFromAudio] ✓ Direct copy");
    }
    
    return stream;
}

// ═══════════════════════════════════════════════════════════════
// End of v1.2.0 Gapless Pro implementation
// ═══════════════════════════════════════════════════════════════
