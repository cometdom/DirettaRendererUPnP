#include "UPnPDevice.hpp"
#include "ProtocolInfoBuilder.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>

// ============================================================================
// Logging system - Variable globale définie dans main.cpp
// ============================================================================
extern bool g_verbose;
#ifdef NOLOG
#define DEBUG_LOG(x) do {} while(0)
#else
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }
#endif

// Helper: XML-escape a string for use in attribute values
static std::string xmlEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 64);
    for (char c : input) {
        switch (c) {
            case '&':  output += "&amp;"; break;
            case '<':  output += "&lt;"; break;
            case '>':  output += "&gt;"; break;
            case '"':  output += "&quot;"; break;
            default:   output += c; break;
        }
    }
    return output;
}

UPnPDevice::UPnPDevice(const Config& config)
    : m_config(config)
    , m_deviceHandle(-1)
    , m_running(false)
    , m_actualPort(0)
    , m_transportState("STOPPED")
    , m_transportStatus("OK")
    , m_currentPosition(0)
    , m_trackDuration(0)
    , m_volume(50)
    , m_mute(false)
{
    DEBUG_LOG("[UPnPDevice] Created: " << m_config.friendlyName);
    
    // Générer le ProtocolInfo basé sur les capacités Diretta/Holo Audio
    DEBUG_LOG("[UPnPDevice] Generating ProtocolInfo...");
    auto caps = ProtocolInfoBuilder::getHoloAudioCapabilities();
    m_protocolInfo = ProtocolInfoBuilder::buildProtocolInfo(caps);
    
    size_t numFormats = std::count(m_protocolInfo.begin(), m_protocolInfo.end(), ',') + 1;
    DEBUG_LOG("[UPnPDevice] ProtocolInfo: " 
              << m_protocolInfo.length() << " chars, "
              << numFormats << " formats");
}

UPnPDevice::~UPnPDevice() {
    stop();
    DEBUG_LOG("[UPnPDevice] Destroyed");
}

bool UPnPDevice::start() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    if (m_running) {
        std::cerr << "[UPnPDevice] Already running" << std::endl;
        return false;
    }
    
    DEBUG_LOG("[UPnPDevice] Starting...");
    
    // 1. Initialize libupnp
    // ⭐ MODIFIÉ: Bind to specific network interface if specified
    const char* interfaceName = m_config.networkInterface.empty() ? nullptr : m_config.networkInterface.c_str();
    
    if (interfaceName != nullptr) {
        std::cout << "🌐 Binding UPnP to interface: " << interfaceName << std::endl;
    } else {
        std::cout << "🌐 Using default interface for UPnP (auto-detect)" << std::endl;
    }
    
    int ret = UpnpInit2(interfaceName, m_config.port);  // ← interfaceName au lieu de nullptr
    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpInit2 failed: " << ret << std::endl;
        
        // ⭐ NOUVEAU: Message d'aide pour multi-homed systems
        if (interfaceName != nullptr) {
            std::cerr << "\n💡 Troubleshooting:" << std::endl;
            std::cerr << "  - Verify interface exists: ip link show" << std::endl;
            std::cerr << "  - Check IP is assigned: ip addr show " << interfaceName << std::endl;
            std::cerr << "  - Or try IP address instead: --bind-ip 192.168.x.x" << std::endl;
        }
        
        return false;
    }

    // Afficher l'IP et port utilisés
    char* ipAddress = UpnpGetServerIpAddress();
    unsigned short port = UpnpGetServerPort();
    std::cout << "✓ UPnP initialized on " << (ipAddress ? ipAddress : "unknown") 
              << ":" << port << std::endl;
    
    // 2. Get server info
    m_ipAddress = UpnpGetServerIpAddress();
    m_actualPort = UpnpGetServerPort();
    
    DEBUG_LOG("[UPnPDevice] Server started: http://" << m_ipAddress 
              << ":" << m_actualPort);
    
    // 3. Enable logging (optional)
    // UpnpInitLog();
    // UpnpSetLogLevel(UPNP_INFO);
    
    // 4. Generate device description
    std::string descXML = generateDescriptionXML();
    
    // 5. Create SCPD files on disk (needed for libupnp webserver)
    // Create temporary directory structure
    system("mkdir -p /tmp/upnp_scpd/AVTransport");
    system("mkdir -p /tmp/upnp_scpd/RenderingControl");
    system("mkdir -p /tmp/upnp_scpd/ConnectionManager");
    
    // Write SCPD files to disk
    std::ofstream avtFile("/tmp/upnp_scpd/AVTransport/scpd.xml");
    if (avtFile.is_open()) {
        avtFile << generateAVTransportSCPD();
        avtFile.close();
    }
    
    std::ofstream rcFile("/tmp/upnp_scpd/RenderingControl/scpd.xml");
    if (rcFile.is_open()) {
        rcFile << generateRenderingControlSCPD();
        rcFile.close();
    }
    
    std::ofstream cmFile("/tmp/upnp_scpd/ConnectionManager/scpd.xml");
    if (cmFile.is_open()) {
        cmFile << generateConnectionManagerSCPD();
        cmFile.close();
    }
    
    // 6. Enable webserver and set root directory
    UpnpEnableWebserver(1);
    UpnpSetWebServerRootDir("/tmp/upnp_scpd");
    
    DEBUG_LOG("[UPnPDevice] ✓ SCPD files created and webserver configured");
    
    // 7. Register root device
    ret = UpnpRegisterRootDevice2(
        UPNPREG_BUF_DESC,
        descXML.c_str(),
        descXML.length(),
        1,  // config_done
        upnpCallbackStatic,
        this,
        &m_deviceHandle
    );
    
    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpRegisterRootDevice2 failed: " 
                  << ret << std::endl;
        UpnpFinish();
        return false;
    }
    
    DEBUG_LOG("[UPnPDevice] ✓ Device registered (handle=" 
              << m_deviceHandle << ")");
    
    // 8. Send SSDP advertisements
    ret = UpnpSendAdvertisement(m_deviceHandle, 1800);  // 30 minutes
    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpSendAdvertisement failed: " 
                  << ret << std::endl;
    } else {
        DEBUG_LOG("[UPnPDevice] ✓ SSDP advertisements sent");
    }
    
    m_running = true;
    
    std::cout << "[UPnPDevice] ✓ Device is now discoverable!" << std::endl;
    std::cout << "[UPnPDevice] Device URL: http://" << m_ipAddress 
              << ":" << m_actualPort << "/description.xml" << std::endl;
    
    return true;
}

void UPnPDevice::stop() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    if (!m_running) {
        return;
    }
    
    DEBUG_LOG("[UPnPDevice] Stopping...");
    
    if (m_deviceHandle >= 0) {
        // Send byebye
        UpnpSendAdvertisement(m_deviceHandle, 0);
        
        // Unregister
        UpnpUnRegisterRootDevice(m_deviceHandle);
        m_deviceHandle = -1;
    }
    
    // Cleanup libupnp
    UpnpFinish();
    
    m_running = false;
    
    DEBUG_LOG("[UPnPDevice] ✓ Stopped");
}

void UPnPDevice::setCallbacks(const Callbacks& callbacks) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_callbacks = callbacks;
    DEBUG_LOG("[UPnPDevice] Callbacks set");
}

// Static callback dispatcher
int UPnPDevice::upnpCallbackStatic(Upnp_EventType eventType, 
                                   const void* event, 
                                   void* cookie) 
{
    UPnPDevice* device = static_cast<UPnPDevice*>(cookie);
    return device->upnpCallback(eventType, event);
}

// Instance callback
int UPnPDevice::upnpCallback(Upnp_EventType eventType, const void* event) {
    switch (eventType) {
        case UPNP_CONTROL_ACTION_REQUEST:
            return handleActionRequest((UpnpActionRequest*)event);
            
        case UPNP_EVENT_SUBSCRIPTION_REQUEST:
            return handleSubscriptionRequest((UpnpSubscriptionRequest*)event);
            
        case UPNP_CONTROL_GET_VAR_REQUEST:
            return handleGetVarRequest((UpnpStateVarRequest*)event);
            
        default:
            // Other events ignored
            break;
    }
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::handleActionRequest(UpnpActionRequest* request) {
    std::string actionName = UpnpString_get_String(
        UpnpActionRequest_get_ActionName(request)
    );
    
    std::string serviceID = UpnpString_get_String(
        UpnpActionRequest_get_ServiceID(request)
    );
    
    DEBUG_LOG("[UPnPDevice] Action: " << actionName 
              << " (service: " << serviceID << ")");
    
    // Dispatch AVTransport actions
    if (serviceID.find("AVTransport") != std::string::npos) {
        if (actionName == "SetAVTransportURI") {
            return actionSetAVTransportURI(request);
        } else if (actionName == "SetNextAVTransportURI") {
            return actionSetNextAVTransportURI(request);
        } else if (actionName == "Play") {
            return actionPlay(request);
        } else if (actionName == "Pause") {
            return actionPause(request);
        } else if (actionName == "Stop") {
            return actionStop(request);
        } else if (actionName == "Seek") {
            return actionSeek(request);
        } else if (actionName == "Next") {
            return actionNext(request);
        } else if (actionName == "Previous") {
            return actionPrevious(request);
        } else if (actionName == "GetTransportInfo") {
            return actionGetTransportInfo(request);
        } else if (actionName == "GetPositionInfo") {
            return actionGetPositionInfo(request);
        } else if (actionName == "GetMediaInfo") {
            return actionGetMediaInfo(request);
        } else if (actionName == "GetTransportSettings") {
            return actionGetTransportSettings(request);
        } else if (actionName == "GetDeviceCapabilities") {
            return actionGetDeviceCapabilities(request);
        } else if (actionName == "GetCurrentTransportActions") {
            return actionGetCurrentTransportActions(request);
        } else if (actionName == "SetPlayMode") {
            return actionSetPlayMode(request);
        }
    }
    
    // Dispatch RenderingControl actions
    if (serviceID.find("RenderingControl") != std::string::npos) {
        if (actionName == "GetVolume") {
            return actionGetVolume(request);
        } else if (actionName == "SetVolume") {
            return actionSetVolume(request);
        } else if (actionName == "GetMute") {
            return actionGetMute(request);
        } else if (actionName == "SetMute") {
            return actionSetMute(request);
        } else if (actionName == "GetVolumeDB") {
            return actionGetVolumeDB(request);
        } else if (actionName == "GetVolumeDBRange") {
            return actionGetVolumeDBRange(request);
        }
    }
    
    // ConnectionManager actions
    if (serviceID.find("ConnectionManager") != std::string::npos) {
        if (actionName == "GetProtocolInfo") {
            IXML_Document* response = createActionResponse("GetProtocolInfo",
                "urn:schemas-upnp-org:service:ConnectionManager:1");
            addResponseArg(response, "Source", "");
            addResponseArg(response, "Sink", m_protocolInfo);
            UpnpActionRequest_set_ActionResult(request, response);
            return UPNP_E_SUCCESS;
        } else if (actionName == "GetCurrentConnectionIDs") {
            IXML_Document* response = createActionResponse("GetCurrentConnectionIDs",
                "urn:schemas-upnp-org:service:ConnectionManager:1");
            addResponseArg(response, "ConnectionIDs", "0");
            UpnpActionRequest_set_ActionResult(request, response);
            return UPNP_E_SUCCESS;
        } else if (actionName == "GetCurrentConnectionInfo") {
            IXML_Document* response = createActionResponse("GetCurrentConnectionInfo",
                "urn:schemas-upnp-org:service:ConnectionManager:1");
            addResponseArg(response, "RcsID", "0");
            addResponseArg(response, "AVTransportID", "0");
            addResponseArg(response, "ProtocolInfo", "");
            addResponseArg(response, "PeerConnectionManager", "");
            addResponseArg(response, "PeerConnectionID", "-1");
            addResponseArg(response, "Direction", "Input");
            addResponseArg(response, "Status", "OK");
            UpnpActionRequest_set_ActionResult(request, response);
            return UPNP_E_SUCCESS;
        }
    }
    
    // Action not supported
    std::cerr << "[UPnPDevice] Unsupported action: " << actionName << std::endl;
    UpnpActionRequest_set_ErrCode(request, 401);  // Invalid Action
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::handleSubscriptionRequest(UpnpSubscriptionRequest* request) {
    std::string serviceID = UpnpString_get_String(
        UpnpSubscriptionRequest_get_ServiceId(request)
    );
    const char* sid = UpnpSubscriptionRequest_get_SID_cstr(request);

    std::cout << "[UPnPDevice] Subscription request for: " << serviceID
              << " SID: " << (sid ? sid : "null") << std::endl;

    // Build initial LastChange XML - different format per service
    std::string lastChange;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        std::stringstream ss;

        if (serviceID.find("AVTransport") != std::string::npos) {
            // During PLAYING: accept subscription without sending full state.
            // Audirvana re-subscribes every ~3 minutes (new SID each time).
            // Sending TransportState=PLAYING triggers re-sync and audio hiccups.
            // The control point already knows the state (it sent Play) and
            // continues polling GetPositionInfo for position/duration.
            if (m_transportState == "PLAYING") {
                DEBUG_LOG("[UPnPDevice] Subscription accepted (PLAYING, no initial event): " << serviceID);
                const char* emptyNames[] = { "LastChange" };
                const char* emptyValues[] = { "" };
                int ret = UpnpAcceptSubscription(
                    m_deviceHandle,
                    UpnpSubscriptionRequest_get_UDN_cstr(request),
                    UpnpSubscriptionRequest_get_ServiceId_cstr(request),
                    emptyNames, emptyValues, 1, sid
                );
                if (ret != UPNP_E_SUCCESS) {
                    std::cerr << "[UPnPDevice] UpnpAcceptSubscription (no-op) failed: " << ret << std::endl;
                }
                return ret;
            }

            // AVTransport: transport state, URIs, metadata
            std::string actions;
            if (m_transportState == "PAUSED_PLAYBACK") {
                actions = "Play,Stop,Seek";
            } else if (m_transportState == "STOPPED") {
                actions = "Play,Seek";
            } else {
                actions = "Stop";
            }

            ss << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
               << "<InstanceID val=\"0\">"
               << "<TransportState val=\"" << m_transportState << "\"/>"
               << "<AVTransportURI val=\"" << xmlEscape(m_currentURI) << "\"/>"
               << "<AVTransportURIMetaData val=\"" << xmlEscape(m_currentMetadata) << "\"/>"
               << "<CurrentTrackURI val=\"" << xmlEscape(m_currentTrackURI) << "\"/>"
               << "<CurrentTrackDuration val=\"" << formatTime(m_trackDuration) << "\"/>"
               << "<CurrentTrackMetaData val=\"" << xmlEscape(m_currentTrackMetadata) << "\"/>"
               << "<NextAVTransportURI val=\"" << xmlEscape(m_nextURI) << "\"/>"
               << "<NextAVTransportURIMetaData val=\"" << xmlEscape(m_nextMetadata) << "\"/>"
               << "<CurrentTransportActions val=\"" << actions << "\"/>"
               << "</InstanceID>"
               << "</Event>";
        } else if (serviceID.find("RenderingControl") != std::string::npos) {
            // RenderingControl: volume and mute state
            ss << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
               << "<InstanceID val=\"0\">"
               << "<Volume channel=\"Master\" val=\"" << m_volume << "\"/>"
               << "<Mute channel=\"Master\" val=\"" << (m_mute ? "1" : "0") << "\"/>"
               << "</InstanceID>"
               << "</Event>";
        } else if (serviceID.find("ConnectionManager") != std::string::npos) {
            // ConnectionManager uses direct variables, not LastChange
            const char* cmVarNames[] = { "SourceProtocolInfo", "SinkProtocolInfo", "CurrentConnectionIDs" };
            const char* cmVarValues[] = { "", m_protocolInfo.c_str(), "0" };

            int ret = UpnpAcceptSubscription(
                m_deviceHandle,
                UpnpSubscriptionRequest_get_UDN_cstr(request),
                UpnpSubscriptionRequest_get_ServiceId_cstr(request),
                cmVarNames, cmVarValues, 3, sid
            );

            if (ret != UPNP_E_SUCCESS) {
                std::cerr << "[UPnPDevice] UpnpAcceptSubscription (CM) failed: " << ret << std::endl;
            }
            return ret;
        }

        lastChange = ss.str();
    }

    // Pre-escape: libupnp inserts raw XML as child nodes of <LastChange>,
    // but UPnP spec requires the LastChange value to be XML-escaped text.
    // Without this, control points (e.g. Audirvana) get empty text content
    // and report "Invalid AVT/RCS last change value".
    std::string escapedLastChange = xmlEscape(lastChange);

    const char* varNames[] = { "LastChange" };
    const char* varValues[] = { escapedLastChange.c_str() };

    int ret = UpnpAcceptSubscription(
        m_deviceHandle,
        UpnpSubscriptionRequest_get_UDN_cstr(request),
        UpnpSubscriptionRequest_get_ServiceId_cstr(request),
        varNames, varValues, 1, sid
    );

    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpAcceptSubscription failed: " << ret << std::endl;
    } else {
        DEBUG_LOG("[UPnPDevice] Subscription accepted for: " << serviceID);
    }

    return ret;
}

int UPnPDevice::handleGetVarRequest(UpnpStateVarRequest* request) {
    std::string varName = UpnpString_get_String(
        UpnpStateVarRequest_get_StateVarName(request)
    );
    
    DEBUG_LOG("[UPnPDevice] GetVar: " << varName);
    
    // Return current value
    if (varName == "TransportState") {
        UpnpStateVarRequest_set_CurrentVal(request, m_transportState.c_str());
    }
    
    return UPNP_E_SUCCESS;
}

// Action implementations continue in next part...
// UPnPDevice.cpp - Part 2: Action Implementations

// ============================================================================
// AVTransport Actions
// ============================================================================

int UPnPDevice::actionSetAVTransportURI(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);
    
    std::string uri = getArgumentValue(actionDoc, "CurrentURI");
    std::string metadata = getArgumentValue(actionDoc, "CurrentURIMetaData");
    
    if (uri.empty()) {
        std::cerr << "[UPnPDevice] SetAVTransportURI: empty URI" << std::endl;
        UpnpActionRequest_set_ErrCode(request, 402);  // Invalid Args
        return UPNP_E_SUCCESS;
    }
    
    DEBUG_LOG("[UPnPDevice] SetAVTransportURI: " << uri);
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentURI = uri;
        m_currentMetadata = metadata;
        m_currentTrackURI = uri;
        m_currentTrackMetadata = metadata;
        m_currentPosition = 0;
        m_trackDuration = 0;
        
        // Effacer l'ancienne queue gapless (nouveau contexte)
        if (!m_nextURI.empty()) {
            DEBUG_LOG("[UPnPDevice] ✓ Clearing old gapless queue (new context)");
            m_nextURI.clear();
            m_nextMetadata.clear();
        }
    }
    
    // Callback
    if (m_callbacks.onSetURI) {
        m_callbacks.onSetURI(uri, metadata);
    }
    
    // Send event notification
    sendAVTransportEvent();
    
    // Response
    IXML_Document* response = createActionResponse("SetAVTransportURI");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionSetNextAVTransportURI(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);

    std::string uri = getArgumentValue(actionDoc, "NextURI");
    std::string metadata = getArgumentValue(actionDoc, "NextURIMetaData");

    DEBUG_LOG("[UPnPDevice] SetNextAVTransportURI: " << uri);
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextURI = uri;
        m_nextMetadata = metadata;
    }
    
    // Callback
    if (m_callbacks.onSetNextURI) {
        m_callbacks.onSetNextURI(uri, metadata);
    }

    // No event here: sending a full AVTransport event (with TransportState=PLAYING)
    // confuses control points like Audirvana into thinking a state change occurred.
    // Standard renderers (gmrender-resurrect, upmpdcli) don't event on SetNextAVTransportURI.
    // The control point already knows the next URI since it sent it.

    // Response
    IXML_Document* response = createActionResponse("SetNextAVTransportURI");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionPlay(UpnpActionRequest* request) {
    std::cout << "[UPnPDevice] Play" << std::endl;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Per UPnP spec: Play while already PLAYING is a no-op.
        // Audirvana sends duplicate Play commands when it receives a
        // SetAVTransportURI event with TransportState=STOPPED after
        // it has already sent Play. Ignore the duplicate.
        if (m_transportState == "PLAYING") {
            DEBUG_LOG("[UPnPDevice] Already PLAYING, ignoring duplicate Play");
            IXML_Document* response = createActionResponse("Play");
            UpnpActionRequest_set_ActionResult(request, response);
            return UPNP_E_SUCCESS;
        }
        m_transportState = "PLAYING";
        m_transportStatus = "OK";
    }

    // Callback - the onPlay handler opens the track and sends a complete
    // AVTransport event via trackChangeCallback/notifyGaplessTransition
    // (or notifyStateChange for resume-from-pause). No need to send
    // another event here; redundant events cause audio hiccups on Audirvana.
    if (m_callbacks.onPlay) {
        m_callbacks.onPlay();
    }

    // Response
    IXML_Document* response = createActionResponse("Play");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionPause(UpnpActionRequest* request) {
    std::cout << "[UPnPDevice] Pause" << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_transportState = "PAUSED_PLAYBACK";
    }
    
    // Callback - onPause handler already sends the event via
    // notifyStateChange("PAUSED_PLAYBACK"). No need to send another here;
    // redundant events cause progress bar hiccups on Audirvana.
    if (m_callbacks.onPause) {
        m_callbacks.onPause();
    }

    // Response
    IXML_Document* response = createActionResponse("Pause");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionStop(UpnpActionRequest* request) {
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "[UPnPDevice] ⛔ STOP ACTION RECEIVED" << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        DEBUG_LOG("[UPnPDevice] Changing state: " << m_transportState 
                  << " → STOPPED");
        m_transportState = "STOPPED";
        m_currentPosition = 0;
        
        // Effacer la queue gapless
        if (!m_nextURI.empty()) {
            DEBUG_LOG("[UPnPDevice] ✓ Clearing gapless queue: " << m_nextURI);
            m_nextURI.clear();
            m_nextMetadata.clear();
        }
    }
    
    // Callback - onStop handler already sends the event via
    // notifyStateChange("STOPPED"). No need to send another here;
    // redundant events cause progress bar hiccups on Audirvana.
    if (m_callbacks.onStop) {
        DEBUG_LOG("[UPnPDevice] ✓ Calling onStop callback...");
        m_callbacks.onStop();
        DEBUG_LOG("[UPnPDevice] ✓ onStop callback completed");
    } else {
        std::cout << "[UPnPDevice] ❌ NO onStop CALLBACK CONFIGURED!" << std::endl;
    }
    
    // Response
    DEBUG_LOG("[UPnPDevice] Creating response...");
    IXML_Document* response = createActionResponse("Stop");
    UpnpActionRequest_set_ActionResult(request, response);
    DEBUG_LOG("[UPnPDevice] ✓ Stop action completed");
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionSeek(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);
    
    std::string unit = getArgumentValue(actionDoc, "Unit");
    std::string target = getArgumentValue(actionDoc, "Target");
    
    std::cout << "[UPnPDevice] Seek: " << unit << " = " << target << std::endl;
    
    // Callback
    if (m_callbacks.onSeek) {
        m_callbacks.onSeek(target);
    }
    
    // Response
    IXML_Document* response = createActionResponse("Seek");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionNext(UpnpActionRequest* request) {
    std::cout << "[UPnPDevice] Next (not implemented)" << std::endl;
    
    // Response
    IXML_Document* response = createActionResponse("Next");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionPrevious(UpnpActionRequest* request) {
    std::cout << "[UPnPDevice] Previous (not implemented)" << std::endl;
    
    // Response
    IXML_Document* response = createActionResponse("Previous");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetTransportInfo(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetTransportInfo");
    addResponseArg(response, "CurrentTransportState", m_transportState);
    addResponseArg(response, "CurrentTransportStatus", m_transportStatus);
    addResponseArg(response, "CurrentSpeed", "1");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetPositionInfo(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    // Log position info for debugging track transitions
    std::string shortURI = m_currentTrackURI;
    if (shortURI.size() > 50) shortURI = "..." + shortURI.substr(shortURI.size() - 50);
    DEBUG_LOG("[UPnPDevice] GetPositionInfo: pos=" << formatTime(m_currentPosition)
              << " dur=" << formatTime(m_trackDuration) << " URI=" << shortURI);

    IXML_Document* response = createActionResponse("GetPositionInfo");
    addResponseArg(response, "Track", "1");
    addResponseArg(response, "TrackDuration", formatTime(m_trackDuration));
    addResponseArg(response, "TrackMetaData", m_currentTrackMetadata);
    addResponseArg(response, "TrackURI", m_currentTrackURI);
    addResponseArg(response, "RelTime", formatTime(m_currentPosition));
    addResponseArg(response, "AbsTime", formatTime(m_currentPosition));
    addResponseArg(response, "RelCount", "2147483647");
    addResponseArg(response, "AbsCount", "2147483647");

    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetMediaInfo(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetMediaInfo");
    addResponseArg(response, "NrTracks", "1");
    addResponseArg(response, "MediaDuration", formatTime(m_trackDuration));
    addResponseArg(response, "CurrentURI", m_currentURI);
    addResponseArg(response, "CurrentURIMetaData", m_currentMetadata);
    addResponseArg(response, "NextURI", m_nextURI);
    addResponseArg(response, "NextURIMetaData", m_nextMetadata);
    addResponseArg(response, "PlayMedium", "NETWORK");
    addResponseArg(response, "RecordMedium", "NOT_IMPLEMENTED");
    addResponseArg(response, "WriteStatus", "NOT_IMPLEMENTED");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetTransportSettings(UpnpActionRequest* request) {
    IXML_Document* response = createActionResponse("GetTransportSettings");
    addResponseArg(response, "PlayMode", "NORMAL");
    addResponseArg(response, "RecQualityMode", "NOT_IMPLEMENTED");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionSetPlayMode(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);
    std::string mode = getArgumentValue(actionDoc, "NewPlayMode");

    // Only NORMAL mode is supported (no shuffle/repeat)
    if (mode != "NORMAL") {
        UpnpActionRequest_set_ErrCode(request, 712);  // Play mode not supported
        return UPNP_E_SUCCESS;
    }

    IXML_Document* response = createActionResponse("SetPlayMode");
    UpnpActionRequest_set_ActionResult(request, response);
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetDeviceCapabilities(UpnpActionRequest* request) {
    IXML_Document* response = createActionResponse("GetDeviceCapabilities");
    addResponseArg(response, "PlayMedia", "NETWORK");
    addResponseArg(response, "RecMedia", "NOT_IMPLEMENTED");
    addResponseArg(response, "RecQualityModes", "NOT_IMPLEMENTED");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetCurrentTransportActions(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    std::string actions;
    if (m_transportState == "PLAYING") {
        actions = "Play,Stop,Pause,Seek,Next,Previous";
    } else if (m_transportState == "PAUSED_PLAYBACK") {
        actions = "Play,Stop,Seek";
    } else if (m_transportState == "STOPPED") {
        actions = "Play,Seek";
    } else {
        actions = "Stop";
    }

    IXML_Document* response = createActionResponse("GetCurrentTransportActions");
    addResponseArg(response, "Actions", actions);

    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

// ============================================================================
// RenderingControl Actions
// ============================================================================

int UPnPDevice::actionGetVolume(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetVolume",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    addResponseArg(response, "CurrentVolume", std::to_string(m_volume));
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionSetVolume(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);
    
    std::string volumeStr = getArgumentValue(actionDoc, "DesiredVolume");
    int volume = std::atoi(volumeStr.c_str());
    
    std::cout << "[UPnPDevice] SetVolume: " << volume << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_volume = volume;
    }
    
    // Send event notification
    sendRenderingControlEvent();
    
    // Response
    IXML_Document* response = createActionResponse("SetVolume",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetMute(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetMute",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    addResponseArg(response, "CurrentMute", m_mute ? "1" : "0");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionSetMute(UpnpActionRequest* request) {
    IXML_Document* actionDoc = UpnpActionRequest_get_ActionRequest(request);
    
    std::string muteStr = getArgumentValue(actionDoc, "DesiredMute");
    bool mute = (muteStr == "1" || muteStr == "true");
    
    std::cout << "[UPnPDevice] SetMute: " << mute << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_mute = mute;
    }
    
    // Send event notification
    sendRenderingControlEvent();
    
    // Response
    IXML_Document* response = createActionResponse("SetMute",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetVolumeDB(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    // Map volume 0-100 to dB range -3600..0 (in 1/256 dB units per UPnP spec)
    // volume 100 = 0 dB, volume 0 = -3600 (1/256 dB)
    int volumeDB = (m_volume * 3600 / 100) - 3600;

    IXML_Document* response = createActionResponse("GetVolumeDB",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    addResponseArg(response, "CurrentVolume", std::to_string(volumeDB));

    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetVolumeDBRange(UpnpActionRequest* request) {
    IXML_Document* response = createActionResponse("GetVolumeDBRange",
        "urn:schemas-upnp-org:service:RenderingControl:1");
    addResponseArg(response, "MinValue", "-3600");
    addResponseArg(response, "MaxValue", "0");

    UpnpActionRequest_set_ActionResult(request, response);

    return UPNP_E_SUCCESS;
}

// Continue in part 3...

std::string UPnPDevice::createPositionInfoXML() const {
    std::stringstream ss;
    ss << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
       << "<InstanceID val=\"0\">"
       << "<RelTime val=\"" << formatTime(m_currentPosition) << "\"/>"
       << "<AbsTime val=\"" << formatTime(m_currentPosition) << "\"/>"
       << "</InstanceID>"
       << "</Event>";
    return ss.str();
}

std::string UPnPDevice::formatTime(int seconds) const {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    
    std::stringstream ss;
    ss << std::setfill('0') 
       << std::setw(2) << h << ":"
       << std::setw(2) << m << ":"
       << std::setw(2) << s;
    return ss.str();
}

// ============================================================================
// Part 3 : Helper Functions & XML Generation - MISSING IMPLEMENTATIONS
// ============================================================================

// Helper: Create action response
IXML_Document* UPnPDevice::createActionResponse(const std::string& actionName,
                                                  const std::string& serviceType) {
    IXML_Document* response = ixmlDocument_createDocument();
    IXML_Element* actionResponse = ixmlDocument_createElement(response,
        (actionName + "Response").c_str());
    ixmlElement_setAttribute(actionResponse, "xmlns:u", serviceType.c_str());
    ixmlNode_appendChild(&response->n, &actionResponse->n);
    return response;
}

// Helper: Add response argument
void UPnPDevice::addResponseArg(IXML_Document* response, 
                                const std::string& name, 
                                const std::string& value) {
    IXML_Element* arg = ixmlDocument_createElement(response, name.c_str());
    IXML_Node* textNode = ixmlDocument_createTextNode(response, value.c_str());
    ixmlNode_appendChild(&arg->n, textNode);
    
    // Get root element (action response)
    IXML_Node* root = ixmlNode_getFirstChild(&response->n);
    ixmlNode_appendChild(root, &arg->n);
}
// Helper: Get argument value from action request
std::string UPnPDevice::getArgumentValue(IXML_Document* actionDoc, 
                                         const std::string& argName) {
    IXML_NodeList* argList = ixmlDocument_getElementsByTagName(actionDoc, 
                                                               argName.c_str());
    if (!argList) return "";
    
    IXML_Node* argNode = ixmlNodeList_item(argList, 0);
    if (!argNode) {
        ixmlNodeList_free(argList);
        return "";
    }
    
    IXML_Node* textNode = ixmlNode_getFirstChild(argNode);
    const char* value = textNode ? ixmlNode_getNodeValue(textNode) : "";
    std::string result = value ? value : "";
    
    ixmlNodeList_free(argList);
    return result;
}

// Helper: Send AVTransport LastChange event to all subscribers
// Per UPnP AVTransport spec: RelativeTimePosition MUST NOT be evented via LastChange.
// Position is obtained by control points via GetPositionInfo polling.
void UPnPDevice::sendAVTransportEvent() {
    if (m_deviceHandle < 0 || !m_running) return;

    // Build LastChange XML with current state (no position - spec forbids it)
    std::string lastChange;
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        state = m_transportState;

        // Determine available actions for current state
        std::string actions;
        if (m_transportState == "PLAYING") {
            actions = "Play,Stop,Pause,Seek,Next,Previous";
        } else if (m_transportState == "PAUSED_PLAYBACK") {
            actions = "Play,Stop,Seek";
        } else if (m_transportState == "STOPPED") {
            actions = "Play,Seek";
        } else {
            actions = "Stop";
        }

        std::stringstream ss;
        ss << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
           << "<InstanceID val=\"0\">"
           << "<TransportState val=\"" << m_transportState << "\"/>"
           << "<AVTransportURI val=\"" << xmlEscape(m_currentURI) << "\"/>"
           << "<AVTransportURIMetaData val=\"" << xmlEscape(m_currentMetadata) << "\"/>"
           << "<CurrentTrackURI val=\"" << xmlEscape(m_currentTrackURI) << "\"/>"
           << "<CurrentTrackDuration val=\"" << formatTime(m_trackDuration) << "\"/>"
           << "<CurrentTrackMetaData val=\"" << xmlEscape(m_currentTrackMetadata) << "\"/>"
           << "<NextAVTransportURI val=\"" << xmlEscape(m_nextURI) << "\"/>"
           << "<NextAVTransportURIMetaData val=\"" << xmlEscape(m_nextMetadata) << "\"/>"
           << "<CurrentTransportActions val=\"" << actions << "\"/>"
           << "</InstanceID>"
           << "</Event>";
        lastChange = ss.str();
    }

    // Pre-escape: libupnp inserts raw XML as child nodes of <LastChange>,
    // but UPnP spec requires the LastChange value to be XML-escaped text.
    std::string escapedLastChange = xmlEscape(lastChange);

    const char* varNames[] = { "LastChange" };
    const char* varValues[] = { escapedLastChange.c_str() };

    std::string udn = "uuid:" + m_config.uuid;
    int ret = UpnpNotify(
        m_deviceHandle,
        udn.c_str(),
        "urn:upnp-org:serviceId:AVTransport",
        varNames, varValues, 1
    );

    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpNotify failed: " << ret << std::endl;
    } else {
        DEBUG_LOG("[UPnPDevice] Event sent: state=" << state << " dur=" << m_trackDuration << "s");
    }
}

// Helper: Send RenderingControl LastChange event to all subscribers
void UPnPDevice::sendRenderingControlEvent() {
    if (m_deviceHandle < 0 || !m_running) return;

    std::string lastChange;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        std::stringstream ss;
        ss << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
           << "<InstanceID val=\"0\">"
           << "<Volume channel=\"Master\" val=\"" << m_volume << "\"/>"
           << "<Mute channel=\"Master\" val=\"" << (m_mute ? "1" : "0") << "\"/>"
           << "</InstanceID>"
           << "</Event>";
        lastChange = ss.str();
    }

    // Pre-escape: libupnp inserts raw XML as child nodes of <LastChange>,
    // but UPnP spec requires the LastChange value to be XML-escaped text.
    std::string escapedLastChange = xmlEscape(lastChange);

    const char* varNames[] = { "LastChange" };
    const char* varValues[] = { escapedLastChange.c_str() };

    std::string udn = "uuid:" + m_config.uuid;
    int ret = UpnpNotify(
        m_deviceHandle,
        udn.c_str(),
        "urn:upnp-org:serviceId:RenderingControl",
        varNames, varValues, 1
    );

    if (ret != UPNP_E_SUCCESS) {
        DEBUG_LOG("[UPnPDevice] RenderingControl UpnpNotify failed: " << ret);
    }
}

// Generate device description XML
std::string UPnPDevice::generateDescriptionXML() {
    std::stringstream ss;
    ss << "<?xml version=\"1.0\"?>\n"
       << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
       << "  <specVersion>\n"
       << "    <major>1</major>\n"
       << "    <minor>0</minor>\n"
       << "  </specVersion>\n"
       << "  <device>\n"
       << "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\n"
       << "    <friendlyName>" << m_config.friendlyName << "</friendlyName>\n"
       << "    <manufacturer>" << m_config.manufacturer << "</manufacturer>\n"
       << "    <modelName>" << m_config.modelName << "</modelName>\n"
       << "    <UDN>uuid:" << m_config.uuid << "</UDN>\n"
       << "    <serviceList>\n"
       << "      <service>\n"
       << "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\n"
       << "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\n"
       << "        <SCPDURL>/AVTransport/scpd.xml</SCPDURL>\n"
       << "        <controlURL>/AVTransport/control</controlURL>\n"
       << "        <eventSubURL>/AVTransport/event</eventSubURL>\n"
       << "      </service>\n"
       << "      <service>\n"
       << "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\n"
       << "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\n"
       << "        <SCPDURL>/RenderingControl/scpd.xml</SCPDURL>\n"
       << "        <controlURL>/RenderingControl/control</controlURL>\n"
       << "        <eventSubURL>/RenderingControl/event</eventSubURL>\n"
       << "      </service>\n"
       << "      <service>\n"
       << "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\n"
       << "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\n"
       << "        <SCPDURL>/ConnectionManager/scpd.xml</SCPDURL>\n"
       << "        <controlURL>/ConnectionManager/control</controlURL>\n"
       << "        <eventSubURL>/ConnectionManager/event</eventSubURL>\n"
       << "      </service>\n"
       << "    </serviceList>\n"
       << "  </device>\n"
       << "</root>\n";
    return ss.str();
}

// Generate AVTransport SCPD
std::string UPnPDevice::generateAVTransportSCPD() {
    return R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion>
    <major>1</major>
    <minor>0</minor>
  </specVersion>
  <actionList>
    <action>
      <name>SetAVTransportURI</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentURI</name>
          <direction>in</direction>
          <relatedStateVariable>AVTransportURI</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentURIMetaData</name>
          <direction>in</direction>
          <relatedStateVariable>AVTransportURIMetaData</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>SetNextAVTransportURI</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>NextURI</name>
          <direction>in</direction>
          <relatedStateVariable>NextAVTransportURI</relatedStateVariable>
        </argument>
        <argument>
          <name>NextURIMetaData</name>
          <direction>in</direction>
          <relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Play</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Speed</name>
          <direction>in</direction>
          <relatedStateVariable>TransportPlaySpeed</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Stop</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Pause</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Seek</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Unit</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable>
        </argument>
        <argument>
          <name>Target</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Next</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>Previous</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetTransportInfo</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentTransportState</name>
          <direction>out</direction>
          <relatedStateVariable>TransportState</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentTransportStatus</name>
          <direction>out</direction>
          <relatedStateVariable>TransportStatus</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentSpeed</name>
          <direction>out</direction>
          <relatedStateVariable>TransportPlaySpeed</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetPositionInfo</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Track</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentTrack</relatedStateVariable>
        </argument>
        <argument>
          <name>TrackDuration</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentTrackDuration</relatedStateVariable>
        </argument>
        <argument>
          <name>TrackMetaData</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentTrackMetaData</relatedStateVariable>
        </argument>
        <argument>
          <name>TrackURI</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentTrackURI</relatedStateVariable>
        </argument>
        <argument>
          <name>RelTime</name>
          <direction>out</direction>
          <relatedStateVariable>RelativeTimePosition</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetMediaInfo</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>NrTracks</name>
          <direction>out</direction>
          <relatedStateVariable>NumberOfTracks</relatedStateVariable>
        </argument>
        <argument>
          <name>MediaDuration</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentMediaDuration</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentURI</name>
          <direction>out</direction>
          <relatedStateVariable>AVTransportURI</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentURIMetaData</name>
          <direction>out</direction>
          <relatedStateVariable>AVTransportURIMetaData</relatedStateVariable>
        </argument>
        <argument>
          <name>NextURI</name>
          <direction>out</direction>
          <relatedStateVariable>NextAVTransportURI</relatedStateVariable>
        </argument>
        <argument>
          <name>NextURIMetaData</name>
          <direction>out</direction>
          <relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable>
        </argument>
        <argument>
          <name>PlayMedium</name>
          <direction>out</direction>
          <relatedStateVariable>PlaybackStorageMedium</relatedStateVariable>
        </argument>
        <argument>
          <name>RecordMedium</name>
          <direction>out</direction>
          <relatedStateVariable>RecordStorageMedium</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetTransportSettings</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>PlayMode</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentPlayMode</relatedStateVariable>
        </argument>
        <argument>
          <name>RecQualityMode</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentRecordQualityMode</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>SetPlayMode</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>NewPlayMode</name>
          <direction>in</direction>
          <relatedStateVariable>CurrentPlayMode</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetDeviceCapabilities</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>PlayMedia</name>
          <direction>out</direction>
          <relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable>
        </argument>
        <argument>
          <name>RecMedia</name>
          <direction>out</direction>
          <relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable>
        </argument>
        <argument>
          <name>RecQualityModes</name>
          <direction>out</direction>
          <relatedStateVariable>PossibleRecordQualityModes</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetCurrentTransportActions</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Actions</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentTransportActions</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes">
      <name>LastChange</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_InstanceID</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_SeekMode</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>REL_TIME</allowedValue>
        <allowedValue>TRACK_NR</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_SeekTarget</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>AVTransportURI</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>AVTransportURIMetaData</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>NextAVTransportURI</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>NextAVTransportURIMetaData</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>TransportState</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>STOPPED</allowedValue>
        <allowedValue>PLAYING</allowedValue>
        <allowedValue>PAUSED_PLAYBACK</allowedValue>
        <allowedValue>TRANSITIONING</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>TransportStatus</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>OK</allowedValue>
        <allowedValue>ERROR_OCCURRED</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>TransportPlaySpeed</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>NumberOfTracks</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTrack</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTrackDuration</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentMediaDuration</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTrackMetaData</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTrackURI</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>RelativeTimePosition</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>PlaybackStorageMedium</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>NETWORK</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>RecordStorageMedium</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>NOT_IMPLEMENTED</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTransportActions</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentPlayMode</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>NORMAL</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentRecordQualityMode</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>NOT_IMPLEMENTED</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>PossiblePlaybackStorageMedia</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>PossibleRecordStorageMedia</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>PossibleRecordQualityModes</name>
      <dataType>string</dataType>
    </stateVariable>
  </serviceStateTable>
</scpd>
)";
}

// Generate RenderingControl SCPD
std::string UPnPDevice::generateRenderingControlSCPD() {
    return R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion>
    <major>1</major>
    <minor>0</minor>
  </specVersion>
  <actionList>
    <action>
      <name>GetVolume</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentVolume</name>
          <direction>out</direction>
          <relatedStateVariable>Volume</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>SetVolume</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>DesiredVolume</name>
          <direction>in</direction>
          <relatedStateVariable>Volume</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetMute</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentMute</name>
          <direction>out</direction>
          <relatedStateVariable>Mute</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>SetMute</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>DesiredMute</name>
          <direction>in</direction>
          <relatedStateVariable>Mute</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetVolumeDB</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>CurrentVolume</name>
          <direction>out</direction>
          <relatedStateVariable>VolumeDB</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetVolumeDBRange</name>
      <argumentList>
        <argument>
          <name>InstanceID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
        </argument>
        <argument>
          <name>Channel</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable>
        </argument>
        <argument>
          <name>MinValue</name>
          <direction>out</direction>
          <relatedStateVariable>VolumeDB</relatedStateVariable>
        </argument>
        <argument>
          <name>MaxValue</name>
          <direction>out</direction>
          <relatedStateVariable>VolumeDB</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes">
      <name>LastChange</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_InstanceID</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_Channel</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>Master</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>Volume</name>
      <dataType>ui2</dataType>
      <allowedValueRange>
        <minimum>0</minimum>
        <maximum>100</maximum>
      </allowedValueRange>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>Mute</name>
      <dataType>boolean</dataType>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>VolumeDB</name>
      <dataType>i2</dataType>
      <allowedValueRange>
        <minimum>-3600</minimum>
        <maximum>0</maximum>
      </allowedValueRange>
    </stateVariable>
  </serviceStateTable>
</scpd>
)";
}

// Generate ConnectionManager SCPD
std::string UPnPDevice::generateConnectionManagerSCPD() {
    return R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion>
    <major>1</major>
    <minor>0</minor>
  </specVersion>
  <actionList>
    <action>
      <name>GetProtocolInfo</name>
      <argumentList>
        <argument>
          <name>Source</name>
          <direction>out</direction>
          <relatedStateVariable>SourceProtocolInfo</relatedStateVariable>
        </argument>
        <argument>
          <name>Sink</name>
          <direction>out</direction>
          <relatedStateVariable>SinkProtocolInfo</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetCurrentConnectionIDs</name>
      <argumentList>
        <argument>
          <name>ConnectionIDs</name>
          <direction>out</direction>
          <relatedStateVariable>CurrentConnectionIDs</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
    <action>
      <name>GetCurrentConnectionInfo</name>
      <argumentList>
        <argument>
          <name>ConnectionID</name>
          <direction>in</direction>
          <relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable>
        </argument>
        <argument>
          <name>RcsID</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable>
        </argument>
        <argument>
          <name>AVTransportID</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable>
        </argument>
        <argument>
          <name>ProtocolInfo</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable>
        </argument>
        <argument>
          <name>PeerConnectionManager</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable>
        </argument>
        <argument>
          <name>PeerConnectionID</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable>
        </argument>
        <argument>
          <name>Direction</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable>
        </argument>
        <argument>
          <name>Status</name>
          <direction>out</direction>
          <relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable>
        </argument>
      </argumentList>
    </action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes">
      <name>SourceProtocolInfo</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>SinkProtocolInfo</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>CurrentConnectionIDs</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_ConnectionID</name>
      <dataType>i4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_RcsID</name>
      <dataType>i4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_AVTransportID</name>
      <dataType>i4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_ProtocolInfo</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_ConnectionManager</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_Direction</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>Input</allowedValue>
        <allowedValue>Output</allowedValue>
      </allowedValueList>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_ConnectionStatus</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>OK</allowedValue>
        <allowedValue>Unknown</allowedValue>
      </allowedValueList>
    </stateVariable>
  </serviceStateTable>
</scpd>
)";
}

// Already implemented in main file: createPositionInfoXML() and formatTime()
// ============================================================================
// Fonctions manquantes finales
// ============================================================================

// Notify state change via events
void UPnPDevice::notifyStateChange(const std::string& state) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_transportState = state;
    }
    sendAVTransportEvent();
}

// Get device URL
std::string UPnPDevice::getDeviceURL() const {
    if (!m_deviceHandle) {
        return "";
    }
    
    // Get server IP and port (no arguments in this libupnp version)
    char* ipAddr = UpnpGetServerIpAddress();
    unsigned short port = UpnpGetServerPort();
    
    if (!ipAddr) {
        return "";
    }
    
    std::stringstream ss;
    ss << "http://" << ipAddr << ":" << port;
    return ss.str();
}

// Set current position (called regularly during playback)
void UPnPDevice::setCurrentPosition(int seconds) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_currentPosition = seconds;
}

// Set track duration (called when track starts)
void UPnPDevice::setTrackDuration(int seconds) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_trackDuration = seconds;
}

// Set current URI (called when track changes)
void UPnPDevice::setCurrentURI(const std::string& uri) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_currentURI = uri;
    m_currentTrackURI = uri;
}

// Set current metadata (called when track changes)
void UPnPDevice::setCurrentMetadata(const std::string& metadata) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_currentMetadata = metadata;
    m_currentTrackMetadata = metadata;
}

// Notify track change (updates state only, no event)
void UPnPDevice::notifyTrackChange(const std::string& uri, const std::string& metadata) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_currentURI = uri;
    m_currentMetadata = metadata;
    m_currentTrackURI = uri;
    m_currentTrackMetadata = metadata;
    m_currentPosition = 0;
    m_nextURI.clear();
    m_nextMetadata.clear();
}

// Atomic gapless transition: update all track data + send single event
// This prevents the race condition where the position thread (1s polling)
// reads stale values from AudioEngine and overwrites the fresh track change
// data between notifyTrackChange() and notifyStateChange() calls.
// The epoch counter allows the position thread to detect and skip stale writes.
void UPnPDevice::notifyGaplessTransition(const std::string& uri, const std::string& metadata, int durationSeconds) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentURI = uri;
        m_currentMetadata = metadata;
        m_currentTrackURI = uri;
        m_currentTrackMetadata = metadata;
        m_currentPosition = 0;
        m_trackDuration = durationSeconds;
        m_nextURI.clear();
        m_nextMetadata.clear();
        // TransportState stays PLAYING - no change needed for gapless
        // Increment epoch so position thread detects the transition
        m_trackEpoch.fetch_add(1, std::memory_order_release);
    }
    // Send event with all consistent data in one shot
    sendAVTransportEvent();
}

// Notify position change (updates internal state for GetPositionInfo polling)
// Per UPnP spec: position is NOT evented via LastChange - control points poll GetPositionInfo
void UPnPDevice::notifyPositionChange(int seconds, int duration) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_currentPosition = seconds;
    m_trackDuration = duration;
}