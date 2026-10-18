#pragma once
#include "core/event.h"
#include "core/settings.h"

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <memory>
namespace swak {
class Store;
class ServerWorker;
struct ServerMailbox;
QJsonObject serverDefaults();
// Replaces the host name lookup of QUIC entries, so tests can give one name several
// addresses. Pass nullptr to restore QHostInfo. Production code does not call it.
using QuicHostLookup = QList<QHostAddress> (*)(const QString &host);
void setQuicHostLookupForTests(QuicHostLookup lookup);
QList<SettingField> serverFields(const QJsonObject &source = {});
QString validateServer(const QJsonObject &source);
class Servers final : public QObject, public SettingsParticipant, public SettingsWidgetProvider
{
    Q_OBJECT
public:
    explicit Servers(Store &store, QObject *parent = nullptr);
    ~Servers() override;
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.udp");
    }
    QString settingsTitle() const override
    {
        return tr("Servers");
    }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    QWidget *createSettingsWidget(QWidget *parent) override;
    QJsonObject sources() const
    {
        return sources_;
    }
    QString put(const QString &id, const QJsonObject &source);
    void remove(const QString &id);
    QString status(const QString &id) const;
    QJsonObject statistics() const;
    QString send(const Event &event);
    void reconnect();
    // Adds a line from another service, such as the TAK REST check, to the entry status.
    void setServerInfo(const QString &id, const QString &text);
    // The sender UID and callsign for TAK logins. Only entries with a login reconnect.
    void setSenderIdentity(const QString &uid, const QString &callsign);
    QString senderUid() const
    {
        return sender_.value(QStringLiteral("senderUid")).toString();
    }
signals:
    void changed();
    void statusChanged();

private:
    void configure();
    QString connectionStatus(const QString &id) const;
    void collect();
    Store &store_;
    QThread thread_;
    ServerWorker *worker_;
    std::shared_ptr<ServerMailbox> mailbox_;
    QTimer timer_;
    QJsonObject sources_;
    QJsonObject stats_;
    QHash<QString, QString> info_;
    QJsonObject sender_;
    quint64 generation_ = 0;
};
} // namespace swak
