#pragma once
#include "core/maps.h"
#include <QDialog>
#include <functional>
class QListWidget;
class QLabel;
namespace swak {
class MapManager final : public QDialog {
    Q_OBJECT
public:
    explicit MapManager(MapSources &sources, QWidget *parent = nullptr, std::function<QString()> save = {});
private:
    void reportError(const QString &message);
    void refresh();
    void edit();
    MapSources &sources_;
    std::function<QString()> save_;
    QString applyError_;
    QListWidget *list_;

    QLabel *status_;
};
}
