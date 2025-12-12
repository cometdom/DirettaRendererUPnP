/**
 * @file AudioEngine.cpp
 * @brief Audio Engine implementation - COMPLETE
 */

#include "AudioEngine.h"
#include <iostream>
#include <thread>
#include <cstring>
#include <algorithm>  

extern "C" {
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

    // Optimize probe settings for faster startup
    av_dict_set(&options, "probesize", "1048576", 0);       // 1 MB (default: 5 MB)
    av_dict_set(&options, "analyzeduration", "1500000", 0); // 1.5 seconds (default: 5s)

    // Automatic reconnection on connection loss
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);  // Max 5 seconds between retries
    
    // Timeout to avoid blocking indefinitely
    av_dict_set(&options, "timeout", "10000000", 0);  // 10 seconds in microseconds
    
    // Improved network buffering
    av_dict_set(&options, "buffer_size", "524288", 0);  // 512KB buffer
    
    // HTTP persistent connections
    av_dict_set(&options, "http_persistent", "1", 0);
    av_dict_set(&options, "multiple_requests", "1", 0);
    
    // User-Agent (some servers check it)
    av_dict_set(&options, "user_agent", "DirettaRenderer/1.0", 0);
    
    // IMPORTANT: Ignore file size to avoid premature EOF
    av_dict_set(&options, "ignore_eof", "1", 0);
    
    std::cout << "[AudioDecoder] Opening with streaming options (reconnect enabled)" << std::endl;
    
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
        std::cout << "[AudioDecoder] Stream duration: " << duration_seconds << "." 
                  << duration_ms << " seconds" << std::endl;
    } else {
        std::cout << "[AudioDecoder] Stream duration: unknown (live stream?)" << std::endl;
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
        std::cout << "[AudioDecoder] ✓ Uncompressed PCM (WAV/AIFF) - low latency path" << std::endl;
    } else {
        std::cout << "[AudioDecoder] ℹ️  Compressed format (" << codec->name 
                  << ") - decoding required" << std::endl;
    }
    
    // Check if DSD - CRITICAL: Use RAW mode for native DSD!
    m_trackInfo.isDSD = false;
    if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR ||
        codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {
        
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
        
        std::cout << "[AudioDecoder] 🎵 DSD" << dsdMultiplier << " detected!" << std::endl;
        std::cout << "[AudioDecoder]    FFmpeg packet rate: " << packetRate << " Hz" << std::endl;
        std::cout << "[AudioDecoder]    True DSD bit rate: " << dsdBitRate << " Hz" << std::endl;
        std::cout << "[AudioDecoder] ⚠️  NO DECODING - Reading raw DSD packets!" << std::endl;
        
        // ⭐ CRITICAL: Activate RAW DSD mode
        m_rawDSD = true;
        m_packet = av_packet_alloc();
        
        // ⭐ DO NOT open codec for DSD!
        // We'll read raw packets with av_read_frame()
        std::cout << "[AudioDecoder] ✓ DSD Native mode ready" << std::endl;
        
        // Calculate duration
        if (audioStream->duration != AV_NOPTS_VALUE) {
            m_trackInfo.duration = av_rescale_q(audioStream->duration, 
                                                audioStream->time_base,
                                                {1, (int)m_trackInfo.sampleRate});
        } else {
            m_trackInfo.duration = 0;
        }
        
        m_eof = false;
        
        std::cout << "[AudioDecoder] ✓ Opened successfully (DSD NATIVE)" << std::endl;
        
        return true;  // ⭐ Exit early - no codec opening needed!
    }
    
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
        std::cout << "[AudioDecoder] ✓ Real bit depth from bits_per_raw_sample: " 
                  << realBitDepth << " bits" << std::endl;
    }
    // Method 2: Deduce from codec ID (for PCM formats like WAV)
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S16BE) {
        realBitDepth = 16;
        std::cout << "[AudioDecoder] ✓ Bit depth from codec ID (PCM16): 16 bits" << std::endl;
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        realBitDepth = 24;
        std::cout << "[AudioDecoder] ✓ Bit depth from codec ID (PCM24): 24 bits" << std::endl;
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S32LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S32BE) {
        realBitDepth = 32;
        std::cout << "[AudioDecoder] ✓ Bit depth from codec ID (PCM32): 32 bits" << std::endl;
    }
    
    // Method 3: Fallback to FFmpeg's internal format
    if (realBitDepth == 0) {
        std::cout << "[AudioDecoder] ⚠️  bits_per_raw_sample not available, using format detection" << std::endl;
        
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
                std::cout << "[AudioDecoder] ⚠️  Unknown format, defaulting to 24-bit" << std::endl;
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


std::cout << "[AudioDecoder] 🎵 PCM: " << m_trackInfo.codec 
          << " " << m_trackInfo.sampleRate << "Hz/"
          << m_trackInfo.bitDepth << "bit/"
          << m_trackInfo.channels << "ch" << std::endl;

    
    std::cout << "[AudioDecoder] 🎵 PCM: " << m_trackInfo.codec 
              << " " << m_trackInfo.sampleRate << "Hz/"
              << m_trackInfo.bitDepth << "bit/"
              << m_trackInfo.channels << "ch" << std::endl;
    
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
        static int callCount = 0;
        callCount++;
        
        if (callCount <= 20 || callCount % 100 == 0) {
            std::cout << "[AudioDecoder::readSamples] Call #" << callCount 
                      << ", requested=" << numSamples << " samples"
                      << ", remaining=" << m_remainingCount << " bytes" << std::endl;
        }
        
        if (m_eof) {
            std::cout << "[AudioDecoder::readSamples] EOF flag set, returning 0" << std::endl;
            return 0;
        }
        
        size_t bytesPerSample = 1;  // DSD: 1 byte per 8 samples per channel, but we'll work in bytes
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
                    std::cout << "[AudioDecoder] EOF reached (DSD)" << std::endl;
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
            static int packetCount = 0;
            packetCount++;
            
            // ⚠️  TEST: DON'T skip any packets - all contain audio data
            /*
            if (packetCount <= 10) {
                static bool warningShown = false;
                if (!warningShown) {
                    std::cout << "[AudioDecoder] ⚠️  Skipping first 10 packets (header/padding)" << std::endl;
                    warningShown = true;
                }
                av_packet_unref(m_packet);
                continue;
            }
            */
            
            // DEBUG: Always log packet processing
            if (packetCount <= 50) {
                std::cout << "[AudioDecoder] 📦 Processing packet #" << packetCount 
                          << ", size=" << dataSize << " bytes"
                          << ", need=" << (totalBytesNeeded - totalBytesRead) << " bytes more" << std::endl;
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
            if (packetCount <= 15) {
                std::cout << "[AudioDecoder] Packet #" << packetCount 
                          << ": used " << std::min(dataSize, bytesNeeded) << " bytes"
                          << " (total: " << totalBytesRead << "/" << totalBytesNeeded << ")" << std::endl;
            }
        }
        
        // ✅ FINAL WORKING CONFIGURATION (discovered through testing)
        // These exact settings are required for proper DSD playback:
        const bool ENABLE_INTERLEAVING = true;   // REQUIRED for stereo (prevents 2× speed)
        const bool ENABLE_BIT_REVERSAL = false;  // NOT needed for DSF files
        const bool INTERLEAVE_BY_BYTE = false;   // Use 32-bit word interleaving
        
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
                
                static bool interleavingLogged = false;
                if (!interleavingLogged) {
                    std::cout << "[AudioDecoder] 🔄 PLANAR → INTERLEAVED (byte-by-byte)" << std::endl;
                    interleavingLogged = true;
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
                
                static bool interleavingLogged = false;
                if (!interleavingLogged) {
                    std::cout << "[AudioDecoder] ✅ PLANAR → INTERLEAVED (32-bit words)" << std::endl;
                    interleavingLogged = true;
                }
            }
        }
        
        // ✅ Bit reversal NOT needed - DSF data works natively with LSB format
        // (Code removed for clarity - was never needed for proper playback)
        
        // Return number of samples actually read
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
                std::cout << "[AudioDecoder] EOF reached" << std::endl;
                
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
                            
                            static bool loggedOnce = false;
                            if (!loggedOnce) {
                                std::cout << "[AudioDecoder] ✅ Buffering " << excess 
                                          << " excess samples for next read" << std::endl;
                                loggedOnce = true;
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
{
    std::cout << "[AudioEngine] Created" << std::endl;
}

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::setAudioCallback(const AudioCallback& callback) {
    m_audioCallback = callback;
}

void AudioEngine::setTrackChangeCallback(const TrackChangeCallback& callback) {
    m_trackChangeCallback = callback;
}

void AudioEngine::setCurrentURI(const std::string& uri, const std::string& metadata, bool forceReopen) {  // ⭐ Ajouter paramètre
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
        
        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_silenceCount = 0;
        m_isDraining = false;
        
        // Si on est en PLAYING, on va automatiquement ouvrir la nouvelle piste
        // au prochain process()
    }
    
    std::cout << "[AudioEngine] Current URI set" << std::endl;
}

void AudioEngine::setNextURI(const std::string& uri, const std::string& metadata) {
    // NO MUTEX HERE - would cause deadlock with audio thread!
    // setNextURI is called from UPnP thread while process() holds the mutex
    m_nextURI = uri;
    m_nextMetadata = metadata;
    std::cout << "[AudioEngine] Next URI set (gapless)" << std::endl;
    
    // TODO: Preload next track in background
}

void AudioEngine::setTrackEndCallback(const TrackEndCallback& callback) {
    m_trackEndCallback = callback;
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
    if (!m_nextURI.empty() && !m_nextDecoder) {
        std::thread([this]() {
            preloadNextTrack();
        }).detach();
    }
    
    return true;
}
void AudioEngine::stop() {
    std::cout << "[AudioEngine] stop() called, current state = " 
              << (int)m_state.load() << std::endl;
    
    // Changer l'état SANS mutex (atomic)
    m_state.store(State::STOPPED);
    
    std::cout << "[AudioEngine] ✓ State changed to STOPPED (without mutex)" << std::endl;
    
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
    
    if (currentState != State::PLAYING) {
        // ⚠️ DISTINCTION CRITIQUE : PAUSED vs STOPPED
        if (currentState == State::STOPPED) {
            // Cleanup UNIQUEMENT si STOPPED (pas PAUSED)
            std::lock_guard<std::mutex> lock(m_mutex);
            
            if (m_currentDecoder || m_nextDecoder) {
                std::cout << "[AudioEngine] 🧹 Cleanup after STOP" << std::endl;
                m_currentDecoder.reset();
                m_nextDecoder.reset();
                m_samplesPlayed = 0;
                // ✅ NE PAS effacer m_currentURI - on veut redémarrer depuis le début
                // Le decoder sera rouvert à position 0 au prochain play()
            }
        } else if (currentState == State::PAUSED) {
            // En PAUSED, on ne fait RIEN - on garde tout en mémoire
            // Le décodeur reste ouvert à sa position actuelle
            // Prêt à reprendre instantanément
        }
        
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Double vérification avec mutex
    if (m_state.load() != State::PLAYING) {
        return false;
    }
    
    // ... reste du code inchangé ...    
    if (!m_currentDecoder) {
        return false;
    }    // ... reste du code ...    // Determine output format
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
    
    std::cout << "[AudioEngine] Preloading next track for gapless..." << std::endl;
    
    // Create decoder for next track
    m_nextDecoder = std::make_unique<AudioDecoder>();
    
    if (!m_nextDecoder->open(m_nextURI)) {
        std::cerr << "[AudioEngine] Failed to preload next track" << std::endl;
        m_nextDecoder.reset();
        return false;
    }
    
    std::cout << "[AudioEngine] ✓ Next track preloaded: " 
              << m_nextDecoder->getTrackInfo().codec << std::endl;
    
    return true;
}

void AudioEngine::transitionToNextTrack() {
    std::cout << "[AudioEngine] Transition to next track (gapless)" << std::endl;
    
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
    
    // Pour le DSD natif raw, on ne peut pas seek
    if (m_rawDSD) {
        std::cerr << "[AudioDecoder] Seek not supported in raw DSD mode" << std::endl;
        return false;
    }
    
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
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::cout << "[AudioEngine] ⏩ Seek to " << seconds << " seconds" << std::endl;
    
    // Vérifier qu'on a un décodeur actif
    if (!m_currentDecoder) {
        std::cerr << "[AudioEngine] Cannot seek: no active decoder" << std::endl;
        return false;
    }
    
    // Vérifier que la position est valide
    const TrackInfo& info = m_currentTrackInfo;
    if (info.sampleRate == 0 || info.duration == 0) {
        std::cerr << "[AudioEngine] Cannot seek: invalid track info" << std::endl;
        return false;
    }
    
    double maxSeconds = static_cast<double>(info.duration) / info.sampleRate;
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds > maxSeconds) {
        std::cout << "[AudioEngine] Seek position clamped to " << maxSeconds << "s" << std::endl;
        seconds = maxSeconds;
    }
    
    // Effectuer le seek dans le décodeur
    if (!m_currentDecoder->seek(seconds)) {
        return false;
    }
    
    // Mettre à jour le compteur de samples
    m_samplesPlayed = static_cast<uint64_t>(seconds * info.sampleRate);
    
    // Réinitialiser les compteurs de drainage
    m_silenceCount = 0;
    m_isDraining = false;
    
    std::cout << "[AudioEngine] ✓ Position updated to " 
              << m_samplesPlayed << " samples (" << seconds << "s)" << std::endl;
    
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
    
    std::cout << "[AudioEngine] Parsed time: " << timeStr 
              << " = " << totalSeconds << " seconds" << std::endl;
    
    return seek(totalSeconds);
 }

uint32_t AudioEngine::getCurrentSampleRate() const {
    return m_currentTrackInfo.sampleRate;
}
