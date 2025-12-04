/**
 * @file DirettaOutput.cpp
 * @brief Diretta Output implementation
 */

#include "DirettaOutput.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

DirettaOutput::DirettaOutput()
    : m_mtu(1500)
    , m_mtuManuallySet(false)
    , m_bufferSeconds(2)
    , m_connected(false)
    , m_playing(false)
{
    std::cout << "[DirettaOutput] Created" << std::endl;
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
    m_mtuManuallySet = true;
    
    std::cout << "[DirettaOutput] ✓ MTU configured: " << m_mtu << " bytes";
    
    if (mtu > 1500) {
        std::cout << " (jumbo frames)";
    }
    
    std::cout << std::endl;
}



bool DirettaOutput::open(const AudioFormat& format, int bufferSeconds) {
    std::cout << "[DirettaOutput] Opening: " 
              << format.sampleRate << "Hz/" 
              << format.bitDepth << "bit/" 
              << format.channels << "ch" << std::endl;
    
    m_currentFormat = format;
    m_totalSamplesSent = 0;
    std::cout << "[DirettaOutput] ⭐ m_totalSamplesSent RESET to 0" << std::endl; 
    
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
        effectiveBuffer = std::min(static_cast<float>(bufferSeconds), 0.8f);
        std::cout << "[DirettaOutput] 🎵 DSD: raw bitstream path" << std::endl;
        
    } else if (!format.isCompressed) {
        // WAV/AIFF: Uncompressed PCM, minimal overhead (just format conversion)
        effectiveBuffer = std::min(static_cast<float>(bufferSeconds), 1.0f);
        std::cout << "[DirettaOutput] ✓ Uncompressed PCM (WAV/AIFF): low-latency path" << std::endl;
        std::cout << "[DirettaOutput]   Buffer: " << effectiveBuffer 
                  << "s (similar to DSD!)" << std::endl;
        
    } else {
        // FLAC/ALAC/etc: Compressed, needs decoding buffer
        effectiveBuffer = std::max(static_cast<float>(bufferSeconds), 2.0f);
        std::cout << "[DirettaOutput] ℹ️  Compressed PCM (FLAC/ALAC): decoding required" << std::endl;
        
        if (bufferSeconds < 2) {
            std::cout << "[DirettaOutput]   Using 2s minimum for decode stability" << std::endl;
        }
    }
    
    m_bufferSeconds = effectiveBuffer;
    std::cout << "[DirettaOutput] → Effective buffer: " << m_bufferSeconds << "s" << std::endl;
    
    // Find Diretta target
    std::cout << "[DirettaOutput] Finding Diretta target..." << std::endl;
    if (!findTarget()) {
        std::cerr << "[DirettaOutput] ❌ Failed to find Diretta target" << std::endl;
        return false;
    }
    
    std::cout << "[DirettaOutput] ✓ Found Diretta target" << std::endl;
    
    // Configure and connect
    if (!configureDiretta(format)) {
        std::cerr << "[DirettaOutput] ❌ Failed to configure Diretta" << std::endl;
        return false;
    }
    
    m_connected = true;
    std::cout << "[DirettaOutput] ✓ Connected and configured" << std::endl;
    
    return true;
}

void DirettaOutput::close() {
    if (!m_connected) {
        return;
    }
    
    std::cout << "[DirettaOutput] Closing..." << std::endl;
    
    // ⚠️  Don't call stop() here if already stopped - avoids double pre_disconnect
    // The onStop callback already called stop(true) for immediate response
    
    // Disconnect SyncBuffer
    if (m_syncBuffer) {
        std::cout << "[DirettaOutput] Disconnecting SyncBuffer..." << std::endl;
        // Only disconnect if still playing (avoid double disconnect)
        if (m_playing) {
            std::cout << "[DirettaOutput] ⚠️  Still playing, forcing immediate disconnect" << std::endl;
            m_syncBuffer->pre_disconnect(true); // Immediate
        }
        m_syncBuffer.reset();
    }
    
    m_udp.reset();
    m_raw.reset();
    m_connected = false;
    m_playing = false;  // Ensure clean state
    
    std::cout << "[DirettaOutput] ✓ Closed" << std::endl;
}

bool DirettaOutput::play() {
    if (!m_connected) {
        std::cerr << "[DirettaOutput] ❌ Not connected" << std::endl;
        return false;
    }
    
    if (m_playing) {
        return true; // Already playing
    }
    
    std::cout << "[DirettaOutput] Starting playback..." << std::endl;
    
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
        std::cout << "[DirettaOutput] ⚠️  stop() called but not playing" << std::endl;
        return;
    }
    
    std::cout << "[DirettaOutput] 🛑 Stopping (immediate=" << immediate << ")..." << std::endl;
    
    if (m_syncBuffer) {
        std::cout << "[DirettaOutput] Calling pre_disconnect(" << immediate << ")..." << std::endl;
        auto start = std::chrono::steady_clock::now();
        
        m_syncBuffer->pre_disconnect(immediate);
        
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "[DirettaOutput] ✓ pre_disconnect completed in " << duration.count() << "ms" << std::endl;
        std::cout << "[DirettaOutput] Calling seek_front() to reset buffer..." << std::endl;
        m_syncBuffer->seek_front();
        std::cout << "[DirettaOutput] ✓ Buffer reset to front" << std::endl;
    } else {
        std::cout << "[DirettaOutput] ⭐ m_totalSamplesSent RESET to 0" << std::endl;  // ⭐ Log pour vérifier
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
    
    std::cout << "[DirettaOutput] ⏸️  Pausing..." << std::endl;
    
    // Sauvegarder la position actuelle
    m_pausedPosition = m_totalSamplesSent;
    
    // Arrêter la lecture
    if (m_syncBuffer) {
        m_syncBuffer->stop();
    }
    
    m_isPaused = true;
    m_playing = false;
    
    std::cout << "[DirettaOutput] ✓ Paused at sample " << m_pausedPosition << std::endl;
}

void DirettaOutput::resume() {
    if (!m_isPaused) {
        return;
    }
    
    std::cout << "[DirettaOutput] ▶️  Resuming from sample " << m_pausedPosition << "..." << std::endl;
    
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
    
    std::cout << "[DirettaOutput] ⚠️  Format change during playback" << std::endl;
    std::cout << "[DirettaOutput] Draining buffer..." << std::endl;
    
    if (m_syncBuffer) {
        m_syncBuffer->pre_disconnect(false); // Drain

    
        // ⭐ ATTENDRE que le buffer soit vide
        std::cout << "[DirettaOutput] Waiting for buffer drain..." << std::endl;
        int timeout_ms = 5000;
        int waited_ms = 0;
        while (m_syncBuffer->is_connect() && waited_ms < timeout_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            waited_ms += 50;
        }
        
        if (waited_ms >= timeout_ms) {
            std::cerr << "[DirettaOutput] ⚠️  Drain timeout!" << std::endl;
        } else {
            std::cout << "[DirettaOutput] ✓ Buffer drained in " << waited_ms << "ms" << std::endl;
        }
    }



    if (!configureDiretta(newFormat)) {
        std::cerr << "[DirettaOutput] ❌ Failed to reconfigure" << std::endl;
        return false;
    }
    
    if (m_playing) {
        m_syncBuffer->play();
    }
    
    m_currentFormat = newFormat;
    std::cout << "[DirettaOutput] ✓ Format changed successfully" << std::endl;
    
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
            std::cout << "[DirettaOutput::sendAudio] DSD: " << numSamples 
                      << " samples → " << dataSize << " bytes" << std::endl;
        }
    } else {
        // PCM: standard calculation
        uint32_t bytesPerSample = (m_currentFormat.bitDepth / 8) * m_currentFormat.channels;
        dataSize = numSamples * bytesPerSample;
    }
    
    DIRETTA::Stream stream;
    stream.resize(dataSize);
    memcpy(stream.get(), data, dataSize);
    
    m_syncBuffer->setStream(stream);
    m_totalSamplesSent += numSamples;

    static int callCount = 0;
    if (++callCount % 500 == 0) {
        double seconds = static_cast<double>(m_totalSamplesSent) / m_currentFormat.sampleRate;
        std::cout << "[DirettaOutput] Position: " << seconds << "s (" 
                  << m_totalSamplesSent << " samples)" << std::endl;
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
        std::cout << "[DirettaOutput] 📊 Physical MTU measured: " << measuredMTU << " bytes" << std::endl;
    } else {
        std::cerr << "[DirettaOutput] ⚠️  Failed to measure MTU" << std::endl;
    }
    
    m_mtu = measuredMTU;  // Utiliser le MTU mesuré
    std::cout << "[DirettaOutput] ✓ MTU: " << m_mtu << " bytes" << std::endl;

    return true;
}

bool DirettaOutput::configureDiretta(const AudioFormat& format) {
    std::cout << "[DirettaOutput] Configuring SyncBuffer..." << std::endl;
    
    if (!m_syncBuffer) {
        std::cout << "[DirettaOutput] Creating SyncBuffer..." << std::endl;
        m_syncBuffer = std::make_unique<DIRETTA::SyncBuffer>();
    }
  
    // ===== BUILD FORMAT =====
    DIRETTA::FormatID formatID;
    
    // CRITICAL: DSD FORMAT
    if (format.isDSD) {
        std::cout << "[DirettaOutput] 🎵 DSD NATIVE MODE" << std::endl;
        
        // ✅ WORKING CONFIGURATION: LSB + LITTLE (native DSF format)
        // Base DSD format - always use FMT_DSD1 and FMT_DSD_SIZ_32
        formatID = DIRETTA::FormatID::FMT_DSD1 | DIRETTA::FormatID::FMT_DSD_SIZ_32;
        formatID |= DIRETTA::FormatID::FMT_DSD_LSB;     // ✅ LSB for DSF files
        formatID |= DIRETTA::FormatID::FMT_DSD_LITTLE;  // ✅ Little Endian
        
        std::cout << "[DirettaOutput]    Format: DSF (LSB + LITTLE)" << std::endl;
        std::cout << "[DirettaOutput]    Word size: 32-bit" << std::endl;
        std::cout << "[DirettaOutput]    DSD Rate: ";
        
        // Determine DSD rate (DSD64, DSD128, etc.)
        // DSD rates are based on 44.1kHz × 64/128/256/512
        if (format.sampleRate == 2822400) {
            std::cout << "DSD64 (2822400 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP64;
            std::cout << "[DirettaOutput]    ✅ DSD64 configured" << std::endl;
        } else if (format.sampleRate == 5644800) {
            std::cout << "DSD128 (5644800 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP128;
            std::cout << "[DirettaOutput]    ✅ DSD128 configured" << std::endl;
        } else if (format.sampleRate == 11289600) {
            std::cout << "DSD256 (11289600 Hz)" << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP256;
            std::cout << "[DirettaOutput]    ✅ DSD256 configured" << std::endl;
        } else {
            std::cerr << "[DirettaOutput]    ⚠️  Unknown DSD rate: " << format.sampleRate << std::endl;
            formatID |= DIRETTA::FormatID::RAT_44100 | DIRETTA::FormatID::RAT_MP64;
        }
    } else {
        // PCM FORMAT (existing code)
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
    std::cout << "[DirettaOutput] 1. Opening..." << std::endl;
    m_syncBuffer->open(
        DIRETTA::Sync::THRED_MODE(5),
        ACQUA::Clock::MilliSeconds(100),
        0, "DirettaRenderer", 0, 0, 0, 0,
        DIRETTA::Sync::MSMODE_AUTO
    );
    
    std::cout << "[DirettaOutput] 2. Setting sink..." << std::endl;
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
    
    std::cout << "[DirettaOutput] 3. Format negotiation with Target..." << std::endl;
    
    // Try to configure the requested format
    std::cout << "[DirettaOutput]    Requesting format: ";
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
        std::cout << "[DirettaOutput]    ✅ Target accepted requested format" << std::endl;
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
    
    std::cout << "[DirettaOutput] 3. Setting format..." << std::endl;
    // Format already configured during negotiation above
    
// 4. Configuring transfer...
std::cout << "[DirettaOutput] 4. Configuring transfer..." << std::endl;

// ⭐ Détecter les formats bas débit qui nécessitent des paquets plus petits
bool isLowBitrate = (format.bitDepth <= 16 && format.sampleRate <= 48000 && !format.isDSD);

if (isLowBitrate) {
    // Pour 16bit/44.1kHz, 16bit/48kHz : paquets plus petits pour éviter les drops
    std::cout << "[DirettaOutput] ⚠️  Low bitrate format detected (" 
              << format.bitDepth << "bit/" << format.sampleRate << "Hz)" << std::endl;
    std::cout << "[DirettaOutput] Using configTransferAuto (smaller packets)" << std::endl;
    
    m_syncBuffer->configTransferAuto(
        ACQUA::Clock::MicroSeconds(200),   // limitCycle
        ACQUA::Clock::MicroSeconds(333),   // minCycle
        ACQUA::Clock::MicroSeconds(10000)  // maxCycle
    );
    std::cout << "[DirettaOutput] ✓ configTransferAuto (packets ~1-3k)" << std::endl;
} else {
    // Pour Hi-Res (24bit, 88.2k+, DSD, etc.) : jumbo frames pour performance max
    std::cout << "[DirettaOutput] ✓ Hi-Res format (" 
              << format.bitDepth << "bit/" << format.sampleRate << "Hz)" << std::endl;
    std::cout << "[DirettaOutput] Using configTransferVarMax (jumbo frames)" << std::endl;
    
    m_syncBuffer->configTransferVarMax(
        ACQUA::Clock::MicroSeconds(200)   // limitCycle
    );
    std::cout << "[DirettaOutput] ✓ configTransferVarMax (Packet Full mode, ~16k)" << std::endl;
}
std::cout << "[DirettaOutput] DEBUG: MTU passed to setSink: " << m_mtu << std::endl;
std::cout << "[DirettaOutput] DEBUG: Check packet size in tcpdump..." << std::endl;

std::cout << "[DirettaOutput] configTransferAuto: limit=200us, min=333us, max=10000us" << std::endl;
// Calculer manuellement au lieu de se fier à getSinkConfigure()
const int bytesPerSample = (format.bitDepth / 8);
const int frameSize = bytesPerSample * format.channels;
const int fs1sec = format.sampleRate;

std::cout << "[DirettaOutput]    Manual calculation:" << std::endl;
std::cout << "[DirettaOutput]      - Bytes per sample: " << bytesPerSample << std::endl;
std::cout << "[DirettaOutput]      - Frame size: " << frameSize << " bytes" << std::endl;
std::cout << "[DirettaOutput]      - Frames per second: " << fs1sec << std::endl;
std::cout << "[DirettaOutput]      - Buffer: " << fs1sec << " × " << m_bufferSeconds 
          << " = " << (fs1sec * m_bufferSeconds) << " frames" << std::endl;
std::cout << "[DirettaOutput]      ⚠️  CRITICAL: This is " << m_bufferSeconds 
          << " seconds of audio buffer in Diretta!" << std::endl;

m_syncBuffer->setupBuffer(fs1sec * m_bufferSeconds, 4, false);    
    std::cout << "[DirettaOutput] 6. Connecting..." << std::endl;
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
    
    std::cout << "[DirettaOutput] ✓ Connected: " << format.sampleRate 
              << "Hz/" << format.bitDepth << "bit/" << format.channels << "ch" << std::endl;
    
    return true;
}
bool DirettaOutput::seek(int64_t samplePosition) {
    std::cout << "[DirettaOutput] 🔍 Seeking to sample " << samplePosition << "..." << std::endl;

    if (!m_syncBuffer) {
        std::cerr << "[DirettaOutput] ⚠️  No SyncBuffer available for seek" << std::endl;
        return false;
     }

    bool wasPlaying = m_playing;
    
    // Si en lecture, arrêter temporairement
    if (wasPlaying && m_syncBuffer) {
        std::cout << "[DirettaOutput] Pausing for seek..." << std::endl;
        m_syncBuffer->stop();
    }
    
    // Seek à la position
    std::cout << "[DirettaOutput] Calling SyncBuffer::seek(" << samplePosition << ")..." << std::endl;
    m_syncBuffer->seek(samplePosition);
    
    // Mettre à jour notre compteur
    m_totalSamplesSent = samplePosition;
    
    // Si on était en lecture, reprendre
    if (wasPlaying && m_syncBuffer) {
        std::cout << "[DirettaOutput] Resuming after seek..." << std::endl;
        m_syncBuffer->play();
    }
   
    std::cout << "[DirettaOutput] ✓ Seeked to sample " << samplePosition << std::endl;
    return true;
   }