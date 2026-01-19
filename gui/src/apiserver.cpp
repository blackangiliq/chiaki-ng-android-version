// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Local API Server for Remote Controller

#include "apiserver.h"
#include "qmlbackend.h"
#include "headlessbackend.h"
#include "settings.h"
#include "discoverymanager.h"
#include "streamsession.h"

#include <chiaki/session.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QMutexLocker>
#include <SDL.h>

ApiServer::ApiServer(QmlBackend *backend, Settings *settings, QObject *parent)
    : QObject(parent)
    , backend(backend)
    , settings(settings)
{
}

ApiServer::~ApiServer()
{
    stop();
}

bool ApiServer::start(quint16 port)
{
    if (server && server->isListening())
        return true;

    server = new QTcpServer(this);
    
    connect(server, &QTcpServer::newConnection, this, &ApiServer::onNewConnection);
    
    if (!server->listen(QHostAddress::LocalHost, port)) {
        qWarning() << "API Server failed to start on port" << port << ":" << server->errorString();
        delete server;
        server = nullptr;
        return false;
    }
    
    qInfo() << "API Server started on http://127.0.0.1:" << port;
    return true;
}

void ApiServer::stop()
{
    if (server) {
        server->close();
        delete server;
        server = nullptr;
    }
    pendingData.clear();
}

void ApiServer::onNewConnection()
{
    while (server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &ApiServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &ApiServer::onDisconnected);
        pendingData[socket] = QByteArray();
    }
}

void ApiServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;

    pendingData[socket].append(socket->readAll());
    
    QByteArray &data = pendingData[socket];
    
    // Check if we have a complete HTTP request
    int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;
    
    // Parse the request line
    int firstLineEnd = data.indexOf("\r\n");
    QString requestLine = QString::fromUtf8(data.left(firstLineEnd));
    QStringList parts = requestLine.split(' ');
    
    if (parts.size() < 2) {
        sendErrorResponse(socket, 400, "Bad Request");
        return;
    }
    
    QString method = parts[0];
    QString path = parts[1];
    
    // Get Content-Length for POST/PUT requests
    int contentLength = 0;
    QByteArray headerSection = data.left(headerEnd);
    int clIndex = headerSection.toLower().indexOf("content-length:");
    if (clIndex >= 0) {
        int clEnd = headerSection.indexOf("\r\n", clIndex);
        QString clValue = QString::fromUtf8(headerSection.mid(clIndex + 15, clEnd - clIndex - 15)).trimmed();
        contentLength = clValue.toInt();
    }
    
    // Check if we have the full body
    QByteArray body = data.mid(headerEnd + 4);
    if (body.size() < contentLength)
        return; // Wait for more data
    
    body = body.left(contentLength);
    
    // Handle the request
    handleRequest(socket, method, path, body);
    
    // Clear processed data
    pendingData[socket].clear();
}

void ApiServer::onDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        pendingData.remove(socket);
        socket->deleteLater();
    }
}

void ApiServer::handleRequest(QTcpSocket *socket, const QString &method, const QString &path, const QByteArray &body)
{
    qDebug() << "API Request:" << method << path;
    
    // Parse JSON body for POST/PUT requests
    QJsonObject jsonBody;
    if (!body.isEmpty() && (method == "POST" || method == "PUT")) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            sendErrorResponse(socket, 400, "Invalid JSON: " + parseError.errorString());
            return;
        }
        jsonBody = doc.object();
    }
    
    // Route requests
    if (method == "GET" && path == "/") {
        // API info
        QJsonObject info;
        info["name"] = "Remote Controller API";
        info["version"] = "2.0";
        info["endpoints"] = QJsonArray({
            QJsonObject({{"method", "GET"}, {"path", "/hosts"}, {"description", "Get discovered and registered hosts"}}),
            QJsonObject({{"method", "POST"}, {"path", "/register"}, {"description", "Register a console"}}),
            QJsonObject({{"method", "POST"}, {"path", "/connect"}, {"description", "Connect to a host"}}),
            QJsonObject({{"method", "POST"}, {"path", "/disconnect"}, {"description", "Disconnect from current session"}}),
            QJsonObject({{"method", "POST"}, {"path", "/wakeup"}, {"description", "Wake up a console"}}),
            QJsonObject({{"method", "GET"}, {"path", "/stream/status"}, {"description", "Get current stream status"}}),
            QJsonObject({{"method", "GET"}, {"path", "/settings"}, {"description", "Get all settings"}}),
            QJsonObject({{"method", "PUT"}, {"path", "/settings"}, {"description", "Update settings"}}),
            QJsonObject({{"method", "GET"}, {"path", "/settings/video"}, {"description", "Get video settings"}}),
            QJsonObject({{"method", "PUT"}, {"path", "/settings/video"}, {"description", "Update video settings"}}),
            QJsonObject({{"method", "GET"}, {"path", "/settings/devices"}, {"description", "Get available audio devices"}})
        });
        sendJsonResponse(socket, 200, QJsonDocument(info));
    }
    // Hosts
    else if (method == "GET" && path == "/hosts") {
        sendJsonResponse(socket, 200, handleGetHosts());
    }
    else if (method == "POST" && path == "/register") {
        sendJsonResponse(socket, 200, handlePostRegister(jsonBody));
    }
    // Stream Control
    else if (method == "POST" && path == "/connect") {
        sendJsonResponse(socket, 200, handlePostConnect(jsonBody));
    }
    else if (method == "POST" && path == "/disconnect") {
        sendJsonResponse(socket, 200, handlePostDisconnect());
    }
    else if (method == "POST" && path == "/wakeup") {
        sendJsonResponse(socket, 200, handlePostWakeup(jsonBody));
    }
    else if (method == "GET" && path == "/stream/status") {
        sendJsonResponse(socket, 200, handleGetStreamStatus());
    }
    // Settings
    else if (method == "GET" && path == "/settings") {
        sendJsonResponse(socket, 200, handleGetSettings());
    }
    else if (method == "PUT" && path == "/settings") {
        sendJsonResponse(socket, 200, handlePutSettings(jsonBody));
    }
    else if (method == "GET" && path == "/settings/video") {
        sendJsonResponse(socket, 200, handleGetVideoSettings());
    }
    else if (method == "PUT" && path == "/settings/video") {
        sendJsonResponse(socket, 200, handlePutVideoSettings(jsonBody));
    }
    else if (method == "GET" && path == "/settings/devices") {
        sendJsonResponse(socket, 200, handleGetSettingsDevices());
    }
    else {
        sendErrorResponse(socket, 404, "Not Found");
    }
}

void ApiServer::sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonDocument &json)
{
    QByteArray body = json.toJson(QJsonDocument::Compact);
    
    QString statusText;
    switch (statusCode) {
        case 200: statusText = "OK"; break;
        case 400: statusText = "Bad Request"; break;
        case 404: statusText = "Not Found"; break;
        case 500: statusText = "Internal Server Error"; break;
        default: statusText = "Unknown"; break;
    }
    
    QString response = QString(
        "HTTP/1.1 %1 %2\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %3\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).arg(statusCode).arg(statusText).arg(body.size());
    
    socket->write(response.toUtf8());
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

void ApiServer::sendErrorResponse(QTcpSocket *socket, int statusCode, const QString &error)
{
    QJsonObject errorObj;
    errorObj["error"] = error;
    errorObj["status"] = statusCode;
    errorObj["success"] = false;
    sendJsonResponse(socket, statusCode, QJsonDocument(errorObj));
}

// ==================== Hosts API ====================

QJsonDocument ApiServer::handleGetHosts()
{
    QJsonArray hostsArray;
    
    QVariantList hostsList;
    if (headlessBackend) {
        hostsList = headlessBackend->hosts();
    } else if (backend) {
        hostsList = backend->hosts();
    }
    
    for (const QVariant &hostVar : hostsList) {
        QVariantMap host = hostVar.toMap();
        
        if (!host["display"].toBool())
            continue;
        
        QJsonObject hostObj;
        hostObj["name"] = host["name"].toString();
        hostObj["address"] = host["address"].toString();
        hostObj["mac"] = host["mac"].toString();
        hostObj["ps5"] = host["ps5"].toBool();
        hostObj["state"] = host["state"].toString();
        hostObj["registered"] = host["registered"].toBool();
        hostObj["discovered"] = host["discovered"].toBool();
        
        if (host.contains("app") && !host["app"].toString().isEmpty()) {
            hostObj["runningApp"] = host["app"].toString();
            hostObj["titleId"] = host["titleId"].toString();
        }
        
        hostsArray.append(hostObj);
    }
    
    QJsonObject response;
    response["success"] = true;
    response["count"] = hostsArray.size();
    response["hosts"] = hostsArray;
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handlePostRegister(const QJsonObject &body)
{
    QString host = body["host"].toString();
    QString psnId = body["psn_id"].toString();
    QString pin = body["pin"].toString();
    QString consolePin = body["console_pin"].toString();
    QString targetStr = body["target"].toString().toLower();
    
    if (host.isEmpty()) {
        QJsonObject error;
        error["success"] = false;
        error["error"] = "Missing required field: host";
        return QJsonDocument(error);
    }
    
    if (psnId.isEmpty()) {
        QJsonObject error;
        error["success"] = false;
        error["error"] = "Missing required field: psn_id";
        return QJsonDocument(error);
    }
    
    if (pin.isEmpty() || pin.length() != 8) {
        QJsonObject error;
        error["success"] = false;
        error["error"] = "Invalid pin: must be 8 digits";
        return QJsonDocument(error);
    }
    
    int target;
    if (targetStr == "ps4_7") target = 800;
    else if (targetStr == "ps4_75") target = 900;
    else if (targetStr == "ps4_8" || targetStr == "ps4") target = 1000;
    else target = 1000100; // PS5
    
    bool broadcast = (host == "255.255.255.255");
    
    QJsonObject response;
    bool started = false;
    
    if (headlessBackend) {
        started = headlessBackend->registerHost(host, psnId, pin, consolePin, broadcast, target, QJSValue());
    } else if (backend) {
        started = backend->registerHost(host, psnId, pin, consolePin, broadcast, target, QJSValue());
    }
    
    if (started) {
        response["success"] = true;
        response["message"] = "Registration process started";
    } else {
        response["success"] = false;
        response["error"] = "Failed to start registration";
    }
    
    return QJsonDocument(response);
}

// ==================== Stream Control API ====================

QJsonDocument ApiServer::handlePostConnect(const QJsonObject &body)
{
    int index = body["index"].toInt(-1);
    QString nickname = body["nickname"].toString();
    
    QJsonObject response;
    
    // Check if already streaming
    StreamSession *existingSession = nullptr;
    if (headlessBackend) {
        existingSession = headlessBackend->session();
    } else if (backend) {
        existingSession = backend->qmlSession();
    }
    
    if (existingSession) {
        response["success"] = false;
        response["error"] = "Already connected. Disconnect first before connecting to another host.";
        return QJsonDocument(response);
    }
    
    if (index < 0) {
        // Find by nickname or address
        QString address = body["address"].toString();
        QVariantList hostsList;
        if (headlessBackend) {
            hostsList = headlessBackend->hosts();
        } else if (backend) {
            hostsList = backend->hosts();
        }
        
        for (int i = 0; i < hostsList.size(); i++) {
            QVariantMap host = hostsList[i].toMap();
            if (host["name"].toString() == nickname || host["address"].toString() == address) {
                index = i;
                break;
            }
        }
    }
    
    if (index < 0) {
        response["success"] = false;
        response["error"] = "Host not found. Provide index, nickname, or address.";
        return QJsonDocument(response);
    }
    
    if (headlessBackend) {
        headlessBackend->connectToHost(index, nickname);
    } else if (backend) {
        backend->connectToHost(index, nickname);
    }
    
    response["success"] = true;
    response["message"] = "Connection initiated";
    response["index"] = index;
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handlePostDisconnect()
{
    QJsonObject response;
    
    // Check if there's an active session first
    StreamSession *existingSession = nullptr;
    if (headlessBackend) {
        existingSession = headlessBackend->session();
    } else if (backend) {
        existingSession = backend->qmlSession();
    }
    
    if (!existingSession) {
        response["success"] = false;
        response["error"] = "No active session to disconnect";
        return QJsonDocument(response);
    }
    
    if (headlessBackend) {
        headlessBackend->stopSession(false);
    } else if (backend) {
        backend->stopSession(false);
    }
    
    response["success"] = true;
    response["message"] = "Disconnect requested";
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handlePostWakeup(const QJsonObject &body)
{
    int index = body["index"].toInt(-1);
    QString nickname = body["nickname"].toString();
    
    QJsonObject response;
    
    if (index < 0) {
        QString address = body["address"].toString();
        QVariantList hostsList;
        if (headlessBackend) {
            hostsList = headlessBackend->hosts();
        } else if (backend) {
            hostsList = backend->hosts();
        }
        
        for (int i = 0; i < hostsList.size(); i++) {
            QVariantMap host = hostsList[i].toMap();
            if (host["name"].toString() == nickname || host["address"].toString() == address) {
                index = i;
                break;
            }
        }
    }
    
    if (index < 0) {
        response["success"] = false;
        response["error"] = "Host not found";
        return QJsonDocument(response);
    }
    
    if (headlessBackend) {
        headlessBackend->wakeUpHost(index, nickname);
    } else if (backend) {
        backend->wakeUpHost(index, nickname);
    }
    
    response["success"] = true;
    response["message"] = "Wakeup signal sent";
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handleGetStreamStatus()
{
    QJsonObject response;
    
    StreamSession *session = nullptr;
    if (headlessBackend) {
        session = headlessBackend->session();
    } else if (backend) {
        session = backend->qmlSession();
    }
    
    if (session) {
        response["streaming"] = true;
        response["connected"] = session->GetConnected();
        response["host"] = session->GetHost();
        response["bitrate"] = session->GetMeasuredBitrate();
        response["packetLoss"] = session->GetAveragePacketLoss();
        response["muted"] = session->GetMuted();
    } else {
        response["streaming"] = false;
        response["connected"] = false;
    }
    
    response["headless"] = isHeadless();
    response["success"] = true;
    
    return QJsonDocument(response);
}

// ==================== Settings API ====================

QJsonDocument ApiServer::handleGetSettings()
{
    QJsonObject response;
    response["success"] = true;
    
    // General Settings
    QJsonObject general;
    general["hardwareDecoder"] = settings->GetHardwareDecoder();
    general["hideCursor"] = settings->GetHideCursor();
    
    // Window Type
    QString windowType;
    switch (settings->GetWindowType()) {
        case WindowType::SelectedResolution: windowType = "selected_resolution"; break;
        case WindowType::CustomResolution: windowType = "custom_resolution"; break;
        case WindowType::Fullscreen: windowType = "fullscreen"; break;
        case WindowType::Zoom: windowType = "zoom"; break;
        case WindowType::Stretch: windowType = "stretch"; break;
        default: windowType = "selected_resolution"; break;
    }
    general["windowType"] = windowType;
    
    // Placebo Preset
    QString placeboPreset;
    switch (settings->GetPlaceboPreset()) {
        case PlaceboPreset::Fast: placeboPreset = "fast"; break;
        case PlaceboPreset::Default: placeboPreset = "default"; break;
        case PlaceboPreset::HighQuality: placeboPreset = "high_quality"; break;
        case PlaceboPreset::Custom: placeboPreset = "custom"; break;
        default: placeboPreset = "default"; break;
    }
    general["placeboPreset"] = placeboPreset;
    
    // Frame Sharing Settings (for Chiki)
    general["frameSharingEnabled"] = settings->GetFrameSharingEnabled();
    general["localRenderDisabled"] = settings->GetLocalRenderDisabled();
    general["showStreamStats"] = settings->GetShowStreamStats();
    
    // Audio Settings
    QString audioOutDevice = settings->GetAudioOutDevice();
    general["audioOutDevice"] = audioOutDevice.isEmpty() ? "Auto" : audioOutDevice;
    
    QString audioInDevice = settings->GetAudioInDevice();
    general["audioInDevice"] = audioInDevice.isEmpty() ? "Auto" : audioInDevice;
    
    general["audioVolume"] = settings->GetAudioVolume();
    general["audioBufferSize"] = (int)settings->GetAudioBufferSizeRaw();
    general["audioVideoDisabled"] = (int)settings->GetAudioVideoDisabled();
    
    // Network Settings
    general["wifiDroppedNotif"] = (int)settings->GetWifiDroppedNotif();
    general["discoveryEnabled"] = settings->GetDiscoveryEnabled();
    
    // Controller/Input Settings
    general["keyboardEnabled"] = settings->GetKeyboardEnabled();
    general["mouseTouchEnabled"] = settings->GetMouseTouchEnabled();
    general["dpadTouchEnabled"] = settings->GetDpadTouchEnabled();
    general["buttonsByPosition"] = settings->GetButtonsByPosition();
    general["allowJoystickBackgroundEvents"] = settings->GetAllowJoystickBackgroundEvents();
    
    // Rumble/Haptics
    RumbleHapticsIntensity rumble = settings->GetRumbleHapticsIntensity();
    QString rumbleStr;
    switch (rumble) {
        case RumbleHapticsIntensity::Off: rumbleStr = "off"; break;
        case RumbleHapticsIntensity::VeryWeak: rumbleStr = "very_weak"; break;
        case RumbleHapticsIntensity::Weak: rumbleStr = "weak"; break;
        case RumbleHapticsIntensity::Normal: rumbleStr = "normal"; break;
        case RumbleHapticsIntensity::Strong: rumbleStr = "strong"; break;
        case RumbleHapticsIntensity::VeryStrong: rumbleStr = "very_strong"; break;
        default: rumbleStr = "normal"; break;
    }
    general["rumbleHapticsIntensity"] = rumbleStr;
    general["hapticOverride"] = settings->GetHapticOverride();
    
    // Stream Settings
    general["streamerMode"] = settings->GetStreamerMode();
    general["automaticConnect"] = settings->GetAutomaticConnect();
    general["startMicUnmuted"] = settings->GetStartMicUnmuted();
    general["fullscreenDoubleClickEnabled"] = settings->GetFullscreenDoubleClickEnabled();
    
    // Display Settings
    general["displayTargetContrast"] = settings->GetDisplayTargetContrast();
    general["displayTargetPeak"] = settings->GetDisplayTargetPeak();
    general["displayTargetTrc"] = settings->GetDisplayTargetTrc();
    general["displayTargetPrim"] = settings->GetDisplayTargetPrim();
    general["customResolutionWidth"] = (int)settings->GetCustomResolutionWidth();
    general["customResolutionHeight"] = (int)settings->GetCustomResolutionHeight();
    general["zoomFactor"] = settings->GetZoomFactor();
    general["packetLossMax"] = settings->GetPacketLossMax();
    
    // Log Settings
    general["logVerbose"] = settings->GetLogVerbose();
    
    response["general"] = general;
    
    // Video Settings - PS5 Local
    QJsonObject ps5Local;
    ps5Local["resolution"] = (int)settings->GetResolutionLocalPS5();
    ps5Local["fps"] = (int)settings->GetFPSLocalPS5();
    ps5Local["bitrate"] = (int)settings->GetBitrateLocalPS5();
    ps5Local["codec"] = settings->GetCodecLocalPS5() == CHIAKI_CODEC_H265 ? "h265" : "h264";
    
    // Video Settings - PS5 Remote
    QJsonObject ps5Remote;
    ps5Remote["resolution"] = (int)settings->GetResolutionRemotePS5();
    ps5Remote["fps"] = (int)settings->GetFPSRemotePS5();
    ps5Remote["bitrate"] = (int)settings->GetBitrateRemotePS5();
    ps5Remote["codec"] = settings->GetCodecRemotePS5() == CHIAKI_CODEC_H265 ? "h265" : "h264";
    
    // Video Settings - PS4 Local
    QJsonObject ps4Local;
    ps4Local["resolution"] = (int)settings->GetResolutionLocalPS4();
    ps4Local["fps"] = (int)settings->GetFPSLocalPS4();
    ps4Local["bitrate"] = (int)settings->GetBitrateLocalPS4();
    
    // Video Settings - PS4 Remote
    QJsonObject ps4Remote;
    ps4Remote["resolution"] = (int)settings->GetResolutionRemotePS4();
    ps4Remote["fps"] = (int)settings->GetFPSRemotePS4();
    ps4Remote["bitrate"] = (int)settings->GetBitrateRemotePS4();
    
    QJsonObject video;
    video["ps5_local"] = ps5Local;
    video["ps5_remote"] = ps5Remote;
    video["ps4_local"] = ps4Local;
    video["ps4_remote"] = ps4Remote;
    
    response["video"] = video;
    
    // Schema - Allowed values for each setting
    QJsonObject schema;
    QJsonObject generalSchema;
    
    // Window Type allowed values
    QJsonArray windowTypeValues;
    windowTypeValues.append("selected_resolution");
    windowTypeValues.append("custom_resolution");
    windowTypeValues.append("fullscreen");
    windowTypeValues.append("zoom");
    windowTypeValues.append("stretch");
    generalSchema["windowType"] = QJsonObject({
        {"type", "string"},
        {"allowedValues", windowTypeValues}
    });
    
    // Placebo Preset allowed values
    QJsonArray placeboPresetValues;
    placeboPresetValues.append("fast");
    placeboPresetValues.append("default");
    placeboPresetValues.append("high_quality");
    placeboPresetValues.append("custom");
    generalSchema["placeboPreset"] = QJsonObject({
        {"type", "string"},
        {"allowedValues", placeboPresetValues}
    });
    
    // Rumble Haptics Intensity allowed values
    QJsonArray rumbleIntensityValues;
    rumbleIntensityValues.append("off");
    rumbleIntensityValues.append("very_weak");
    rumbleIntensityValues.append("weak");
    rumbleIntensityValues.append("normal");
    rumbleIntensityValues.append("strong");
    rumbleIntensityValues.append("very_strong");
    generalSchema["rumbleHapticsIntensity"] = QJsonObject({
        {"type", "string"},
        {"allowedValues", rumbleIntensityValues}
    });
    
    // Audio Video Disabled allowed values
    QJsonArray audioVideoDisabledValues;
    audioVideoDisabledValues.append(0); // Audio and Video Enabled
    audioVideoDisabledValues.append(1); // Audio Disabled
    audioVideoDisabledValues.append(2); // Video Disabled
    audioVideoDisabledValues.append(3); // Audio and Video Disabled
    generalSchema["audioVideoDisabled"] = QJsonObject({
        {"type", "integer"},
        {"allowedValues", audioVideoDisabledValues},
        {"description", "0=Both Enabled, 1=Audio Disabled, 2=Video Disabled, 3=Both Disabled"}
    });
    
    // Type hints
    generalSchema["hardwareDecoder"] = QJsonObject({{"type", "string"}});
    generalSchema["hideCursor"] = QJsonObject({{"type", "boolean"}});
    generalSchema["frameSharingEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["localRenderDisabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["showStreamStats"] = QJsonObject({{"type", "boolean"}});
    generalSchema["audioOutDevice"] = QJsonObject({{"type", "string"}, {"description", "Use GET /settings/devices to get available devices"}});
    generalSchema["audioInDevice"] = QJsonObject({{"type", "string"}, {"description", "Use GET /settings/devices to get available devices"}});
    generalSchema["audioVolume"] = QJsonObject({{"type", "integer"}, {"min", 0}, {"max", 128}});
    generalSchema["audioBufferSize"] = QJsonObject({{"type", "integer"}, {"min", 0}, {"description", "0 = automatic"}});
    generalSchema["wifiDroppedNotif"] = QJsonObject({{"type", "integer"}, {"min", 0}, {"max", 100}});
    generalSchema["discoveryEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["keyboardEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["mouseTouchEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["dpadTouchEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["buttonsByPosition"] = QJsonObject({{"type", "boolean"}});
    generalSchema["allowJoystickBackgroundEvents"] = QJsonObject({{"type", "boolean"}});
    generalSchema["hapticOverride"] = QJsonObject({{"type", "number"}, {"min", 0.0}, {"max", 1.0}});
    generalSchema["streamerMode"] = QJsonObject({{"type", "boolean"}});
    generalSchema["automaticConnect"] = QJsonObject({{"type", "boolean"}});
    generalSchema["startMicUnmuted"] = QJsonObject({{"type", "boolean"}});
    generalSchema["fullscreenDoubleClickEnabled"] = QJsonObject({{"type", "boolean"}});
    generalSchema["displayTargetContrast"] = QJsonObject({{"type", "integer"}});
    generalSchema["displayTargetPeak"] = QJsonObject({{"type", "integer"}});
    generalSchema["displayTargetTrc"] = QJsonObject({{"type", "integer"}});
    generalSchema["displayTargetPrim"] = QJsonObject({{"type", "integer"}});
    generalSchema["customResolutionWidth"] = QJsonObject({{"type", "integer"}, {"min", 0}});
    generalSchema["customResolutionHeight"] = QJsonObject({{"type", "integer"}, {"min", 0}});
    generalSchema["zoomFactor"] = QJsonObject({{"type", "number"}, {"min", 0.1}, {"max", 10.0}});
    generalSchema["packetLossMax"] = QJsonObject({{"type", "number"}, {"min", 0.0}, {"max", 1.0}});
    generalSchema["logVerbose"] = QJsonObject({{"type", "boolean"}});
    
    schema["general"] = generalSchema;
    
    response["schema"] = schema;
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handlePutSettings(const QJsonObject &body)
{
    QJsonObject response;
    response["success"] = true;
    QJsonArray updated;
    
    // General settings
    if (body.contains("hardwareDecoder")) {
        settings->SetHardwareDecoder(body["hardwareDecoder"].toString());
        updated.append("hardwareDecoder");
    }
    
    if (body.contains("hideCursor")) {
        settings->SetHideCursor(body["hideCursor"].toBool());
        updated.append("hideCursor");
    }
    
    if (body.contains("windowType")) {
        QString wt = body["windowType"].toString().toLower();
        WindowType type = WindowType::SelectedResolution;
        if (wt == "fullscreen") type = WindowType::Fullscreen;
        else if (wt == "zoom") type = WindowType::Zoom;
        else if (wt == "stretch") type = WindowType::Stretch;
        else if (wt == "custom_resolution") type = WindowType::CustomResolution;
        settings->SetWindowType(type);
        updated.append("windowType");
    }
    
    if (body.contains("placeboPreset")) {
        QString pp = body["placeboPreset"].toString().toLower();
        PlaceboPreset preset = PlaceboPreset::Default;
        if (pp == "fast") preset = PlaceboPreset::Fast;
        else if (pp == "high_quality") preset = PlaceboPreset::HighQuality;
        else if (pp == "custom") preset = PlaceboPreset::Custom;
        settings->SetPlaceboPreset(preset);
        updated.append("placeboPreset");
    }
    
    // Frame Sharing Settings (for Chiki)
    if (body.contains("frameSharingEnabled")) {
        settings->SetFrameSharingEnabled(body["frameSharingEnabled"].toBool());
        updated.append("frameSharingEnabled");
    }
    
    if (body.contains("localRenderDisabled")) {
        settings->SetLocalRenderDisabled(body["localRenderDisabled"].toBool());
        updated.append("localRenderDisabled");
    }
    
    if (body.contains("showStreamStats")) {
        settings->SetShowStreamStats(body["showStreamStats"].toBool());
        updated.append("showStreamStats");
    }
    
    // Audio Settings
    if (body.contains("audioOutDevice")) {
        QString device = body["audioOutDevice"].toString();
        if (device == "Auto" || device.isEmpty()) {
            settings->SetAudioOutDevice("");
        } else {
            settings->SetAudioOutDevice(device);
        }
        updated.append("audioOutDevice");
    }
    
    if (body.contains("audioInDevice")) {
        QString device = body["audioInDevice"].toString();
        if (device == "Auto" || device.isEmpty()) {
            settings->SetAudioInDevice("");
        } else {
            settings->SetAudioInDevice(device);
        }
        updated.append("audioInDevice");
    }
    
    if (body.contains("audioVolume")) {
        settings->SetAudioVolume(body["audioVolume"].toInt());
        updated.append("audioVolume");
    }
    
    if (body.contains("audioBufferSize")) {
        settings->SetAudioBufferSize(body["audioBufferSize"].toInt());
        updated.append("audioBufferSize");
    }
    
    if (body.contains("audioVideoDisabled")) {
        settings->SetAudioVideoDisabled(static_cast<ChiakiDisableAudioVideo>(body["audioVideoDisabled"].toInt()));
        updated.append("audioVideoDisabled");
    }
    
    // Network Settings
    if (body.contains("wifiDroppedNotif")) {
        settings->SetWifiDroppedNotif(body["wifiDroppedNotif"].toInt());
        updated.append("wifiDroppedNotif");
    }
    
    if (body.contains("discoveryEnabled")) {
        settings->SetDiscoveryEnabled(body["discoveryEnabled"].toBool());
        updated.append("discoveryEnabled");
    }
    
    // Controller/Input Settings
    if (body.contains("keyboardEnabled")) {
        settings->SetKeyboardEnabled(body["keyboardEnabled"].toBool());
        updated.append("keyboardEnabled");
    }
    
    if (body.contains("mouseTouchEnabled")) {
        settings->SetMouseTouchEnabled(body["mouseTouchEnabled"].toBool());
        updated.append("mouseTouchEnabled");
    }
    
    if (body.contains("dpadTouchEnabled")) {
        settings->SetDpadTouchEnabled(body["dpadTouchEnabled"].toBool());
        updated.append("dpadTouchEnabled");
    }
    
    if (body.contains("buttonsByPosition")) {
        settings->SetButtonsByPosition(body["buttonsByPosition"].toBool());
        updated.append("buttonsByPosition");
    }
    
    if (body.contains("allowJoystickBackgroundEvents")) {
        settings->SetAllowJoystickBackgroundEvents(body["allowJoystickBackgroundEvents"].toBool());
        updated.append("allowJoystickBackgroundEvents");
    }
    
    // Rumble/Haptics
    if (body.contains("rumbleHapticsIntensity")) {
        QString ri = body["rumbleHapticsIntensity"].toString().toLower();
        RumbleHapticsIntensity intensity = RumbleHapticsIntensity::Normal;
        if (ri == "off") intensity = RumbleHapticsIntensity::Off;
        else if (ri == "very_weak") intensity = RumbleHapticsIntensity::VeryWeak;
        else if (ri == "weak") intensity = RumbleHapticsIntensity::Weak;
        else if (ri == "strong") intensity = RumbleHapticsIntensity::Strong;
        else if (ri == "very_strong") intensity = RumbleHapticsIntensity::VeryStrong;
        settings->SetRumbleHapticsIntensity(intensity);
        updated.append("rumbleHapticsIntensity");
    }
    
    if (body.contains("hapticOverride")) {
        settings->SetHapticOverride(body["hapticOverride"].toDouble());
        updated.append("hapticOverride");
    }
    
    // Stream Settings
    if (body.contains("streamerMode")) {
        settings->SetStreamerMode(body["streamerMode"].toBool());
        updated.append("streamerMode");
    }
    
    if (body.contains("automaticConnect")) {
        settings->SetAutomaticConnect(body["automaticConnect"].toBool());
        updated.append("automaticConnect");
    }
    
    if (body.contains("startMicUnmuted")) {
        settings->SetStartMicUnmuted(body["startMicUnmuted"].toBool());
        updated.append("startMicUnmuted");
    }
    
    if (body.contains("fullscreenDoubleClickEnabled")) {
        settings->SetFullscreenDoubleClickEnabled(body["fullscreenDoubleClickEnabled"].toBool());
        updated.append("fullscreenDoubleClickEnabled");
    }
    
    // Display Settings
    if (body.contains("displayTargetContrast")) {
        settings->SetDisplayTargetContrast(body["displayTargetContrast"].toInt());
        updated.append("displayTargetContrast");
    }
    
    if (body.contains("displayTargetPeak")) {
        settings->SetDisplayTargetPeak(body["displayTargetPeak"].toInt());
        updated.append("displayTargetPeak");
    }
    
    if (body.contains("displayTargetTrc")) {
        settings->SetDisplayTargetTrc(body["displayTargetTrc"].toInt());
        updated.append("displayTargetTrc");
    }
    
    if (body.contains("displayTargetPrim")) {
        settings->SetDisplayTargetPrim(body["displayTargetPrim"].toInt());
        updated.append("displayTargetPrim");
    }
    
    if (body.contains("customResolutionWidth")) {
        settings->SetCustomResolutionWidth(body["customResolutionWidth"].toInt());
        updated.append("customResolutionWidth");
    }
    
    if (body.contains("customResolutionHeight")) {
        settings->SetCustomResolutionHeight(body["customResolutionHeight"].toInt());
        updated.append("customResolutionHeight");
    }
    
    if (body.contains("zoomFactor")) {
        settings->SetZoomFactor(body["zoomFactor"].toDouble());
        updated.append("zoomFactor");
    }
    
    if (body.contains("packetLossMax")) {
        settings->SetPacketLossMax(body["packetLossMax"].toDouble());
        updated.append("packetLossMax");
    }
    
    // Log Settings
    if (body.contains("logVerbose")) {
        settings->SetLogVerbose(body["logVerbose"].toBool());
        updated.append("logVerbose");
    }
    
    response["updated"] = updated;
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handleGetVideoSettings()
{
    QJsonObject response;
    response["success"] = true;
    
    // Resolution mapping
    auto resolutionToString = [](ChiakiVideoResolutionPreset res) -> QString {
        switch (res) {
            case CHIAKI_VIDEO_RESOLUTION_PRESET_360p: return "360p";
            case CHIAKI_VIDEO_RESOLUTION_PRESET_540p: return "540p";
            case CHIAKI_VIDEO_RESOLUTION_PRESET_720p: return "720p";
            case CHIAKI_VIDEO_RESOLUTION_PRESET_1080p: return "1080p";
            default: return "720p";
        }
    };
    
    auto fpsToString = [](ChiakiVideoFPSPreset fps) -> QString {
        switch (fps) {
            case CHIAKI_VIDEO_FPS_PRESET_30: return "30";
            case CHIAKI_VIDEO_FPS_PRESET_60: return "60";
            default: return "60";
        }
    };
    
    // PS5 Settings
    QJsonObject ps5;
    QJsonObject ps5Local;
    ps5Local["resolution"] = resolutionToString(settings->GetResolutionLocalPS5());
    ps5Local["fps"] = fpsToString(settings->GetFPSLocalPS5());
    ps5Local["bitrate"] = (int)settings->GetBitrateLocalPS5();
    ps5Local["codec"] = settings->GetCodecLocalPS5() == CHIAKI_CODEC_H265 ? "h265" : "h264";
    
    QJsonObject ps5Remote;
    ps5Remote["resolution"] = resolutionToString(settings->GetResolutionRemotePS5());
    ps5Remote["fps"] = fpsToString(settings->GetFPSRemotePS5());
    ps5Remote["bitrate"] = (int)settings->GetBitrateRemotePS5();
    ps5Remote["codec"] = settings->GetCodecRemotePS5() == CHIAKI_CODEC_H265 ? "h265" : "h264";
    
    ps5["local"] = ps5Local;
    ps5["remote"] = ps5Remote;
    response["ps5"] = ps5;
    
    // PS4 Settings
    QJsonObject ps4;
    QJsonObject ps4Local;
    ps4Local["resolution"] = resolutionToString(settings->GetResolutionLocalPS4());
    ps4Local["fps"] = fpsToString(settings->GetFPSLocalPS4());
    ps4Local["bitrate"] = (int)settings->GetBitrateLocalPS4();
    
    QJsonObject ps4Remote;
    ps4Remote["resolution"] = resolutionToString(settings->GetResolutionRemotePS4());
    ps4Remote["fps"] = fpsToString(settings->GetFPSRemotePS4());
    ps4Remote["bitrate"] = (int)settings->GetBitrateRemotePS4();
    
    ps4["local"] = ps4Local;
    ps4["remote"] = ps4Remote;
    response["ps4"] = ps4;
    
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handlePutVideoSettings(const QJsonObject &body)
{
    QJsonObject response;
    response["success"] = true;
    QJsonArray updated;
    
    // Helper functions
    auto stringToResolution = [](const QString &res) -> ChiakiVideoResolutionPreset {
        if (res == "360p") return CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
        if (res == "540p") return CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
        if (res == "720p") return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
        if (res == "1080p") return CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
        return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
    };
    
    auto stringToFps = [](const QString &fps) -> ChiakiVideoFPSPreset {
        if (fps == "30") return CHIAKI_VIDEO_FPS_PRESET_30;
        if (fps == "60") return CHIAKI_VIDEO_FPS_PRESET_60;
        return CHIAKI_VIDEO_FPS_PRESET_60;
    };
    
    // PS5 Local Settings
    if (body.contains("ps5_local")) {
        QJsonObject ps5Local = body["ps5_local"].toObject();
        
        if (ps5Local.contains("resolution")) {
            settings->SetResolutionLocalPS5(stringToResolution(ps5Local["resolution"].toString()));
            updated.append("ps5_local.resolution");
        }
        if (ps5Local.contains("fps")) {
            settings->SetFPSLocalPS5(stringToFps(ps5Local["fps"].toString()));
            updated.append("ps5_local.fps");
        }
        if (ps5Local.contains("bitrate")) {
            settings->SetBitrateLocalPS5(ps5Local["bitrate"].toInt());
            updated.append("ps5_local.bitrate");
        }
        if (ps5Local.contains("codec")) {
            ChiakiCodec codec = ps5Local["codec"].toString().toLower() == "h265" ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
            settings->SetCodecLocalPS5(codec);
            updated.append("ps5_local.codec");
        }
    }
    
    // PS5 Remote Settings
    if (body.contains("ps5_remote")) {
        QJsonObject ps5Remote = body["ps5_remote"].toObject();
        
        if (ps5Remote.contains("resolution")) {
            settings->SetResolutionRemotePS5(stringToResolution(ps5Remote["resolution"].toString()));
            updated.append("ps5_remote.resolution");
        }
        if (ps5Remote.contains("fps")) {
            settings->SetFPSRemotePS5(stringToFps(ps5Remote["fps"].toString()));
            updated.append("ps5_remote.fps");
        }
        if (ps5Remote.contains("bitrate")) {
            settings->SetBitrateRemotePS5(ps5Remote["bitrate"].toInt());
            updated.append("ps5_remote.bitrate");
        }
        if (ps5Remote.contains("codec")) {
            ChiakiCodec codec = ps5Remote["codec"].toString().toLower() == "h265" ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
            settings->SetCodecRemotePS5(codec);
            updated.append("ps5_remote.codec");
        }
    }
    
    // PS4 Local Settings
    if (body.contains("ps4_local")) {
        QJsonObject ps4Local = body["ps4_local"].toObject();
        
        if (ps4Local.contains("resolution")) {
            settings->SetResolutionLocalPS4(stringToResolution(ps4Local["resolution"].toString()));
            updated.append("ps4_local.resolution");
        }
        if (ps4Local.contains("fps")) {
            settings->SetFPSLocalPS4(stringToFps(ps4Local["fps"].toString()));
            updated.append("ps4_local.fps");
        }
        if (ps4Local.contains("bitrate")) {
            settings->SetBitrateLocalPS4(ps4Local["bitrate"].toInt());
            updated.append("ps4_local.bitrate");
        }
    }
    
    // PS4 Remote Settings
    if (body.contains("ps4_remote")) {
        QJsonObject ps4Remote = body["ps4_remote"].toObject();
        
        if (ps4Remote.contains("resolution")) {
            settings->SetResolutionRemotePS4(stringToResolution(ps4Remote["resolution"].toString()));
            updated.append("ps4_remote.resolution");
        }
        if (ps4Remote.contains("fps")) {
            settings->SetFPSRemotePS4(stringToFps(ps4Remote["fps"].toString()));
            updated.append("ps4_remote.fps");
        }
        if (ps4Remote.contains("bitrate")) {
            settings->SetBitrateRemotePS4(ps4Remote["bitrate"].toInt());
            updated.append("ps4_remote.bitrate");
        }
    }
    
    response["updated"] = updated;
    return QJsonDocument(response);
}

QJsonDocument ApiServer::handleGetSettingsDevices()
{
    QJsonObject response;
    response["success"] = true;
    
    // Get audio devices
    QJsonArray inputDevices;
    QJsonArray outputDevices;
    
    // Input devices (capture = true)
    int inputCount = SDL_GetNumAudioDevices(true);
    for (int i = 0; i < inputCount; i++) {
        const char* deviceName = SDL_GetAudioDeviceName(i, true);
        if (deviceName) {
            inputDevices.append(QString::fromUtf8(deviceName));
        }
    }
    
    // Output devices (capture = false)
    int outputCount = SDL_GetNumAudioDevices(false);
    for (int i = 0; i < outputCount; i++) {
        const char* deviceName = SDL_GetAudioDeviceName(i, false);
        if (deviceName) {
            outputDevices.append(QString::fromUtf8(deviceName));
        }
    }
    
    QJsonObject devices;
    devices["input"] = inputDevices;
    devices["output"] = outputDevices;
    devices["currentInput"] = settings->GetAudioInDevice().isEmpty() ? "Auto" : settings->GetAudioInDevice();
    devices["currentOutput"] = settings->GetAudioOutDevice().isEmpty() ? "Auto" : settings->GetAudioOutDevice();
    
    response["devices"] = devices;
    
    return QJsonDocument(response);
}
