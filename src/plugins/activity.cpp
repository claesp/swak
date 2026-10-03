#include "core/interfaces.h"
#include "core/activity.h"
#include "core/preferences.h"
#include <QTableView>
#include <QSortFilterProxyModel>
#include <QHeaderView>
#include <QMenu>

namespace swak {
class ActivityTable final : public QTableView, public PreferenceParticipant {
public:
    explicit ActivityTable(QWidget *parent) : QTableView(parent) {}
    QString preferenceId() const override { return QStringLiteral("org.swak.activity.table"); }
    QJsonObject savePreferences() const override
    { return {{QStringLiteral("header"), QString::fromLatin1(horizontalHeader()->saveState().toBase64())}}; }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto bytes = preferenceText(values, QStringLiteral("header"), {}, 16384).toLatin1();
        if (!bytes.isEmpty()) horizontalHeader()->restoreState(QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
    }
};
class ActivityPlugin final : public QObject, public Plugin, public PanePlugin, public MenuPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "activity/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MenuPlugin)
public:
    QString name() const override { return tr("Events"); }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        auto *table = new ActivityTable(parent);
        table->setObjectName(QStringLiteral("activityTable"));
        table->setAccessibleName(name());
        auto *proxy = new QSortFilterProxyModel(table);
        proxy->setSourceModel(&services.activity);
        proxy->setSortRole(ActivityModel::SortRole);
        proxy->setSortLocaleAware(true);
        table->setModel(proxy);
        table->setSortingEnabled(true);
        table->sortByColumn(0, Qt::DescendingOrder);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionsMovable(true);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setColumnWidth(0, 190);
        table->setColumnWidth(3, 180);
        return table;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        auto *clear = menu.addAction(tr("Clear"));
        clear->setObjectName(QStringLiteral("clearActivityAction"));
        auto *model = &services.activity;
        connect(clear, &QAction::triggered, model, &ActivityModel::clear);
    }
};
}
#include "activity.moc"
