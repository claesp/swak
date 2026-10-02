#pragma once
#include "core/data.h"
#include <QDialog>
#include <functional>
class QListWidget;
class QLabel;
namespace swak {
class DataManager final : public QDialog {
    Q_OBJECT
public:
    explicit DataManager(DataSources &sources, QWidget *parent = nullptr, std::function<QString()> save = {});
private:
    void refresh();
    void updateStatus();
    void edit(bool adding);
    void save();
    DataSources &sources_;
    std::function<QString()> save_;
    QString applyError_;
    QListWidget *list_;
    QLabel *status_;
};
}
