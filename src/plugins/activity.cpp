#include "core/activity.h"

#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/ui_icons.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

namespace swak {
namespace {
// The saved header state is valid only for this column layout.
constexpr int HeaderLayout = ActivityModel::ColumnCount;
QString cell(const QModelIndex &row, int column)
{
    return row.siblingAtColumn(column).data().toString();
}
// One event as one line of plain text for the clipboard.
QString eventText(const QModelIndex &row)
{
    return QStringLiteral("%1\t%2\t%3\t%4")
        .arg(cell(row, ActivityModel::TimeColumn),
             cell(row, ActivityModel::SeverityColumn),
             cell(row, ActivityModel::SourceColumn),
             cell(row, ActivityModel::MessageColumn));
}
void copyEvent(const QModelIndex &row)
{
    if (row.isValid()) {
        QGuiApplication::clipboard()->setText(eventText(row));
    }
}
class ActivityTable final : public QTableView, public PreferenceParticipant
{
public:
    explicit ActivityTable(QWidget *parent) : QTableView(parent)
    {
        copy_ = new QAction(tr("Copy"), this);
        copy_->setObjectName(QStringLiteral("copyActivityEvent"));
        copy_->setShortcut(QKeySequence::Copy);
        copy_->setShortcutContext(Qt::WidgetShortcut);
        connect(copy_, &QAction::triggered, this, [this] {
            copyEvent(currentIndex());
        });
        addAction(copy_);
        view_ = new QAction(tr("View event ..."), this);
        view_->setObjectName(QStringLiteral("viewActivityEvent"));
        connect(view_, &QAction::triggered, this, [this] {
            openEvent(currentIndex());
        });
        addAction(view_);
        // Double-click and Enter open the full event.
        connect(this, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            openEvent(index);
        });
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.activity.table");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("header"), widgetStateText(horizontalHeader()->saveState())},
                {QStringLiteral("layout"), HeaderLayout}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        // Earlier profiles saved five columns, including Urgency. Keep the default layout then.
        if (values.value(QStringLiteral("layout")).toInt() != HeaderLayout) {
            return;
        }
        horizontalHeader()->restoreState(widgetStateBytes(values, QStringLiteral("header"), 16384));
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const auto index = indexAt(event->pos());
        if (!index.isValid()) {
            return;
        }
        setCurrentIndex(index);
        Modal<QMenu> menu(this);
        menu->setObjectName(QStringLiteral("activityContextMenu"));
        menu->addAction(view_);
        menu->addAction(copy_);
        menu->exec(event->globalPos());
        event->accept();
    }

private:
    void openEvent(const QModelIndex &index)
    {
        if (!index.isValid()) {
            return;
        }
        // Keep a persistent row. New events can arrive while the dialog is open.
        const QPersistentModelIndex row(index.siblingAtColumn(0));
        Modal<QDialog> dialog(this);
        dialog->setObjectName(QStringLiteral("activityDetails"));
        dialog->setWindowTitle(tr("Event"));
        dialog->resize(640, 360);
        auto *layout = new QVBoxLayout(dialog.get());
        auto *form = new QFormLayout;
        for (const auto &[column, label] :
             {std::pair{int{ActivityModel::TimeColumn}, tr("Time")},
              std::pair{int{ActivityModel::SeverityColumn}, tr("Severity")},
              std::pair{int{ActivityModel::SourceColumn}, tr("Source")}}) {
            auto *value = new QLabel(cell(row, column), dialog.get());
            value->setTextFormat(Qt::PlainText);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, value);
        }
        layout->addLayout(form);
        auto *text = new QPlainTextEdit(cell(row, ActivityModel::MessageColumn), dialog.get());
        text->setObjectName(QStringLiteral("activityDetailsText"));
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        text->setAccessibleName(tr("Event"));
        layout->addWidget(text);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog.get());
        auto *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
        copy->setObjectName(QStringLiteral("activityDetailsCopy"));
        connect(copy, &QPushButton::clicked, dialog.get(), [row] {
            copyEvent(row);
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
        layout->addWidget(buttons);
        dialog->exec();
    }
    QAction *copy_ = nullptr;
    QAction *view_ = nullptr;
};
} // namespace
class ActivityPlugin final : public QObject, public Plugin, public PanePlugin, public MenuPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "activity/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MenuPlugin)
public:
    QString name() const override
    {
        return tr("Events");
    }
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
        table->sortByColumn(ActivityModel::TimeColumn, Qt::DescendingOrder);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionsMovable(true);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setColumnWidth(ActivityModel::TimeColumn, 190);
        table->setColumnWidth(ActivityModel::SourceColumn, 180);
        return table;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        auto *clear = menu.addAction(uiIcon(UiIcon::Clear), tr("Clear ..."));
        clear->setObjectName(QStringLiteral("clearActivityAction"));
        auto *model = &services.activity;
        connect(clear, &QAction::triggered, model, [model, &menu] {
            if (confirmAction(&menu, tr("Clear events"), tr("Clear all events?"), tr("Clear"))) {
                model->clear();
            }
        });
    }
};
} // namespace swak
#include "activity.moc"
