#pragma once
#include "core/settings.h"
#include "core/tak_shapes.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace swak {
class Identity;
class Servers;
class UserObjects;

// Sends shared local objects as TAK CoT through all connected outputs.
//
// The user shares each object explicitly. A shared object stays shared: SWAK sends each
// change, sends a delete when the object is deleted, and sends all shared objects again
// every 15 minutes, so late receivers get them. When the outgoing queue is full or no
// output is connected, SWAK tries again every 5 seconds. Received events are never sent.
class ObjectSharing final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    static constexpr int MaxShared = 256;
    static constexpr int MaxRecipients = 64;
    static constexpr int RetryMilliseconds = 5000;
    static constexpr int RepublishMilliseconds = 15 * 60 * 1000;
    ObjectSharing(UserObjects &objects,
                  Servers *servers,
                  Identity *identity,
                  QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.sharing");
    }
    QString settingsTitle() const override
    {
        return tr("Sharing");
    }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    QString validateSettings(const QJsonObject &values) const override;

    bool automatic() const
    {
        return automatic_;
    }
    void setAutomatic(bool enabled);
    bool isShared(const QString &id) const;
    QList<ShareRecipient> recipients(const QString &id) const;
    // Shares an object. Empty recipients send it to the groups of the sender.
    // Returns an error when the object does not exist or TAK has no equal type.
    QString share(const QString &id, const QList<ShareRecipient> &recipients = {});
    // Sends a delete to the receivers and stops sending the object.
    void stopSharing(const QString &id);
    // The error of the last failed send. Empty after a successful send.
    // Tests and later status displays read it.
    // cppcheck-suppress unusedFunction
    const QString &lastError() const
    {
        return lastError_;
    }
    // Sends queued events now. Tests and the retry timer use it.
    void flush();
signals:
    void changed();

private:
    struct State
    {
        QList<ShareRecipient> recipients;
        QJsonObject sent;
        QString cotType;
        bool deleted = false;
    };
    void objectsChanged();
    void enqueue(const QString &id);
    void queueDelete(const Event &event);
    Event sender() const;
    void report(const QString &error);
    UserObjects &objects_;
    QPointer<Servers> servers_;
    QPointer<Identity> identity_;
    QHash<QString, State> states_;
    QStringList pending_;
    QList<Event> deletes_;
    QTimer retry_;
    QTimer republish_;
    QString lastError_;
    bool automatic_ = false;
};
} // namespace swak
