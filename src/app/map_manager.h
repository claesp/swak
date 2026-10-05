#pragma once
#include "core/maps.h"

#include <QDialog>

#include <functional>
class QLabel;
namespace swak {
class ProviderTable;
class MapManager final : public QDialog
{
    Q_OBJECT
public:
    explicit MapManager(MapSources &sources,
                        QWidget *parent = nullptr,
                        std::function<QString()> save = {},
                        bool embedded = false);

private:
    void reportError(const QString &message);
    void refresh();
    void edit();
    MapSources &sources_;
    std::function<QString()> save_;
    QString applyError_;
    ProviderTable *list_ = nullptr;

    QLabel *status_ = nullptr;
};
} // namespace swak
