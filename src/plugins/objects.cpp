#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/store.h"

#include <QContextMenuEvent>
#include <QHeaderView>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTableView>

#include <algorithm>

namespace swak {
namespace {
class ObjectsModel final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0) {
            return tr("Title");
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

protected:
    bool filterAcceptsColumn(int column, const QModelIndex &) const override
    {
        return column < 2;
    }
};
class ObjectsTable final : public QTableView, public PreferenceParticipant
{
public:
    ObjectsTable(PluginServices &services, QWidget *parent)
        : QTableView(parent), store_(services.store), tools_(services.mapTools),
          selection_(services.selection)
    {
        setObjectName(QStringLiteral("objectsTable"));
        setAccessibleName(tr("Objects"));
        auto *proxy = new ObjectsModel(this);
        proxy->setSourceModel(&store_);
        proxy->setSortLocaleAware(true);
        setModel(proxy);
        setSortingEnabled(true);
        sortByColumn(0, Qt::AscendingOrder);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setAlternatingRowColors(true);
        horizontalHeader()->setSectionsMovable(true);
        horizontalHeader()->setStretchLastSection(true);
        connect(this, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
            const auto item = itemAt(index);
            if (item && tools_) {
                emit tools_->centerRequested(item->point);
            }
        });
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.objects");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("header"), widgetStateText(horizontalHeader()->saveState())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        horizontalHeader()->restoreState(widgetStateBytes(values, QStringLiteral("header"), 16384));
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const auto index =
            event->reason() == QContextMenuEvent::Keyboard ? currentIndex() : indexAt(event->pos());
        const auto item = itemAt(index);
        if (!item) {
            return;
        }
        Modal<QMenu> menu(this);
        menu->setObjectName(QStringLiteral("objectsContextMenu"));
        auto *information = menu->addAction(tr("Information"));
        information->setObjectName(QStringLiteral("mapItemInformation"));
        connect(information, &QAction::triggered, menu.get(), [this, id = item->id] {
            const auto &events = store_.events();
            const auto found =
                std::find_if(events.cbegin(), events.cend(), [&id](const auto &message) {
                    return message.uid == id;
                });
            if (found != events.cend()) {
                if (selection_) {
                    selection_->select(cotMapItem(*found));
                }
                store_.select(id);
            }
        });
        if (tools_) {
            tools_->populateContextMenu(*item, *menu);
        }
        connect(&store_, &QAbstractItemModel::rowsRemoved, menu.get(), &QMenu::close);
        connect(&store_, &QAbstractItemModel::modelReset, menu.get(), &QMenu::close);
        menu->exec(event->globalPos());
        event->accept();
    }

private:
    std::optional<MapItem> itemAt(const QModelIndex &index) const
    {
        if (!index.isValid()) {
            return {};
        }
        const auto *proxy = static_cast<ObjectsModel *>(model());
        const auto source = proxy->mapToSource(index);
        if (!source.isValid() || source.row() >= store_.events().size()) {
            return {};
        }
        return cotMapItem(store_.events().at(source.row()));
    }
    Store &store_;
    QPointer<MapTools> tools_;
    QPointer<MapSelection> selection_;
};
} // namespace
class ObjectsPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "objects/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Objects");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new ObjectsTable(services, parent);
    }
};
} // namespace swak
#include "objects.moc"
