#ifndef PROTOCOL_INFO_BUILDER_H
#define PROTOCOL_INFO_BUILDER_H

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

/**
 * @brief Builder class for generating UPnP ProtocolInfo strings
 * based on Diretta target capabilities
 */
class ProtocolInfoBuilder {
public:
    struct AudioCapabilities {
        // PCM capabilities
        std::vector<int> pcmRates;
        int pcmBits;              // 16, 24, or 32
        int pcmChannels;          // typically 2 for stereo
        
        // DSD capabilities
        std::vector<int> dsdRates;
        int dsdChannels;          // typically 2 for stereo
        
        // Codec support flags
        bool supportFLAC;
        bool supportMP3;
        bool supportAAC;
        bool supportWAV;
        
        AudioCapabilities() 
            : pcmBits(32)
            , pcmChannels(2)
            , dsdChannels(2)
            , supportFLAC(true)
            , supportMP3(true)
            , supportAAC(true)
            , supportWAV(true)
        {}
    };

    /**
     * @brief Initialize with full Holo Audio Spring 3 Level 2 capabilities
     * as detected from Diretta target
     */
    static AudioCapabilities getHoloAudioCapabilities() {
        AudioCapabilities caps;
        
        // PCM rates from 44.1 kHz to 1536 kHz
        caps.pcmRates = {
            44100, 48000, 88200, 96000, 176400, 192000,
            352800, 384000, 705600, 768000, 1411200, 1536000
        };
        
        // DSD rates from DSD64 to DSD1024
        // Family 44.1x
        caps.dsdRates = {
            2822400,   // DSD64
            5644800,   // DSD128
            11289600,  // DSD256
            22579200,  // DSD512
            45158400,  // DSD1024
            // Family 48x
            3072000,   // DSD64 (48x)
            6144000,   // DSD128 (48x)
            12288000,  // DSD256 (48x)
            24576000,  // DSD512 (48x)
            49152000   // DSD1024 (48x)
        };
        
        caps.pcmBits = 32;
        caps.pcmChannels = 2;
        caps.dsdChannels = 2;
        
        return caps;
    }

    /**
     * @brief Build complete ProtocolInfo string for UPnP GetProtocolInfo response
     */
    static std::string buildProtocolInfo(const AudioCapabilities& caps) {
        std::vector<std::string> protocols;
        
        // Add PCM formats (uncompressed)
        addPCMProtocols(protocols, caps);
        
        // Add DSD formats
        addDSDProtocols(protocols, caps);
        
        // Add compressed formats
        if (caps.supportFLAC) {
            protocols.push_back("http-get:*:audio/flac:*");
            protocols.push_back("http-get:*:audio/x-flac:*");
        }
        
        if (caps.supportWAV) {
            protocols.push_back("http-get:*:audio/wav:*");
            protocols.push_back("http-get:*:audio/x-wav:*");
        }
        
        if (caps.supportMP3) {
            protocols.push_back("http-get:*:audio/mpeg:*");
            protocols.push_back("http-get:*:audio/mp3:*");
        }
        
        if (caps.supportAAC) {
            protocols.push_back("http-get:*:audio/aac:*");
            protocols.push_back("http-get:*:audio/mp4:*");
            protocols.push_back("http-get:*:audio/x-m4a:*");
        }
        
        // Join all protocols with comma separator
        return joinProtocols(protocols);
    }

    /**
     * @brief Get DSD designation string (DSD64, DSD128, etc.)
     */
    static std::string getDSDDesignation(int rate) {
        switch(rate) {
            case 2822400:  return "DSD64";
            case 3072000:  return "DSD64_48x";
            case 5644800:  return "DSD128";
            case 6144000:  return "DSD128_48x";
            case 11289600: return "DSD256";
            case 12288000: return "DSD256_48x";
            case 22579200: return "DSD512";
            case 24576000: return "DSD512_48x";
            case 45158400: return "DSD1024";
            case 49152000: return "DSD1024_48x";
            default:       return "DSD_UNKNOWN";
        }
    }

    /**
     * @brief Get PCM designation string for logging
     */
    static std::string getPCMDesignation(int rate) {
        if (rate >= 1000000) {
            return std::to_string(rate / 1000) + "kHz";
        } else if (rate >= 1000) {
            double khz = rate / 1000.0;
            std::ostringstream oss;
            oss << khz << "kHz";
            return oss.str();
        }
        return std::to_string(rate) + "Hz";
    }

private:
    static void addPCMProtocols(std::vector<std::string>& protocols, 
                                const AudioCapabilities& caps) {
        for (int rate : caps.pcmRates) {
            std::ostringstream oss;
            
            // Standard L16 format (network byte order, big-endian)
            oss << "http-get:*:audio/L16;rate=" << rate 
                << ";channels=" << caps.pcmChannels << ":*";
            protocols.push_back(oss.str());
            
            // Alternative L24 and L32 formats for high-resolution
            if (caps.pcmBits >= 24) {
                oss.str("");
                oss << "http-get:*:audio/L24;rate=" << rate 
                    << ";channels=" << caps.pcmChannels << ":*";
                protocols.push_back(oss.str());
            }
            
            if (caps.pcmBits == 32) {
                oss.str("");
                oss << "http-get:*:audio/L32;rate=" << rate 
                    << ";channels=" << caps.pcmChannels << ":*";
                protocols.push_back(oss.str());
            }
        }
    }

    static void addDSDProtocols(std::vector<std::string>& protocols, 
                                const AudioCapabilities& caps) {
        for (int rate : caps.dsdRates) {
            std::ostringstream oss;
            
            // Native DSD format
            oss << "http-get:*:audio/dsd;rate=" << rate 
                << ";channels=" << caps.dsdChannels << ":*";
            protocols.push_back(oss.str());
            
            // Alternative DSD MIME types
            oss.str("");
            oss << "http-get:*:audio/x-dsd;rate=" << rate 
                << ";channels=" << caps.dsdChannels << ":*";
            protocols.push_back(oss.str());
            
            // DSD over PCM (DoP) - rate is doubled for DoP
            if (rate <= 11289600) { // DoP typically limited to DSD256
                int dopRate = rate / 16; // DoP packs 16 DSD bits into PCM samples
                oss.str("");
                oss << "http-get:*:audio/L24;rate=" << dopRate 
                    << ";channels=" << caps.dsdChannels << ":DLNA.ORG_PN=DSD";
                protocols.push_back(oss.str());
            }
        }
    }

    static std::string joinProtocols(const std::vector<std::string>& protocols) {
        if (protocols.empty()) {
            return "";
        }
        
        std::ostringstream result;
        result << protocols[0];
        
        for (size_t i = 1; i < protocols.size(); ++i) {
            result << "," << protocols[i];
        }
        
        return result.str();
    }
};

/**
 * @brief Helper class for parsing Diretta target capabilities from logs
 */
class DirettaCapabilityParser {
public:
    /**
     * @brief Parse ALSA capabilities from Diretta target log lines
     * Example log format:
     *   "support PCM 44100"
     *   "support DSD 2822400"
     *   "support rate_min 44100"
     *   "support rate max 1536000"
     */
    static ProtocolInfoBuilder::AudioCapabilities parseFromLog(
        const std::vector<std::string>& logLines) {
        
        ProtocolInfoBuilder::AudioCapabilities caps;
        
        for (const auto& line : logLines) {
            if (line.find("support PCM") != std::string::npos) {
                int rate = extractRate(line);
                if (rate > 0) {
                    caps.pcmRates.push_back(rate);
                }
            }
            else if (line.find("support DSD") != std::string::npos) {
                int rate = extractRate(line);
                if (rate > 0) {
                    caps.dsdRates.push_back(rate);
                }
            }
            else if (line.find("support SND_PCM_FORMAT_S32") != std::string::npos) {
                caps.pcmBits = 32;
            }
            else if (line.find("support SND_PCM_FORMAT_S24") != std::string::npos) {
                if (caps.pcmBits < 24) caps.pcmBits = 24;
            }
            else if (line.find("support ch max") != std::string::npos) {
                int channels = extractRate(line);
                if (channels > 0) {
                    caps.pcmChannels = channels;
                    caps.dsdChannels = channels;
                }
            }
        }
        
        // Sort rates for clean output
        std::sort(caps.pcmRates.begin(), caps.pcmRates.end());
        std::sort(caps.dsdRates.begin(), caps.dsdRates.end());
        
        return caps;
    }

private:
    static int extractRate(const std::string& line) {
        // Find last number in the line
        size_t pos = line.find_last_of(' ');
        if (pos != std::string::npos) {
            try {
                return std::stoi(line.substr(pos + 1));
            } catch (...) {
                return -1;
            }
        }
        return -1;
    }
};

#endif // PROTOCOL_INFO_BUILDER_H
