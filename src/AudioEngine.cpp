/**
 * @file AudioEngine.cpp
 * @brief Audio Engine implementation - COMPLETE
 */

#include "AudioEngine.h"
#include "DirettaOutput.h"
#include <iostream>
#include <thread>
#include <cstring>
#include <algorithm>  

extern "C" {

// ============================================================================
// Logging system - Variable globale définie dans main.cpp
// ============================================================================
extern bool g_verbose;
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }
#include <libavutil/opt.h>
}

// ============================================================================
// AudioBuffer
// ============================================================================

AudioBuffer::AudioBuffer(size_t size)
    : m_data(nullptr)
    , m_size(0)
{
    if (size > 0) {
        resize(size);
    }
}

AudioBuffer::~AudioBuffer() {
    if (m_data) {
        delete[] m_data;
    }
}

void AudioBuffer::resize(size_t size) {
    if (m_data) {
        delete[] m_data;
    }
    m_size = size;
    m_data = new uint8_t[size];
}

// ============================================================================
// AudioDecoder
// ============================================================================

AudioDecoder::AudioDecoder()
    : m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_swrContext(nullptr)
    , m_audioStreamIndex(-1)
    , m_eof(false)
    , m_rawDSD(false)         // ⭐ DSD mode off by default
    , m_packet(nullptr)       // ⭐ Packet for raw reading
    , m_remainingCount(0)
{
}

AudioDecoder::~AudioDecoder() {
    close();
}

bool AudioDecoder::open(const std::string& url) {
    std::cout << "[AudioDecoder] Opening: " << url.substr(0, 80) << "..." << std::endl;
    
    // Open input file
    m_formatContext = avformat_alloc_context();
    if (!m_formatContext) {
        std::cerr << "[AudioDecoder] Failed to allocate format context" << std::endl;
        return false;
    }
    
    // Configure FFmpeg options for robust HTTP streaming (Qobuz)
    AVDictionary* options = nullptr;
    
    // Automatic reconnection on connection loss
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);  // Max 5 seconds between retries
    
    // Timeout to avoid blocking indefinitely
    av_dict_set(&options, "timeout", "10000000", 0);  // 10 seconds in microseconds
    
    // Improved network buffering
    av_dict_set(&options, "buffer_size", "32768", 0);  // 32KB buffer
    
    // HTTP persistent connections
    av_dict_set(&options, "http_persistent", "1", 0);
    av_dict_set(&options, "multiple_requests", "1", 0);
    
    // User-Agent (some servers check it)
    av_dict_set(&options, "user_agent", "DirettaRenderer/1.0", 0);
    
    // IMPORTANT: Ignore file size to avoid premature EOF
    av_dict_set(&options, "ignore_eof", "1", 0);
    
    DEBUG_LOG("[AudioDecoder] Opening with streaming options (reconnect enabled)");
    
    if (avformat_open_input(&m_formatContext, url.c_str(), nullptr, &options) < 0) {
        std::cerr << "[AudioDecoder] Failed to open input: " << url << std::endl;
        av_dict_free(&options);
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
        return false;
    }
    
    // Free unused options
    av_dict_free(&options);
    
    // Retrieve stream information
    if (avformat_find_stream_info(m_formatContext, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to find stream info" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Log duration information
    if (m_formatContext->duration != AV_NOPTS_VALUE) {
        int64_t duration_seconds = m_formatContext->duration / AV_TIME_BASE;
        int64_t duration_ms = (m_formatContext->duration % AV_TIME_BASE) * 1000 / AV_TIME_BASE;
        DEBUG_LOG("[AudioDecoder] Stream duration: " << duration_seconds << "." 
                  << duration_ms << " seconds");
    } else {
        DEBUG_LOG("[AudioDecoder] Stream duration: unknown (live stream?)");
    }
    
    // Find audio stream
    m_audioStreamIndex = -1;
    for (unsigned int i = 0; i < m_formatContext->nb_streams; i++) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audioStreamIndex = i;
            break;
        }
    }
    
    if (m_audioStreamIndex == -1) {
        std::cerr << "[AudioDecoder] No audio stream found" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    AVStream* audioStream = m_formatContext->streams[m_audioStreamIndex];
    AVCodecParameters* codecpar = audioStream->codecpar;
    
    // ═══════════════════════════════════════════════════════════
    // DIAGNOSTIC: Detect Audirvana pre-decoded streams
    // ═══════════════════════════════════════════════════════════
    bool isAudirvana = false;
    if (m_formatContext && m_formatContext->url) {
        std::string urlStr(m_formatContext->url);
        isAudirvana = (urlStr.find("audirvana") != std::string::npos);
    }

    if (isAudirvana) {
        std::cout << "\n════════════════════════════════════════════════════════" << std::endl;
        std::cout << "🎯 Audirvana detected - applying special handling" << std::endl;
        std::cout << "════════════════════════════════════════════════════════" << std::endl;
        
        const AVCodec* diagnostic_codec = avcodec_find_decoder(codecpar->codec_id);
        
        std::cout << "📊 Stream analysis:" << std::endl;
        std::cout << "   Codec: " << (diagnostic_codec ? diagnostic_codec->name : "unknown") << std::endl;
        std::cout << "   Sample rate: " << codecpar->sample_rate << " Hz" << std::endl;
        std::cout << "   Channels: " << codecpar->ch_layout.nb_channels << std::endl;
        std::cout << "   Bit depth: " << codecpar->bits_per_coded_sample << " bits" << std::endl;
        
        bool isPCM = (codecpar->codec_id >= AV_CODEC_ID_FIRST_AUDIO && 
                      codecpar->codec_id <= AV_CODEC_ID_PCM_F64LE &&
                      codecpar->codec_id != AV_CODEC_ID_DSD_LSBF &&
                      codecpar->codec_id != AV_CODEC_ID_DSD_MSBF &&
                      codecpar->codec_id != AV_CODEC_ID_DSD_MSBF_PLANAR &&
                      codecpar->codec_id != AV_CODEC_ID_DSD_LSBF_PLANAR);
        
        if (isPCM) {
            std::cout << "   → Already-decoded PCM detected" << std::endl;
            std::cout << "   → Will use passthrough mode (no re-decoding)" << std::endl;
        }
        
        std::cout << "════════════════════════════════════════════════════════\n" << std::endl;
    }
    
    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "[AudioDecoder] Codec not found" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Allocate codec context
    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        std::cerr << "[AudioDecoder] Failed to allocate codec context" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(m_codecContext, codecpar) < 0) {
        std::cerr << "[AudioDecoder] Failed to copy codec parameters" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Open codec
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to open codec" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Fill track info
    m_trackInfo.sampleRate = codecpar->sample_rate;
    m_trackInfo.channels = codecpar->ch_layout.nb_channels;
    m_trackInfo.codec = codec->name;
    
    // ✅ Classify codec complexity for buffer optimization
    // Uncompressed formats (WAV/AIFF): minimal latency
    // Compressed formats (FLAC/ALAC): need decoding buffer
    bool isUncompressedPCM = (
        codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S16BE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S24BE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S32LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S32BE
    );
    
    m_trackInfo.isCompressed = !isUncompressedPCM;
    
    if (isUncompressedPCM) {
        DEBUG_LOG("[AudioDecoder] ✓ Uncompressed PCM (WAV/AIFF) - low latency path");
    } else {
        DEBUG_LOG("[AudioDecoder] ℹ️  Compressed format (" << codec->name 
                  << ") - decoding required");
    }
    
    // Check if DSD - CRITICAL: Use RAW mode for native DSD!
    m_trackInfo.isDSD = false;
    if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR ||
        codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {
        
        // ⚠️  Check if this is Audirvana (which pre-decodes/wraps DSD strangely)
        if (isAudirvana) {
            // ════════════════════════════════════════════════════════
            // AUDIRVANA DSD: Use FFmpeg decoding (NOT raw mode)
            // ════════════════════════════════════════════════════════
            std::cout << "[AudioDecoder] ⚠️  Audirvana DSD: Using FFmpeg decoding" << std::endl;
            std::cout << "[AudioDecoder]     (Audirvana sends DSD with strange wrapper)" << std::endl;
            
            m_rawDSD = false;  // Let FFmpeg decode
            m_trackInfo.isDSD = false;  // Treat as PCM for Diretta
            
            // Will fall through to standard PCM decoding below
            // FFmpeg will convert the "fltp" format to PCM
            
        } else {
            // ════════════════════════════════════════════════════════
            // OTHER SOURCES: Use DSD native mode
            // ════════════════════════════════════════════════════════
            std::cout << "[AudioDecoder] ════════════════════════════════════════" << std::endl;
            std::cout << "[AudioDecoder] 🎵 DSD NATIVE MODE ACTIVATED!" << std::endl;
            std::cout << "[AudioDecoder] ════════════════════════════════════════" << std::endl;
            
            m_trackInfo.isDSD = true;
            m_trackInfo.bitDepth = 1; // DSD is 1-bit
            
            // CRITICAL: FFmpeg reports packet rate, not DSD bit rate!
            // For DSD: bit_rate = packet_rate × 8 (8 bits per byte)
            // DSD64 = 2822400 Hz, but FFmpeg reports 352800 Hz (packet rate)
            uint32_t packetRate = codecpar->sample_rate;  // 352800 for DSD64
            uint32_t dsdBitRate = packetRate * 8;          // 2822400 for DSD64
            
            m_trackInfo.sampleRate = dsdBitRate;  // ⭐ Use TRUE DSD bit rate!
            
            // Determine DSD rate (DSD64, DSD128, etc.)
            // DSD64 = 2822400 Hz = 44100 * 64
            int dsdMultiplier = dsdBitRate / 44100;
            m_trackInfo.dsdRate = dsdMultiplier;
            
            DEBUG_LOG("[AudioDecoder] 🎵 DSD" << dsdMultiplier << " detected!");
            DEBUG_LOG("[AudioDecoder]    FFmpeg packet rate: " << packetRate << " Hz");
            DEBUG_LOG("[AudioDecoder]    True DSD bit rate: " << dsdBitRate << " Hz");
            DEBUG_LOG("[AudioDecoder] ⚠️  NO DECODING - Reading raw DSD packets!");
            
            // ⭐ CRITICAL: Activate RAW DSD mode
            m_rawDSD = true;
            m_packet = av_packet_alloc();
            
            // ⭐ DO NOT open codec for DSD!
            // We'll read raw packets with av_read_frame()
            DEBUG_LOG("[AudioDecoder] ✓ DSD Native mode ready");
            
            // Calculate duration
            if (audioStream->duration != AV_NOPTS_VALUE) {
                m_trackInfo.duration = av_rescale_q(audioStream->duration, 
                                                    audioStream->time_base,
                                                    {1, (int)m_trackInfo.sampleRate});
            } else {
            }
            
            m_eof = false;
            
            std::cout << "[AudioDecoder] ✓ Opened successfully (DSD NATIVE)" << std::endl;
            
            return true;  // ⭐ Exit early - no codec opening needed!
        }  // End of else (non-Audirvana DSD native mode)
    }  // End of DSD detection
    
    // ══════════════════════════════════════════════════════════════
    // PCM MODE - Open codec and prepare for decoding
    // ══════════════════════════════════════════════════════════════
    
    m_rawDSD = false;  // Not DSD, use normal decoding
    
    // PCM format detection
    switch (codecpar->format) {
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            m_trackInfo.bitDepth = 16;
            break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            m_trackInfo.bitDepth = 32;
            break;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            m_trackInfo.bitDepth = 32; // Float treated as 32-bit
            break;
        default:
            m_trackInfo.bitDepth = 24; // Default assumption
            break;
    }

    m_rawDSD = false;  // Not DSD, use normal decoding
    
    // ⭐ CRITICAL FIX: Detect REAL bit depth from source
    int realBitDepth = 0;
    
    // Method 1: Try bits_per_raw_sample (most reliable for FLAC/ALAC)
    if (codecpar->bits_per_raw_sample > 0 && codecpar->bits_per_raw_sample <= 32) {
        realBitDepth = codecpar->bits_per_raw_sample;
        DEBUG_LOG("[AudioDecoder] ✓ Real bit depth from bits_per_raw_sample: " 
                  << realBitDepth << " bits");
    }
    // Method 2: Deduce from codec ID (for PCM formats like WAV)
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S16BE) {
        realBitDepth = 16;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM16): 16 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        realBitDepth = 24;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM24): 24 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S32LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S32BE) {
        realBitDepth = 32;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM32): 32 bits");
    }
    
    // Method 3: Fallback to FFmpeg's internal format
    if (realBitDepth == 0) {
        DEBUG_LOG("[AudioDecoder] ⚠️  bits_per_raw_sample not available, using format detection");
        
        switch (codecpar->format) {
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                realBitDepth = 16;
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                realBitDepth = 32;
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                realBitDepth = 32;
                break;
            default:
                realBitDepth = 24;
                DEBUG_LOG("[AudioDecoder] ⚠️  Unknown format, defaulting to 24-bit");
                break;
        }
    }
    
    // Safety check
    if (realBitDepth != 16 && realBitDepth != 24 && realBitDepth != 32) {
        std::cerr << "[AudioDecoder] ❌ Invalid bit depth detected: " << realBitDepth 
                  << ", falling back to 24-bit" << std::endl;
        realBitDepth = 24;
    }
    
    m_trackInfo.bitDepth = realBitDepth;


DEBUG_LOG("[AudioDecoder] 🎵 PCM: " << m_trackInfo.codec 
          << " " << m_trackInfo.sampleRate << "Hz/"
          << m_trackInfo.bitDepth << "bit/"
          << m_trackInfo.channels << "ch")

    
    DEBUG_LOG("[AudioDecoder] 🎵 PCM: " << m_trackInfo.codec 
              << " " << m_trackInfo.sampleRate << "Hz/"
              << m_trackInfo.bitDepth << "bit/"
              << m_trackInfo.channels << "ch");
    
    // Calculate duration
    if (audioStream->duration != AV_NOPTS_VALUE) {
        m_trackInfo.duration = av_rescale_q(audioStream->duration, 
                                            audioStream->time_base,
                                            {1, (int)m_trackInfo.sampleRate});
    } else {
        m_trackInfo.duration = 0;
    }
    
    m_eof = false;
    
    std::cout << "[AudioDecoder] ✓ Opened successfully" << std::endl;
    
    return true;
}

void AudioDecoder::close() {
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_packet) {  // ⭐ Free DSD packet
        av_packet_free(&m_packet);
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
    m_audioStreamIndex = -1;
    m_eof = false;
    m_rawDSD = false;  // ⭐ Reset DSD flag
}

size_t AudioDecoder::readSamples(AudioBuffer& buffer, size_t numSamples,
                                uint32_t outputRate, uint32_t outputBits) {
    
    // ══════════════════════════════════════════════════════════════
    // DSD NATIVE MODE - Read raw packets without decoding
    // ══════════════════════════════════════════════════════════════
    if (m_rawDSD) {
        m_readCallCount++;
    if (m_readCallCount % 100 == 0) {
        DEBUG_LOG("[readSamples] Call " << m_readCallCount);
}
        
        if (m_eof) {
            DEBUG_LOG("[AudioDecoder::readSamples] EOF flag set, returning 0");
            return 0;
        }
        
        size_t bytesPerSample = 1;  // DSD: 1 byte per 8 samples per channel, but we'll work in bytes
        (void)bytesPerSample;  // Silence unused variable warning
        size_t totalBytesNeeded = (numSamples * m_trackInfo.channels) / 8;
        size_t totalBytesRead = 0;
        
        // Ensure buffer is large enough
        if (buffer.size() < totalBytesNeeded) {
            buffer.resize(totalBytesNeeded);
        }
        
        uint8_t* outputPtr = buffer.data();
        
        // CRITICAL: First, use remaining samples from internal buffer
        if (m_remainingCount > 0) {
            size_t bytesToUse = std::min(m_remainingCount, totalBytesNeeded);
            memcpy(outputPtr, m_remainingSamples.data(), bytesToUse);
            outputPtr += bytesToUse;
            totalBytesRead += bytesToUse;
            
            // Shift remaining data
            if (bytesToUse < m_remainingCount) {
                size_t remaining = m_remainingCount - bytesToUse;
                memmove(m_remainingSamples.data(), 
                        m_remainingSamples.data() + bytesToUse,
                        remaining);
                m_remainingCount = remaining;
            } else {
                m_remainingCount = 0;
            }
            
            // If we have enough, return now
            if (totalBytesRead >= totalBytesNeeded) {
                return numSamples;
            }
        }
        
        // Need more data - read packets
        while (totalBytesRead < totalBytesNeeded) {
            int ret = av_read_frame(m_formatContext, m_packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    DEBUG_LOG("[AudioDecoder] EOF reached (DSD)");
                    m_eof = true;
                }
                break;
            }
            
            // Skip non-audio packets
            if (m_packet->stream_index != m_audioStreamIndex) {
                av_packet_unref(m_packet);
                continue;
            }
            
            size_t dataSize = m_packet->size;
            
            // Debug: count packets
            // Removed static variable - now m_packetCount (instance member)
               m_packetCount++;
            
            // ⚠️  TEST: DON'T skip any packets - all contain audio data
            /*
            if (m_packetCount <= 10) {
                if (!m_dsdWarningShown) {
                    DEBUG_LOG("[AudioDecoder] ⚠️  Skipping first 10 packets (header/padding)");
                    m_dsdWarningShown = true;
                }
                av_packet_unref(m_packet);
                continue;
            }
            */
            
            // DEBUG: Always log packet processing
            if (m_packetCount <= 50) {
                DEBUG_LOG("[AudioDecoder] 📦 Processing packet #" << m_packetCount 
                          << ", size=" << dataSize << " bytes"
                          << ", need=" << (totalBytesNeeded - totalBytesRead) << " bytes more");
            }
            
            // Process this packet
            size_t bytesNeeded = totalBytesNeeded - totalBytesRead;
            
            if (dataSize <= bytesNeeded) {
                // Use entire packet
                memcpy(outputPtr, m_packet->data, dataSize);
                outputPtr += dataSize;
                totalBytesRead += dataSize;
            } else {
                // Use part of packet, save rest to buffer
                memcpy(outputPtr, m_packet->data, bytesNeeded);
                totalBytesRead += bytesNeeded;
                
                // Save remaining to internal buffer
                size_t remainingBytes = dataSize - bytesNeeded;
                if (m_remainingSamples.size() < remainingBytes) {
                    m_remainingSamples.resize(remainingBytes);
                }
                memcpy(m_remainingSamples.data(), 
                       m_packet->data + bytesNeeded, 
                       remainingBytes);
                m_remainingCount = remainingBytes;
            }
            
            av_packet_unref(m_packet);
            
            // Debug first few times
            if (m_packetCount <= 15) {
                DEBUG_LOG("[AudioDecoder] Packet #" << m_packetCount 
                          << ": used " << std::min(dataSize, bytesNeeded) << " bytes"
                          << " (total: " << totalBytesRead << "/" << totalBytesNeeded << ")");
            }
        }
        
        // ✅ FINAL WORKING CONFIGURATION (discovered through testing)
        // These exact settings are required for proper DSD playback:
        const bool ENABLE_INTERLEAVING = true;   // REQUIRED for stereo (prevents 2× speed
        const bool ENABLE_BIT_REVERSAL = false;  // NOT needed for DSF files
        const bool INTERLEAVE_BY_BYTE = false;   // Use 32-bit word interleaving
        (void)ENABLE_BIT_REVERSAL;  // Silence unused variable warning
        
        // Convert PLANAR to INTERLEAVED if enabled
        if (ENABLE_INTERLEAVING && m_trackInfo.channels == 2) {
            // FFmpeg gives: [LLLL...][RRRR...] (planar by channel)
            
            // Create temp buffer for interleaving
            AudioBuffer tempBuffer(totalBytesRead);
            memcpy(tempBuffer.data(), buffer.data(), totalBytesRead);
            
            size_t bytesPerChannel = totalBytesRead / 2;
            
            if (INTERLEAVE_BY_BYTE) {
                // Interleave BYTE by BYTE: [L0 R0 L1 R1 L2 R2...]
                uint8_t* src = tempBuffer.data();
                uint8_t* dst = buffer.data();
                
                for (size_t i = 0; i < bytesPerChannel; i++) {
                    dst[i * 2]     = src[i];                     // Left byte
                    dst[i * 2 + 1] = src[bytesPerChannel + i];   // Right byte
                }
            
                if (!m_interleavingLoggedDOP) {
                    DEBUG_LOG("[AudioDecoder] 🔄 PLANAR → INTERLEAVED (byte-by-byte)");
                    m_interleavingLoggedDOP = true;
                }
            } else {
                // ✅ WORKING: Interleave by 32-bit WORDS
                size_t wordsPerChannel = bytesPerChannel / 4;
                
                uint32_t* src = reinterpret_cast<uint32_t*>(tempBuffer.data());
                uint32_t* dst = reinterpret_cast<uint32_t*>(buffer.data());
                
                for (size_t i = 0; i < wordsPerChannel; i++) {
                    dst[i * 2]     = src[i];                      // Left word
                    dst[i * 2 + 1] = src[wordsPerChannel + i];    // Right word
                }
                if (!m_interleavingLoggedNative) {
                    DEBUG_LOG("[AudioDecoder] ✅ PLANAR → INTERLEAVED (32-bit words)");
                    m_interleavingLoggedNative = true;
                }
            }
        }


    // ✅ DEBUG: Dump first 64 bytes to understand Audirvana's format
    if (g_verbose) {
    if (!m_dumpedFirstPacket && totalBytesRead >= 64) {
        std::cout << "\n[DEBUG] First 64 bytes from Audirvana DFF:" << std::endl;
        std::cout << "[DEBUG] Hex dump:" << std::endl;
        
        const uint8_t* data = buffer.data();
        for (int i = 0; i < 64; i++) {
            printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        
        std::cout << "\n[DEBUG] Codec: " << m_trackInfo.codec << std::endl;
        std::cout << "[DEBUG] Sample rate: " << m_trackInfo.sampleRate << std::endl;
        std::cout << "[DEBUG] Channels: " << m_trackInfo.channels << std::endl;
        
        m_dumpedFirstPacket = true;
    }
    }

    // ✅ CRITICAL: Convert DFF for Diretta (Bit reversal ONLY, no byte swap)
    // According to SDK: FMT_DSD_SIZ_32 uses Little Endian for BOTH DSF and DFF
    // Only the BIT order differs (LSB vs MSB)
    // ⚠️  EXCEPTION: Audirvana serves DSF with .dff URL, skip bit reversal
    bool isAudirvana = false;
    if (m_formatContext && m_formatContext->url) {
        std::string url(m_formatContext->url);
        isAudirvana = (url.find("audirvana") != std::string::npos);
    }

    if (m_trackInfo.codec.find("msbf") != std::string::npos && !isAudirvana) {
        uint8_t* data = buffer.data();
        
        // Lookup table for bit reversal
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
        
        // Bit reversal ONLY (no byte swap!)
        for (size_t i = 0; i < totalBytesRead; i++) {
            data[i] = bitReverseTable[data[i]];
        }
    
        if (!m_bitReversalLogged) {
            std::cout << "[AudioDecoder] 🔄 DFF: Bit reversal ONLY (MSB→LSB, keep LE)" << std::endl;
            m_bitReversalLogged = true;
        }
      }else if (isAudirvana) {

        if (!m_resamplingLogged) {
        std::cout << "[AudioDecoder] ⚠️  Audirvana detected: Skipping bit reversal" << std::endl;
        std::cout << "[AudioDecoder]     (DSF data with .dff URL - already LSB)" << std::endl;
        m_resamplingLogged = true;
      }    
    }
        return (totalBytesRead * 8) / m_trackInfo.channels;
    }

    // ══════════════════════════════════════════════════════════════
    // PCM MODE - Normal decoding with resampling
    // ══════════════════════════════════════════════════════════════
    
    if (!m_codecContext || m_eof) {
        return 0;
    }
    
    // Initialize resampler if needed (not for DSD)
    if (!m_trackInfo.isDSD && !m_swrContext) {
        if (!initResampler(outputRate, outputBits)) {
            return 0;
        }
    }
    
    size_t totalSamplesRead = 0;
    // ✅ CRITICAL FIX: 24-bit uses S32 container (4 bytes), not 3!
size_t bytesPerSample;
if (m_trackInfo.isDSD) {
    bytesPerSample = 1;
} else {
    // For PCM: 16-bit = 2 bytes, 24-bit and 32-bit = 4 bytes
    bytesPerSample = (outputBits == 16) ? 2 : 4;
    bytesPerSample *= m_trackInfo.channels;
}
    
    // Ensure buffer is large enough
    if (buffer.size() < numSamples * bytesPerSample) {
        buffer.resize(numSamples * bytesPerSample);
    }
    
    uint8_t* outputPtr = buffer.data();
    
    // CRITICAL FIX: D'abord, utiliser les samples restants du buffer interne
    if (m_remainingCount > 0) {
        size_t samplesToUse = std::min(m_remainingCount, numSamples);
        memcpy(outputPtr, m_remainingSamples.data(), samplesToUse * bytesPerSample);
        outputPtr += samplesToUse * bytesPerSample;
        totalSamplesRead += samplesToUse;
        
        // S'il reste encore des samples dans le buffer interne, les décaler
        if (samplesToUse < m_remainingCount) {
            size_t remaining = m_remainingCount - samplesToUse;
            memmove(m_remainingSamples.data(), 
                    m_remainingSamples.data() + samplesToUse * bytesPerSample,
                    remaining * bytesPerSample);
            m_remainingCount = remaining;
        } else {
            m_remainingCount = 0;
        }
        
        // Si on a déjà assez de samples, retourner maintenant
        if (totalSamplesRead >= numSamples) {
            return totalSamplesRead;
        }
    }
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return totalSamplesRead; // Retourner ce qu'on a déjà lu du buffer
    }
    
    while (totalSamplesRead < numSamples && !m_eof) {
        // Read packet
        int ret = av_read_frame(m_formatContext, packet);
        
        if (ret < 0) {
            // Log position when EOF occurs
            if (m_formatContext->pb && m_formatContext->pb->pos > 0) {
                std::cout << "[AudioDecoder] Bytes read from stream: " << m_formatContext->pb->pos << std::endl;
            }
            
            if (ret == AVERROR_EOF) {
                m_eof = true;
                DEBUG_LOG("[AudioDecoder] EOF reached");
                
                // Check if we read the expected duration
                std::cout << "[AudioDecoder] Samples decoded: " << totalSamplesRead << std::endl;
            } else if (ret == AVERROR(ETIMEDOUT)) {
                std::cerr << "[AudioDecoder] ⚠️  Timeout - connection too slow or lost" << std::endl;
                m_eof = true;
            } else if (ret == AVERROR(ECONNRESET)) {
                std::cerr << "[AudioDecoder] ⚠️  Connection reset by server" << std::endl;
                m_eof = true;
            } else if (ret == AVERROR_EXIT) {
                std::cerr << "[AudioDecoder] ⚠️  Exit requested" << std::endl;
                m_eof = true;
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                std::cerr << "[AudioDecoder] ⚠️  Read error (" << ret << "): " << errbuf << std::endl;
                m_eof = true;
            }
            break;
        }
        
        // Skip non-audio packets
        if (packet->stream_index != m_audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send packet to decoder
        ret = avcodec_send_packet(m_codecContext, packet);
        av_packet_unref(packet);
        
        if (ret < 0) {
            std::cerr << "[AudioDecoder] Error sending packet to decoder" << std::endl;
            break;
        }
        
        // Receive decoded frames
        while (ret >= 0 && totalSamplesRead < numSamples) {
            ret = avcodec_receive_frame(m_codecContext, frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cerr << "[AudioDecoder] Error receiving frame from decoder" << std::endl;
                av_frame_unref(frame);
                av_packet_free(&packet);
                av_frame_free(&frame);
                return totalSamplesRead;
            }
            
            // Process frame
            size_t frameSamples = frame->nb_samples;
            
            if (m_trackInfo.isDSD) {
                // DSD: Direct copy (no resampling!)
                size_t bytesToCopy = frameSamples * m_trackInfo.channels;
                size_t remainingSpace = (numSamples - totalSamplesRead) * bytesPerSample;
                
                if (bytesToCopy > remainingSpace) {
                    bytesToCopy = remainingSpace;
                    frameSamples = bytesToCopy / m_trackInfo.channels;
                }
                
                // Copy DSD data
                if (frame->format == AV_SAMPLE_FMT_U8) {
                    memcpy(outputPtr, frame->data[0], bytesToCopy);
                } else if (frame->format == AV_SAMPLE_FMT_U8P) {
                    // Planar to interleaved
                    for (size_t i = 0; i < frameSamples; i++) {
                        for (uint32_t ch = 0; ch < m_trackInfo.channels; ch++) {
                            *outputPtr++ = frame->data[ch][i];
                        }
                    }
                    outputPtr -= bytesToCopy; // Reset pointer after increment
                }
                
                outputPtr += bytesToCopy;
                totalSamplesRead += frameSamples;
                
            } else {
                // PCM: Resample if needed
                size_t samplesNeeded = numSamples - totalSamplesRead;
                
                if (m_swrContext) {
                    // Calculate TOTAL output samples (without limiting)
                    int64_t totalOutSamples = av_rescale_rnd(
                        swr_get_delay(m_swrContext, m_codecContext->sample_rate) + frameSamples,
                        outputRate,
                        m_codecContext->sample_rate,
                        AV_ROUND_UP
                    );
                    
                    // CRITICAL FIX: Allouer un buffer temporaire pour TOUS les samples convertis
                    size_t tempBufferSize = totalOutSamples * bytesPerSample;
                    AudioBuffer tempBuffer(tempBufferSize);
                    uint8_t* tempPtr = tempBuffer.data();
                    
                    // Convertir TOUTE la frame
                    int convertedSamples = swr_convert(
                        m_swrContext,
                        &tempPtr,
                        totalOutSamples,
                        (const uint8_t**)frame->data,
                        frameSamples
                    );
                    
                    if (convertedSamples > 0) {
                        // Déterminer combien on peut utiliser maintenant
                        size_t samplesToUse = std::min((size_t)convertedSamples, samplesNeeded);
                        size_t bytesToUse = samplesToUse * bytesPerSample;
                        
                        // Copier vers le buffer de sortie
                        memcpy(outputPtr, tempBuffer.data(), bytesToUse);
                        outputPtr += bytesToUse;
                        totalSamplesRead += samplesToUse;
                        
                        // CRITICAL: S'il reste des samples, les stocker dans le buffer interne
                        if ((size_t)convertedSamples > samplesToUse) {
                            size_t excess = convertedSamples - samplesToUse;
                            size_t excessBytes = excess * bytesPerSample;
                            
                            // Redimensionner le buffer interne si nécessaire
                            if (m_remainingSamples.size() < excessBytes) {
                                m_remainingSamples.resize(excessBytes);
                            }
                            
                            // Copier l'excédent
                            memcpy(m_remainingSamples.data(), 
                                   tempBuffer.data() + bytesToUse,
                                   excessBytes);
                            m_remainingCount = excess;
                        
                            if (!m_resamplerInitLogged) {
                                std::cout << "[AudioDecoder] ✅ Buffering " << excess 
                                          << " excess samples for next read" << std::endl;
                                m_resamplerInitLogged = true;
                            }
                        }
                    }
                } else {
                    // No resampling - direct copy
                    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
                    size_t bytesToCopy = samplesToCopy * bytesPerSample;
                    
                    memcpy(outputPtr, frame->data[0], bytesToCopy);
                    outputPtr += bytesToCopy;
                    totalSamplesRead += samplesToCopy;
                    
                    // CRITICAL: S'il reste des samples dans la frame, les stocker
                    if (frameSamples > samplesToCopy) {
                        size_t excess = frameSamples - samplesToCopy;
                        size_t excessBytes = excess * bytesPerSample;
                        
                        if (m_remainingSamples.size() < excessBytes) {
                            m_remainingSamples.resize(excessBytes);
                        }
                        
                        memcpy(m_remainingSamples.data(),
                               frame->data[0] + bytesToCopy,
                               excessBytes);
                        m_remainingCount = excess;
                        
                        std::cout << "[AudioDecoder] ✅ Buffering " << excess 
                                  << " excess samples (no resampling)" << std::endl;
                    }
                }
            }
            
            av_frame_unref(frame);
        }
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
    
    return totalSamplesRead;
}

bool AudioDecoder::initResampler(uint32_t outputRate, uint32_t outputBits) {
    // Don't resample DSD!
    if (m_trackInfo.isDSD) {
        std::cout << "[AudioDecoder] DSD: No resampling, native passthrough" << std::endl;
        return true;
    }
    
    // Free existing resampler
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    
    // Determine output format
    AVSampleFormat outFormat;
    switch (outputBits) {
        case 16:
            outFormat = AV_SAMPLE_FMT_S16;
            break;
        case 24:
        case 32:
            outFormat = AV_SAMPLE_FMT_S32;
            break;
        default:
            outFormat = AV_SAMPLE_FMT_S32;
            break;
    }
    
    // Allocate resampler with new API
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, m_codecContext->ch_layout.nb_channels);
    av_channel_layout_default(&outLayout, m_codecContext->ch_layout.nb_channels);
    
    int ret = swr_alloc_set_opts2(
        &m_swrContext,
        &outLayout,
        outFormat,
        outputRate,
        &inLayout,
        m_codecContext->sample_fmt,
        m_codecContext->sample_rate,
        0,
        nullptr
    );
    
    if (ret < 0 || !m_swrContext) {
        std::cerr << "[AudioDecoder] Failed to allocate resampler" << std::endl;
        return false;
    }
    
    // Initialize resampler
    if (swr_init(m_swrContext) < 0) {
        std::cerr << "[AudioDecoder] Failed to initialize resampler" << std::endl;
        swr_free(&m_swrContext);
        return false;
    }
    
    std::cout << "[AudioDecoder] Resampler: " << m_codecContext->sample_rate 
              << "Hz → " << outputRate << "Hz, " << outputBits << "bit" << std::endl;
    
    return true;
}

// ============================================================================
// AudioEngine
// ============================================================================

AudioEngine::AudioEngine()
    : m_state(State::STOPPED)
    , m_trackNumber(1)
    , m_samplesPlayed(0)
    , m_silenceCount(0)
    , m_isDraining(false)
    , m_nextTrackPrepared(false)  // ⭐ v1.2.0: Gapless Pro
{
    std::cout << "[AudioEngine] Created" << std::endl;
}

AudioEngine::~AudioEngine() {
    stop();
    waitForPreloadThread();
}

void AudioEngine::waitForPreloadThread() {
    if (m_preloadThread.joinable()) {
        m_preloadThread.join();
    }
}

void AudioEngine::setAudioCallback(const AudioCallback& callback) {
    m_audioCallback = callback;
}

void AudioEngine::setTrackChangeCallback(const TrackChangeCallback& callback) {
    m_trackChangeCallback = callback;
}

void AudioEngine::setCurrentURI(const std::string& uri, const std::string& metadata, bool forceReopen) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // CRITICAL: Si on change d'URI pendant la lecture, fermer les décodeurs
    // pour forcer l'ouverture de la nouvelle piste
    bool uriChanged = (uri != m_currentURI);
    
    m_currentURI = uri;
    m_currentMetadata = metadata;
    
    // ⭐ NOUVEAU : Forcer la réouverture même si l'URI est la même (pour Stop)
    if (uriChanged || forceReopen) {
        std::cout << "[AudioEngine] ⚠️  " 
                  << (forceReopen ? "Forced reopen" : "URI changed") 
                  << " - closing decoders to load new track" << std::endl;
        
        // Fermer les décodeurs pour forcer réouverture
        m_currentDecoder.reset();
        m_nextDecoder.reset();
        
        // ⭐⭐⭐ CRITICAL FIX: Clear gapless queue when changing URI
        // Otherwise, the old "next track" will play after the new track finishes!
        {
            std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
            m_pendingNextURI.clear();
            m_pendingNextMetadata.clear();
            m_pendingNextTrack.store(false, std::memory_order_release);
        }
        m_nextURI.clear();
        m_nextMetadata.clear();
        
        std::cout << "[AudioEngine] ✓ Gapless queue cleared" << std::endl;
        
        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_silenceCount = 0;
        m_isDraining = false;
        
        // Arrêter le préchargement en cours si existant
        if (m_preloadRunning.load(std::memory_order_acquire)) {
            m_preloadRunning.store(false, std::memory_order_release);
            std::cout << "[AudioEngine] ⚠️  Cancelling ongoing preload" << std::endl;
        }
        
        // Si on est en PLAYING, on va automatiquement ouvrir la nouvelle piste
        // au prochain process()
    }
    
    std::cout << "[AudioEngine] Current URI set" << std::endl;
}
void AudioEngine::setNextURI(const std::string& uri, const std::string& metadata) {
    // Thread-safe: Use pending mechanism to defer to audio thread
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
        m_pendingNextURI = uri;
        m_pendingNextMetadata = metadata;
    }
    m_pendingNextTrack.store(true, std::memory_order_release);
    std::cout << "[AudioEngine] Next URI queued (gapless)" << std::endl;
}

void AudioEngine::setTrackEndCallback(const TrackEndCallback& callback) {
    m_trackEndCallback = callback;
}

void AudioEngine::setNextTrackCallback(const NextTrackCallback& callback) {
    m_nextTrackCallback = callback;
    DEBUG_LOG("[AudioEngine] ✓ Next track callback set (Gapless Pro)");
}

bool AudioEngine::play() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_currentURI.empty()) {
        std::cerr << "[AudioEngine] No URI set" << std::endl;
        return false;
    }
    
    // If paused, just resume
    if (m_state == State::PAUSED && m_currentDecoder) {
        std::cout << "[AudioEngine] Resume" << std::endl;
        m_state = State::PLAYING;
        return true;
    }
    
    std::cout << "[AudioEngine] Play" << std::endl;
    
    // Open current track if not already open OR if at EOF
    if (!m_currentDecoder || m_currentDecoder->isEOF()) {
        std::cout << "[AudioEngine] Opening track (new or after EOF)" << std::endl;
        
        if (!openCurrentTrack()) {
            std::cerr << "[AudioEngine] Failed to open track" << std::endl;
            return false;
        }
    }
    
    m_state = State::PLAYING;
    m_samplesPlayed = 0;
    m_silenceCount = 0;
    m_isDraining = false;
    
    // Preload next track in background if set (for gapless)
    // Use joinable thread instead of detached to prevent use-after-free
    if (!m_nextURI.empty() && !m_nextDecoder && !m_preloadRunning.load(std::memory_order_acquire)) {
        waitForPreloadThread();
        m_preloadRunning.store(true, std::memory_order_release);
        m_preloadThread = std::thread([this]() {
            preloadNextTrack();
            m_preloadRunning.store(false, std::memory_order_release);
        });
    }
    
    return true;
}
void AudioEngine::stop() {
    std::cout << "[AudioEngine] stop() called, current state = " 
              << (int)m_state.load() << std::endl;
    
    // Changer l'état SANS mutex (atomic)
    m_state.store(State::STOPPED);

    // Clear pending flags
    m_pendingNextTrack.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
        m_pendingNextURI.clear();
        m_pendingNextMetadata.clear();
    }

    // Wait for preload thread before cleanup
    waitForPreloadThread();

    std::cout << "[AudioEngine] ✓ State changed to STOPPED" << std::endl;

    // CRITICAL: Nettoyer TOUT pour forcer réouverture au prochain play()
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        std::cout << "[AudioEngine] Cleaning up decoders and state..." << std::endl;
        
        // Fermer les décodeurs
        m_currentDecoder.reset();
        m_nextDecoder.reset();
        
        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_silenceCount = 0;
        m_isDraining = false;
        
        // CRITICAL: NE PAS effacer m_currentURI !
        // On veut pouvoir redémarrer la même piste depuis le début
        
        std::cout << "[AudioEngine] ✓ Full cleanup completed" << std::endl;
    } else {
        std::cout << "[AudioEngine] ⚠️  Mutex busy, cleanup deferred" << std::endl;
        // Le cleanup sera fait au prochain process() qui verra l'état STOPPED
    }
}


void AudioEngine::pause() {
    std::cout << "[AudioEngine] Pause requested" << std::endl;
    
    // ⭐ NE PAS bloquer sur le mutex !
    // Changer l'état directement (m_state est atomique)
    State expected = State::PLAYING;  // ⭐ Correct type
    if (m_state.compare_exchange_strong(expected, State::PAUSED)) {
        std::cout << "[AudioEngine] ✓ State changed to PAUSED" << std::endl;
    }
    
    std::cout << "[AudioEngine] Pause" << std::endl;
}
double AudioEngine::getPosition() const {
    if (m_currentTrackInfo.sampleRate == 0) {
        return 0.0;
    }
    return static_cast<double>(m_samplesPlayed) / m_currentTrackInfo.sampleRate;
}

bool AudioEngine::process(size_t samplesNeeded) {
    // Vérification rapide sans mutex
    State currentState = m_state.load();
    
    // ⭐⭐⭐ CRITICAL: Process async seek request (lock-free check)
    // This runs in the audio thread, so we can safely take the mutex
    if (m_seekRequested.load(std::memory_order_acquire)) {
        double targetSeconds = m_seekTarget.load(std::memory_order_acquire);
        m_seekRequested.store(false, std::memory_order_release);
        
        std::cout << "[AudioEngine] 🔍 Processing async seek to " << targetSeconds << "s" << std::endl;
        
        // Now we can safely take the mutex (we're in the audio thread)
        std::lock_guard<std::mutex> seekLock(m_mutex);
        
        // Validate decoder exists
        if (!m_currentDecoder) {
            std::cerr << "[AudioEngine] ❌ No decoder for seek" << std::endl;
            // Don't return false - continue playing
        } else {
            // Validate position
            const TrackInfo& info = m_currentTrackInfo;
            if (info.sampleRate > 0 && info.duration > 0) {
                double maxSeconds = static_cast<double>(info.duration) / info.sampleRate;
                if (targetSeconds > maxSeconds) {
                    targetSeconds = maxSeconds;
                }
                if (targetSeconds < 0) {
                    targetSeconds = 0;
                }
                
                // Perform the actual seek
                if (m_currentDecoder->seek(targetSeconds)) {
                    // Update position
                    m_samplesPlayed = static_cast<uint64_t>(targetSeconds * info.sampleRate);
                    
                    // Reset drainage counters
                    m_silenceCount = 0;
                    m_isDraining = false;
                    
                    std::cout << "[AudioEngine] ✓ Seek completed to " << targetSeconds << "s" << std::endl;
                    DEBUG_LOG("[AudioEngine] ✓ Position updated to " 
                              << m_samplesPlayed << " samples (" << targetSeconds << "s)");
                } else {
                    std::cerr << "[AudioEngine] ❌ Seek failed in decoder" << std::endl;
                }
            }
        }
        
        // Continue processing after seek
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);    
    // Double vérification avec mutex
    if (m_state.load() != State::PLAYING) {
        return false;
    }

    // Apply pending next URI from UPnP thread
    if (m_pendingNextTrack.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
            m_nextURI = m_pendingNextURI;
            m_nextMetadata = m_pendingNextMetadata;
            m_pendingNextURI.clear();
            m_pendingNextMetadata.clear();
        }
        m_pendingNextTrack.store(false, std::memory_order_release);
        std::cout << "[AudioEngine] Pending next URI applied (gapless)" << std::endl;
    }

    // Safety net: auto-reopen if decoder null while PLAYING
    if (!m_currentDecoder) {
        if (!m_currentURI.empty()) {
            if (!openCurrentTrack()) {
                std::cerr << "[AudioEngine] Failed to reopen track" << std::endl;
                m_state = State::STOPPED;
                if (m_trackEndCallback) {
                    m_trackEndCallback();
                }
                return false;
            }
            m_samplesPlayed = 0;
            m_silenceCount = 0;
            m_isDraining = false;
        } else {
            return false;
        }
    }

    // Determine output format
    uint32_t outputRate = m_currentTrackInfo.sampleRate;
    uint32_t outputBits = m_currentTrackInfo.bitDepth;
    uint32_t outputChannels = m_currentTrackInfo.channels;
    
    // For DSD, keep native rate and bit depth
    if (!m_currentTrackInfo.isDSD) {
        // For PCM, we can target specific output format if needed
        // For now, keep source format (bit-perfect)
    }
    
    // Read samples from decoder
    size_t samplesRead = m_currentDecoder->readSamples(
        m_buffer,
        samplesNeeded,
        outputRate,
        outputBits
    );
    
    // ⚡ CRITICAL: Preload next track as soon as EOF flag is set (for gapless)
    // Check AFTER readSamples() because EOF flag is set during the read
    if (!m_nextDecoder && !m_nextURI.empty() && m_currentDecoder->isEOF()) {
        std::cout << "[AudioEngine] 📀 EOF flag detected, preloading next track for gapless..." << std::endl;
        preloadNextTrack();
    }
    
    if (samplesRead > 0) {
        // Call audio callback to send data to output
        if (m_audioCallback) {
            bool continuePlayback = m_audioCallback(
                m_buffer,
                samplesRead,
                outputRate,
                outputBits,
                outputChannels
            );
            
            if (!continuePlayback) {
                std::cout << "[AudioEngine] Playback stopped by callback" << std::endl;
                m_state = State::STOPPED;
                return false;
            }
        }
        
        m_samplesPlayed += samplesRead;
    }
    
    // Check for actual end of data (no more samples can be read)
    if (samplesRead == 0) {
        
        // Log "Track finished" only once
        if (!m_isDraining) {
            std::cout << "[AudioEngine] ⚠️  No more samples available from decoder" << std::endl;
            m_isDraining = true;
            m_silenceCount = 0;
        }
    
        // Check if we have a next track ready for gapless
        if (m_nextDecoder) {
            std::cout << "[AudioEngine] 🎵 Transitioning to next track (gapless)..." << std::endl;
            m_isDraining = false;
            transitionToNextTrack();
            return true;  // Continue playback with new track
        } 
        
        // ⭐ NEW (v1.0.16): Check if next track exists but decoder was cleared (format change)
        if (!m_nextURI.empty()) {
            std::cout << "[AudioEngine] 🔄 Next track with format change detected" << std::endl;
            std::cout << "[AudioEngine] Transitioning with stop/start sequence..." << std::endl;
            
            // Save next URI before stopping
            std::string nextURI = m_nextURI;
            std::string nextMetadata = m_nextMetadata;
            
            // Signal track end to allow clean transition
            if (m_trackEndCallback) {
                m_trackEndCallback();
            }
            
            // Apply next URI as current
            m_currentURI = nextURI;
            m_currentMetadata = nextMetadata;
            m_nextURI.clear();
            m_nextMetadata.clear();
            
            // Reset for new track
            m_isDraining = false;
            m_samplesPlayed = 0;
            m_trackNumber++;
            
            // Stop current playback (will close DirettaOutput)
            std::cout << "[AudioEngine] Stopping for format change..." << std::endl;
            m_currentDecoder.reset();
            
            // Reopen with new track (will be done in next process() call via openCurrentTrack())
            return true;  // Continue playback state
        }
        
        // No next track - drain buffer and stop
        std::cout << "[AudioEngine] 🔇 No next track, draining buffer..." << std::endl;
        
        if (m_silenceCount == 0) {
            std::cout << "[AudioEngine] 🔇 No next track, waiting for Diretta drain..." << std::endl;
        }
        
        m_silenceCount++;
        
        // After a short wait to ensure last samples were sent, signal stop
        // Diretta has ~2-4s of buffer, but we don't need to send silence
        // The stop() function will wait for buffer_empty()
        if (m_silenceCount > 5) {  // 5 * ~92ms = ~500ms safety margin
            std::cout << "[AudioEngine] ✓ Last samples sent, signaling stop" << std::endl;
            m_silenceCount = 0;
            m_isDraining = false;
            m_state = State::STOPPED;
            
            if (m_trackEndCallback) {
                m_trackEndCallback();
            }
            
            return false;  // Stop processing, let DirettaOutput::stop() drain
        }
        
        // Return false to stop sending samples, but keep state as PLAYING briefly
        return false;
    }

     return true;
}
bool AudioEngine::openCurrentTrack() {
    // Note: This function is called from play() which already holds the mutex
    
    if (m_currentURI.empty()) {
        std::cerr << "[AudioEngine] No current URI set" << std::endl;
        return false;
    }
    
    std::cout << "[AudioEngine] Opening track: " << m_currentURI.substr(0, 80) << "..." << std::endl;
    
    // Create decoder
    m_currentDecoder = std::make_unique<AudioDecoder>();
    
    if (!m_currentDecoder->open(m_currentURI)) {
        std::cerr << "[AudioEngine] Failed to open track" << std::endl;
        m_currentDecoder.reset();
        return false;
    }
    
    m_currentTrackInfo = m_currentDecoder->getTrackInfo();
    
    std::cout << "[AudioEngine] ✓ Track opened: ";
    if (m_currentTrackInfo.isDSD) {
        std::cout << "DSD" << m_currentTrackInfo.dsdRate 
                  << " (" << m_currentTrackInfo.sampleRate << " Hz)";
    } else {
        std::cout << m_currentTrackInfo.sampleRate << "Hz/"
                  << m_currentTrackInfo.bitDepth << "bit";
    }
    std::cout << "/" << m_currentTrackInfo.channels << "ch" << std::endl;
    
    // Call track change callback with URI and metadata
    if (m_trackChangeCallback) {
        m_trackChangeCallback(m_trackNumber, m_currentTrackInfo, m_currentURI, m_currentMetadata);
    }
    
    return true;
}

bool AudioEngine::preloadNextTrack() {
    if (m_nextURI.empty()) {
        return false;
    }
    
    DEBUG_LOG("[AudioEngine] Preloading next track for gapless...");
    
    // Create decoder for next track
    m_nextDecoder = std::make_unique<AudioDecoder>();
    
    if (!m_nextDecoder->open(m_nextURI)) {
        std::cerr << "[AudioEngine] Failed to preload next track" << std::endl;
        m_nextDecoder.reset();
        return false;
    }

    // Check format compatibility for gapless playback
    // Format changes require clean stop/start to avoid audio artifacts
    TrackInfo nextInfo = m_nextDecoder->getTrackInfo();
    bool formatWillChange = (
        nextInfo.sampleRate != m_currentTrackInfo.sampleRate ||
        nextInfo.bitDepth != m_currentTrackInfo.bitDepth ||
        nextInfo.channels != m_currentTrackInfo.channels ||
        nextInfo.isDSD != m_currentTrackInfo.isDSD
    );

    if (formatWillChange) {
        DEBUG_LOG("[AudioEngine] ⚠️  FORMAT CHANGE DETECTED - Gapless disabled");
        DEBUG_LOG("[AudioEngine] Current: "
                  << m_currentTrackInfo.sampleRate << "Hz/"
                  << m_currentTrackInfo.bitDepth << "bit/"
                  << m_currentTrackInfo.channels << "ch"
                  << (m_currentTrackInfo.isDSD ? " (DSD)" : ""));
        DEBUG_LOG("[AudioEngine] Next: "
                  << nextInfo.sampleRate << "Hz/"
                  << nextInfo.bitDepth << "bit/"
                  << nextInfo.channels << "ch"
                  << (nextInfo.isDSD ? " (DSD)" : ""));
        DEBUG_LOG("[AudioEngine] 🔄 Will use stop/start sequence instead of gapless");

        // Don't keep nextDecoder - force stop/start sequence
        m_nextDecoder.reset();
        
        // ⭐ CRITICAL FIX (v1.0.16): Keep m_nextURI!
        // Do NOT clear m_nextURI - it will be used for non-gapless transition
        // The EOF handler will see format change and trigger proper reopen
        // m_nextURI.clear();     // ❌ REMOVED - was causing next track to be lost
        // m_nextMetadata.clear(); // ❌ REMOVED
        
        return false;
    }

    DEBUG_LOG("[AudioEngine] ✓ Next track preloaded: "
              << m_nextDecoder->getTrackInfo().codec);

    return true;
}

void AudioEngine::transitionToNextTrack() {
    DEBUG_LOG("[AudioEngine] Transition to next track (gapless)");
    
    // CRITICAL: Move next URI to current URI BEFORE clearing
    m_currentURI = m_nextURI;
    m_currentMetadata = m_nextMetadata;
    
    m_currentDecoder = std::move(m_nextDecoder);
    m_trackNumber++;
    m_samplesPlayed = 0;
    
    // Clear next URI after moving to current
    m_nextURI.clear();
    m_nextMetadata.clear();
    
    if (m_currentDecoder) {
        m_currentTrackInfo = m_currentDecoder->getTrackInfo();
        if (m_trackChangeCallback) {
            m_trackChangeCallback(m_trackNumber, m_currentTrackInfo, m_currentURI, m_currentMetadata);
        }
    }
}
bool AudioDecoder::seek(double seconds) {
    if (!m_formatContext || m_audioStreamIndex < 0) {
        std::cerr << "[AudioDecoder] Cannot seek: no file open" << std::endl;
        return false;
    }
    
    // ⭐ v1.2.1: DSD raw seek with file repositioning
    if (m_rawDSD) {
        std::cout << "[AudioDecoder] DSD seek to " << seconds << "s (with file repositioning)" << std::endl;
        
        // Calculate byte position in DSD file
        // For DSD: sampleRate = bits per second per channel
        int64_t bitsPerSecond = m_trackInfo.sampleRate * m_trackInfo.channels;
        int64_t targetBit = static_cast<int64_t>(seconds * bitsPerSecond);
        int64_t targetByte = targetBit / 8;
        
        std::cout << "[AudioDecoder]   Target: " << targetByte << " bytes (" << targetBit << " bits)" << std::endl;
        std::cout << "[AudioDecoder]   Format: " << m_trackInfo.sampleRate << " Hz, " 
                  << m_trackInfo.channels << " channels" << std::endl;
        
        // Seek in file using byte position
        AVIOContext* avio = m_formatContext->pb;
        if (avio) {
            // Use SEEK_SET to position from start of file
            int64_t result = avio_seek(avio, targetByte, SEEK_SET);
            if (result >= 0) {
                std::cout << "[AudioDecoder]   ✓ File repositioned to byte " << result << std::endl;
            } else {
                std::cerr << "[AudioDecoder]   ⚠️  avio_seek failed, code: " << result << std::endl;
                // Continue anyway - may still work approximately
            }
        } else {
            std::cerr << "[AudioDecoder]   ⚠️  No AVIOContext available for file seek" << std::endl;
        }
        
        // Flush codec buffers to clear old data
        if (m_codecContext) {
            avcodec_flush_buffers(m_codecContext);
            std::cout << "[AudioDecoder]   ✓ Codec buffers flushed" << std::endl;
        }
        
        // Reset internal buffers
        m_remainingCount = 0;
        m_eof = false;
        
        std::cout << "[AudioDecoder]   ✓ DSD seek completed" << std::endl;
        return true;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // ✅ PCM: Normal FFmpeg seek (unchanged)
    // ═══════════════════════════════════════════════════════════════
    
    std::cout << "[AudioDecoder] Seeking to " << seconds << " seconds..." << std::endl;
    
    // Convertir le temps en timestamp FFmpeg
    AVStream* stream = m_formatContext->streams[m_audioStreamIndex];
    int64_t timestamp = av_rescale_q(
        static_cast<int64_t>(seconds * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        stream->time_base
    );
    
    // Effectuer le seek
    // AVSEEK_FLAG_BACKWARD : cherche le keyframe le plus proche AVANT la position
    int ret = av_seek_frame(m_formatContext, m_audioStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[AudioDecoder] Seek failed: " << errbuf << std::endl;
        return false;
    }
    
    // Vider les buffers du codec
    if (m_codecContext) {
        avcodec_flush_buffers(m_codecContext);
    }
    
    // Réinitialiser les buffers internes
    m_remainingCount = 0;
    m_eof = false;
    
    std::cout << "[AudioDecoder] ✓ Seek successful to ~" << seconds << "s" << std::endl;
    
    return true;
}


// ============================================================================
// AudioEngine::seek() - Seek avec mise à jour de la position
// ============================================================================

bool AudioEngine::seek(double seconds) {
    // ⭐⭐⭐ CRITICAL FIX: Async seek to avoid deadlock
    // The UPnP thread calling this should not block waiting for mutex
    // Instead, we set atomic flags and let the audio thread handle the seek
    
    std::cout << "[AudioEngine] ⏩ Seek requested to " << seconds << " seconds (async)" << std::endl;
    
    // Quick validation without mutex
    if (m_state.load(std::memory_order_acquire) != State::PLAYING) {
        std::cerr << "[AudioEngine] ❌ Cannot seek when not playing" << std::endl;
        return false;
    }
    
    // Clamp to valid range (optimistic check, will be validated in audio thread)
    const TrackInfo& info = m_currentTrackInfo;
    if (info.sampleRate > 0 && info.duration > 0) {
        double maxSeconds = static_cast<double>(info.duration) / info.sampleRate;
        if (seconds < 0) {
            seconds = 0;
        }
        if (seconds > maxSeconds) {
            DEBUG_LOG("[AudioEngine] Seek position clamped to " << maxSeconds << "s");
            seconds = maxSeconds;
        }
    }
    
    // ⭐ Set seek request atomically (lock-free, non-blocking)
    m_seekTarget.store(seconds, std::memory_order_release);
    m_seekRequested.store(true, std::memory_order_release);
    
    std::cout << "[AudioEngine] ✓ Seek queued, will be processed by audio thread" << std::endl;
    
    // Return immediately - UPnP thread doesn't wait
    return true;
}

// ============================================================================
// AudioEngine::seek() - Version avec string "HH:MM:SS"
// ============================================================================

bool AudioEngine::seek(const std::string& timeStr) {
    // Parser le format HH:MM:SS ou MM:SS
    int hours = 0, minutes = 0, seconds = 0;
    
    // Compter les ':'
    size_t colonCount = std::count(timeStr.begin(), timeStr.end(), ':');
    
    if (colonCount == 2) {
        // Format HH:MM:SS
        if (sscanf(timeStr.c_str(), "%d:%d:%d", &hours, &minutes, &seconds) != 3) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    } else if (colonCount == 1) {
        // Format MM:SS
        if (sscanf(timeStr.c_str(), "%d:%d", &minutes, &seconds) != 2) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    } else {
        // Format numérique simple (secondes)
        try {
            double secs = std::stod(timeStr);
            return seek(secs);
        } catch (...) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    }
    
    // Convertir en secondes totales
    double totalSeconds = hours * 3600.0 + minutes * 60.0 + seconds;
    
    DEBUG_LOG("[AudioEngine] Parsed time: " << timeStr 
              << " = " << totalSeconds << " seconds");
    
    return seek(totalSeconds);
 }

uint32_t AudioEngine::getCurrentSampleRate() const {
    return m_currentTrackInfo.sampleRate;
}
// ═══════════════════════════════════════════════════════════════
// ⭐ v1.2.0: Gapless Pro - Prepare next track for gapless
// ═══════════════════════════════════════════════════════════════

void AudioEngine::prepareNextTrackForGapless() {
    // Check if next track exists
    if (m_nextURI.empty()) {
        DEBUG_LOG("[AudioEngine] No next track for gapless");
        return;
    }
    
    // Check if already prepared
    if (m_nextTrackPrepared) {
        DEBUG_LOG("[AudioEngine] Next track already prepared");
        return;
    }
    
    DEBUG_LOG("[AudioEngine] 🎵 Preparing next track for gapless: " << m_nextURI);
    
    try {
        // ⭐ OPTIMISATION v1.2.0: Réutiliser m_nextDecoder si déjà ouvert
        if (!m_nextDecoder) {
            DEBUG_LOG("[AudioEngine] Opening next track decoder...");
            m_nextDecoder = std::make_unique<AudioDecoder>();
            
            if (!m_nextDecoder->open(m_nextURI)) {
                std::cerr << "[AudioEngine] ❌ Failed to open next track for gapless" << std::endl;
                m_nextDecoder.reset();  // Cleanup on failure
                return;
            }
            DEBUG_LOG("[AudioEngine] ✓ Next track decoder opened");
        } else {
            DEBUG_LOG("[AudioEngine] ♻️  Reusing pre-loaded next track decoder");
        }
        
        // Get format from m_nextDecoder
        const TrackInfo& nextTrackInfo = m_nextDecoder->getTrackInfo();
        
        // Create AudioFormat for DirettaOutput
        AudioFormat nextFormat;
        nextFormat.sampleRate = nextTrackInfo.sampleRate;
        nextFormat.bitDepth = nextTrackInfo.bitDepth;
        nextFormat.channels = nextTrackInfo.channels;
        nextFormat.isDSD = nextTrackInfo.isDSD;
        nextFormat.isCompressed = nextTrackInfo.isCompressed;
        
        // Read first chunk (1 second of audio for buffering)
        size_t samplesToRead = nextTrackInfo.sampleRate;  // 1 second
        
        AudioBuffer nextBuffer;
        size_t bytesPerSample = (nextTrackInfo.bitDepth / 8) * nextTrackInfo.channels;
        nextBuffer.resize(samplesToRead * bytesPerSample);
        
        // Read from m_nextDecoder
        size_t samplesRead = m_nextDecoder->readSamples(nextBuffer, samplesToRead,
                                                        nextTrackInfo.sampleRate,
                                                        nextTrackInfo.bitDepth);
        
        if (samplesRead > 0) {
            // Call callback to send to DirettaOutput
            if (m_nextTrackCallback) {
                DEBUG_LOG("[AudioEngine] 📤 Sending " << samplesRead 
                          << " samples to gapless buffer");
                m_nextTrackCallback(nextBuffer.data(), samplesRead, nextFormat);
                m_nextTrackPrepared = true;
                
                DEBUG_LOG("[AudioEngine] ✅ Next track prepared for gapless transition");
            } else {
                DEBUG_LOG("[AudioEngine] ⚠️  No next track callback set");
            }
        } else {
            DEBUG_LOG("[AudioEngine] ⚠️  Failed to read samples from next track");
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[AudioEngine] ❌ Exception preparing next track: " 
                  << e.what() << std::endl;
        m_nextDecoder.reset();  // Cleanup on exception
    }
}

// ═══════════════════════════════════════════════════════════════
// End of v1.2.0 Gapless Pro implementation
// ═══════════════════════════════════════════════════════════════
