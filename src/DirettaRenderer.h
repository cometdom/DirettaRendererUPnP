#pragma once

#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>

// Forward declarations
class UPnPDevice;
class AudioEngine;
class DirettaOutput;

class DirettaRenderer {
public:
    struct Config {
        std::string name;
        int port;
        std::string uuid;
        bool gaplessEnabled;
        float bufferSeconds;  // Changed from int to float (v1.0.9)
        int targetIndex;  // -1 = interactive selection, >= 0 = specific target
        
        Config();
    };
    
    DirettaRenderer(const Config& config);
    ~DirettaRenderer();
    
    bool start();
    void stop();
    
    bool isRunning() const { return m_running; }
    
private:
    // UPnP Callbacks
    void onSetURI(const std::string& uri, const std::string& metadata);
    void onSetNextURI(const std::string& uri, const std::string& metadata);
    void onPlay();
    void onPause();
    void onStop();
    void onSeek(const std::string& target);
    
    // Thread functions
    void audioThreadFunc();
    void upnpThreadFunc();
    void ssdpThreadFunc();
    void positionThreadFunc();  // → NOUVEAU : mise à jour position pour eventing
    
    // Internal methods
    void updatePosition();
    void handleEOF();
    
    // Configuration
    Config m_config;
    uint32_t m_networkMTU = 16128;  // ⭐ MTU hardcodé pour performance maximale
    
    // Components
    std::unique_ptr<UPnPDevice> m_upnp;
    std::unique_ptr<AudioEngine> m_audioEngine;
    std::unique_ptr<DirettaOutput> m_direttaOutput;
    
    // Threads
    std::thread m_audioThread;
    std::thread m_upnpThread;
    std::thread m_ssdpThread;
    std::thread m_positionThread;  // → NOUVEAU : eventing de position
    
    // State
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    
    // Gapless
    std::string m_currentURI;
    std::string m_currentMetadata;
    std::string m_nextURI;
    std::string m_nextMetadata;

    // Callback synchronization - prevents race with close()
    mutable std::mutex m_callbackMutex;
    std::condition_variable m_callbackCV;
    bool m_callbackRunning{false};

    void waitForCallbackComplete() {
        std::unique_lock<std::mutex> lk(m_callbackMutex);
        bool completed = m_callbackCV.wait_for(lk, std::chrono::seconds(5),
            [this]{ return !m_callbackRunning; });
        if (!completed) {
            std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
            m_callbackRunning = false;
        }
    }
};
