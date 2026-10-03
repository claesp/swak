#pragma once
#include "core/marker_collections.h"
#include <QDialog>
#include <functional>
class QLabel;
namespace swak {
class ProviderTable;
class MarkerManager final : public QDialog {
    Q_OBJECT
public:
    MarkerManager(MarkerCollections &collections, QWidget *parent, std::function<QString()> save, bool embedded = false);
private:
    void refresh();
    void edit(bool adding);
    void report(const QString &error);
    void save();
    MarkerCollections &collections_;
    std::function<QString()> save_;
    ProviderTable *table_;
    QLabel *status_;
};
}
