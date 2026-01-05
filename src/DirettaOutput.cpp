/**
 * @file DirettaOutput.cpp
 * @brief Diretta Output implementation
 */

#include "DirettaOutput.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
// ════════════════════════════════════════════════════════════════
// ⭐ v1.2.3 : Bit reversal lookup table for DSD MSB<->LSB conversion
// ════════════════════════════════════════════════════════════════

static const uint8_t bitReverseTable[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};
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
    
    // ⭐ v1.4.0: SDK Diretta Gapless Pro handles ALL buffering intelligently
    // User controls buffer via --buffer parameter, SDK adapts automatically
    // Tests show even --buffer 0 works perfectly - SDK manages everything!
    
    float effectiveBuffer = bufferSeconds;  // Respect user choice by default
    
    if (format.isDSD) {
        // DSD: Minimal buffer optimal (SDK manages sync perfectly)
        // Even 0.02s works great, but we cap at 0.05s for safety
        effectiveBuffer = std::min(bufferSeconds, 0.05f);
        std::cout << "[DirettaOutput] 🎵 DSD mode: minimal buffer " 
                  << effectiveBuffer << "s (SDK-managed)" << std::endl;
        
    } else {
        // PCM (compressed or uncompressed): SDK handles efficiently
        // No forced minimums - user has full control
        std::cout << "[DirettaOutput] 🎵 PCM mode: user buffer " 
                  << effectiveBuffer << "s (SDK-managed)" << std::endl;
        
        // Optional warning if VERY small buffer (might cause issues on slow networks)
        if (effectiveBuffer < 0.1f && effectiveBuffer > 0.0f) {
            std::cout << "[DirettaOutput] ⚠️  Small buffer (" << effectiveBuffer 
                      << "s) - may cause underruns on slow networks" << std::endl;
            std::cout << "[DirettaOutput]    💡 Tip: Use --buffer 0.5 or higher for network streaming" << std::endl;
        }
    }
    
    m_bufferSeconds = effectiveBuffer;
    std::cout << "[DirettaOutput] → Buffer: " << m_bufferSeconds << "s" << std::endl;
    
    // Find Diretta target
    DEBUG_LOG("[DirettaOutput] Finding Diretta target...");
    if (!findAndSelectTarget(m_targetIndex)) {  // Use configured target index
        std::cerr << "[DirettaOutput] ❌ Failed to find or select Diretta target" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[DirettaOutput] ✓ Target found and selected");
    
    // Configure Diretta with the format
    if (!configureDiretta(format)) {
        std::cerr << "[DirettaOutput] ❌ Failed to configure Diretta" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[DirettaOutput] ✓ Diretta configured");
    
    // Network optimization
    optimizeNetworkConfig(format);
    
    std::cout << "[DirettaOutput] ✅ Connection established" << std::endl;
    std::cout << "[DirettaOutput]    Format: ";
    if (format.isDSD) {
        std::cout << "DSD" << (format.sampleRate / 44100) << " (" << format.sampleRate << "Hz)";
    } else {
        std::cout << "PCM " << format.bitDepth << "-bit " << format.sampleRate << "Hz";
    }
    std::cout << " " << format.channels << "ch" << std::endl;
    std::cout << "[DirettaOutput]    Buffer: " << m_bufferSeconds << "s (SDK-managed)" << std::endl;
    std::cout << "[DirettaOutput]    MTU: " << m_mtu << " bytes" << std::endl;
    
    m_connected = true;
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
    
    std::cout << "[DirettaOutput] ⚠️  Format change - COMPLETE CLOSE/REOPEN REQUIRED" << std::endl;
    std::cout << "[DirettaOutput]    (DAC hardware needs time to reinitialize)" << std::endl;
    
    bool wasPlaying = m_playing;
    
    // ⭐ STEP 1: COMPLETE CLOSE
    std::cout << "[DirettaOutput] 1. Closing connection completely..." << std::endl;
    close();  // Complete close instead of just pre_disconnect
    
    // ⭐ STEP 2: WAIT FOR DAC HARDWARE REINITIALIZATION
    // High-end DACs like Holo Audio Spring 3 need time to:
    // - Reset PLLs
    // - Reconfigure clocking
    // - Lock onto new format
    std::cout << "[DirettaOutput] 2. Waiting for DAC hardware reinitialization (600ms)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::cout << "[DirettaOutput]    ✓ DAC ready for new format" << std::endl;
    
    // ⭐ STEP 3: REOPEN WITH NEW FORMAT
    std::cout << "[DirettaOutput] 3. Reopening with new format..." << std::endl;
    if (!open(newFormat, m_bufferSeconds)) {
        std::cerr << "[DirettaOutput] ❌ Failed to reopen with new format" << std::endl;
        return false;
    }
    
    // ⭐ STEP 4: RESTART PLAYBACK IF NEEDED
    if (wasPlaying) {
        std::cout << "[DirettaOutput] 4. Restarting playback..." << std::endl;
        if (!play()) {
            std::cerr << "[DirettaOutput] ❌ Failed to restart playback" << std::endl;
            return false;
        }
        
        // Additional wait for DAC lock
        std::cout << "[DirettaOutput]    Waiting for DAC lock (200ms)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    std::cout << "[DirettaOutput] ✅ Format changed successfully" << std::endl;
    std::cout << "[DirettaOutput]    New format: " << newFormat.sampleRate << "Hz/" 
              << newFormat.bitDepth << "bit/" << newFormat.channels << "ch" << std::endl;
    
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
        
        // ⭐ v1.2.3 : Configuration intelligente basée sur format source
        
        // Détecter format source (DSF = LSB, DFF = MSB)
        bool sourceIsLSB = (format.dsdSourceFormat == DSDSourceFormat::DSF);
        bool sourceIsMSB = (format.dsdSourceFormat == DSDSourceFormat::DFF);
        
        // Pour l'instant, on configure toujours en LSB+LITTLE (compatible avec la majorité des DACs)
        // et on fait le bit reversal si nécessaire dans sendAudio()
        formatID |= DIRETTA::FormatID::FMT_DSD_LSB;
        formatID |= DIRETTA::FormatID::FMT_DSD_LITTLE;
        
        // Calculer si on a besoin de bit reversal
        // Target est LSB, donc reverse si source est MSB (DFF)
        m_needDsdBitReversal = sourceIsMSB;
        
        DEBUG_LOG("[DirettaOutput] 📀 DSD Configuration:");
        DEBUG_LOG("[DirettaOutput]    Source format: " 
                  << (sourceIsLSB ? "DSF (LSB)" : sourceIsMSB ? "DFF (MSB)" : "Unknown"));
        DEBUG_LOG("[DirettaOutput]    Target format: LSB + LITTLE");
        DEBUG_LOG("[DirettaOutput]    Bit reversal needed: " << (m_needDsdBitReversal ? "YES" : "NO"));
        DEBUG_LOG("[DirettaOutput]    Word size: 32-bit container");
        
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

// ════════════════════════════════════════════════════════════════
// ⭐ v1.2.1 : Silence SEULEMENT lors de VRAIS changements de format
// ════════════════════════════════════════════════════════════════

// Variable statique pour mémoriser le dernier format configuré
static DIRETTA::FormatID lastConfiguredFormat = static_cast<DIRETTA::FormatID>(0);

// Vérifier si c'est un VRAI changement de format
bool isFirstConfiguration = (lastConfiguredFormat == static_cast<DIRETTA::FormatID>(0));
bool isFormatChange = !isFirstConfiguration && (lastConfiguredFormat != formatID);

if (m_syncBuffer && isFormatChange) {
    // ⭐ VRAI changement de format détecté !
    
    // Détecter si on était en DSD en regardant le format PRÉCÉDENT
    DIRETTA::FormatID previousFormat = lastConfiguredFormat;
    bool wasDSD = (static_cast<uint32_t>(previousFormat) & 
                   static_cast<uint32_t>(DIRETTA::FormatID::FMT_DSD1)) != 0;
    
    // Calculer nombre de silence buffers nécessaires
    int silenceCount = wasDSD ? 100 : 30;
    uint8_t silenceValue = wasDSD ? 0x69 : 0x00;
    
    DEBUG_LOG("[DirettaOutput] 🔇 Format change detected, sending " 
              << silenceCount << " silence buffers...");
    DEBUG_LOG("[DirettaOutput]   Previous: 0x" << std::hex 
              << static_cast<uint32_t>(previousFormat) << std::dec);
    DEBUG_LOG("[DirettaOutput]   New: 0x" << std::hex 
              << static_cast<uint32_t>(formatID) << std::dec);
    
    // Envoyer silence buffers
    for (int i = 0; i < silenceCount; i++) {
        DIRETTA::Stream stream;
        stream.resize(8192);
        std::memset(stream.get(), silenceValue, 8192);
        m_syncBuffer->setStream(stream);
        
        // Petit délai tous les 10 buffers
        if (i > 0 && i % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    
    // Attendre stabilisation DAC
    std::this_thread::sleep_for(std::chrono::milliseconds(wasDSD ? 100 : 50));
    
    DEBUG_LOG("[DirettaOutput] ✅ Silence buffers sent, DAC stabilized");
    
} else if (isFirstConfiguration) {
    DEBUG_LOG("[DirettaOutput] ℹ️  First configuration, no silence needed");
} else {
    DEBUG_LOG("[DirettaOutput] ℹ️  Same format, no silence needed");
}
// ════════════════════════════════════════════════════════════════
// Configurer le nouveau format (ton code existant)
// ════════════════════════════════════════════════════════════════

m_syncBuffer->setSinkConfigure(formatID);

// Mémoriser le format configuré pour la prochaine fois
lastConfiguredFormat = formatID;

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
    
    // ⭐ DSD SEEK CONVERSION
    int64_t seekPosition = samplePosition;
    
    if (m_currentFormat.isDSD) {
        // DSD: Convert to bytes (32-bit containers)
        // Same calculation as in createStreamFromAudio()
        seekPosition = samplePosition * m_currentFormat.channels * 4;
        
        std::cout << "[DirettaOutput] DSD seek conversion:" << std::endl;
        std::cout << "   Input position (bits): " << samplePosition << std::endl;
        std::cout << "   Output position (bytes): " << seekPosition << std::endl;
        std::cout << "   Format: DSD" << (m_currentFormat.sampleRate / 44100) 
                  << " (" << m_currentFormat.sampleRate << " Hz)" << std::endl;
    }
    
    // Perform seek
    DEBUG_LOG("[DirettaOutput] → Calling SDK seek(" << seekPosition << ")");
    m_syncBuffer->seek(seekPosition);
    m_totalSamplesSent = samplePosition;  // Keep in original units
    
    // Resume if was playing
    if (wasPlaying && m_syncBuffer) {
        m_syncBuffer->play();
    }
   
    DEBUG_LOG("[DirettaOutput] ✓ Seeked to position " << seekPosition);
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
} else if (m_currentFormat.isDSD && m_needDsdBitReversal) {
        // ⭐ v1.2.3 : DSD with bit reversal (DFF → LSB conversion)
        uint8_t* output = stream.get();
        for (size_t i = 0; i < dataSize; i++) {
            output[i] = bitReverseTable[data[i]];
        }
        
        static int dsdRevCount = 0;
        if (dsdRevCount++ < 3) {
            DEBUG_LOG("[sendAudio] DSD bit reversal: " << dataSize << " bytes");
        }
    } else {
        // For other formats (16-bit, 32-bit, DSD without reversal): direct copy
        memcpy(stream.get(), data, dataSize);
    }
    return stream;
}

// ═══════════════════════════════════════════════════════════════
// End of v1.2.0 Gapless Pro implementation
// ═══════════════════════════════════════════════════════════════
