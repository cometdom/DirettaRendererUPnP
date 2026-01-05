/**
 * @file DirettaRenderer.h
 * @brief Simplified Diretta UPnP Renderer
 *
 * Refactored to use unified DirettaSync class.
 * Connection and format management delegated to DirettaSync.
 */

#pragma once

#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iostream>

// Forward declarations
class UPnPDevice;
class AudioEngine;
class DirettaSync;
struct AudioFormat;

class DirettaRenderer {
public:
    struct Config {
        std::string name = "Diretta UPnP Renderer";
        int port = 49152;
        std::string uuid;
        bool gaplessEnabled = true;
        int targetIndex = -1;  // -1 = interactive, >= 0 = specific
        std::string networkInterface;  // Empty = auto-detect

        Config();
    };

    DirettaRenderer(const Config& config);
    ~DirettaRenderer();

    bool start();
    void stop();

    bool isRunning() const { return m_running; }

private:
    // Thread functions
    void audioThreadFunc();
    void upnpThreadFunc();
    void positionThreadFunc();

    // Helper to wait for audio callback completion
    void waitForCallbackComplete();

    // Configuration
    Config m_config;

    // Components
    std::unique_ptr<UPnPDevice> m_upnp;
    std::unique_ptr<AudioEngine> m_audioEngine;
    std::unique_ptr<DirettaSync> m_direttaSync;

    // Threads
    std::thread m_audioThread;
    std::thread m_upnpThread;
    std::thread m_positionThread;

    // State
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;

    // Current track info
    std::string m_currentURI;
    std::string m_currentMetadata;

    // Callback synchronization
    mutable std::mutex m_callbackMutex;
    std::condition_variable m_callbackCV;
    bool m_callbackRunning{false};

    // DAC stabilization timing
    std::chrono::steady_clock::time_point m_lastStopTime;
};
