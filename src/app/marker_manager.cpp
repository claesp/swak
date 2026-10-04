#include "app/marker_manager.h"

#include "core/delete_confirmation.h"
#include "core/logging.h"
#include "core/provider_table.h"
#include "core/single_line_label.h"

#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
namespace swak {
MarkerManager::MarkerManager(MarkerCollections &collections,
                             QWidget *parent,
                             std::function<QString()> save,
                             bool embedded)
    : QDialog(parent), collections_(collections), save_(std::move(save))
{
    if (embedded) {
        setWindowFlags(Qt::Widget);
    }
    setObjectName(QStringLiteral("markerCollectionsDialog"));
    setWindowTitle(tr("Marker Collections"));
    resize(700, 460);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(
        new SingleLineLabel(tr("Enable collections for new markers. Existing markers keep their "
                               "saved collection references. Built-in collections are read-only."),
                            this));
    table_ = new ProviderTable(false, this);
    table_->setObjectName(QStringLiteral("markerCollectionList"));
    table_->setAccessibleName(tr("Marker collections"));
    table_->setHorizontalHeaderLabels({tr("Active"), tr("Collection"), tr("Symbols")});
    layout->addWidget(table_);
    auto *row = new QHBoxLayout;
    const auto button = [this, row](const QString &label, const QString &id) {
        auto *value = new QPushButton(label, this);
        value->setObjectName(id);
        row->addWidget(value);
        return value;
    };
    auto *add = button(tr("Add"), QStringLiteral("addMarkerCollection"));
    auto *editButton = button(tr("Edit"), QStringLiteral("editMarkerCollection"));
    auto *remove = button(tr("Delete ..."), QStringLiteral("deleteMarkerCollection"));
    auto *import = button(tr("Import"), QStringLiteral("importMarkerCollection"));
    auto *exportButton = button(tr("Export"), QStringLiteral("exportMarkerCollection"));
    layout->addLayout(row);
    status_ = new SingleLineLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setObjectName(QStringLiteral("markerCollectionStatus"));
    layout->addWidget(status_);
    if (!embedded) {
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    }
    connect(add, &QPushButton::clicked, this, [this] {
        edit(true);
    });
    connect(editButton, &QPushButton::clicked, this, [this] {
        edit(false);
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int column) {
        if (column) {
            edit(false);
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto id = table_->selectedId();
        if (id.isEmpty() || !confirmDeletion(this,
                                             collections_.collections()
                                                 .value(id)
                                                 .toObject()
                                                 .value(QStringLiteral("name"))
                                                 .toString(id))) {
            return;
        }
        const auto error = collections_.remove(id);
        if (error.isEmpty()) {
            this->save();
        } else {
            report(error);
        }
    });
    connect(import, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this,
            tr("Import marker collection"),
            collections_.directory(),
            tr("Collection archives (*.zip *.tar *.tar.gz *.tgz);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        collections_.setDirectory(QFileInfo(path).absolutePath());
        const auto error = collections_.importArchive(path);
        if (error.isEmpty()) {
            this->save();
        } else {
            report(error);
        }
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto id = table_->selectedId();
        if (id.isEmpty()) {
            return;
        }
        const auto directory =
            collections_.directory().isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                : collections_.directory();
        QFileDialog dialog(this, tr("Export marker collection"), directory);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilters({tr("ZIP archive (*.zip)"), tr("Compressed TAR archive (*.tar.gz)")});
        dialog.setDefaultSuffix(QStringLiteral("zip"));
        dialog.selectFile(id);
        connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog](const QString &filter) {
            dialog.setDefaultSuffix(filter.contains(QStringLiteral("*.tar.gz"))
                                        ? QStringLiteral("tar.gz")
                                        : QStringLiteral("zip"));
        });
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return;
        }
        const auto path = dialog.selectedFiles().first();
        collections_.setDirectory(QFileInfo(path).absolutePath());
        const auto error = collections_.exportArchive(id, path);
        if (error.isEmpty()) {
            this->save();
        } else {
            report(error);
        }
    });
    connect(&collections_, &MarkerCollections::changed, this, &MarkerManager::refresh);
    refresh();
}
void MarkerManager::report(const QString &error)
{
    status_->setText(error);
    if (!error.isEmpty()) {
        logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.markers"), error);
    }
}
void MarkerManager::save()
{
    report(save_ ? save_() : tr("No profile writer is available."));
}
void MarkerManager::refresh()
{
    const auto selected = table_->selectedId();
    const QSignalBlocker block(table_);
    table_->clearProviders();
    const auto entries = collections_.collections();
    auto ids = entries.keys();
    const QCollator order;
    std::sort(ids.begin(), ids.end(), [&entries, &order](const auto &a, const auto &b) {
        return order.compare(entries.value(a).toObject().value(QStringLiteral("name")).toString(),
                             entries.value(b).toObject().value(QStringLiteral("name")).toString()) <
               0;
    });
    for (const auto &id : ids) {
        const auto value = entries.value(id).toObject();
        const bool valid = MarkerCollections::validate(value).isEmpty();
        auto *active = table_->append(
            id,
            value.value(QStringLiteral("name")).toString(),
            valid ? QString::number(value.value(QStringLiteral("symbols")).toArray().size())
                  : tr("Unavailable"),
            QStringLiteral("<qt>%1</qt>")
                .arg(tr("ID: %1. Version %2. %3")
                         .arg(id,
                              value.value(QStringLiteral("version")).toString(),
                              value.value(QStringLiteral("description")).toString())
                         .toHtmlEscaped()),
            value.value(QStringLiteral("enabled")).toBool(),
            valid);
        connect(active, &QAbstractButton::clicked, this, [this, id](bool enabled) {
            QTimer::singleShot(0, this, [this, id, enabled] {
                const auto error = collections_.setEnabled(id, enabled);
                if (error.isEmpty()) {
                    this->save();
                } else {
                    report(error);
                }
            });
        });
    }
    table_->selectId(selected);
    if (table_->currentRow() < 0 && table_->rowCount()) {
        table_->setCurrentCell(0, 1);
    }
}
void MarkerManager::edit(bool adding)
{
    const auto id = table_->selectedId();
    if (!adding && id.isEmpty()) {
        return;
    }
    if (!adding && collections_.builtIn(id)) {
        report(tr("Built-in collections are read-only. Use Add to create a copy."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("editMarkerCollectionDialog"));
    dialog.setWindowTitle(adding ? tr("Add marker collection") : tr("Edit marker collection"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    layout->addLayout(form);
    auto *source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("collectionTemplate"));
    const auto entries = collections_.collections();
    auto ids = entries.keys();
    const QCollator order;
    std::sort(ids.begin(), ids.end(), [&entries, &order](const auto &a, const auto &b) {
        return order.compare(entries.value(a).toObject().value(QStringLiteral("name")).toString(),
                             entries.value(b).toObject().value(QStringLiteral("name")).toString()) <
               0;
    });
    for (const auto &key : ids) {
        if (MarkerCollections::validate(entries.value(key).toObject()).isEmpty()) {
            source->addItem(entries.value(key).toObject().value(QStringLiteral("name")).toString(),
                            key);
        }
    }
    if (adding) {
        form->addRow(tr("Copy symbols from"), source);
    } else {
        source->hide();
    }
    const auto original = entries.value(id).toObject();
    auto *name = new QLineEdit(
        adding ? QString{} : original.value(QStringLiteral("name")).toString(), &dialog);
    name->setMaxLength(100);
    name->setObjectName(QStringLiteral("collectionName"));
    form->addRow(tr("Name"), name);
    auto *description = new QLineEdit(
        adding ? QString{} : original.value(QStringLiteral("description")).toString(), &dialog);
    description->setMaxLength(1024);
    description->setObjectName(QStringLiteral("collectionDescription"));
    form->addRow(tr("Description"), description);
    auto *version = new QLineEdit(adding ? QStringLiteral("1.0.0")
                                         : original.value(QStringLiteral("version")).toString(),
                                  &dialog);
    version->setMaxLength(32);
    form->addRow(tr("Version"), version);
    auto *error = new SingleLineLabel(&dialog);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto value = adding ? entries.value(source->currentData().toString()).toObject() : original;
        value.insert(QStringLiteral("id"),
                     adding ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id);
        value.insert(QStringLiteral("name"), name->text().trimmed());
        value.insert(QStringLiteral("description"), description->text());
        value.insert(QStringLiteral("version"), version->text());
        const auto problem = collections_.put(value);
        if (!problem.isEmpty()) {
            error->setText(problem);
            return;
        }
        table_->selectId(value.value(QStringLiteral("id")).toString());
        save();
        dialog.accept();
    });
    dialog.exec();
}
} // namespace swak
