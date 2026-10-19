#include "core/missions.h"

#include "core/file_shares.h"
#include "core/identity.h"
#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/server_choice.h"
#include "core/servers.h"
#include "core/single_line_label.h"
#include "core/store.h"
#include "core/tak_server_api.h"
#include "core/tak_shapes.h"
#include "core/text.h"
#include "core/user_objects.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace swak {
namespace {
QString roleText(MissionSubscription::Role role)
{
    switch (role) {
    case MissionSubscription::Role::ReadOnly:
        return QObject::tr("read-only");
    case MissionSubscription::Role::Subscriber:
        return QObject::tr("subscriber");
    case MissionSubscription::Role::Owner:
        return QObject::tr("owner");
    }
    return {};
}
// Adds mission commands to the menus of local objects and mission objects.
class MissionTool final : public MapTool
{
public:
    MissionTool(PluginServices &services, QObject *parent)
        : MapTool(parent), missions_(services.missions), objects_(services.objects),
          identity_(services.identity), store_(services.store)
    {
    }
    void populateInformationMenu(const MapItem &item, QMenu &menu) override
    {
        populateContextMenu(item, menu);
    }
    void populateContextMenu(const MapItem &item, QMenu &menu) override
    {
        if (!missions_ || missions_->subscriptions().isEmpty()) {
            return;
        }
        if (item.owner == QStringLiteral("org.swak.events")) {
            // A mission object can leave its mission.
            const auto &events = store_.events();
            const auto found =
                std::find_if(events.cbegin(), events.cend(), [&item](const Event &event) {
                    return event.uid == item.id;
                });
            if (found == events.cend()) {
                return;
            }
            for (const auto &mission : missions_->subscriptions()) {
                if (found->origin == missionOrigin(mission.serverId, mission.name) &&
                    mission.editable()) {
                    auto *remove = menu.addAction(tr("Remove from %1").arg(mission.name));
                    remove->setObjectName(QStringLiteral("removeFromMission"));
                    connect(remove, &QAction::triggered, this, [this, mission, uid = item.id] {
                        if (missions_) {
                            missions_->removeObject(mission.serverId, mission.name, uid);
                        }
                    });
                }
            }
            return;
        }
        const auto object = objects_ ? objects_->find(item.id) : std::nullopt;
        if (!object || sharedCotType(*object).isEmpty()) {
            return;
        }
        auto *submenu = menu.addMenu(tr("Add to Mission"));
        submenu->setObjectName(QStringLiteral("addToMission"));
        for (const auto &mission : missions_->subscriptions()) {
            auto *add = submenu->addAction(mission.name);
            add->setEnabled(mission.editable());
            connect(add, &QAction::triggered, this, [this, mission, id = item.id] {
                const auto current = objects_ ? objects_->find(id) : std::nullopt;
                Event sender;
                if (identity_) {
                    identity_->apply(sender);
                }
                const auto event =
                    current
                        ? sharedObjectEvent(*current, sender, {}, QDateTime::currentDateTimeUtc())
                        : std::nullopt;
                if (event && missions_) {
                    missions_->addEvent(mission.serverId, mission.name, *event);
                }
            });
        }
    }

private:
    QPointer<Missions> missions_;
    QPointer<UserObjects> objects_;
    QPointer<Identity> identity_;
    Store &store_;
};
class MissionsPane final : public QWidget, public PreferenceParticipant
{
public:
    MissionsPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), missions_(services.missions), servers_(services.servers),
          files_(services.files)
    {
        setObjectName(QStringLiteral("missionsPane"));
        auto *layout = new QVBoxLayout(this);
        auto *row = new QHBoxLayout;
        serverChoice_ = new QComboBox(this);
        serverChoice_->setObjectName(QStringLiteral("missionServer"));
        serverChoice_->setAccessibleName(tr("TAK Server"));
        auto *refresh = new QPushButton(tr("Refresh"), this);
        refresh->setObjectName(QStringLiteral("listMissions"));
        auto *subscribe = new QPushButton(tr("Subscribe ..."), this);
        subscribe->setObjectName(QStringLiteral("subscribeMission"));
        row->addWidget(serverChoice_, 1);
        row->addWidget(refresh);
        row->addWidget(subscribe);
        layout->addLayout(row);
        splitter_ = new QSplitter(Qt::Vertical, this);
        splitter_->setObjectName(QStringLiteral("missionsSplitter"));
        available_ = new QTableWidget(0, 4, splitter_);
        available_->setObjectName(QStringLiteral("missionList"));
        available_->setHorizontalHeaderLabels(
            {tr("Name"), tr("Description"), tr("Creator"), tr("Password")});
        available_->setSelectionBehavior(QAbstractItemView::SelectRows);
        available_->setSelectionMode(QAbstractItemView::SingleSelection);
        available_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        available_->verticalHeader()->hide();
        available_->horizontalHeader()->setStretchLastSection(true);
        auto *subscribedSide = new QWidget(splitter_);
        auto *subscribedLayout = new QVBoxLayout(subscribedSide);
        subscribedLayout->setContentsMargins(0, 0, 0, 0);
        subscribed_ = new QListWidget(subscribedSide);
        subscribed_->setObjectName(QStringLiteral("missionSubscriptions"));
        subscribed_->setAccessibleName(tr("Subscribed missions"));
        subscribedLayout->addWidget(subscribed_);
        auto *actions = new QHBoxLayout;
        auto *reload = new QPushButton(tr("Reload"), subscribedSide);
        reload->setObjectName(QStringLiteral("reloadMission"));
        auto *unsubscribe = new QPushButton(tr("Unsubscribe"), subscribedSide);
        unsubscribe->setObjectName(QStringLiteral("unsubscribeMission"));
        auto *exportButton = new QPushButton(tr("Export ..."), subscribedSide);
        exportButton->setObjectName(QStringLiteral("exportMission"));
        actions->addWidget(reload);
        actions->addWidget(unsubscribe);
        actions->addWidget(exportButton);
        actions->addStretch();
        subscribedLayout->addLayout(actions);
        auto *details = new QSplitter(Qt::Horizontal, subscribedSide);
        fileList_ = new QListWidget(details);
        fileList_->setObjectName(QStringLiteral("missionFiles"));
        fileList_->setAccessibleName(tr("Mission files"));
        auto *logSide = new QWidget(details);
        auto *logLayout = new QVBoxLayout(logSide);
        logLayout->setContentsMargins(0, 0, 0, 0);
        logs_ = new QListWidget(logSide);
        logs_->setObjectName(QStringLiteral("missionLogs"));
        logs_->setWordWrap(true);
        logLayout->addWidget(logs_);
        auto *logRow = new QHBoxLayout;
        logInput_ = new QLineEdit(logSide);
        logInput_->setObjectName(QStringLiteral("missionLogInput"));
        logInput_->setPlaceholderText(tr("Log entry"));
        logInput_->setMaxLength(4096);
        auto *addLog = new QPushButton(tr("Add"), logSide);
        addLog->setObjectName(QStringLiteral("addMissionLog"));
        logRow->addWidget(logInput_);
        logRow->addWidget(addLog);
        logLayout->addLayout(logRow);
        subscribedLayout->addWidget(details);
        auto *fetch = new QPushButton(tr("Download File ..."), subscribedSide);
        fetch->setObjectName(QStringLiteral("downloadMissionFile"));
        subscribedLayout->addWidget(fetch);
        layout->addWidget(splitter_);
        status_ = new SingleLineLabel({}, this);
        status_->setObjectName(QStringLiteral("missionsStatus"));
        status_->hide();
        layout->addWidget(status_);
        if (!missions_) {
            setEnabled(false);
            return;
        }
        if (services.mapTools) {
            services.mapTools->add(new MissionTool(services, this));
        }
        connect(refresh, &QPushButton::clicked, this, [this] {
            const auto id = serverChoice_->currentData().toString();
            if (id.isEmpty()) {
                showStatus(tr("Add a TAK Server entry with a REST API port."));
                return;
            }
            showStatus(tr("Reading the mission list…"));
            missions_->list(id);
        });
        connect(missions_,
                &Missions::listed,
                this,
                [this](const QString &id, const QList<Mission> &list, const QString &error) {
                    listedServer_ = id;
                    list_ = list;
                    showAvailable();
                    showStatus(error.isEmpty()
                                   ? tr("%n missions.", nullptr, static_cast<int>(list.size()))
                                   : error);
                });
        connect(subscribe, &QPushButton::clicked, this, &MissionsPane::subscribeSelected);
        connect(
            available_, &QTableWidget::cellDoubleClicked, this, &MissionsPane::subscribeSelected);
        connect(missions_,
                &Missions::subscribed,
                this,
                [this](const QString &, const QString &name, const QString &error) {
                    showStatus(error.isEmpty() ? tr("Subscribed to %1.").arg(name) : error);
                });
        connect(missions_, &Missions::changed, this, &MissionsPane::showSubscriptions);
        connect(subscribed_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
            missions_->setVisible(item->data(Qt::UserRole).toString(),
                                  item->data(Qt::UserRole + 1).toString(),
                                  item->checkState() == Qt::Checked);
        });
        connect(subscribed_, &QListWidget::currentRowChanged, this, &MissionsPane::showDetails);
        connect(reload, &QPushButton::clicked, this, [this] {
            if (const auto *mission = current()) {
                missions_->reload(mission->serverId, mission->name);
            }
        });
        connect(unsubscribe, &QPushButton::clicked, this, [this] {
            if (const auto *mission = current()) {
                const auto serverId = mission->serverId;
                const auto name = mission->name;
                missions_->unsubscribe(serverId, name);
            }
        });
        connect(exportButton, &QPushButton::clicked, this, [this] {
            if (const auto *mission = current()) {
                const auto serverId = mission->serverId;
                const auto name = mission->name;
                const auto error = missions_->exportArchive(serverId, name);
                showStatus(error.isEmpty() ? tr("Downloading the mission archive…") : error);
            }
        });
        connect(missions_,
                &Missions::exported,
                this,
                [this](const QString &,
                       const QString &name,
                       const QByteArray &archive,
                       const QString &error) {
                    if (!error.isEmpty()) {
                        showStatus(error);
                        return;
                    }
                    // The file dialog is a child of this pane. When a nested loop deletes the
                    // pane, Qt deletes the dialog too, and the dialog returns no file.
                    const auto path = QFileDialog::getSaveFileName(this,
                                                                   tr("Export Mission"),
                                                                   name + QStringLiteral(".zip"),
                                                                   tr("Zip archives (*.zip)"));
                    if (path.isEmpty()) {
                        return;
                    }
                    QSaveFile file(path);
                    const bool saved = file.open(QIODevice::WriteOnly) &&
                                       file.write(archive) == archive.size() && file.commit();
                    showStatus(saved ? tr("The mission archive is saved.")
                                     : tr("Cannot save the mission archive."));
                });
        connect(addLog, &QPushButton::clicked, this, &MissionsPane::writeLog);
        connect(logInput_, &QLineEdit::returnPressed, this, &MissionsPane::writeLog);
        connect(fetch, &QPushButton::clicked, this, [this] {
            const auto *mission = current();
            const auto index = fileList_->currentRow();
            if (!mission || index < 0 || index >= mission->files.size() || !files_) {
                showStatus(tr("Select a mission file."));
                return;
            }
            const auto &file = mission->files.at(index);
            ServerFile server;
            server.hash = file.hash;
            server.name = file.name;
            server.submitter = file.submitter;
            server.size = file.size;
            const auto error = files_->downloadServerFile(mission->serverId, server);
            showStatus(error.isEmpty()
                           ? tr("Downloading %1. Files > Received shows the result.").arg(file.name)
                           : error);
        });
        connect(missions_, &Missions::invited, this, [this](const QString &, const QString &name) {
            showStatus(tr("You are invited to the mission %1.").arg(name));
        });
        if (servers_) {
            connect(servers_, &Servers::changed, this, &MissionsPane::showServers);
        }
        showServers();
        showSubscriptions();
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.missions.view");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("splitter"), widgetStateText(splitter_->saveState())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        splitter_->restoreState(widgetStateBytes(values, QStringLiteral("splitter"), 1024));
    }

private:
    void showStatus(const QString &text)
    {
        status_->setText(text);
        status_->setVisible(!text.isEmpty());
    }
    void showServers()
    {
        fillTakServerChoice(*serverChoice_, servers_, QStringLiteral("/api/missions"));
    }
    void showAvailable()
    {
        available_->setRowCount(static_cast<int>(list_.size()));
        for (int row = 0; row < list_.size(); ++row) {
            const auto &mission = list_.at(row);
            const QStringList cells{mission.name,
                                    mission.description,
                                    mission.creatorUid,
                                    mission.passwordProtected ? tr("Yes") : tr("No")};
            for (int column = 0; column < cells.size(); ++column) {
                auto *item = new QTableWidgetItem(cells.at(column));
                item->setToolTip(plainToolTip(cells.at(column)));
                available_->setItem(row, column, item);
            }
        }
    }
    void subscribeSelected()
    {
        const auto row = available_->currentRow();
        if (row < 0 || row >= list_.size()) {
            showStatus(tr("Select a mission."));
            return;
        }
        const auto mission = list_.at(row);
        QString password;
        if (mission.passwordProtected) {
            Modal<QInputDialog> dialog(this);
            dialog->setObjectName(QStringLiteral("missionPassword"));
            dialog->setWindowTitle(tr("Mission Password"));
            dialog->setLabelText(tr("Password for %1").arg(mission.name));
            dialog->setTextEchoMode(QLineEdit::Password);
            if (dialog->exec() != QDialog::Accepted || !dialog || !missions_) {
                return;
            }
            password = dialog->textValue();
        }
        const auto error = missions_->subscribe(listedServer_, mission.name, password);
        showStatus(error.isEmpty() ? tr("Subscribing to %1…").arg(mission.name) : error);
    }
    const MissionSubscription *current() const
    {
        const auto *item = subscribed_->currentItem();
        return item ? missions_->subscription(item->data(Qt::UserRole).toString(),
                                              item->data(Qt::UserRole + 1).toString())
                    : nullptr;
    }
    void showSubscriptions()
    {
        const auto *selected = subscribed_->currentItem();
        const auto previous = selected
                                  ? selected->data(Qt::UserRole).toString() + QLatin1Char('/') +
                                        selected->data(Qt::UserRole + 1).toString()
                                  : QString{};
        const QSignalBlocker blocker(subscribed_);
        subscribed_->clear();
        for (const auto &mission : missions_->subscriptions()) {
            auto *item =
                new QListWidgetItem(tr("%1 · %2 · %n objects · %3", nullptr, mission.objects)
                                        .arg(mission.name, roleText(mission.role), mission.status),
                                    subscribed_);
            item->setData(Qt::UserRole, mission.serverId);
            item->setData(Qt::UserRole + 1, mission.name);
            item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(mission.visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(tr("Checked missions show their objects on the map."));
            if (mission.serverId + QLatin1Char('/') + mission.name == previous) {
                subscribed_->setCurrentItem(item);
            }
        }
        if (!subscribed_->currentItem() && subscribed_->count() > 0) {
            subscribed_->setCurrentRow(0);
        }
        showDetails();
    }
    void showDetails()
    {
        fileList_->clear();
        logs_->clear();
        const auto *mission = current();
        if (!mission) {
            return;
        }
        for (const auto &file : mission->files) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1 (%2)").arg(
                    file.name,
                    file.size < 0 ? tr("unknown size") : QLocale().formattedDataSize(file.size)),
                fileList_);
            item->setToolTip(plainToolTip(file.submitter));
        }
        for (const auto &entry : mission->logs) {
            new QListWidgetItem(
                QStringLiteral("%1  %2\n%3")
                    .arg(entry.time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                         entry.creatorUid,
                         entry.content),
                logs_);
        }
        logInput_->setEnabled(mission->editable());
    }
    void writeLog()
    {
        const auto *mission = current();
        if (!mission) {
            showStatus(tr("Select a mission."));
            return;
        }
        const auto error = missions_->addLog(mission->serverId, mission->name, logInput_->text());
        showStatus(error);
        if (error.isEmpty()) {
            logInput_->clear();
        }
    }
    QPointer<Missions> missions_;
    QPointer<Servers> servers_;
    QPointer<FileShares> files_;
    QComboBox *serverChoice_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QTableWidget *available_ = nullptr;
    QListWidget *subscribed_ = nullptr;
    QListWidget *fileList_ = nullptr;
    QListWidget *logs_ = nullptr;
    QLineEdit *logInput_ = nullptr;
    SingleLineLabel *status_ = nullptr;
    QString listedServer_;
    QList<Mission> list_;
};
} // namespace
class MissionsPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "missions/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Missions");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new MissionsPane(services, parent);
    }
};
} // namespace swak
#include "missions.moc"

#include <algorithm>
