#include "core/interfaces.h"
#include "core/projection.h"
#include "core/store.h"
#include <QHeaderView>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QTableView>

namespace swak {
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
        auto *table = new QTableView(parent);
        table->setObjectName(QStringLiteral("eventsTable"));
        table->setAccessibleName(tr("Received and generated events"));
        auto *proxy = new QSortFilterProxyModel(table);
        proxy->setSourceModel(&store);
        table->setModel(proxy);
        table->setSortingEnabled(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
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
