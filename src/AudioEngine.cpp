/**
 * @file AudioEngine.cpp
 * @brief Audio Engine implementation - COMPLETE
 */

#include "AudioEngine.h"
#include "DirettaRingBuffer.h"  // For kBitReverseLUT
#include <iostream>
#include <thread>
#include <cstring>
#include <algorithm>
#include "memcpyfast_audio.h"

extern "C" {

// ============================================================================
// Logging system - Variable globale définie dans main.cpp
// ============================================================================
extern bool g_verbose;
#ifdef NOLOG
#define DEBUG_LOG(x) do {} while(0)
#else
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }
#endif
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

AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : m_data(other.m_data)
    , m_size(other.m_size)
{
    other.m_data = nullptr;
    other.m_size = 0;
}

AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        delete[] m_data;
        m_data = other.m_data;
        m_size = other.m_size;
        other.m_data = nullptr;
        other.m_size = 0;
    }
    return *this;
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
    , m_rawDSD(false)         // DSD mode off by default
    , m_packet(nullptr)       // Reusable packet for raw reading
    , m_frame(nullptr)        // Reusable frame for PCM decoding
    , m_dsdRemainderReadPos(0)   // DSD remainder ring read position
    , m_dsdRemainderWritePos(0)  // DSD remainder ring write position
    , m_pcmFifo(nullptr)      // PCM overflow FIFO
    , m_resampleBufferCapacity(0)
    , m_bypassMode(false)     // PCM bypass disabled by default
    , m_resamplerInitialized(false)
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

    // Find audio stream using FFmpeg's recommended API (handles NULL codecpar in FFmpeg 5.x)
    m_audioStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_audioStreamIndex < 0) {
        std::cerr << "[AudioDecoder] No audio stream found" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }

    AVStream* audioStream = m_formatContext->streams[m_audioStreamIndex];
    if (!audioStream || !audioStream->codecpar) {
        std::cerr << "[AudioDecoder] Audio stream has invalid codec parameters" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
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
        std::cout << "Audirvana detected - applying special handling" << std::endl;
        std::cout << "════════════════════════════════════════════════════════" << std::endl;

        const AVCodec* diagnostic_codec = avcodec_find_decoder(codecpar->codec_id);

        std::cout << "Stream analysis:" << std::endl;
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
            std::cout << "   -> Already-decoded PCM detected" << std::endl;
            std::cout << "   -> Will use passthrough mode (no re-decoding)" << std::endl;
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

    // Classify codec complexity for buffer optimization
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
        DEBUG_LOG("[AudioDecoder] Uncompressed PCM (WAV/AIFF) - low latency path");
    } else {
        DEBUG_LOG("[AudioDecoder] Compressed format (" << codec->name
                  << ") - decoding required");
    }

    // Check if DSD - CRITICAL: Use RAW mode for native DSD!
    m_trackInfo.isDSD = false;
    if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR ||
        codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {

        // Check if this is Audirvana (which pre-decodes/wraps DSD strangely)
        if (isAudirvana) {
            // ════════════════════════════════════════════════════════
            // AUDIRVANA DSD: Use FFmpeg decoding (NOT raw mode)
            // ════════════════════════════════════════════════════════
            std::cout << "[AudioDecoder] Audirvana DSD: Using FFmpeg decoding" << std::endl;
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
            std::cout << "[AudioDecoder] DSD NATIVE MODE ACTIVATED!" << std::endl;
            std::cout << "[AudioDecoder] ════════════════════════════════════════" << std::endl;

            m_trackInfo.isDSD = true;
            m_trackInfo.bitDepth = 1; // DSD is 1-bit

            // Detect DSF vs DFF from file extension (for correct bit ordering)
            if (m_formatContext && m_formatContext->url) {
                std::string url(m_formatContext->url);
                if (url.find(".dsf") != std::string::npos ||
                    url.find(".DSF") != std::string::npos) {
                    m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DSF;
                    DEBUG_LOG("[AudioDecoder] DSD source format: DSF (LSB first)");
                } else if (url.find(".dff") != std::string::npos ||
                           url.find(".DFF") != std::string::npos) {
                    m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DFF;
                    DEBUG_LOG("[AudioDecoder] DSD source format: DFF (MSB first)");
                } else {
                    // Fallback: detect from codec ID
                    if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
                        codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {
                        m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DSF;
                        DEBUG_LOG("[AudioDecoder] DSD source format: DSF (from codec)");
                    } else {
                        m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DFF;
                        DEBUG_LOG("[AudioDecoder] DSD source format: DFF (from codec)");
                    }
                }
            }

            // CRITICAL: FFmpeg reports packet rate, not DSD bit rate!
            // For DSD: bit_rate = packet_rate × 8 (8 bits per byte)
            // DSD64 = 2822400 Hz, but FFmpeg reports 352800 Hz (packet rate)
            uint32_t packetRate = codecpar->sample_rate;  // 352800 for DSD64
            uint32_t dsdBitRate = packetRate * 8;          // 2822400 for DSD64

            m_trackInfo.sampleRate = dsdBitRate;  // Use TRUE DSD bit rate!

            // Determine DSD rate (DSD64, DSD128, etc.)
            // DSD64 = 2822400 Hz = 44100 * 64
            int dsdMultiplier = dsdBitRate / 44100;
            m_trackInfo.dsdRate = dsdMultiplier;

            DEBUG_LOG("[AudioDecoder] DSD" << dsdMultiplier << " detected!");
            DEBUG_LOG("[AudioDecoder]    FFmpeg packet rate: " << packetRate << " Hz");
            DEBUG_LOG("[AudioDecoder]    True DSD bit rate: " << dsdBitRate << " Hz");
            DEBUG_LOG("[AudioDecoder] NO DECODING - Reading raw DSD packets!");

            // CRITICAL: Activate RAW DSD mode
            m_rawDSD = true;
            m_packet = av_packet_alloc();

#ifdef DIRETTA_DSD_DIAGNOSTICS
            std::cout << "\n[DSD DIAGNOSTIC] Reading first packets to understand layout:" << std::endl;
            for (int i = 0; i < 3; i++) {
                AVPacket* testPkt = av_packet_alloc();
                int ret = av_read_frame(m_formatContext, testPkt);
                if (ret >= 0 && testPkt->stream_index == m_audioStreamIndex) {
                    std::cout << "[DSD DIAGNOSTIC] Packet " << i << ":" << std::endl;
                    std::cout << "  stream_index: " << testPkt->stream_index << std::endl;
                    std::cout << "  size: " << testPkt->size << " bytes" << std::endl;
                    std::cout << "  pts: " << testPkt->pts << std::endl;
                    std::cout << "  duration: " << testPkt->duration << std::endl;

                    // First 16 bytes (should be L channel start)
                    std::cout << "  data[0..15] (L start): ";
                    for (int j = 0; j < 16 && j < testPkt->size; j++) {
                        printf("%02X ", testPkt->data[j]);
                    }
                    printf("\n");

                    // Bytes at 4096 (should be R channel start if layout is [4096 L][4096 R])
                    if (testPkt->size > 4096 + 16) {
                        std::cout << "  data[4096..4111] (R start?): ";
                        for (int j = 4096; j < 4096 + 16; j++) {
                            printf("%02X ", testPkt->data[j]);
                        }
                        printf("\n");
                    }

                    // Last 16 bytes
                    if (testPkt->size > 16) {
                        std::cout << "  data[" << (testPkt->size - 16) << ".." << (testPkt->size-1) << "] (end): ";
                        for (int j = testPkt->size - 16; j < testPkt->size; j++) {
                            printf("%02X ", testPkt->data[j]);
                        }
                        printf("\n");
                    }
                }
                av_packet_unref(testPkt);
                av_packet_free(&testPkt);
            }

            // Seek back to beginning
            av_seek_frame(m_formatContext, m_audioStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
            std::cout << "[DSD DIAGNOSTIC] Seek back to start complete\n" << std::endl;
#endif // DIRETTA_DSD_DIAGNOSTICS

            // DO NOT open codec for DSD!
            // We'll read raw packets with av_read_frame()
            DEBUG_LOG("[AudioDecoder] DSD Native mode ready");

            // Calculate duration
            if (audioStream->duration != AV_NOPTS_VALUE) {
                m_trackInfo.duration = av_rescale_q(audioStream->duration,
                                                    audioStream->time_base,
                                                    {1, (int)m_trackInfo.sampleRate});
            }

            m_eof = false;

            // C1: Pre-allocate DSD buffers at track open to avoid first-frame allocation
            // Size based on MAX_DSD_SAMPLES (131072) / 8 = 16384 bytes per channel
            // Use 32KB to have headroom for any chunk size
            static constexpr size_t DSD_BUFFER_PREALLOC = 32768;
            if (m_dsdBufferCapacity < DSD_BUFFER_PREALLOC) {
                m_dsdLeftBuffer.resize(DSD_BUFFER_PREALLOC);
                m_dsdRightBuffer.resize(DSD_BUFFER_PREALLOC);
                m_dsdBufferCapacity = DSD_BUFFER_PREALLOC;
                DEBUG_LOG("[AudioDecoder] Pre-allocated DSD buffers: " << DSD_BUFFER_PREALLOC << " bytes/channel");
            }

            std::cout << "[AudioDecoder] Opened successfully (DSD NATIVE)" << std::endl;

            return true;  // Exit early - no codec opening needed!
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

    // CRITICAL FIX: Detect REAL bit depth from source
    int realBitDepth = 0;

    // Method 1: Try bits_per_raw_sample (most reliable for FLAC/ALAC)
    if (codecpar->bits_per_raw_sample > 0 && codecpar->bits_per_raw_sample <= 32) {
        realBitDepth = codecpar->bits_per_raw_sample;
        DEBUG_LOG("[AudioDecoder] Real bit depth from bits_per_raw_sample: "
                  << realBitDepth << " bits");
    }
    // Method 2: Deduce from codec ID (for PCM formats like WAV)
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
             codecpar->codec_id == AV_CODEC_ID_PCM_S16BE) {
        realBitDepth = 16;
        DEBUG_LOG("[AudioDecoder] Bit depth from codec ID (PCM16): 16 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
             codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        realBitDepth = 24;
        DEBUG_LOG("[AudioDecoder] Bit depth from codec ID (PCM24): 24 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S32LE ||
             codecpar->codec_id == AV_CODEC_ID_PCM_S32BE) {
        realBitDepth = 32;
        DEBUG_LOG("[AudioDecoder] Bit depth from codec ID (PCM32): 32 bits");
    }

    // Method 3: Fallback to FFmpeg's internal format
    if (realBitDepth == 0) {
        DEBUG_LOG("[AudioDecoder] bits_per_raw_sample not available, using format detection");

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
                DEBUG_LOG("[AudioDecoder] Unknown format, defaulting to 24-bit");
                break;
        }
    }

    // Safety check
    if (realBitDepth != 16 && realBitDepth != 24 && realBitDepth != 32) {
        std::cerr << "[AudioDecoder] Invalid bit depth detected: " << realBitDepth
                  << ", falling back to 24-bit" << std::endl;
        realBitDepth = 24;
    }

    m_trackInfo.bitDepth = realBitDepth;

    // Detect S24 alignment hint for 24-bit content
    // This hint helps the ring buffer when track starts with silence
    m_trackInfo.s24Alignment = TrackInfo::S24Alignment::Unknown;
    if (realBitDepth == 24) {
        // PCM_S24LE/BE codecs: data is truly 24-bit, packed LSB-aligned in 32-bit
        if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
            codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
            m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
            DEBUG_LOG("[AudioDecoder] S24 hint: LSB-aligned (PCM_S24)");
        }
        // FLAC/ALAC with 24-bit: typically LSB-aligned in S32 container
        else if (codecpar->codec_id == AV_CODEC_ID_FLAC ||
                 codecpar->codec_id == AV_CODEC_ID_ALAC) {
            m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
            DEBUG_LOG("[AudioDecoder] S24 hint: LSB-aligned (FLAC/ALAC)");
        }
        // Other decoders: check sample format
        else if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32 ||
                 m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32P) {
            // S32 format with 24-bit content is typically LSB-aligned
            m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
            DEBUG_LOG("[AudioDecoder] S24 hint: LSB-aligned (S32 format)");
        }
    }

    DEBUG_LOG("[AudioDecoder] PCM: " << m_trackInfo.codec
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

    std::cout << "[AudioDecoder] Opened successfully" << std::endl;

    return true;
}

void AudioDecoder::close() {
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_frame) {  // Free reusable frame
        av_frame_free(&m_frame);
    }
    if (m_packet) {  // Free reusable packet
        av_packet_free(&m_packet);
    }
    if (m_pcmFifo) {  // Free PCM overflow FIFO
        av_audio_fifo_free(m_pcmFifo);
        m_pcmFifo = nullptr;
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
    m_audioStreamIndex = -1;
    m_eof = false;
    m_rawDSD = false;
    m_resampleBufferCapacity = 0;  // Reset capacity tracking
    m_dsdBufferCapacity = 0;       // Reset DSD buffer capacity tracking
    dsdRemainderClear();           // Reset DSD packet remainder ring
    m_bypassMode = false;          // Reset PCM bypass mode
    m_resamplerInitialized = false;
    m_cachedResamplerDelay = 0;    // D2: Reset cached delay
    m_delayRefreshCounter = 0;
}

size_t AudioDecoder::readSamples(AudioBuffer& buffer, size_t numSamples,
                                uint32_t outputRate, uint32_t outputBits) {

    // ══════════════════════════════════════════════════════════════
    // DSD NATIVE MODE - Read raw packets without decoding
    // ══════════════════════════════════════════════════════════════

    if (m_rawDSD) {
        if (m_eof) {
            return 0;
        }

        // Calculate bytes needed
        size_t totalBytesNeeded = (numSamples * m_trackInfo.channels) / 8;
        size_t bytesPerChannelNeeded = totalBytesNeeded / m_trackInfo.channels;

        // Ensure pre-allocated DSD buffers are large enough (resize only if capacity insufficient)
        if (m_dsdBufferCapacity < bytesPerChannelNeeded) {
            m_dsdLeftBuffer.resize(bytesPerChannelNeeded);
            m_dsdRightBuffer.resize(bytesPerChannelNeeded);
            m_dsdBufferCapacity = bytesPerChannelNeeded;
        }

        // Use offset tracking instead of vector operations (zero allocations)
        size_t leftOffset = 0;
        size_t rightOffset = 0;
        uint8_t* leftData = m_dsdLeftBuffer.data();
        uint8_t* rightData = m_dsdRightBuffer.data();

        // Ensure output buffer is large enough
        if (buffer.size() < totalBytesNeeded) {
            buffer.resize(totalBytesNeeded);
        }

        // Use remaining data from previous DSD packet reads (O(1) ring buffer)
        size_t remainderAvail = dsdRemainderAvailable();
        if (remainderAvail > 0) {
            size_t toUse = std::min(remainderAvail, bytesPerChannelNeeded);
            size_t popped = dsdRemainderPop(leftData + leftOffset,
                                            rightData + rightOffset,
                                            toUse);
            leftOffset += popped;
            rightOffset += popped;
        }

        // Read packets until we have enough data
        // DSF layout: each packet is [blockSize L][blockSize R]
        while (leftOffset < bytesPerChannelNeeded && !m_eof) {
            int ret = av_read_frame(m_formatContext, m_packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    m_eof = true;
                }
                break;
            }

            if (m_packet->stream_index != m_audioStreamIndex) {
                av_packet_unref(m_packet);
                continue;
            }

            m_packetCount++;
            size_t packetSize = m_packet->size;
            size_t blockSize = packetSize / 2;  // Each channel gets half

            // L is first half, R is second half
            const uint8_t* pktL = m_packet->data;
            const uint8_t* pktR = m_packet->data + blockSize;

            size_t stillNeed = bytesPerChannelNeeded - leftOffset;
            size_t toTake = std::min(blockSize, stillNeed);

            memcpy(leftData + leftOffset, pktL, toTake);
            leftOffset += toTake;
            memcpy(rightData + rightOffset, pktR, toTake);
            rightOffset += toTake;

            // Debug first few packets
            if (m_packetCount <= 3) {
                std::cout << "[DSD READ] Packet " << m_packetCount
                          << ": size=" << packetSize
                          << " block=" << blockSize
                          << " took=" << toTake << std::endl;
                std::cout << "[DSD READ]   L[0..7]: ";
                for (size_t i = 0; i < 8 && i < blockSize; i++) printf("%02X ", pktL[i]);
                printf("\n");
                std::cout << "[DSD READ]   R[0..7]: ";
                for (size_t i = 0; i < 8 && i < blockSize; i++) printf("%02X ", pktR[i]);
                printf("\n");
            }

            // Save DSD packet excess (O(1) ring buffer push)
            if (toTake < blockSize) {
                size_t excess = blockSize - toTake;
                dsdRemainderPush(pktL + toTake, pktR + toTake, excess);
            }

            av_packet_unref(m_packet);
        }

        // Build output: [all L][all R]
        size_t actualPerCh = std::min(leftOffset, rightOffset);
        size_t totalBytes = actualPerCh * 2;

        if (actualPerCh > 0) {
            memcpy_audio(buffer.data(), leftData, actualPerCh);
            memcpy_audio(buffer.data() + actualPerCh, rightData, actualPerCh);
        }

        // Debug output
        if (m_packetCount <= 5) {
            std::cout << "[DSD OUT] " << totalBytes << " bytes, " << actualPerCh << " per ch" << std::endl;
            std::cout << "[DSD OUT]   L: ";
            for (size_t i = 0; i < 8 && i < actualPerCh; i++) printf("%02X ", buffer.data()[i]);
            printf("\n");
            std::cout << "[DSD OUT]   R: ";
            for (size_t i = 0; i < 8 && i < actualPerCh; i++) printf("%02X ", buffer.data()[actualPerCh + i]);
            printf("\n");
        }

        // Bit reversal for DFF (MSB) files - DSF is LSB, no reversal needed
        if (m_trackInfo.codec.find("msbf") != std::string::npos) {
            // Use shared LUT from DirettaRingBuffer (cache-friendly, single copy in memory)
            const uint8_t* rev = DirettaRingBuffer::kBitReverseLUT;
            for (size_t i = 0; i < totalBytes; i++) {
                buffer.data()[i] = rev[buffer.data()[i]];
            }
        }

        return (totalBytes * 8) / m_trackInfo.channels;
    }

    // ══════════════════════════════════════════════════════════════
    // PCM MODE - Normal decoding with resampling
    // ══════════════════════════════════════════════════════════════

    if (!m_codecContext || m_eof) {
        return 0;
    }

    // Initialize resampler if needed (not for DSD)
    // Check m_resamplerInitialized instead of m_swrContext because
    // bypass mode doesn't use swrContext but still counts as initialized
    if (!m_trackInfo.isDSD && !m_resamplerInitialized) {
        if (!initResampler(outputRate, outputBits)) {
            return 0;
        }
    }

    size_t totalSamplesRead = 0;
    // CRITICAL FIX: 24-bit uses S32 container (4 bytes), not 3!
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

    // First, drain any samples from PCM FIFO (O(1) circular buffer read)
    if (m_pcmFifo && av_audio_fifo_size(m_pcmFifo) > 0) {
        int fifoSamples = av_audio_fifo_size(m_pcmFifo);
        int samplesToRead = std::min(fifoSamples, (int)(numSamples - totalSamplesRead));

        uint8_t* readPtrs[1] = { outputPtr };
        int samplesRead = av_audio_fifo_read(m_pcmFifo, (void**)readPtrs, samplesToRead);

        if (samplesRead > 0) {
            outputPtr += samplesRead * bytesPerSample;
            totalSamplesRead += samplesRead;
        }

        // If FIFO provided enough samples, return early
        if (totalSamplesRead >= numSamples) {
            return totalSamplesRead;
        }
    }

    // Lazy initialization of reusable structures (allocated once, reused via unref)
    if (!m_packet) {
        m_packet = av_packet_alloc();
    }
    if (!m_frame) {
        m_frame = av_frame_alloc();
    }

    if (!m_packet || !m_frame) {
        return totalSamplesRead; // Retourner ce qu'on a déjà lu du buffer
    }

    while (totalSamplesRead < numSamples && !m_eof) {
        // Read packet
        int ret = av_read_frame(m_formatContext, m_packet);

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
                std::cerr << "[AudioDecoder] Timeout - connection too slow or lost" << std::endl;
                m_eof = true;
            } else if (ret == AVERROR(ECONNRESET)) {
                std::cerr << "[AudioDecoder] Connection reset by server" << std::endl;
                m_eof = true;
            } else if (ret == AVERROR_EXIT) {
                std::cerr << "[AudioDecoder] Exit requested" << std::endl;
                m_eof = true;
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                std::cerr << "[AudioDecoder] Read error (" << ret << "): " << errbuf << std::endl;
                m_eof = true;
            }
            break;
        }

        // Skip non-audio packets
        if (m_packet->stream_index != m_audioStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(m_codecContext, m_packet);
        av_packet_unref(m_packet);

        if (ret < 0) {
            std::cerr << "[AudioDecoder] Error sending packet to decoder" << std::endl;
            break;
        }

        // Receive decoded frames
        while (ret >= 0 && totalSamplesRead < numSamples) {
            ret = avcodec_receive_frame(m_codecContext, m_frame);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cerr << "[AudioDecoder] Error receiving frame from decoder" << std::endl;
                av_frame_unref(m_frame);
                return totalSamplesRead;
            }

            // Process frame
            size_t frameSamples = m_frame->nb_samples;

            if (m_trackInfo.isDSD) {
                // DSD: Direct copy (no resampling!)
                size_t bytesToCopy = frameSamples * m_trackInfo.channels;
                size_t remainingSpace = (numSamples - totalSamplesRead) * bytesPerSample;

                if (bytesToCopy > remainingSpace) {
                    bytesToCopy = remainingSpace;
                    frameSamples = bytesToCopy / m_trackInfo.channels;
                }

                // Copy DSD data
                if (m_frame->format == AV_SAMPLE_FMT_U8) {
                    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
                } else if (m_frame->format == AV_SAMPLE_FMT_U8P) {
                    // Planar to interleaved
                    for (size_t i = 0; i < frameSamples; i++) {
                        for (uint32_t ch = 0; ch < m_trackInfo.channels; ch++) {
                            *outputPtr++ = m_frame->data[ch][i];
                        }
                    }
                    outputPtr -= bytesToCopy; // Reset pointer after increment
                }

                outputPtr += bytesToCopy;
                totalSamplesRead += frameSamples;

            } else {
                // PCM: Resample if needed, or bypass for bit-perfect playback
                size_t samplesNeeded = numSamples - totalSamplesRead;

                // Check for bypass format mismatch (canBypass checked codec context,
                // but actual frame format could differ at runtime)
                if (m_bypassMode) {
                    AVSampleFormat expectedFmt = (outputBits == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
                    AVSampleFormat frameFmt = (AVSampleFormat)m_frame->format;
                    bool formatMismatch = (frameFmt != expectedFmt);
                    bool isPlanar = av_sample_fmt_is_planar(frameFmt);

                    if (formatMismatch || isPlanar) {
                        std::cerr << "[AudioDecoder] BYPASS MISMATCH: frame format "
                                  << av_get_sample_fmt_name(frameFmt)
                                  << (isPlanar ? " (PLANAR)" : "")
                                  << " != expected " << av_get_sample_fmt_name(expectedFmt)
                                  << " - falling back to resampler" << std::endl;
                        // Disable bypass and reinitialize resampler
                        m_bypassMode = false;
                        m_resamplerInitialized = false;
                        if (!initResampler(outputRate, outputBits)) {
                            return totalSamplesRead;
                        }
                    }
                }

                if (m_bypassMode) {
                    // BYPASS PATH: Direct copy from decoded frame (bit-perfect)
                    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
                    size_t bytesToCopy = samplesToCopy * bytesPerSample;

                    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
                    outputPtr += bytesToCopy;
                    totalSamplesRead += samplesToCopy;

                    // Store excess samples in FIFO
                    if (frameSamples > samplesToCopy && m_pcmFifo) {
                        size_t excess = frameSamples - samplesToCopy;
                        uint8_t* excessPtr = m_frame->data[0] + bytesToCopy;
                        uint8_t* excessPtrs[1] = { excessPtr };

                        int written = av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
                        if (written < 0) {
                            std::cerr << "[AudioDecoder] FIFO write failed (bypass): " << written << std::endl;
                        }
                    }
                } else if (m_swrContext) {
                    // D2: Use cached resampler delay (refreshed every DELAY_REFRESH_INTERVAL frames)
                    // swr_get_delay() stabilizes quickly - no need to call every frame
                    if (++m_delayRefreshCounter >= DELAY_REFRESH_INTERVAL) {
                        m_cachedResamplerDelay = swr_get_delay(m_swrContext, m_codecContext->sample_rate);
                        m_delayRefreshCounter = 0;
                    }

                    // Calculate TOTAL output samples (without limiting)
                    int64_t totalOutSamples = av_rescale_rnd(
                        m_cachedResamplerDelay + frameSamples,
                        outputRate,
                        m_codecContext->sample_rate,
                        AV_ROUND_UP
                    );

                    // Reuse member buffer with capacity growth (eliminates per-call allocation)
                    size_t tempBufferSize = totalOutSamples * bytesPerSample;
                    if (tempBufferSize > m_resampleBufferCapacity) {
                        // Should not happen with pre-allocated 256KB buffer
                        // Log warning but don't block - fall back to dynamic allocation
                        DEBUG_LOG("[AudioDecoder] WARNING: Resampler buffer insufficient: "
                                  << tempBufferSize << " > " << m_resampleBufferCapacity
                                  << " - pre-allocation may need increase");
                        // Fall back to dynamic allocation only if absolutely necessary
                        size_t newCapacity = static_cast<size_t>(tempBufferSize * 1.5);
                        m_resampleBuffer.resize(newCapacity);
                        m_resampleBufferCapacity = m_resampleBuffer.size();
                    }
                    uint8_t* tempPtr = m_resampleBuffer.data();

                    // Convertir TOUTE la frame
                    int convertedSamples = swr_convert(
                        m_swrContext,
                        &tempPtr,
                        totalOutSamples,
                        (const uint8_t**)m_frame->data,
                        frameSamples
                    );

                    if (convertedSamples > 0) {
                        // Déterminer combien on peut utiliser maintenant
                        size_t samplesToUse = std::min((size_t)convertedSamples, samplesNeeded);
                        size_t bytesToUse = samplesToUse * bytesPerSample;

                        // Copier vers le buffer de sortie
                        memcpy_audio(outputPtr, m_resampleBuffer.data(), bytesToUse);
                        outputPtr += bytesToUse;
                        totalSamplesRead += samplesToUse;

                        // Store excess samples in FIFO (O(1) write to circular buffer)
                        if ((size_t)convertedSamples > samplesToUse && m_pcmFifo) {
                            size_t excess = convertedSamples - samplesToUse;
                            uint8_t* excessPtr = m_resampleBuffer.data() + bytesToUse;
                            uint8_t* excessPtrs[1] = { excessPtr };

                            int written = av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
                            if (written < 0) {
                                std::cerr << "[AudioDecoder] FIFO write failed: " << written << std::endl;
                            } else if (!m_resamplerInitLogged) {
                                std::cout << "[AudioDecoder] FIFO buffering " << excess
                                          << " excess samples for next read" << std::endl;
                                m_resamplerInitLogged = true;
                            }
                        }
                    }
                } else {
                    // No resampling - direct copy
                    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
                    size_t bytesToCopy = samplesToCopy * bytesPerSample;

                    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
                    outputPtr += bytesToCopy;
                    totalSamplesRead += samplesToCopy;

                    // Store excess samples in FIFO (O(1) write to circular buffer)
                    if (frameSamples > samplesToCopy && m_pcmFifo) {
                        size_t excess = frameSamples - samplesToCopy;
                        uint8_t* excessPtr = m_frame->data[0] + bytesToCopy;
                        uint8_t* excessPtrs[1] = { excessPtr };

                        int written = av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
                        if (written < 0) {
                            std::cerr << "[AudioDecoder] FIFO write failed: " << written << std::endl;
                        } else {
                            std::cout << "[AudioDecoder] FIFO buffering " << excess
                                      << " excess samples (no resampling)" << std::endl;
                        }
                    }
                }
            }

            av_frame_unref(m_frame);
        }
    }

    // Unref for reuse (no deallocation - freed in close())
    av_packet_unref(m_packet);
    av_frame_unref(m_frame);

    return totalSamplesRead;
}

bool AudioDecoder::initResampler(uint32_t outputRate, uint32_t outputBits) {
    // Don't resample DSD!
    if (m_trackInfo.isDSD) {
        std::cout << "[AudioDecoder] DSD: No resampling, native passthrough" << std::endl;
        return true;
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

    // Check if we can bypass resampling entirely (bit-perfect path)
    if (canBypass(outputRate, outputBits)) {
        std::cout << "[AudioDecoder] PCM BYPASS enabled - bit-perfect path ("
                  << av_get_sample_fmt_name(m_codecContext->sample_fmt) << "/"
                  << outputRate << "Hz/" << outputBits << "bit)" << std::endl;

        // Free existing resampler if any
        if (m_swrContext) {
            swr_free(&m_swrContext);
        }

        // Still need FIFO for frame overflow handling
        if (m_pcmFifo) {
            av_audio_fifo_free(m_pcmFifo);
            m_pcmFifo = nullptr;
        }

        // Smaller FIFO for bypass (less overflow expected)
        int fifoSize = 8192;
        if (outputRate > 192000) fifoSize = 32768;

        m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSize);
        if (!m_pcmFifo) {
            std::cerr << "[AudioDecoder] Failed to allocate PCM FIFO for bypass" << std::endl;
            m_bypassMode = false;
            m_resamplerInitialized = false;
            return false;
        }

        m_bypassMode = true;
        m_resamplerInitialized = true;
        return true;
    }

    m_bypassMode = false;

    // Free existing resampler
    if (m_swrContext) {
        swr_free(&m_swrContext);
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

    // Initialize PCM FIFO for overflow handling (O(1) circular buffer)
    // Dynamic sizing based on sample rate to handle high-res formats
    if (m_pcmFifo) {
        av_audio_fifo_free(m_pcmFifo);
        m_pcmFifo = nullptr;
    }

    // FIFO size: scale with sample rate using 64-bit math to avoid overflow
    // Base: 8192 samples at 48kHz, scales proportionally
    // 384kHz: ~65536 samples, 768kHz: ~131072 samples
    int fifoSize = static_cast<int>((static_cast<int64_t>(8192) * outputRate) / 48000);
    if (fifoSize < 4096) fifoSize = 4096;    // Minimum for stability
    if (fifoSize > 262144) fifoSize = 262144; // Maximum reasonable size

    m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSize);
    if (!m_pcmFifo) {
        std::cerr << "[AudioDecoder] Failed to allocate PCM FIFO" << std::endl;
        swr_free(&m_swrContext);
        return false;
    }

    std::cout << "[AudioDecoder] Resampler: " << m_codecContext->sample_rate
              << "Hz -> " << outputRate << "Hz, " << outputBits << "bit"
              << " (FIFO: " << fifoSize << " samples)" << std::endl;

    // Pre-allocate resampler buffer to fixed capacity (eliminates hot-path allocation)
    // 256KB covers up to 768kHz/32-bit stereo with headroom
    static constexpr size_t RESAMPLER_BUFFER_CAPACITY = 262144;
    if (m_resampleBuffer.size() < RESAMPLER_BUFFER_CAPACITY) {
        m_resampleBuffer.resize(RESAMPLER_BUFFER_CAPACITY);
        m_resampleBufferCapacity = RESAMPLER_BUFFER_CAPACITY;
        DEBUG_LOG("[AudioDecoder] Pre-allocated resampler buffer: " << RESAMPLER_BUFFER_CAPACITY << " bytes");
    }

    // D2: Initialize cached delay (will be refreshed periodically in readSamples)
    m_cachedResamplerDelay = swr_get_delay(m_swrContext, m_codecContext->sample_rate);
    m_delayRefreshCounter = 0;

    m_resamplerInitialized = true;
    return true;
}

/**
 * @brief Check if PCM bypass mode can be used
 *
 * Bypass skips the SwrContext entirely for bit-perfect playback when:
 * - Format is uncompressed (WAV, AIFF) - NOT FLAC, ALAC, etc.
 * - Sample rates match exactly
 * - Channel counts match
 * - Format is packed integer (S16 or S32) - NOT planar, NOT float
 * - Bit depth matches (or is S32 container with 24-bit content)
 *
 * Compressed formats (FLAC, ALAC) are NEVER bypassed - they decode to
 * planar format which requires conversion through SwrContext.
 */
bool AudioDecoder::canBypass(uint32_t outputRate, uint32_t outputBits) const {
    // DSD never uses bypass (handled separately)
    if (m_trackInfo.isDSD) {
        return false;
    }

    // Compressed formats (FLAC, ALAC, etc.) NEVER bypass
    // They decode to planar format which requires conversion
    if (m_trackInfo.isCompressed) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (compressed format requires decoding)");
        return false;
    }

    if (!m_codecContext) {
        return false;
    }

    // Sample rate must match exactly
    if (m_codecContext->sample_rate != (int)outputRate) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (sample rate mismatch: "
                  << m_codecContext->sample_rate << " vs " << outputRate << ")");
        return false;
    }

    // Channel count must match
    if (m_codecContext->ch_layout.nb_channels != (int)m_trackInfo.channels) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (channel mismatch)");
        return false;
    }

    // Format must be packed integer (NOT planar, NOT float)
    AVSampleFormat fmt = m_codecContext->sample_fmt;

    // Explicit planar check - planar formats would cause "accelerated" playback
    // because data[0] would only contain one channel
    if (av_sample_fmt_is_planar(fmt)) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (planar format " << av_get_sample_fmt_name(fmt)
                  << " requires interleaving)");
        return false;
    }

    bool isPackedInteger = (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S32);

    if (!isPackedInteger) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (format " << av_get_sample_fmt_name(fmt)
                  << " requires conversion)");
        return false;
    }

    // Bit depth must match (accounting for S32 container with 24-bit content)
    bool bitDepthMatch = false;
    if (outputBits == 16 && fmt == AV_SAMPLE_FMT_S16) {
        bitDepthMatch = true;
    } else if ((outputBits == 24 || outputBits == 32) && fmt == AV_SAMPLE_FMT_S32) {
        bitDepthMatch = true;
    }

    if (!bitDepthMatch) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (bit depth mismatch: "
                  << m_trackInfo.bitDepth << " vs " << outputBits << ")");
        return false;
    }

    DEBUG_LOG("[AudioDecoder] canBypass: YES (bit-perfect path enabled)");
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

    // NOUVEAU : Forcer la réouverture même si l'URI est la même (pour Stop)
    if (uriChanged || forceReopen) {
        std::cout << "[AudioEngine] "
                  << (forceReopen ? "Forced reopen" : "URI changed")
                  << " - closing decoders to load new track" << std::endl;

        // Fermer les décodeurs pour forcer réouverture
        m_currentDecoder.reset();
        m_nextDecoder.reset();

        // CRITICAL FIX: Clear gapless queue when changing URI
        // Otherwise, the old "next track" will play after the new track finishes!
        {
            std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
            m_pendingNextURI.clear();
            m_pendingNextMetadata.clear();
            m_pendingNextTrack.store(false, std::memory_order_release);
        }
        m_nextURI.clear();
        m_nextMetadata.clear();

        std::cout << "[AudioEngine] Gapless queue cleared" << std::endl;

        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_silenceCount = 0;
        m_isDraining = false;

        // Arrêter le préchargement en cours si existant
        if (m_preloadRunning.load(std::memory_order_acquire)) {
            m_preloadRunning.store(false, std::memory_order_release);
            std::cout << "[AudioEngine] Cancelling ongoing preload" << std::endl;
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

    std::cout << "[AudioEngine] State changed to STOPPED" << std::endl;

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

        std::cout << "[AudioEngine] Full cleanup completed" << std::endl;
    } else {
        std::cout << "[AudioEngine] Mutex busy, cleanup deferred" << std::endl;
        // Le cleanup sera fait au prochain process() qui verra l'état STOPPED
    }
}


void AudioEngine::pause() {
    std::cout << "[AudioEngine] Pause requested" << std::endl;

    // NE PAS bloquer sur le mutex !
    // Changer l'état directement (m_state est atomique)
    State expected = State::PLAYING;  // Correct type
    if (m_state.compare_exchange_strong(expected, State::PAUSED)) {
        std::cout << "[AudioEngine] State changed to PAUSED" << std::endl;
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
    // CRITICAL: Process async seek request (lock-free check)
    // This runs in the audio thread, so we can safely take the mutex
    if (m_seekRequested.load(std::memory_order_acquire)) {
        double targetSeconds = m_seekTarget.load(std::memory_order_acquire);
        m_seekRequested.store(false, std::memory_order_release);

        std::cout << "[AudioEngine] Processing async seek to " << targetSeconds << "s" << std::endl;

        // Now we can safely take the mutex (we're in the audio thread)
        std::lock_guard<std::mutex> seekLock(m_mutex);

        // Validate decoder exists
        if (!m_currentDecoder) {
            std::cerr << "[AudioEngine] No decoder for seek" << std::endl;
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

                    std::cout << "[AudioEngine] Seek completed to " << targetSeconds << "s" << std::endl;
                    DEBUG_LOG("[AudioEngine] Position updated to "
                              << m_samplesPlayed << " samples (" << targetSeconds << "s)");
                } else {
                    std::cerr << "[AudioEngine] Seek failed in decoder" << std::endl;
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

    // CRITICAL: Preload next track as soon as EOF flag is set (for gapless)
    // Check AFTER readSamples() because EOF flag is set during the read
    if (!m_nextDecoder && !m_nextURI.empty() && m_currentDecoder->isEOF()) {
        std::cout << "[AudioEngine] EOF flag detected, preloading next track for gapless..." << std::endl;
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
            std::cout << "[AudioEngine] No more samples available from decoder" << std::endl;
            m_isDraining = true;
            m_silenceCount = 0;
        }

        // Check if we have a next track ready for gapless
        if (m_nextDecoder) {
            std::cout << "[AudioEngine] Transitioning to next track (gapless)..." << std::endl;
            m_isDraining = false;
            transitionToNextTrack();
            return true;  // Continue playback with new track
        }

        // NEW (v1.0.16): Check if next track exists but decoder was cleared (format change)
        if (!m_nextURI.empty()) {
            std::cout << "[AudioEngine] Next track with format change detected" << std::endl;
            std::cout << "[AudioEngine] Transitioning with stop/start sequence..." << std::endl;

            // Save next URI before stopping
            std::string nextURI = m_nextURI;
            std::string nextMetadata = m_nextMetadata;

            // NOTE: Do NOT call m_trackEndCallback() here!
            // trackEndCallback is for playlist END (releases Diretta target).
            // For format changes, we want to keep the connection alive and
            // let DirettaSync::open() handle the format transition.

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
        std::cout << "[AudioEngine] No next track, draining buffer..." << std::endl;

        if (m_silenceCount == 0) {
            std::cout << "[AudioEngine] No next track, waiting for Diretta drain..." << std::endl;
        }

        m_silenceCount++;

        // After a short wait to ensure last samples were sent, signal stop
        // Diretta has ~2-4s of buffer, but we don't need to send silence
        // The stop() function will wait for buffer_empty()
        if (m_silenceCount > 5) {  // 5 * ~92ms = ~500ms safety margin
            std::cout << "[AudioEngine] Last samples sent, signaling stop" << std::endl;
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

    std::cout << "[AudioEngine] Track opened: ";
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
        DEBUG_LOG("[AudioEngine] FORMAT CHANGE DETECTED - Gapless disabled");
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
        DEBUG_LOG("[AudioEngine] Will use stop/start sequence instead of gapless");

        // Don't keep nextDecoder - force stop/start sequence
        m_nextDecoder.reset();

        // CRITICAL FIX (v1.0.16): Keep m_nextURI!
        // Do NOT clear m_nextURI - it will be used for non-gapless transition
        // The EOF handler will see format change and trigger proper reopen
        // m_nextURI.clear();     // REMOVED - was causing next track to be lost
        // m_nextMetadata.clear(); // REMOVED

        return false;
    }

    DEBUG_LOG("[AudioEngine] Next track preloaded: "
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

    // DSD native mode - seek at container level (no codec involved)
    if (m_rawDSD) {
        std::cout << "[AudioDecoder] DSD seek to " << seconds << " seconds..." << std::endl;

        AVStream* stream = m_formatContext->streams[m_audioStreamIndex];
        int64_t timestamp = av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE),
            AV_TIME_BASE_Q,
            stream->time_base
        );

        int ret = av_seek_frame(m_formatContext, m_audioStreamIndex,
                                timestamp, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cerr << "[AudioDecoder] DSD seek failed: " << errbuf << std::endl;
            return false;
        }

        // Clear stale DSD buffered data from before the seek
        dsdRemainderClear();
        m_eof = false;

        // Reset packet counter for cleaner debug output
        m_packetCount = 0;

        std::cout << "[AudioDecoder] DSD seek successful to ~" << seconds << "s" << std::endl;
        return true;
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

    // Reset PCM FIFO (clear stale samples from before the seek)
    if (m_pcmFifo) {
        av_audio_fifo_reset(m_pcmFifo);
    }
    m_eof = false;

    std::cout << "[AudioDecoder] Seek successful to ~" << seconds << "s" << std::endl;

    return true;
}

// ============================================================================
// AudioEngine::seek() - Seek avec mise à jour de la position
// ============================================================================

bool AudioEngine::seek(double seconds) {
    // CRITICAL FIX: Async seek to avoid deadlock
    // The UPnP thread calling this should not block waiting for mutex
    // Instead, we set atomic flags and let the audio thread handle the seek

    std::cout << "[AudioEngine] Seek requested to " << seconds << " seconds (async)" << std::endl;

    // Quick validation without mutex
    if (m_state.load(std::memory_order_acquire) != State::PLAYING) {
        std::cerr << "[AudioEngine] Cannot seek when not playing" << std::endl;
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

    // Set seek request atomically (lock-free, non-blocking)
    m_seekTarget.store(seconds, std::memory_order_release);
    m_seekRequested.store(true, std::memory_order_release);

    std::cout << "[AudioEngine] Seek queued, will be processed by audio thread" << std::endl;

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
