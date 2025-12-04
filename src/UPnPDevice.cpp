#include "UPnPDevice.hpp"
#include "ProtocolInfoBuilder.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdlib>

// Helper pour extraire une valeur d'un document IXML
static const char* ixmlGetFirstDocumentItem(IXML_Document* doc, const char* item) {
    IXML_NodeList* nodeList = ixmlDocument_getElementsByTagName(doc, (char*)item);
    if (!nodeList) return nullptr;
    
    IXML_Node* node = ixmlNodeList_item(nodeList, 0);
    if (!node) {
        ixmlNodeList_free(nodeList);
        return nullptr;
    }
    
    IXML_Node* textNode = ixmlNode_getFirstChild(node);
    const char* value = nullptr;
    if (textNode) {
        value = ixmlNode_getNodeValue(textNode);
    }
    
    ixmlNodeList_free(nodeList);
    return value;
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
    std::cout << "[UPnPDevice] Created: " << m_config.friendlyName << std::endl;
    
    // Générer le ProtocolInfo basé sur les capacités Diretta/Holo Audio
    std::cout << "[UPnPDevice] Generating ProtocolInfo..." << std::endl;
    auto caps = ProtocolInfoBuilder::getHoloAudioCapabilities();
    m_protocolInfo = ProtocolInfoBuilder::buildProtocolInfo(caps);
    
    size_t numFormats = std::count(m_protocolInfo.begin(), m_protocolInfo.end(), ',') + 1;
    std::cout << "[UPnPDevice] ProtocolInfo: " 
              << m_protocolInfo.length() << " chars, "
              << numFormats << " formats" << std::endl;
}

UPnPDevice::~UPnPDevice() {
    stop();
    std::cout << "[UPnPDevice] Destroyed" << std::endl;
}

bool UPnPDevice::start() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    if (m_running) {
        std::cerr << "[UPnPDevice] Already running" << std::endl;
        return false;
    }
    
    std::cout << "[UPnPDevice] Starting..." << std::endl;
    
    // 1. Initialize libupnp
    int ret = UpnpInit2(nullptr, m_config.port);
    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpInit2 failed: " << ret << std::endl;
        return false;
    }
    
    // 2. Get server info
    m_ipAddress = UpnpGetServerIpAddress();
    m_actualPort = UpnpGetServerPort();
    
    std::cout << "[UPnPDevice] Server started: http://" << m_ipAddress 
              << ":" << m_actualPort << std::endl;
    
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
    
    std::cout << "[UPnPDevice] ✓ SCPD files created and webserver configured" << std::endl;
    
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
    
    std::cout << "[UPnPDevice] ✓ Device registered (handle=" 
              << m_deviceHandle << ")" << std::endl;
    
    // 8. Send SSDP advertisements
    ret = UpnpSendAdvertisement(m_deviceHandle, 1800);  // 30 minutes
    if (ret != UPNP_E_SUCCESS) {
        std::cerr << "[UPnPDevice] UpnpSendAdvertisement failed: " 
                  << ret << std::endl;
    } else {
        std::cout << "[UPnPDevice] ✓ SSDP advertisements sent" << std::endl;
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
    
    std::cout << "[UPnPDevice] Stopping..." << std::endl;
    
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
    
    std::cout << "[UPnPDevice] ✓ Stopped" << std::endl;
}

void UPnPDevice::setCallbacks(const Callbacks& callbacks) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_callbacks = callbacks;
    std::cout << "[UPnPDevice] Callbacks set" << std::endl;
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
    
    std::cout << "[UPnPDevice] Action: " << actionName 
              << " (service: " << serviceID << ")" << std::endl;
    
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
        }
    }
    
    // ConnectionManager actions
    if (serviceID.find("ConnectionManager") != std::string::npos) {
        if (actionName == "GetProtocolInfo") {
            IXML_Document* response = createActionResponse("GetProtocolInfo");
            addResponseArg(response, "Source", "");
            
            // Liste explicite des formats supportés
            // Requis par certains contrôleurs stricts comme Audirvana
            std::string sinkProtocols = 
                // WAV (requis par Audirvana)
                "http-get:*:audio/wav:*,"
                "http-get:*:audio/x-wav:*,"
                "http-get:*:audio/wave:*,"
                "http-get:*:audio/x-pn-wav:*,"
                // AIFF (requis par Audirvana)
                "http-get:*:audio/aiff:*,"
                "http-get:*:audio/x-aiff:*,"
                // FLAC
                "http-get:*:audio/flac:*,"
                "http-get:*:audio/x-flac:*,"
                // ALAC (Apple Lossless)
                "http-get:*:audio/m4a:*,"
                "http-get:*:audio/x-m4a:*,"
                "http-get:*:audio/mp4:*,"
                // MP3
                "http-get:*:audio/mpeg:*,"
                "http-get:*:audio/mp3:*,"
                "http-get:*:audio/x-mpeg:*,"
                // OGG
                "http-get:*:audio/ogg:*,"
                "http-get:*:audio/x-ogg:*,"
                // DSD (DSF/DFF)
                "http-get:*:audio/dsd:*,"
                "http-get:*:audio/x-dsd:*,"
                "http-get:*:audio/dsf:*,"
                "http-get:*:audio/x-dsf:*,"
                "http-get:*:audio/dff:*,"
                "http-get:*:audio/x-dff:*,"
                // WMA
                "http-get:*:audio/x-ms-wma:*,"
                // APE
                "http-get:*:audio/x-ape:*,"
                // Wildcard générique en dernier (pour compatibilité)
                "http-get:*:audio/*:*";
            
            addResponseArg(response, "Sink", sinkProtocols);
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
    
    std::cout << "[UPnPDevice] Subscription request for: " << serviceID << std::endl;
    
    // Accept all subscriptions
    // UpnpSubscriptionRequest_set_ErrCode(request, UPNP_E_SUCCESS);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::handleGetVarRequest(UpnpStateVarRequest* request) {
    std::string varName = UpnpString_get_String(
        UpnpStateVarRequest_get_StateVarName(request)
    );
    
    std::cout << "[UPnPDevice] GetVar: " << varName << std::endl;
    
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
    
    std::cout << "[UPnPDevice] SetAVTransportURI: " << uri << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentURI = uri;
        m_currentMetadata = metadata;
        m_currentTrackURI = uri;
        m_currentTrackMetadata = metadata;
        m_currentPosition = 0;
        m_trackDuration = 0;
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
    
    std::cout << "[UPnPDevice] SetNextAVTransportURI: " << uri << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextURI = uri;
        m_nextMetadata = metadata;
    }
    
    // Callback
    if (m_callbacks.onSetNextURI) {
        m_callbacks.onSetNextURI(uri, metadata);
    }
    
    // Send event notification
    sendAVTransportEvent();
    
    // Response
    IXML_Document* response = createActionResponse("SetNextAVTransportURI");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionPlay(UpnpActionRequest* request) {
    std::cout << "[UPnPDevice] Play" << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_transportState = "PLAYING";
        m_transportStatus = "OK";
    }
    
    // Callback
    if (m_callbacks.onPlay) {
        m_callbacks.onPlay();
    }
    
    // Send event notification
    sendAVTransportEvent();
    
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
    
    // Callback
    if (m_callbacks.onPause) {
        m_callbacks.onPause();
    }
    
    // Send event notification
    sendAVTransportEvent();
    
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
        std::cout << "[UPnPDevice] Changing state: " << m_transportState 
                  << " → STOPPED" << std::endl;
        m_transportState = "STOPPED";
        m_currentPosition = 0;
    }
    
    // Callback
    if (m_callbacks.onStop) {
        std::cout << "[UPnPDevice] ✓ Calling onStop callback..." << std::endl;
        m_callbacks.onStop();
        std::cout << "[UPnPDevice] ✓ onStop callback completed" << std::endl;
    } else {
        std::cout << "[UPnPDevice] ❌ NO onStop CALLBACK CONFIGURED!" << std::endl;
    }    
    // Send event notification
    sendAVTransportEvent();
    
    // Response
    std::cout << "[UPnPDevice] Creating response..." << std::endl;
    IXML_Document* response = createActionResponse("Stop");
    UpnpActionRequest_set_ActionResult(request, response);
    std::cout << "[UPnPDevice] ✓ Stop action completed" << std::endl;
    
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

int UPnPDevice::actionGetDeviceCapabilities(UpnpActionRequest* request) {
    IXML_Document* response = createActionResponse("GetDeviceCapabilities");
    addResponseArg(response, "PlayMedia", "NETWORK");
    addResponseArg(response, "RecMedia", "NOT_IMPLEMENTED");
    addResponseArg(response, "RecQualityModes", "NOT_IMPLEMENTED");
    
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

// ============================================================================
// RenderingControl Actions
// ============================================================================

int UPnPDevice::actionGetVolume(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetVolume");
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
    IXML_Document* response = createActionResponse("SetVolume");
    UpnpActionRequest_set_ActionResult(request, response);
    
    return UPNP_E_SUCCESS;
}

int UPnPDevice::actionGetMute(UpnpActionRequest* request) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    
    IXML_Document* response = createActionResponse("GetMute");
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
    IXML_Document* response = createActionResponse("SetMute");
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
IXML_Document* UPnPDevice::createActionResponse(const std::string& actionName) {
    IXML_Document* response = ixmlDocument_createDocument();
    IXML_Element* actionResponse = ixmlDocument_createElement(response, 
        (actionName + "Response").c_str());
    ixmlElement_setAttribute(actionResponse, "xmlns:u", 
        "urn:schemas-upnp-org:service:AVTransport:1");
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

// Helper: Send AVTransport event
void UPnPDevice::sendAVTransportEvent() {
    // Event notification would go here
    // For now, we just track state changes
}

// Helper: Send RenderingControl event
void UPnPDevice::sendRenderingControlEvent() {
    // Event notification would go here
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
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="no">
      <name>A_ARG_TYPE_InstanceID</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>AVTransportURI</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>AVTransportURIMetaData</name>
      <dataType>string</dataType>
    </stateVariable>
    <stateVariable sendEvents="yes">
      <name>TransportState</name>
      <dataType>string</dataType>
      <allowedValueList>
        <allowedValue>STOPPED</allowedValue>
        <allowedValue>PLAYING</allowedValue>
        <allowedValue>PAUSED_PLAYBACK</allowedValue>
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
      <name>CurrentTrack</name>
      <dataType>ui4</dataType>
    </stateVariable>
    <stateVariable sendEvents="no">
      <name>CurrentTrackDuration</name>
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
  </actionList>
  <serviceStateTable>
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
    m_transportState = state;
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

// Notify track change (sends event to subscribers)
void UPnPDevice::notifyTrackChange(const std::string& uri, const std::string& metadata) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentURI = uri;
        m_currentMetadata = metadata;
        m_currentTrackURI = uri;
        m_currentTrackMetadata = metadata;
    }
    // Send AVTransport event to notify subscribers
    sendAVTransportEvent();
}

// Notify position change (sends event to subscribers)
void UPnPDevice::notifyPositionChange(int seconds, int duration) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentPosition = seconds;
        m_trackDuration = duration;
    }
    // Send AVTransport event to notify subscribers (mConnect, BubbleUPnP)
    sendAVTransportEvent();
}