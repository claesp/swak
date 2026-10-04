#pragma once
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRadioButton>
#include <QTableWidget>

namespace swak {
class ProviderTable final : public QTableWidget
{
public:
    explicit ProviderTable(bool exclusive, QWidget *parent)
        : QTableWidget(0, 3, parent), exclusive_(exclusive), group_(new QButtonGroup(this))
    {
        group_->setExclusive(exclusive);
        setHorizontalHeaderLabels({tr("Active"), tr("Provider"), tr("Format")});
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setAlternatingRowColors(true);
        verticalHeader()->hide();
        horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    }
    void clearProviders()
    {
        for (auto *button : group_->buttons()) {
            delete button;
        }
        setRowCount(0);
    }
    QString selectedId() const
    {
        const auto *entry = item(currentRow(), 1);
        return entry ? entry->data(Qt::UserRole).toString() : QString{};
    }
    void selectId(const QString &id)
    {
        for (int row = 0; row < rowCount(); ++row) {
            if (item(row, 1)->data(Qt::UserRole).toString() == id) {
                setCurrentCell(row, 1);
                return;
            }
        }
    }
    QAbstractButton *append(const QString &id,
                            const QString &name,
                            const QString &format,
                            const QString &endpoint,
                            bool active,
                            bool available)
    {
        const int row = rowCount();
        insertRow(row);
        auto *entry = new QTableWidgetItem(name);
        entry->setData(Qt::UserRole, id);
        entry->setToolTip(endpoint);
        setItem(row, 1, entry);
        setItem(row, 2, new QTableWidgetItem(format));
        auto *cell = new QWidget(this);
        auto *layout = new QHBoxLayout(cell);
        layout->setContentsMargins(4, 0, 4, 0);
        QAbstractButton *activeButton = exclusive_
                                            ? static_cast<QAbstractButton *>(new QRadioButton(cell))
                                            : new QCheckBox(cell);
        activeButton->setObjectName(QStringLiteral("activeProvider/") + id);
        activeButton->setAccessibleName(tr("Activate %1").arg(name));
        group_->addButton(activeButton);
        activeButton->setChecked(active);
        activeButton->setEnabled(available);
        layout->addWidget(activeButton, 0, Qt::AlignCenter);
        setCellWidget(row, 0, cell);
        return activeButton;
    }

private:
    bool exclusive_;
    QButtonGroup *group_;
};
} // namespace swak
