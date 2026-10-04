#pragma once
#include "core/plugin_catalog.h"

#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace swak {
inline QWidget *createPluginPanel(const QList<QObject *> &plugins, QWidget *parent)
{
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    auto *table = new QTableWidget(0, 3, panel);
    table->setObjectName(QStringLiteral("pluginTable"));
    table->setAccessibleName(QObject::tr("Built-in plugins"));
    table->setHorizontalHeaderLabels(
        {QObject::tr("Name"), QObject::tr("Version"), QObject::tr("Interfaces")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    for (auto *plugin : plugins) {
        const auto manifest = pluginManifest(plugin);
        if (manifest.isEmpty()) {
            continue;
        }
        QStringList interfaces;
        for (const auto &value : manifest.value(QStringLiteral("interfaces")).toArray()) {
            interfaces.append(value.toString());
        }
        const int row = table->rowCount();
        table->insertRow(row);
        auto *name = new QTableWidgetItem(manifest.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, manifest);
        table->setItem(row, 0, name);
        table->setItem(
            row, 1, new QTableWidgetItem(manifest.value(QStringLiteral("version")).toString()));
        table->setItem(row, 2, new QTableWidgetItem(interfaces.join(QStringLiteral(", "))));
    }
    table->setSortingEnabled(true);
    table->sortItems(0);
    layout->addWidget(table, 1);
    auto *details = new QPlainTextEdit(panel);
    details->setObjectName(QStringLiteral("pluginDetails"));
    details->setAccessibleName(QObject::tr("Plugin manifest"));
    details->setReadOnly(true);
    details->setPlaceholderText(QObject::tr("Select a plugin to view its manifest."));
    layout->addWidget(details, 1);
    QObject::connect(table, &QTableWidget::itemSelectionChanged, panel, [table, details] {
        const auto rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            details->clear();
            return;
        }
        const auto manifest = table->item(rows.first().row(), 0)->data(Qt::UserRole).toJsonObject();
        QStringList lines;
        for (auto it = manifest.begin(); it != manifest.end(); ++it) {
            const auto value = it.value();
            QString text;
            if (value.isString()) {
                text = value.toString();
            } else if (value.isArray()) {
                QStringList values;
                for (const auto &entry : value.toArray()) {
                    values.append(entry.toString());
                }
                text = values.isEmpty() ? QObject::tr("None") : values.join(QStringLiteral(", "));
            } else {
                text = value.toVariant().toString();
            }
            lines.append(it.key() + QStringLiteral(": ") + text);
        }
        details->setPlainText(lines.join(QStringLiteral("\n\n")));
    });
    auto *notice = new QLabel(QObject::tr("Plugins run with application permissions. This build "
                                          "does not load external plugin files."),
                              panel);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    if (table->rowCount() > 0) {
        table->selectRow(0);
    }
    return panel;
}
} // namespace swak
