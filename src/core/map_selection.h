#pragma once
#include "core/event.h"
#include "core/projection.h"

#include <QColor>
#include <QEvent>
#include <QIcon>
#include <QJsonObject>
#include <QObject>
#include <QPainterPath>
#include <QPointer>
#include <QWidget>

#include <optional>
#include <utility>
class QPainter;
namespace swak {
struct MapItem
{
    enum class Kind
    {
        Point,
        Line,
        Polygon
    };
    Kind kind = Kind::Point;
    QString owner;
    QString id;
    QString label;
    QRectF cachedLabelBounds;
    QString cachedLabelText;
    double cachedLabelFontHeight = 0;
    QIcon icon;
    QString type;
    QString description;
    QJsonObject properties;
    QPointF point;
    QPainterPath path;
    QColor color;
    double radius = 6;
    bool scaleIcon = true;
    double lineWidth = 2;
    QString symbol = {};
    QPainterPath markerOutline;
    Qt::PenStyle outlineStyle = Qt::DashLine;
};
MapItem cotMapItem(const Event &message);
class MapSelection final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    const std::optional<MapItem> &item() const
    {
        return item_;
    }
    void select(MapItem selected)
    {
        item_ = std::move(selected);
        emit changed();
    }
    void clear()
    {
        if (item_) {
            item_.reset();
            emit changed();
        }
    }
    void paint(QPainter &painter,
               const Viewport &view,
               std::optional<QPointF> point = {},
               double opacity = 1) const;
signals:
    void changed();

private:
    std::optional<MapItem> item_;
};
class PaneContent : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
    void watchPane(QWidget *pane)
    {
        pane_ = pane;
        pane->installEventFilter(this);
    }
    virtual void paneClosed() {}

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == pane_ && event->type() == QEvent::Close) {
            paneClosed();
        }
        return QWidget::eventFilter(object, event);
    }

public:
    virtual Qt::DockWidgetArea preferredDockArea() const
    {
        return Qt::BottomDockWidgetArea;
    }
signals:
    void visibilityRequested(bool visible);

private:
    QPointer<QWidget> pane_;
};
} // namespace swak
