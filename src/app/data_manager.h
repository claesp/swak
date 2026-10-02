#pragma once
#include "core/data.h"
#include <QDialog>
#include <functional>
class QLabel;
namespace swak {
class ProviderTable;
class DataManager final : public QDialog {
    Q_OBJECT
public:
    explicit DataManager(DataSources &sources, QWidget *parent = nullptr, std::function<QString()> save = {}, bool embedded = false);
private:
    void refresh();
    void updateStatus();
    void edit(bool adding);
    void save();
    DataSources &sources_;
    std::function<QString()> save_;
    QString applyError_;
    ProviderTable *list_;
    QLabel *status_;
};
}
