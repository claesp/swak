#pragma once
#include "core/settings.h"
#include "core/event.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <memory>
namespace swak {
class Store;
class ServerWorker;
struct ServerMailbox;
QJsonObject serverDefaults();
QList<SettingField> serverFields();
QString validateServer(const QJsonObject &source);
class Servers final : public QObject, public SettingsParticipant, public SettingsWidgetProvider {
    Q_OBJECT
public:
    explicit Servers(Store &store, QObject *parent = nullptr);
    ~Servers() override;
    QString preferenceId() const override { return QStringLiteral("org.swak.udp"); }
    QString settingsTitle() const override { return tr("Servers"); }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    QString validateSettings(const QJsonObject &values) const override;
    QWidget *createSettingsWidget(QWidget *parent) override;
    QJsonObject sources() const { return sources_; }
    QString put(const QString &id, const QJsonObject &source);
    void remove(const QString &id);
    QString status(const QString &id) const;
    QJsonObject statistics() const;
    QString send(const Event &event);
    void reconnect();
    QJsonObject lan() const { return lan_; }
    QString setLan(const QJsonObject &values);
signals:
    void changed();
    void statusChanged();
    void managementRequested();
private:
    void configure();
    void collect();
    Store &store_;
    QThread thread_;
    ServerWorker *worker_;
    std::shared_ptr<ServerMailbox> mailbox_;
    QTimer timer_;
    QJsonObject sources_;
    QJsonObject lan_;
    QJsonObject stats_;
    quint64 generation_ = 0;
};
}
