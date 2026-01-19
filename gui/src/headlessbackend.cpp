// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Headless Backend - No GUI, API only

#include "headlessbackend.h"
#include "apiserver.h"
#include "host.h"
#include "qmlbackend.h"

#include <chiaki/session.h>
#include <chiaki/regist.h>

#include <QDebug>
#include <QTimer>
#include <QThread>

HeadlessBackend::HeadlessBackend(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_discoveryManager(this)
{
    // Setup discovery manager with settings
    m_discoveryManager.SetSettings(settings);
    
    // Connect discovery signals
    connect(&m_discoveryManager, &DiscoveryManager::HostsUpdated, 
            this, &HeadlessBackend::onDiscoveryHostsChanged);
    
    // Enable discovery
    m_discoveryManager.SetActive(true);
    
    qInfo() << "Headless backend initialized";
}

HeadlessBackend::~HeadlessBackend()
{
    stop();
}

bool HeadlessBackend::start(quint16 apiPort)
{
    // Start API server (runs on main thread but uses non-blocking I/O)
    m_apiServer = new ApiServer(nullptr, m_settings, this);
    m_apiServer->setHeadlessBackend(this);
    
    if (!m_apiServer->start(apiPort)) {
        qCritical() << "Failed to start API Server on port" << apiPort;
        delete m_apiServer;
        m_apiServer = nullptr;
        return false;
    }
    
    qInfo() << "API Server running on http://127.0.0.1:" << apiPort;
    qInfo() << "Headless mode started - waiting for API commands...";
    
    return true;
}

void HeadlessBackend::stop()
{
    if (m_session) {
        stopSession(false);
    }
    
    if (m_apiServer) {
        m_apiServer->stop();
        delete m_apiServer;
        m_apiServer = nullptr;
    }
    
    FrameSharing::instance().shutdown();
}

void HeadlessBackend::onDiscoveryHostsChanged()
{
    updateDiscoveryHosts();
    emit hostsChanged();
}

void HeadlessBackend::updateDiscoveryHosts()
{
    m_discoveryHosts = m_discoveryManager.GetHosts();
}

HeadlessBackend::DisplayServer HeadlessBackend::displayServerAt(int index) const
{
    DisplayServer server;
    
    // First: discovered hosts
    int i = 0;
    for (const auto &host : m_discoveryHosts) {
        if (i == index) {
            server.valid = true;
            server.discovered = true;
            server.discovery_host = host;
            
            HostMAC mac = host.GetHostMAC();
            if (m_settings->GetRegisteredHostRegistered(mac)) {
                server.registered = true;
                server.registered_host = m_settings->GetRegisteredHost(mac);
            }
            return server;
        }
        i++;
    }
    
    // Then: manual hosts
    for (const auto &manual : m_settings->GetManualHosts()) {
        if (i == index) {
            server.valid = true;
            server.discovered = false;
            server.manual_host = manual;
            
            if (manual.GetRegistered() && m_settings->GetRegisteredHostRegistered(manual.GetMAC())) {
                server.registered = true;
                server.registered_host = m_settings->GetRegisteredHost(manual.GetMAC());
            }
            return server;
        }
        i++;
    }
    
    return server;
}

QVariantList HeadlessBackend::hosts() const
{
    QVariantList result;
    
    // Discovery hosts
    for (const auto &host : m_discoveryHosts) {
        QVariantMap m;
        m["display"] = true;
        m["discovered"] = true;
        m["name"] = host.host_name;
        m["address"] = host.host_addr;
        m["mac"] = host.host_id;
        m["ps5"] = host.ps5;
        m["state"] = chiaki_discovery_host_state_string(host.state);
        m["app"] = host.running_app_name;
        m["titleId"] = host.running_app_titleid;
        
        HostMAC mac = host.GetHostMAC();
        m["registered"] = m_settings->GetRegisteredHostRegistered(mac);
        
        result.append(m);
    }
    
    // Manual hosts
    for (const auto &manual : m_settings->GetManualHosts()) {
        bool found = false;
        for (const auto &disc : m_discoveryHosts) {
            if (disc.host_addr == manual.GetHost()) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            QVariantMap m;
            m["display"] = true;
            m["discovered"] = false;
            m["name"] = manual.GetHost();
            m["address"] = manual.GetHost();
            m["registered"] = manual.GetRegistered();
            m["ps5"] = true;
            m["state"] = "unknown";
            result.append(m);
        }
    }
    
    return result;
}

bool HeadlessBackend::registerHost(const QString &host, const QString &psn_id, 
                                   const QString &pin, const QString &cpin,
                                   bool broadcast, int target, const QJSValue &callback)
{
    Q_UNUSED(callback);
    
    qInfo() << "Starting registration for host:" << host << "target:" << target;
    
    // Create registration info
    ChiakiRegistInfo info = {};
    QByteArray hostb = host.toUtf8();
    info.host = hostb.constData();
    info.target = static_cast<ChiakiTarget>(target);
    info.broadcast = broadcast;
    info.pin = (uint32_t)pin.toULong();
    info.console_pin = (uint32_t)cpin.toULong();
    info.holepunch_info = nullptr;
    info.rudp = nullptr;
    
    QByteArray psn_idb;
    if (target == CHIAKI_TARGET_PS4_8) {
        psn_idb = psn_id.toUtf8();
        info.psn_online_id = psn_idb.constData();
    } else {
        QByteArray account_id = QByteArray::fromBase64(psn_id.toUtf8());
        if (account_id.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            qWarning() << "Invalid PSN Account ID size:" << account_id.size() << "expected:" << CHIAKI_PSN_ACCOUNT_ID_SIZE;
            emit error(tr("Invalid Account-ID"), tr("The PSN Account-ID must be exactly %1 bytes encoded as base64.").arg(CHIAKI_PSN_ACCOUNT_ID_SIZE));
            return false;
        }
        info.psn_online_id = nullptr;
        memcpy(info.psn_account_id, account_id.constData(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
    }
    
    // Create QmlRegist object to handle registration
    auto regist = new QmlRegist(info, m_settings->GetLogLevelMask(), this);
    
    connect(regist, &QmlRegist::log, this, [](ChiakiLogLevel level, QString msg) {
        qDebug() << "[REGIST]" << chiaki_log_level_char(level) << msg;
    });
    
    connect(regist, &QmlRegist::failed, this, [this, regist]() {
        qWarning() << "Registration failed";
        emit error("Registration Failed", "Failed to register with the console");
        regist->deleteLater();
    });
    
    connect(regist, &QmlRegist::success, this, [this, host, regist](const RegisteredHost &rhost) {
        qInfo() << "Registration successful for" << rhost.GetServerNickname();
        m_settings->AddRegisteredHost(rhost);
        
        // Also save as manual host if not discovered
        ManualHost manual_host;
        manual_host.SetHost(host);
        manual_host.Register(rhost);
        m_settings->SetManualHost(manual_host);
        
        emit hostsChanged();
        regist->deleteLater();
    });
    
    return true;
}

void HeadlessBackend::connectToHost(int index, QString nickname)
{
    Q_UNUSED(nickname);
    
    // Prevent double connect calls
    if (m_connectInProgress) {
        qWarning() << "Connect already in progress, ignoring duplicate call";
        return;
    }
    
    // If already streaming, ignore
    if (m_session) {
        qWarning() << "Session already active, disconnect first";
        emit error("Connection Error", "Already connected. Disconnect first.");
        return;
    }
    
    DisplayServer server = displayServerAt(index);
    if (!server.valid) {
        qWarning() << "Invalid host index:" << index;
        emit error("Connection Error", "Invalid host index");
        return;
    }
    
    if (!server.registered) {
        qWarning() << "Host not registered";
        emit error("Connection Error", "Host not registered");
        return;
    }
    
    m_connectInProgress = true;
    
    StreamSessionConnectInfo connect_info(
        m_settings,
        server.registered_host.GetTarget(),
        server.GetHostAddr(),
        server.registered_host.GetServerNickname(),
        server.registered_host.GetRPRegistKey(),
        server.registered_host.GetRPKey(),
        QString(),  // initial_login_pin
        QString(),  // duid
        false,      // auto_regist
        false,      // fullscreen
        false,      // zoom
        false       // stretch
    );
    
    createSession(connect_info);
    m_connectInProgress = false;
}

void HeadlessBackend::createSession(const StreamSessionConnectInfo &connect_info)
{
    // Safety check - should not happen due to connectToHost check
    if (m_session) {
        qWarning() << "Session already exists, cannot create new one";
        return;
    }
    
    try {
        m_session = new StreamSession(connect_info, this);
    } catch (const std::exception &e) {
        qCritical() << "Failed to create session:" << e.what();
        emit error("Session Error", e.what());
        return;
    }
    
    connect(m_session, &StreamSession::SessionQuit, 
            this, &HeadlessBackend::onSessionQuit);
    
    // Frame processing (headless - just share frames, no rendering)
    connect(m_session, &StreamSession::FfmpegFrameAvailable, 
            this, &HeadlessBackend::processFrame, Qt::QueuedConnection);
    
    // Initialize frame sharing
    if (m_settings->GetFrameSharingEnabled()) {
        FrameSharing::instance().initialize(1920, 1080);
        qInfo() << "Frame sharing enabled via shared memory";
    }
    
    m_session->Start();
    emit sessionChanged(m_session);
    
    qInfo() << "Stream session started";
}

void HeadlessBackend::processFrame()
{
    if (!m_session)
        return;
    
    ChiakiFfmpegDecoder *decoder = m_session->GetFfmpegDecoder();
    if (!decoder)
        return;
    
    int32_t frames_lost;
    AVFrame *frame = chiaki_ffmpeg_decoder_pull_frame(decoder, &frames_lost);
    if (!frame)
        return;
    
    // In headless mode: only share frame via memory, no rendering
    if (m_settings->GetFrameSharingEnabled() && FrameSharing::instance().isActive()) {
        AVFrame *shareFrame = frame;
        AVFrame *swFrame = nullptr;
        
        // Hardware frame? Transfer to software first
        if (frame->hw_frames_ctx) {
            swFrame = av_frame_alloc();
            if (swFrame && av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                av_frame_copy_props(swFrame, frame);
                shareFrame = swFrame;
            } else {
                if (swFrame) av_frame_free(&swFrame);
                swFrame = nullptr;
                shareFrame = nullptr;
            }
        }
        
        // Queue frame for async sharing (non-blocking)
        if (shareFrame && shareFrame->data[0]) {
            FrameSharing::instance().queueFrame(shareFrame);
        }
        
        if (swFrame) av_frame_free(&swFrame);
    }
    
    av_frame_free(&frame);
}

void HeadlessBackend::stopSession(bool sleep)
{
    // Prevent double disconnect calls
    if (m_disconnectInProgress) {
        qWarning() << "Disconnect already in progress, ignoring duplicate call";
        return;
    }
    
    if (!m_session) {
        qWarning() << "No active session to disconnect";
        return;
    }
    
    m_disconnectInProgress = true;
    
    // Disconnect signals first to prevent onSessionQuit from being called
    disconnect(m_session, &StreamSession::SessionQuit, 
               this, &HeadlessBackend::onSessionQuit);
    
    if (sleep) {
        m_session->GoToBed();
    } else {
        m_session->Stop();
    }
    
    // Wait a bit for clean shutdown
    QThread::msleep(100);
    
    m_session->deleteLater();
    m_session = nullptr;
    
    FrameSharing::instance().shutdown();
    
    emit sessionChanged(nullptr);
    
    m_disconnectInProgress = false;
    
    qInfo() << "Stream session stopped";
}

void HeadlessBackend::onSessionQuit(ChiakiQuitReason reason, const QString &reason_str)
{
    Q_UNUSED(reason);
    qInfo() << "Session quit:" << reason_str;
    
    // Safety check - session might already be cleaned up by stopSession
    if (!m_session) {
        qWarning() << "onSessionQuit called but session already null";
        return;
    }
    
    // Prevent stopSession from running after this
    m_disconnectInProgress = true;
    
    m_session->deleteLater();
    m_session = nullptr;
    
    FrameSharing::instance().shutdown();
    
    emit sessionChanged(nullptr);
    
    m_disconnectInProgress = false;
}

void HeadlessBackend::wakeUpHost(int index, QString nickname)
{
    Q_UNUSED(nickname);
    
    DisplayServer server = displayServerAt(index);
    if (!server.valid || !server.registered) {
        qWarning() << "Cannot wake up: invalid or unregistered host";
        return;
    }
    
    sendWakeup(server);
}

bool HeadlessBackend::sendWakeup(const DisplayServer &server)
{
    if (!server.registered)
        return false;
    
    return sendWakeup(
        server.GetHostAddr(),
        server.registered_host.GetRPRegistKey(),
        server.IsPS5()
    );
}

bool HeadlessBackend::sendWakeup(const QString &host, const QByteArray &regist_key, bool ps5)
{
    qInfo() << "Sending wakeup to" << host;
    
    try {
        m_discoveryManager.SendWakeup(host, regist_key, ps5);
        return true;
    } catch (const std::exception &e) {
        qWarning() << "Wakeup failed:" << e.what();
        return false;
    }
}
