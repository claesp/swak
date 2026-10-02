#pragma once
#include "core/maps.h"
#include <QDialog>
class QListWidget;
class QComboBox;
class QLabel;
namespace swak {
class MapManager final : public QDialog {
    Q_OBJECT
public:
    explicit MapManager(MapSources &sources, QWidget *parent = nullptr);
private:
    void refresh();
    void edit();
    MapSources &sources_;
    QListWidget *list_;
    QComboBox *formats_;
    QLabel *status_;
};
}
