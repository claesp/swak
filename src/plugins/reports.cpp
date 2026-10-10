#include "core/citrap.h"
#include "core/files.h"
#include "core/interfaces.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/servers.h"
#include "core/single_line_label.h"
#include "core/tak_server_api.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace swak {
namespace {
static_assert(MaxCitrapAttachmentBytes <= MaxRegularFileBytes,
              "readRegularFile must accept the attachment limit.");
class ReportsPane final : public QWidget, public PreferenceParticipant
{
public:
    ReportsPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), citrap_(services.citrap), servers_(services.servers)
    {
        setObjectName(QStringLiteral("reportsPane"));
        auto *layout = new QVBoxLayout(this);
        auto *row = new QHBoxLayout;
        serverChoice_ = new QComboBox(this);
        serverChoice_->setObjectName(QStringLiteral("reportServer"));
        serverChoice_->setAccessibleName(tr("TAK Server"));
        auto *refresh = new QPushButton(tr("Refresh"), this);
        refresh->setObjectName(QStringLiteral("listReports"));
        auto *create = new QPushButton(tr("New Report ..."), this);
        create->setObjectName(QStringLiteral("newReport"));
        row->addWidget(serverChoice_, 1);
        row->addWidget(refresh);
        row->addWidget(create);
        layout->addLayout(row);
        table_ = new QTableWidget(0, 6, this);
        table_->setObjectName(QStringLiteral("citrapReports"));
        table_->setHorizontalHeaderLabels(
            {tr("Title"), tr("Type"), tr("Status"), tr("Importance"), tr("Callsign"), tr("Time")});
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_);
        auto *actions = new QHBoxLayout;
        auto *close = new QPushButton(tr("Close"), this);
        close->setObjectName(QStringLiteral("closeReport"));
        auto *reopen = new QPushButton(tr("Reopen"), this);
        reopen->setObjectName(QStringLiteral("reopenReport"));
        actions->addWidget(close);
        actions->addWidget(reopen);
        actions->addStretch();
        layout->addLayout(actions);
        status_ = new SingleLineLabel({}, this);
        status_->setObjectName(QStringLiteral("reportStatus"));
        status_->hide();
        layout->addWidget(status_);
        if (!citrap_) {
            setEnabled(false);
            return;
        }
        connect(refresh, &QPushButton::clicked, this, [this] {
            const auto id = serverChoice_->currentData().toString();
            if (id.isEmpty()) {
                showStatus(tr("Add a TAK Server entry with a REST API port."));
                return;
            }
            showStatus(tr("Reading the reports…"));
            citrap_->list(id);
        });
        connect(
            citrap_,
            &Citrap::listed,
            this,
            [this](const QString &id, const QList<CitrapReport> &reports, const QString &error) {
                serverId_ = id;
                reports_ = reports;
                showReports();
                showStatus(error.isEmpty()
                               ? tr("%n reports.", nullptr, static_cast<int>(reports.size()))
                               : error);
            });
        connect(create, &QPushButton::clicked, this, &ReportsPane::newReport);
        connect(citrap_,
                &Citrap::created,
                this,
                [this](const QString &id, const QString &, const QString &error) {
                    showStatus(error.isEmpty() ? tr("The report is sent.") : error);
                    citrap_->list(id);
                });
        connect(close, &QPushButton::clicked, this, [this] {
            setStatus(QStringLiteral("Closed"));
        });
        connect(reopen, &QPushButton::clicked, this, [this] {
            setStatus(QStringLiteral("Open"));
        });
        connect(citrap_,
                &Citrap::updated,
                this,
                [this](const QString &id, const QString &, const QString &error) {
                    showStatus(error.isEmpty() ? tr("The report is updated.") : error);
                    if (error.isEmpty()) {
                        citrap_->list(id);
                    }
                });
        if (servers_) {
            connect(servers_, &Servers::changed, this, &ReportsPane::showServers);
        }
        showServers();
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.reports");
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
            if (!TakServerApi::apiUrl(it.value().toObject(), QStringLiteral("/citrap")).isEmpty()) {
                const auto name = it.value().toObject().value(QStringLiteral("name")).toString();
                serverChoice_->addItem(name.isEmpty() ? it.key() : name, it.key());
            }
        }
        const auto index = serverChoice_->findData(previous);
        if (index >= 0) {
            serverChoice_->setCurrentIndex(index);
        }
    }
    void showReports()
    {
        table_->setRowCount(static_cast<int>(reports_.size()));
        for (int row = 0; row < reports_.size(); ++row) {
            const auto &report = reports_.at(row);
            const QStringList cells{report.title.isEmpty() ? report.id : report.title,
                                    report.type,
                                    report.status,
                                    report.importance,
                                    report.callsign,
                                    report.time.isValid() ? report.time.toLocalTime().toString(
                                                                QStringLiteral("yyyy-MM-dd HH:mm"))
                                                          : QString{}};
            for (int column = 0; column < cells.size(); ++column) {
                auto *item = new QTableWidgetItem(cells.at(column));
                item->setToolTip(column == 0 && !report.description.isEmpty() ? report.description
                                                                              : cells.at(column));
                table_->setItem(row, column, item);
            }
        }
    }
    void setStatus(const QString &status)
    {
        const auto row = table_->currentRow();
        if (row < 0 || row >= reports_.size()) {
            showStatus(tr("Select a report."));
            return;
        }
        auto report = reports_.at(row);
        report.status = status;
        const auto error = citrap_->update(serverId_, report);
        showStatus(error.isEmpty() ? tr("Updating the report…") : error);
    }
    void newReport()
    {
        const auto serverId = serverChoice_->currentData().toString();
        if (serverId.isEmpty()) {
            showStatus(tr("Add a TAK Server entry with a REST API port."));
            return;
        }
        Modal<QDialog> dialog(this);
        dialog->setObjectName(QStringLiteral("newReportDialog"));
        dialog->setWindowTitle(tr("New Report"));
        auto *form = new QFormLayout(dialog.get());
        auto *title = new QLineEdit(dialog.get());
        title->setObjectName(QStringLiteral("reportTitle"));
        title->setMaxLength(128);
        form->addRow(tr("Title"), title);
        auto *type = new QComboBox(dialog.get());
        type->setObjectName(QStringLiteral("reportType"));
        type->addItems({tr("Incident"), tr("Observation"), tr("Hazard"), tr("Other")});
        form->addRow(tr("Type"), type);
        auto *importance = new QComboBox(dialog.get());
        importance->setObjectName(QStringLiteral("reportImportance"));
        importance->addItems({tr("Low"), tr("Medium"), tr("High"), tr("Critical")});
        form->addRow(tr("Importance"), importance);
        auto *latitude = new QDoubleSpinBox(dialog.get());
        latitude->setObjectName(QStringLiteral("reportLatitude"));
        latitude->setRange(-90, 90);
        latitude->setDecimals(6);
        form->addRow(tr("Latitude (°)"), latitude);
        auto *longitude = new QDoubleSpinBox(dialog.get());
        longitude->setObjectName(QStringLiteral("reportLongitude"));
        longitude->setRange(-180, 180);
        longitude->setDecimals(6);
        form->addRow(tr("Longitude (°)"), longitude);
        auto *description = new QPlainTextEdit(dialog.get());
        description->setObjectName(QStringLiteral("reportDescription"));
        form->addRow(tr("Description"), description);
        auto *files = new QListWidget(dialog.get());
        files->setObjectName(QStringLiteral("reportAttachments"));
        auto *addFile = new QPushButton(tr("Add File ..."), dialog.get());
        addFile->setObjectName(QStringLiteral("addReportFile"));
        connect(addFile, &QPushButton::clicked, dialog.get(), [files, &dialog] {
            const auto paths = QFileDialog::getOpenFileNames(dialog.get(), tr("Attachments"));
            for (const auto &path : paths) {
                if (files->count() < MaxCitrapAttachments) {
                    files->addItem(path);
                }
            }
        });
        form->addRow(tr("Attachments"), files);
        form->addRow({}, addFile);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Send"));
        connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
        form->addRow(buttons);
        if (dialog->exec() != QDialog::Accepted || !dialog || !citrap_) {
            return;
        }
        QList<CitrapAttachment> attachments;
        for (int row = 0; row < files->count(); ++row) {
            const auto path = files->item(row)->text();
            const auto file = readRegularFile(path, MaxCitrapAttachmentBytes);
            if (!file.error.isEmpty()) {
                showStatus(file.error);
                return;
            }
            attachments.append({QFileInfo(path).fileName(), file.bytes});
        }
        CitrapReport report;
        report.title = title->text();
        report.type = type->currentText();
        report.importance = importance->currentText();
        report.description = description->toPlainText();
        report.latitude = latitude->value();
        report.longitude = longitude->value();
        report.time = QDateTime::currentDateTimeUtc();
        const auto error = citrap_->create(serverId, report, attachments);
        showStatus(error.isEmpty() ? tr("Sending the report…") : error);
    }
    QPointer<Citrap> citrap_;
    QPointer<Servers> servers_;
    QComboBox *serverChoice_ = nullptr;
    QTableWidget *table_ = nullptr;
    SingleLineLabel *status_ = nullptr;
    QString serverId_;
    QList<CitrapReport> reports_;
};
} // namespace
class ReportsPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "reports/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Reports");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new ReportsPane(services, parent);
    }
};
} // namespace swak
#include "reports.moc"
