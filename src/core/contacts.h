#pragma once
#include "core/event.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QTimer>

namespace swak {
class Store;
class Servers;
class TakServerApi;
// One TAK user that SWAK can address.
struct Contact
{
    QString uid;
    QString callsign;
    QString team;
    QString role;
    QString endpoint;
    QString device;
    // The server entry that reported the contact.
    QString origin;
    QDateTime lastReport;
    // True while the contact has a current position on the map.
    bool online = false;
    bool operator==(const Contact &) const = default;
};
// Reads a /Marti/api/contacts/all reply. Returns false for other content.
bool readTakContacts(const QByteArray &json, const QString &origin, QList<Contact> *contacts);

// The contacts of all connections. Received position reports with a contact endpoint add
// contacts. The TAK REST contact list adds users without a current position. The model
// rebuilds at most once a second, so busy streams do not rebuild it for each message.
class Contacts final : public QAbstractTableModel
{
    Q_OBJECT
public:
    static constexpr int MaxContacts = 2048;
    enum Column
    {
        CallsignColumn,
        TeamColumn,
        RoleColumn,
        ServerColumn,
        ReportColumn,
        ColumnCount
    };
    Contacts(Store &store, Servers *servers, TakServerApi *api, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    const QList<Contact> &contacts() const
    {
        return contacts_;
    }
    // Reads the REST contact lists of all entries with a TAK REST API. The result arrives later.
    void refreshServers();
    // Rebuilds the list now. Tests and the Refresh command use it.
    void rebuild();
signals:
    void refreshed(const QString &error);

private:
    Store &store_;
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QList<Contact> contacts_;
    QHash<QString, QList<Contact>> serverContacts_;
    QTimer timer_;
};
} // namespace swak
