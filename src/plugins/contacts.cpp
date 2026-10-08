#include "core/contacts.h"

#include "core/chat.h"
#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/projection.h"
#include "core/single_line_label.h"
#include "core/store.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace swak {
namespace {
class ContactsPane final : public QWidget, public PreferenceParticipant
{
public:
    ContactsPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), contacts_(services.contacts), store_(services.store),
          tools_(services.mapTools), chat_(services.chat)
    {
        setObjectName(QStringLiteral("contactsPane"));
        auto *layout = new QVBoxLayout(this);
        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("contactsTable"));
        table_->setAccessibleName(tr("Contacts"));
        auto *proxy = new QSortFilterProxyModel(table_);
        proxy->setSourceModel(contacts_);
        proxy->setSortLocaleAware(true);
        table_->setModel(proxy);
        table_->setSortingEnabled(true);
        table_->sortByColumn(Contacts::CallsignColumn, Qt::AscendingOrder);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setAlternatingRowColors(true);
        table_->horizontalHeader()->setSectionsMovable(true);
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(table_);
        auto *row = new QHBoxLayout;
        auto *refresh = new QPushButton(tr("Refresh"), this);
        refresh->setObjectName(QStringLiteral("refreshContacts"));
        refresh->setToolTip(tr("Read the contact lists of the connected TAK Servers."));
        row->addWidget(refresh);
        row->addStretch();
        layout->addLayout(row);
        status_ =
            new SingleLineLabel(tr("Contacts appear when TAK users report their position."), this);
        status_->setObjectName(QStringLiteral("contactsStatus"));
        layout->addWidget(status_);
        if (!contacts_) {
            refresh->setEnabled(false);
            return;
        }
        connect(refresh, &QPushButton::clicked, this, [this] {
            status_->setText(tr("Reading contact lists…"));
            contacts_->refreshServers();
        });
        connect(contacts_, &Contacts::refreshed, this, [this](const QString &error) {
            status_->setText(error.isEmpty() ? tr("%n contacts.", nullptr, contacts_->rowCount())
                                             : error);
        });
        connect(table_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index) {
            show(index.data(Qt::UserRole).toString());
        });
        connect(table_, &QWidget::customContextMenuRequested, this, [this](QPoint position) {
            // A keyboard request has no row under the position. Use the current row then.
            auto index = table_->indexAt(position);
            if (!index.isValid()) {
                index = table_->currentIndex();
            }
            const auto uid = index.data(Qt::UserRole).toString();
            if (uid.isEmpty()) {
                return;
            }
            Modal<QMenu> menu(this);
            menu->setObjectName(QStringLiteral("contactMenu"));
            auto *show = menu->addAction(tr("Show on map"));
            show->setObjectName(QStringLiteral("showContact"));
            show->setEnabled(position_(uid).has_value());
            connect(show, &QAction::triggered, this, [this, uid] {
                this->show(uid);
            });
            if (chat_) {
                auto *talk = menu->addAction(tr("Chat with"));
                talk->setObjectName(QStringLiteral("chatWithContact"));
                const auto callsign =
                    index.siblingAtColumn(Contacts::CallsignColumn).data().toString();
                connect(talk, &QAction::triggered, this, [this, uid, callsign] {
                    if (chat_ && chat_->openDirect(uid, callsign).isEmpty()) {
                        status_->setText(tr("A chat with this contact cannot be opened."));
                    }
                });
            }
            menu->exec(table_->viewport()->mapToGlobal(position));
        });
    }

    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.contacts");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("header"),
                 QString::fromLatin1(table_->horizontalHeader()->saveState().toBase64())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto bytes = preferenceText(values, QStringLiteral("header"), {}, 16384).toLatin1();
        if (!bytes.isEmpty()) {
            table_->horizontalHeader()->restoreState(
                QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
        }
    }

private:
    std::optional<QPointF> position_(const QString &uid) const
    {
        const auto &events = store_.events();
        const auto found = std::find_if(events.cbegin(), events.cend(), [&uid](const Event &event) {
            return event.uid == uid;
        });
        if (found == events.cend()) {
            return {};
        }
        return project(found->latitude, found->longitude);
    }
    // Centers the map on a contact with a current position and selects it.
    void show(const QString &uid)
    {
        const auto world = position_(uid);
        if (!world) {
            status_->setText(tr("This contact has no current position."));
            return;
        }
        if (tools_) {
            emit tools_->centerRequested(*world);
        }
        store_.select(uid);
    }
    QPointer<Contacts> contacts_;
    Store &store_;
    QPointer<MapTools> tools_;
    QPointer<Chat> chat_;
    QTableView *table_ = nullptr;
    SingleLineLabel *status_ = nullptr;
};
} // namespace
class ContactsPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "contacts/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Contacts");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new ContactsPane(services, parent);
    }
};
} // namespace swak
#include "contacts.moc"
