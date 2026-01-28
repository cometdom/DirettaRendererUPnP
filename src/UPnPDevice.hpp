#pragma once

#include <upnp/upnp.h>
#include <upnp/ixml.h>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include "ProtocolInfoBuilder.h"

/**
 * UPnP MediaRenderer Device using libupnp
 * 
 * Handles:
 * - SSDP Discovery (automatic)
 * - SOAP Actions (AVTransport, RenderingControl)
 * - Event Notifications (automatic subscriptions)
 * - State management
 */
class UPnPDevice {
public:
    // Callbacks to DirettaRenderer
    using SetURICallback = std::function<void(const std::string& uri, const std::string& metadata)>;
    using SetNextURICallback = std::function<void(const std::string& uri, const std::string& metadata)>;
    using PlayCallback = std::function<void()>;
    using PauseCallback = std::function<void()>;
    using StopCallback = std::function<void()>;
    using SeekCallback = std::function<void(const std::string& target)>;
    
    struct Callbacks {
        SetURICallback onSetURI;
        SetNextURICallback onSetNextURI;
        PlayCallback onPlay;
        PauseCallback onPause;
        StopCallback onStop;
        SeekCallback onSeek;
    };
    
    struct Config {
        std::string friendlyName;
        std::string manufacturer;
        std::string modelName;
        std::string uuid;
        int port;
        std::string networkInterface;
        
        Config() 
            : friendlyName("Diretta Renderer")
            , manufacturer("DIY Audio")
            , modelName("UPnP Diretta Renderer")
            , uuid("uuid:diretta-renderer-12345")
            , port(0)  // 0 = auto
            , networkInterface("")  // Empty = auto-detect
        {}
    };
    
    UPnPDevice(const Config& config);
    ~UPnPDevice();
    
    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const { return m_running; }
    
    // Callbacks
    void setCallbacks(const Callbacks& callbacks);
    
    // State notifications (automatic event sending to subscribers)
    void notifyStateChange(const std::string& state);
    void notifyTrackChange(const std::string& uri, const std::string& metadata);
    void notifyPositionChange(int seconds, int duration);
    
    // Getters
    std::string getDeviceURL() const;
    std::string getIPAddress() const;
    int getPort() const;
    
    // State queries (for GetTransportInfo, GetPositionInfo)
    std::string getCurrentState() const;
    std::string getCurrentURI() const;
    std::string getCurrentMetadata() const;
    int getCurrentPosition() const;
    int getTrackDuration() const;
    
    // State setters (from AudioEngine)
    void setCurrentPosition(int seconds);
    void setTrackDuration(int seconds);
    void setCurrentURI(const std::string& uri);
    void setCurrentMetadata(const std::string& metadata);

private:
    // libupnp callback (static)
    static int upnpCallbackStatic(Upnp_EventType eventType, 
                                  const void* event, 
                                  void* cookie);
    
    // Instance callback
    int upnpCallback(Upnp_EventType eventType, const void* event);
    
    // Handlers
    int handleActionRequest(UpnpActionRequest* request);
    int handleSubscriptionRequest(UpnpSubscriptionRequest* request);
    int handleGetVarRequest(UpnpStateVarRequest* request);
    
    // AVTransport Actions
    int actionSetAVTransportURI(UpnpActionRequest* request);
    int actionSetNextAVTransportURI(UpnpActionRequest* request);
    int actionPlay(UpnpActionRequest* request);
    int actionPause(UpnpActionRequest* request);
    int actionStop(UpnpActionRequest* request);
    int actionSeek(UpnpActionRequest* request);
    int actionNext(UpnpActionRequest* request);
    int actionPrevious(UpnpActionRequest* request);
    int actionGetTransportInfo(UpnpActionRequest* request);
    int actionGetPositionInfo(UpnpActionRequest* request);
    int actionGetMediaInfo(UpnpActionRequest* request);
    int actionGetTransportSettings(UpnpActionRequest* request);
    int actionGetDeviceCapabilities(UpnpActionRequest* request);
    
    // RenderingControl Actions
    int actionGetVolume(UpnpActionRequest* request);
    int actionSetVolume(UpnpActionRequest* request);
    int actionGetMute(UpnpActionRequest* request);
    int actionSetMute(UpnpActionRequest* request);
    
    // Helpers
    std::string generateDescriptionXML();
    std::string generateAVTransportSCPD();
    std::string generateRenderingControlSCPD();
    std::string generateConnectionManagerSCPD();
    std::string createPositionInfoXML() const;
    std::string formatTime(int seconds) const;
    
    void sendAVTransportEvent();
    void sendRenderingControlEvent();
    
    IXML_Document* createActionResponse(const std::string& actionName);
    void addResponseArg(IXML_Document* response, 
                       const std::string& name, 
                       const std::string& value);
    
    std::string getArgumentValue(IXML_Document* actionDoc,
                                 const std::string& argName);

    // Configuration
    Config m_config;
    
    // libupnp handles
    UpnpDevice_Handle m_deviceHandle;
    
    // State
    mutable std::mutex m_stateMutex;
    bool m_running;
    std::string m_ipAddress;
    int m_actualPort;
    
    // Transport state
    std::string m_transportState;      // STOPPED, PLAYING, PAUSED_PLAYBACK, TRANSITIONING
    std::string m_transportStatus;     // OK, ERROR_OCCURRED
    std::string m_currentURI;
    std::string m_currentMetadata;
    std::string m_nextURI;
    std::string m_nextMetadata;
    int m_currentPosition;             // seconds
    int m_trackDuration;               // seconds
    std::string m_currentTrackURI;
    std::string m_currentTrackMetadata;
    
    // Rendering state
    int m_volume;                      // 0-100
    bool m_mute;
    
    // Callbacks
    Callbacks m_callbacks;
    
    // Protocol Info (cached at initialization)
    std::string m_protocolInfo;
};
