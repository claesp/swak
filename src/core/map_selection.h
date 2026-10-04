#pragma once
#include "core/projection.h"
#include <QObject>
#include <QWidget>
#include <QJsonObject>
#include <QPainterPath>
#include <QColor>
#include <QIcon>
#include <optional>
#include <utility>
class QPainter;
namespace swak {
struct MapItem {
    enum class Kind { Point, Line, Polygon };
    Kind kind = Kind::Point;
    QString owner;
    QString id;
    QString label;
    QIcon icon;
    QString type;
    QString description;
    QJsonObject properties;
    QPointF point;
    QPainterPath path;
    QColor color;
    double radius = 6;
    double lineWidth = 2;
    QString symbol = {};
    QPainterPath markerOutline;
    Qt::PenStyle outlineStyle = Qt::DashLine;
};
class MapSelection final : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    const std::optional<MapItem> &item() const { return item_; }
    void select(MapItem selected) { item_ = std::move(selected); emit changed(); }
    void clear() { if (item_) { item_.reset(); emit changed(); } }
    void paint(QPainter &painter, const Viewport &view, std::optional<QPointF> point = {}, double opacity = 1) const;
signals:
    void changed();
private:
    std::optional<MapItem> item_;
};
class PaneContent : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual Qt::DockWidgetArea preferredDockArea() const { return Qt::BottomDockWidgetArea; }
signals:
    void visibilityRequested(bool visible);
};
}
