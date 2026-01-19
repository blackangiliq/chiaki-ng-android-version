// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Headless Backend - No GUI, API only

#ifndef CHIAKI_HEADLESSBACKEND_H
#define CHIAKI_HEADLESSBACKEND_H

#include <QObject>
#include <QCoreApplication>
#include <QJSValue>

#include "settings.h"
#include "discoverymanager.h"
#include "streamsession.h"
#include "framesharing.h"

#include <chiaki/regist.h>

class ApiServer;
class QmlRegist;

class HeadlessBackend : public QObject
{
    Q_OBJECT

public:
    explicit HeadlessBackend(Settings *settings, QObject *parent = nullptr);
    ~HeadlessBackend();

    bool start(quint16 apiPort = 5218);
    void stop();

    // API-compatible methods (same as QmlBackend)
    QVariantList hosts() const;
    bool registerHost(const QString &host, const QString &psn_id, const QString &pin, 
                     const QString &cpin, bool broadcast, int target, const QJSValue &callback);
    void connectToHost(int index, QString nickname = QString());
    void stopSession(bool sleep);
    void wakeUpHost(int index, QString nickname = QString());
    
    StreamSession *session() const { return m_session; }
    Settings *getSettings() const { return m_settings; }

signals:
    void sessionChanged(StreamSession *session);
    void hostsChanged();
    void error(const QString &title, const QString &text);

private slots:
    void onDiscoveryHostsChanged();
    void onSessionQuit(ChiakiQuitReason reason, const QString &reason_str);
    void processFrame();

private:
    void updateDiscoveryHosts();
    void createSession(const StreamSessionConnectInfo &connect_info);
    
    struct DisplayServer {
        bool valid = false;
        DiscoveryHost discovery_host;
        ManualHost manual_host;
        bool discovered;
        RegisteredHost registered_host;
        bool registered;
        
        QString GetHostAddr() const { return discovered ? discovery_host.host_addr : manual_host.GetHost(); }
        bool IsPS5() const { return discovered ? discovery_host.ps5 :
            (registered ? chiaki_target_is_ps5(registered_host.GetTarget()) : true); }
    };
    
    DisplayServer displayServerAt(int index) const;
    bool sendWakeup(const DisplayServer &server);
    bool sendWakeup(const QString &host, const QByteArray &regist_key, bool ps5);

    Settings *m_settings = nullptr;
    ApiServer *m_apiServer = nullptr;
    DiscoveryManager m_discoveryManager;
    StreamSession *m_session = nullptr;
    
    QList<DiscoveryHost> m_discoveryHosts;
    
    // State flags to prevent race conditions from API double-calls
    bool m_connectInProgress = false;
    bool m_disconnectInProgress = false;
};

#endif // CHIAKI_HEADLESSBACKEND_H
