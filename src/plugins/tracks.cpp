#include "core/interfaces.h"
#include "core/projection.h"
#include "core/store.h"
#include "core/preferences.h"
#include <QJsonArray>
#include <QHeaderView>
#include <QPainter>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTableView>

namespace swak {
class EventTable final : public QTableView, public PreferenceParticipant {
public:
    using QTableView::QTableView;
    QString preferenceId() const override { return QStringLiteral("org.swak.events"); }
    QJsonObject savePreferences() const override
    {
        QJsonArray columns;
        const auto *header = horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            columns.append(QJsonObject{{QStringLiteral("id"), columnIds().at(logical)},
                {QStringLiteral("width"), header->sectionSize(logical)},
                {QStringLiteral("hidden"), header->isSectionHidden(logical)}});
        }
        const int sort = header->sortIndicatorSection();
        return {{QStringLiteral("columns"), columns},
                {QStringLiteral("sortColumn"), sort >= 0 && sort < columnIds().size() ? columnIds().at(sort) : QString{}},
                {QStringLiteral("sortOrder"), static_cast<int>(header->sortIndicatorOrder())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto columns = values.value(QStringLiteral("columns")).toArray();
        auto *header = horizontalHeader();
        QSet<int> restored;
        for (const auto &entry : columns) {
            const auto column = entry.toObject();
            const int logical = static_cast<int>(columnIds().indexOf(column.value(QStringLiteral("id")).toString()));
            if (logical < 0 || restored.contains(logical)) continue;
            const int visual = static_cast<int>(restored.size());
            restored.insert(logical);
            header->moveSection(header->visualIndex(logical), visual);
            header->resizeSection(logical, preferenceInteger(column, QStringLiteral("width"), 100, 24, 4096));
            header->setSectionHidden(logical, column.value(QStringLiteral("hidden")).toBool(false));
        }
        const int sort = static_cast<int>(columnIds().indexOf(values.value(QStringLiteral("sortColumn")).toString()));
        if (sort >= 0) sortByColumn(sort, static_cast<Qt::SortOrder>(
            preferenceInteger(values, QStringLiteral("sortOrder"), Qt::AscendingOrder, 0, 1)));
    }
private:
    static const QStringList &columnIds()
    {
        static const QStringList ids{QStringLiteral("identity"), QStringLiteral("type"), QStringLiteral("latitude"),
                                     QStringLiteral("longitude"), QStringLiteral("stale")};
        return ids;
    }
};
class TracksPlugin final : public QObject, public MapLayer, public ToolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.MapLayer/1.0")
    Q_INTERFACES(swak::MapLayer swak::ToolPlugin)
public:
    QString name() const override { return tr("Events"); }
    int order() const override { return 100; }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const QRectF visible(QPointF(0, 0), view.size);
        int labels = 0;
        for (const auto &event : store.events()) {
            if (event.start > now || event.stale <= now) continue;
            const QPointF point = view.screen(event.latitude, event.longitude);
            if (!visible.adjusted(-12, -12, 12, 12).contains(point)) continue;
            const bool selected = event.uid == store.selected();
            painter.setPen(QPen(selected ? QColor("#ffffff") : QColor("#13202a"), selected ? 3 : 2));
            painter.setBrush(event.type.startsWith(QLatin1String("a-f")) ? QColor("#54c8ff") :
                             event.type.startsWith(QLatin1String("a-h")) ? QColor("#ff7a7a") : QColor("#f6d477"));
            painter.drawEllipse(point, selected ? 8 : 6, selected ? 8 : 6);
            if (selected || labels++ < 100) {
                painter.setPen(QColor("#ffffff"));
                painter.drawText(point + QPointF(12, -8), (event.callsign.isEmpty() ? event.uid : event.callsign).left(32));
            }
        }
    }
    QWidget *create(Store &store, const Codec &, QWidget *parent) override
    {
        auto *table = new EventTable(parent);
        table->setObjectName(QStringLiteral("eventsTable"));
        table->setAccessibleName(tr("Received and generated events"));
        auto *proxy = new QSortFilterProxyModel(table);
        proxy->setSourceModel(&store);
        table->setModel(proxy);
        table->setSortingEnabled(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionsMovable(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
        connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, table,
                [&store](const QModelIndex &current) { if (current.isValid()) store.select(current.data(Qt::UserRole).toString()); });
        connect(&store, &Store::selectionChanged, table, [table, proxy, &store] {
            for (int row = 0; row < proxy->rowCount(); ++row) {
                if (proxy->index(row, 0).data(Qt::UserRole).toString() == store.selected()) {
                    table->selectRow(row);
                    return;
                }
            }
            table->clearSelection();
        });
        return table;
    }
};
}
#include "tracks.moc"
