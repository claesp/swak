#include "app/data_manager.h"

#include "core/delete_confirmation.h"
#include "core/logging.h"
#include "core/modal.h"
#include "core/provider_table.h"
#include "core/settings_dialog.h"
#include "core/text.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
namespace swak {
DataManager::DataManager(DataSources &sources,
                         QWidget *parent,
                         std::function<QString()> save,
                         bool embedded)
    : QDialog(parent), sources_(sources), save_(std::move(save))
{
    if (embedded) {
        setWindowFlags(Qt::Widget);
    }
    setObjectName(QStringLiteral("dataProvidersDialog"));
    setWindowTitle(tr("Data Providers"));
    resize(700, 460);
    auto *layout = new QVBoxLayout(this);
    auto *notice =
        new QLabel(tr("Enabled KML/KMZ feeds refresh on their own schedules over HTTPS.\nRemote "
                      "links and icons stay disabled. Changes apply immediately."),
                   this);
    notice->setObjectName(QStringLiteral("providerNotice"));
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    notice->setMinimumHeight(notice->fontMetrics().lineSpacing() * 2);
    layout->addWidget(notice);
    list_ = new ProviderTable(false, this);
    list_->setObjectName(QStringLiteral("dataProviderList"));
    list_->setAccessibleName(tr("Data providers"));
    layout->addWidget(list_);
    auto *row = new QHBoxLayout;
    auto button = [&](const QString &label, const QString &id) {
        auto *value = new QPushButton(label, this);
        value->setObjectName(id);
        row->addWidget(value);
        return value;
    };
    auto *add = button(tr("Add"), QStringLiteral("addDataProvider"));
    auto *editButton = button(tr("Edit"), QStringLiteral("editDataProvider"));
    auto *remove = button(tr("Delete ..."), QStringLiteral("removeDataProvider"));
    auto *update = button(tr("Refresh now"), QStringLiteral("refreshDataProvider"));
    layout->addLayout(row);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setMinimumHeight(status_->fontMetrics().lineSpacing() * 2);
    status_->setTextFormat(Qt::PlainText);
    status_->setObjectName(QStringLiteral("dataProviderStatus"));
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
    connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int, int column) {
        if (column != 0) {
            edit(false);
        }
    });
    connect(list_, &QTableWidget::currentCellChanged, this, [this] {
        updateStatus();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto id = list_->selectedId();
        if (id.isEmpty() || !confirmDeletion(this,
                                             sources_.sources()
                                                 .value(id)
                                                 .toObject()
                                                 .value(QStringLiteral("name"))
                                                 .toString(id))) {
            return;
        }
        sources_.remove(id);
        this->save();
    });
    connect(update, &QPushButton::clicked, this, [this] {
        if (!list_->selectedId().isEmpty()) {
            sources_.refresh(list_->selectedId());
        }
    });
    connect(&sources_, &DataSources::changed, this, &DataManager::refresh);
    connect(&sources_, &DataSources::contentChanged, this, &DataManager::updateStatus);
    refresh();
}
void DataManager::save()
{
    const auto error = save_ ? save_() : tr("No profile writer is available.");
    if (!error.isEmpty()) {
        status_->setText(error);
        logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.data"), error);
    }
}
void DataManager::refresh()
{
    const auto selected = list_->selectedId();
    const QSignalBlocker blocked(list_);
    list_->clearProviders();
    const auto entries = sources_.sources();
    const auto ids = idsByName(entries);
    for (const auto &id : ids) {
        const auto source = entries[id].toObject();
        const auto *provider = sources_.provider(source);
        const auto endpoint = plainToolTip(source.value(QStringLiteral("endpoint")).toString());
        auto *button = list_->append(id,
                                     source.value(QStringLiteral("name")).toString(),
                                     provider ? provider->dataName() : tr("Plugin unavailable"),
                                     endpoint,
                                     source.value(QStringLiteral("enabled")).toBool(),
                                     provider != nullptr);
        connect(button, &QAbstractButton::clicked, this, [this, id](bool enabled) {
            QTimer::singleShot(0, this, [this, id, enabled] {
                list_->selectId(id);
                auto value = sources_.sources().value(id).toObject();
                if (value.isEmpty()) {
                    return;
                }
                value.insert(QStringLiteral("enabled"), enabled);
                const auto error = sources_.put(id, value);
                if (!error.isEmpty()) {
                    refresh();
                    status_->setText(error);
                    return;
                }
                save();
            });
        });
    }
    list_->selectId(selected);
    if (list_->currentRow() < 0 && list_->rowCount()) {
        list_->setCurrentCell(0, 1);
    }
    updateStatus();
}
void DataManager::updateStatus()
{
    if (list_->selectedId().isEmpty()) {
        status_->clear();
        return;
    }
    const auto id = list_->selectedId();
    const auto source = sources_.sources().value(id).toObject();
    status_->setText(sources_.status(id) + (source.value(QStringLiteral("ignoreTls")).toBool()
                                                ? tr(" Certificate checks are disabled.")
                                                : QString{}));
}
void DataManager::edit(bool adding)
{
    if (adding && (sources_.sources().size() >= 8 || sources_.providers().isEmpty())) {
        status_->setText(tr("An available data plugin and fewer than eight feeds are required."));
        return;
    }
    if (!adding && list_->selectedId().isEmpty()) {
        return;
    }
    const auto id =
        adding ? QUuid::createUuid().toString(QUuid::WithoutBraces) : list_->selectedId();
    const auto source = sources_.sources().value(id).toObject();
    if (!adding && !sources_.provider(source)) {
        status_->setText(tr("Install the missing data plugin to edit this entry."));
        return;
    }
    QList<SettingsPage> pages;
    for (auto *provider : sources_.providers()) {
        if (!adding && provider != sources_.provider(source)) {
            continue;
        }
        auto values = adding ? dataDefaults() : source;
        values.insert(QStringLiteral("format"), provider->dataId());
        pages.append({adding ? provider->dataId() : id,
                      provider->dataName(),
                      dataFields(),
                      values,
                      validateDataSource,
                      [this, id](const QJsonObject &value) {
                          applyError_ = sources_.put(id, value);
                      }});
    }
    applyError_.clear();
    Modal<SettingsDialog> dialog(
        pages,
        [this] {
            return !applyError_.isEmpty() ? applyError_
                   : save_                ? save_()
                                          : tr("No profile writer is available.");
        },
        this,
        true,
        adding);
    dialog->setWindowTitle(adding ? tr("Add data provider") : tr("Edit data provider"));
    if (dialog->exec() == QDialog::Accepted) {
        list_->selectId(id);
    }
}
} // namespace swak
