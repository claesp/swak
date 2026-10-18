#include "core/excheck.h"
#include "core/interfaces.h"
#include "core/missions.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/servers.h"
#include "core/single_line_label.h"
#include "core/tak_server_api.h"
#include "core/text.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace swak {
namespace {
class ChecklistPane final : public QWidget, public PreferenceParticipant
{
public:
    ChecklistPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), excheck_(services.excheck), servers_(services.servers),
          missions_(services.missions)
    {
        setObjectName(QStringLiteral("checklistPane"));
        auto *layout = new QVBoxLayout(this);
        auto *row = new QHBoxLayout;
        serverChoice_ = new QComboBox(this);
        serverChoice_->setObjectName(QStringLiteral("checklistServer"));
        serverChoice_->setAccessibleName(tr("TAK Server"));
        auto *refresh = new QPushButton(tr("Refresh"), this);
        refresh->setObjectName(QStringLiteral("listChecklists"));
        auto *start = new QPushButton(tr("Start ..."), this);
        start->setObjectName(QStringLiteral("startChecklist"));
        start->setToolTip(tr("Start a checklist from a server template."));
        row->addWidget(serverChoice_, 1);
        row->addWidget(refresh);
        row->addWidget(start);
        layout->addLayout(row);
        splitter_ = new QSplitter(Qt::Horizontal, this);
        splitter_->setObjectName(QStringLiteral("checklistSplitter"));
        checklists_ = new QListWidget(splitter_);
        checklists_->setObjectName(QStringLiteral("checklistList"));
        checklists_->setAccessibleName(tr("Checklists"));
        auto *side = new QWidget(splitter_);
        auto *sideLayout = new QVBoxLayout(side);
        sideLayout->setContentsMargins(0, 0, 0, 0);
        tasks_ = new QTableWidget(side);
        tasks_->setObjectName(QStringLiteral("checklistTasks"));
        tasks_->setSelectionBehavior(QAbstractItemView::SelectRows);
        tasks_->setSelectionMode(QAbstractItemView::SingleSelection);
        tasks_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tasks_->verticalHeader()->hide();
        tasks_->horizontalHeader()->setStretchLastSection(true);
        sideLayout->addWidget(tasks_);
        auto *actions = new QHBoxLayout;
        auto *complete = new QPushButton(tr("Complete"), side);
        complete->setObjectName(QStringLiteral("completeTask"));
        auto *pending = new QPushButton(tr("Pending"), side);
        pending->setObjectName(QStringLiteral("pendingTask"));
        auto *mission = new QPushButton(tr("Subscribe as Mission"), side);
        mission->setObjectName(QStringLiteral("checklistMission"));
        mission->setToolTip(
            tr("Active checklists are missions. Subscribe to follow their changes."));
        actions->addWidget(complete);
        actions->addWidget(pending);
        actions->addStretch();
        actions->addWidget(mission);
        sideLayout->addLayout(actions);
        splitter_->setStretchFactor(1, 3);
        layout->addWidget(splitter_);
        status_ = new SingleLineLabel({}, this);
        status_->setObjectName(QStringLiteral("checklistStatus"));
        status_->hide();
        layout->addWidget(status_);
        if (!excheck_) {
            setEnabled(false);
            return;
        }
        connect(refresh, &QPushButton::clicked, this, [this] {
            const auto id = serverChoice_->currentData().toString();
            if (id.isEmpty()) {
                showStatus(tr("Add a TAK Server entry with a REST API port."));
                return;
            }
            showStatus(tr("Reading the checklists…"));
            excheck_->list(id);
        });
        connect(excheck_,
                &ExCheck::listed,
                this,
                [this](const QString &id,
                       const QList<Checklist> &list,
                       const QList<ChecklistTemplate> &templates,
                       const QString &error) {
                    serverId_ = id;
                    list_ = list;
                    templates_ = templates;
                    showChecklists();
                    showStatus(error.isEmpty()
                                   ? tr("%n checklists.", nullptr, static_cast<int>(list.size()))
                                   : error);
                });
        connect(checklists_, &QListWidget::currentRowChanged, this, [this](int index) {
            // The task table shows only the selected checklist. Clear it until its reply
            // arrives, so that Complete and Pending cannot change another checklist.
            current_ = {};
            requestedServer_.clear();
            requested_.clear();
            showTasks();
            if (index >= 0 && index < list_.size()) {
                requestedServer_ = serverId_;
                requested_ = list_.at(index).uid;
                showStatus(tr("Reading the tasks…"));
                excheck_->load(serverId_, requested_);
            }
        });
        connect(excheck_,
                &ExCheck::loaded,
                this,
                [this](const QString &serverId, const Checklist &checklist, const QString &error) {
                    // A reply for an earlier selection is stale.
                    if (serverId != requestedServer_ || checklist.uid != requested_) {
                        return;
                    }
                    if (!error.isEmpty()) {
                        showStatus(error);
                        return;
                    }
                    current_ = checklist;
                    showTasks();
                    showStatus({});
                });
        connect(complete, &QPushButton::clicked, this, [this] {
            update(QStringLiteral("Complete"));
        });
        connect(pending, &QPushButton::clicked, this, [this] {
            update(QStringLiteral("Pending"));
        });
        connect(excheck_,
                &ExCheck::updated,
                this,
                [this](const QString &id, const QString &uid, const QString &error) {
                    showStatus(error.isEmpty() ? tr("The task is updated.") : error);
                    if (error.isEmpty() && uid == current_.uid) {
                        excheck_->load(id, uid);
                    }
                });
        connect(start, &QPushButton::clicked, this, &ChecklistPane::startChecklist);
        connect(excheck_, &ExCheck::started, this, [this](const QString &id, const QString &error) {
            showStatus(error.isEmpty() ? tr("The checklist is started.") : error);
            if (error.isEmpty()) {
                excheck_->list(id);
            }
        });
        connect(mission, &QPushButton::clicked, this, [this] {
            if (current_.uid.isEmpty() || !missions_) {
                showStatus(tr("Select a checklist."));
                return;
            }
            const auto error = missions_->subscribe(serverId_, current_.uid);
            showStatus(error.isEmpty() ? tr("Subscribing. Missions shows the checklist mission.")
                                       : error);
        });
        if (servers_) {
            connect(servers_, &Servers::changed, this, &ChecklistPane::showServers);
        }
        showServers();
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.checklists");
    }
    QJsonObject savePreferences() const override
    {
        return {
            {QStringLiteral("splitter"), QString::fromLatin1(splitter_->saveState().toBase64())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto bytes = preferenceText(values, QStringLiteral("splitter"), {}, 1024).toLatin1();
        if (!bytes.isEmpty()) {
            splitter_->restoreState(
                QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
        }
    }

private:
    void showStatus(const QString &text)
    {
        status_->setText(text);
        status_->setVisible(!text.isEmpty());
    }
    void showServers()
    {
        const auto previous = serverChoice_->currentData().toString();
        serverChoice_->clear();
        if (!servers_) {
            return;
        }
        const auto sources = servers_->sources();
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            if (!TakServerApi::apiUrl(it.value().toObject(), QStringLiteral("/excheck"))
                     .isEmpty()) {
                const auto name = it.value().toObject().value(QStringLiteral("name")).toString();
                serverChoice_->addItem(name.isEmpty() ? it.key() : name, it.key());
            }
        }
        const auto index = serverChoice_->findData(previous);
        if (index >= 0) {
            serverChoice_->setCurrentIndex(index);
        }
    }
    void showChecklists()
    {
        const QSignalBlocker blocker(checklists_);
        checklists_->clear();
        for (const auto &checklist : std::as_const(list_)) {
            auto *item = new QListWidgetItem(checklist.name, checklists_);
            item->setToolTip(plainToolTip(checklist.description.isEmpty() ? checklist.templateName
                                                                          : checklist.description));
        }
        current_ = {};
        showTasks();
    }
    void showTasks()
    {
        const auto columns = std::max<qsizetype>(current_.columns.size(), 1);
        tasks_->clear();
        tasks_->setColumnCount(static_cast<int>(columns + 1));
        QStringList headers =
            current_.columns.isEmpty() ? QStringList{tr("Task")} : current_.columns;
        headers.append(tr("Status"));
        tasks_->setHorizontalHeaderLabels(headers);
        tasks_->setRowCount(static_cast<int>(current_.tasks.size()));
        for (int row = 0; row < current_.tasks.size(); ++row) {
            const auto &task = current_.tasks.at(row);
            for (int column = 0; column < columns; ++column) {
                auto *item = new QTableWidgetItem(task.values.value(column));
                item->setToolTip(plainToolTip(item->text()));
                tasks_->setItem(row, column, item);
            }
            auto *state = new QTableWidgetItem(task.status.isEmpty() ? tr("Pending") : task.status);
            if (task.complete()) {
                state->setForeground(QColor(QStringLiteral("#2e7d32")));
            }
            tasks_->setItem(row, static_cast<int>(columns), state);
        }
    }
    void update(const QString &status)
    {
        const auto row = tasks_->currentRow();
        if (row < 0 || row >= current_.tasks.size()) {
            showStatus(tr("Select a task."));
            return;
        }
        const auto error =
            excheck_->setStatus(serverId_, current_.uid, current_.tasks.at(row), status);
        showStatus(error.isEmpty() ? tr("Updating the task…") : error);
    }
    void startChecklist()
    {
        if (templates_.isEmpty()) {
            showStatus(tr("Refresh first. The server has no checklist templates yet."));
            return;
        }
        Modal<QDialog> dialog(this);
        dialog->setObjectName(QStringLiteral("startChecklistDialog"));
        dialog->setWindowTitle(tr("Start Checklist"));
        auto *form = new QFormLayout(dialog.get());
        auto *choice = new QComboBox(dialog.get());
        choice->setObjectName(QStringLiteral("checklistTemplate"));
        for (const auto &entry : std::as_const(templates_)) {
            choice->addItem(entry.name, entry.uid);
        }
        form->addRow(tr("Template"), choice);
        auto *name = new QLineEdit(dialog.get());
        name->setObjectName(QStringLiteral("checklistName"));
        name->setMaxLength(128);
        form->addRow(tr("Name"), name);
        auto *description = new QLineEdit(dialog.get());
        description->setObjectName(QStringLiteral("checklistDescription"));
        description->setMaxLength(512);
        form->addRow(tr("Description"), description);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
        connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
        form->addRow(buttons);
        if (dialog->exec() != QDialog::Accepted || !dialog || !excheck_) {
            return;
        }
        const auto error = excheck_->start(
            serverId_, choice->currentData().toString(), name->text(), description->text());
        showStatus(error.isEmpty() ? tr("Starting the checklist…") : error);
    }
    QPointer<ExCheck> excheck_;
    QPointer<Servers> servers_;
    QPointer<Missions> missions_;
    QComboBox *serverChoice_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QListWidget *checklists_ = nullptr;
    QTableWidget *tasks_ = nullptr;
    SingleLineLabel *status_ = nullptr;
    QString serverId_;
    QList<Checklist> list_;
    QList<ChecklistTemplate> templates_;
    Checklist current_;
    // The server and checklist of the last load request.
    QString requestedServer_;
    QString requested_;
};
} // namespace
class ChecklistsPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "checklists/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Checklists");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new ChecklistPane(services, parent);
    }
};
} // namespace swak
#include "checklists.moc"

#include <algorithm>
