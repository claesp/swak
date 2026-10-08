#pragma once
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

namespace swak {
class TakServerApi;
// One TAK Server group, called a channel in TAK clients. A name can appear twice, once for
// each direction: IN groups receive data and OUT groups send data.
struct TakGroup
{
    QString name;
    QString direction;
    QString description;
    bool active = false;
    // The object as the server sent it. SWAK returns it with only the active flag changed.
    QJsonObject raw;
};
// Reads the data array of a /groups/all reply. Returns false for other content.
bool readTakGroups(const QByteArray &json, QList<TakGroup> *groups);
// The /groups/active request body: every group, active when its name is in activeNames.
QByteArray activeGroupsBody(const QList<TakGroup> &groups, const QSet<QString> &activeNames);
// The channel names in order, each once, and the active names.
QStringList channelNames(const QList<TakGroup> &groups);
QSet<QString> activeChannels(const QList<TakGroup> &groups);

// Loads and sets the active channels of a TAK Server entry through its REST API.
class TakChannels final : public QObject
{
    Q_OBJECT
public:
    explicit TakChannels(TakServerApi &api, QObject *parent = nullptr);
    void load(const QString &serverId);
    void apply(const QString &serverId,
               const QList<TakGroup> &groups,
               const QSet<QString> &activeNames,
               const QString &clientUid);
signals:
    void loaded(const QString &serverId, const QList<swak::TakGroup> &groups, const QString &error);
    void applied(const QString &serverId, const QString &error);

private:
    TakServerApi &api_;
};
} // namespace swak
Q_DECLARE_METATYPE(swak::TakGroup)
