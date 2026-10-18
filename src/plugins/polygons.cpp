#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/measure.h"
#include "core/menu_order.h"
#include "core/modal.h"
#include "core/object_editor.h"
#include "core/polygon.h"
#include "core/store.h"
#include "core/toolbar.h"
#include "core/ui_icons.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolBar>
#include <QUuid>

#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
namespace swak {
namespace {
// An area is a closed outline with a fill. A route is an open line with several points.
enum class Outline
{
    Area,
    Route
};
QPointF screenPoint(QPointF point, const Viewport &view)
{
    return (point - view.center) * view.scale() +
           QPointF(view.size.width() / 2, view.size.height() / 2);
}
QPointF worldPoint(QPointF point, const Viewport &view)
{
    return view.center +
           (point - QPointF(view.size.width() / 2, view.size.height() / 2)) / view.scale();
}
QPolygonF screenPolygon(const QPolygonF &points, const Viewport &view)
{
    QPolygonF result;
    for (const auto &point : points) {
        result.append(screenPoint(point, view));
    }
    return result;
}
class PolygonEditor final : public ObjectEditor, public SettingsParticipant
{
public:
    PolygonEditor(UserObjects &objects,
                  MarkerCollections &collections,
                  QWidget *parent,
                  Outline kind = Outline::Area)
        : ObjectEditor(objects, collections, parent), outline(kind)
    {
        setObjectName(route() ? QStringLiteral("routeEditor") : QStringLiteral("polygonEditor"));
        key_ = defaultKey();
    }
    QString preferenceId() const override
    {
        return route() ? QStringLiteral("org.swak.routes") : QStringLiteral("org.swak.polygons");
    }
    QString settingsTitle() const override
    {
        return route() ? tr("Routes") : tr("Areas");
    }
    QList<SettingField> settingsFields() const override
    {
        QList<SettingField> fields{
            keySetting(QStringLiteral("createKey"),
                       route() ? tr("Create route") : tr("Create area")),
            numberSetting(QStringLiteral("vertexRadius"),
                          route() ? tr("Point radius (pixels)") : tr("Corner radius (pixels)"),
                          1,
                          6),
            numberSetting(QStringLiteral("lineWidth"),
                          route() ? tr("Line width (pixels)") : tr("Outline width (pixels)"),
                          1,
                          6)};
        if (!route()) {
            fields.append(
                numberSetting(QStringLiteral("fillOpacity"), tr("Fill opacity"), 0, 1, 2));
        }
        fields.append(
            numberSetting(QStringLiteral("guideOpacity"), tr("Guide line opacity"), 0, 1, 2));
        return fields;
    }
    QJsonObject savePreferences() const override
    {
        auto state = editorState();
        state.insert(QStringLiteral("createKey"),
                     QKeySequence(key_).toString(QKeySequence::PortableText));
        if (!route()) {
            state.insert(QStringLiteral("areaShortcutVersion"), 3);
            state.insert(QStringLiteral("fillOpacity"), fillOpacity);
        }
        state.insert(QStringLiteral("vertexRadius"), radius);
        state.insert(QStringLiteral("lineWidth"), lineWidth);
        state.insert(QStringLiteral("guideOpacity"), guideOpacity);
        return state;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        if (readKey(values)) {
            return {};
        }
        return route() ? tr("Use one letter, with optional Shift, to create a route.")
                       : tr("Use one letter, with optional Shift, to create an area.");
    }
    void restorePreferences(const QJsonObject &values) override
    {
        key_ = readKey(values);
        if (!key_) {
            key_ = defaultKey();
        }
        radius = preferenceNumber(values, QStringLiteral("vertexRadius"), 3, 1, 6);
        lineWidth = preferenceNumber(values, QStringLiteral("lineWidth"), route() ? 3 : 2, 1, 6);
        fillOpacity = preferenceNumber(values, QStringLiteral("fillOpacity"), 0.3, 0, 1);
        guideOpacity = preferenceNumber(values, QStringLiteral("guideOpacity"), 0.5, 0, 1);
        restoreEditorState(values);
        emit configurationChanged();
    }
    int createKey() const
    {
        return key_;
    }
    bool route() const
    {
        return outline == Outline::Route;
    }
    const Outline outline;
    double radius = 3, lineWidth = 2, fillOpacity = 0.3, guideOpacity = 0.5;

private:
    int defaultKey() const
    {
        return QKeyCombination(Qt::ShiftModifier, route() ? Qt::Key_R : Qt::Key_A).toCombined();
    }
    int readKey(const QJsonObject &values) const
    {
        auto text = preferenceText(values,
                                   QStringLiteral("createKey"),
                                   route() ? QStringLiteral("Shift+R") : QStringLiteral("Shift+A"),
                                   32);
        if (!route()) {
            if (values.value(QStringLiteral("areaShortcutVersion")).toInt() < 2 &&
                text == QStringLiteral("P")) {
                text = QStringLiteral("A");
            }
            if (values.value(QStringLiteral("areaShortcutVersion")).toInt() < 3 &&
                text == QStringLiteral("A")) {
                text = QStringLiteral("Shift+A");
            }
        }
        const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (key.count() != 1 || (key[0].keyboardModifiers() != Qt::NoModifier &&
                                 key[0].keyboardModifiers() != Qt::ShiftModifier)) {
            return 0;
        }
        return key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z ? key[0].toCombined() : 0;
    }
    int key_ = 0;
};
class PolygonTool final : public MapTool
{
public:
    PolygonTool(PluginServices &services, PolygonEditor &editor)
        : MapTool(&editor), objects_(*services.objects), selection_(*services.selection),
          store_(services.store), tools_(*services.mapTools), editor_(editor)
    {
        setObjectName(editor_.route() ? QStringLiteral("routeTool")
                                      : QStringLiteral("polygonTool"));
        connect(&editor_, &ObjectEditor::applied, this, [this](const QString &id) {
            resetPointer();
            select(id);
            if (map_) {
                map_->window()->activateWindow();
                map_->setFocus(Qt::OtherFocusReason);
            }
            emit changed();
        });
        connect(&editor_, &ObjectEditor::cancelled, this, [this] {
            resetPointer();
            emit changed();
        });
        connect(&editor_, &ObjectEditor::configurationChanged, this, [this] {
            if (selection_.item() && selection_.item()->owner == owner()) {
                select(selection_.item()->id);
            }
            emit changed();
            emit overlayChanged();
        });
        connect(&objects_, &UserObjects::changed, this, [this] {
            emit itemsChanged();
            if (selection_.item() && selection_.item()->owner == owner()) {
                const auto id = selection_.item()->id;
                if (objects_.find(id)) {
                    select(id);
                } else {
                    selection_.clear();
                }
            }
            emit changed();
        });
        connect(&objects_, &UserObjects::restored, this, [this](const QString &id) {
            const auto item = objects_.find(id);
            if (item && item->type == type()) {
                cancel();
                select(id);
            }
        });
    }
    void activate(QWidget &map) override
    {
        cancel();
        map_ = &map;
        drawing_ = true;
        selection_.clear();
        store_.select({});
        map.setCursor(Qt::CrossCursor);
        emit changed();
    }
    bool interactionActive() const override
    {
        return drawing_ || moving_ >= 0 || movingObject_.has_value() || editor_.draft().has_value();
    }
    std::optional<double> areaFillOpacity() const override
    {
        if (editor_.route()) {
            return std::nullopt;
        }
        return editor_.fillOpacity;
    }
    bool guidesActive() const override
    {
        return drawing_ || moving_ >= 0 || movingObject_.has_value();
    }
    QString statusText() const override
    {
        if (!guidesActive()) {
            return {};
        }
        // Areas keep their earlier short status text.
        return editor_.route()
                   ? QKeySequence(editor_.createKey()).toString(QKeySequence::NativeText)
                   : QStringLiteral("A");
    }
    bool cancel() override
    {
        const bool active = interactionActive();
        resetPointer();
        editor_.cancel();
        if (active) {
            emit changed();
        }
        return active;
    }
    bool dismiss() override
    {
        if (moving_ >= 0) {
            moving_ = -1;
            skipMoveRelease_ = false;
            if (map_) {
                map_->unsetCursor();
            }
            emit changed();
            return true;
        }
        return cancel();
    }
    bool deleteSelection() override
    {
        if (interactionActive() || !selection_.item() || selection_.item()->owner != owner()) {
            return false;
        }
        deleteObject(objects_, selection_.item()->id, &editor_);
        return true;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &map) override
    {
        map_ = &map;
        view_ = view;
        if (event.type() == QEvent::KeyPress) {
            auto &key = static_cast<QKeyEvent &>(event);
            if (key.keyCombination().toCombined() == editor_.createKey()) {
                if (!key.isAutoRepeat()) {
                    if (interactionActive()) {
                        cancel();
                    } else {
                        emit activationRequested();
                    }
                }
                return true;
            }
            if (key.modifiers() != Qt::NoModifier) {
                return false;
            }
            if (key.key() == Qt::Key_Escape) {
                return dismiss();
            }
            if (drawing_ && (key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter)) {
                if (!key.isAutoRepeat() && points_.size() >= 2) {
                    auto candidate = points_;
                    if (!editor_.route() && candidate.size() == 2) {
                        candidate.append(worldPoint(pointer_, view));
                    }
                    complete(candidate);
                }
                return true;
            }
            if (key.key() == Qt::Key_Delete || key.key() == Qt::Key_Backspace) {
                return key.isAutoRepeat() || deleteSelection();
            }
        }
        if (event.type() == QEvent::MouseButtonDblClick && editingVertices_ && editor_.draft() &&
            moving_ < 0) {
            auto &mouse = static_cast<QMouseEvent &>(event);
            if (mouse.button() == Qt::LeftButton) {
                const auto points = vertices(editor_.draft()->geometry);
                for (qsizetype i = 0; i < points.size(); ++i) {
                    if (near(screenPoint(points.at(i), view), mouse.position())) {
                        moving_ = static_cast<int>(i);
                        skipMoveRelease_ = true;
                        pointer_ = mouse.position();
                        map.setCursor(Qt::CrossCursor);
                        map.setFocus();
                        emit changed();
                        return true;
                    }
                }
            }
        }
        if (event.type() == QEvent::MouseMove) {
            pointer_ = static_cast<QMouseEvent &>(event).position();
            if (guidesActive()) {
                emit overlayChanged();
                return true;
            }
        }
        if ((event.type() == QEvent::MouseButtonPress ||
             event.type() == QEvent::MouseButtonDblClick) &&
            guidesActive() && static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
            return true;
        }
        if (event.type() == QEvent::MouseButtonRelease) {
            auto &mouse = static_cast<QMouseEvent &>(event);
            if (mouse.button() != Qt::LeftButton) {
                return false;
            }
            pointer_ = mouse.position();
            if (skipMoveRelease_) {
                skipMoveRelease_ = false;
                return true;
            }
            if (drawing_) {
                addPoint(view, map);
                return true;
            }
            if (movingObject_) {
                auto item = *movingObject_;
                item.geometry = polygonGeometry(movedPoints(worldPoint(pointer_, view)));
                const auto problem = objects_.put(item);
                if (problem.isEmpty()) {
                    resetPointer();
                    select(item.id);
                    emit changed();
                } else {
                    error(problem);
                }
                return true;
            }
            if (moving_ >= 0 && editor_.draft()) {
                auto candidate = vertices(editor_.draft()->geometry);
                if (moving_ >= candidate.size()) {
                    dismiss();
                    return true;
                }
                candidate[moving_] = worldPoint(pointer_, view);
                if (editor_.setGeometry(polygonGeometry(candidate))) {
                    moving_ = -1;
                    skipMoveRelease_ = false;
                    map.unsetCursor();
                    emit changed();
                } else {
                    map.setCursor(Qt::CrossCursor);
                }
                return true;
            }
            if (editor_.draft()) {
                return true;
            }
        }
        if (event.type() == QEvent::ContextMenu) {
            auto &context = static_cast<QContextMenuEvent &>(event);
            pointer_ = context.pos();
            if (guidesActive()) {
                return true;
            }
            if (editor_.draft()) {
                if (editingVertices_) {
                    vertexMenu(context, view, map);
                }
                return true;
            }
        }
        return false;
    }
    bool interact(const MapItem &item, QEvent &event, QWidget &map) override
    {
        if (item.owner != owner()) {
            return false;
        }
        const auto record = objects_.find(item.id);
        if (!record || record->type != type()) {
            return true;
        }
        map_ = &map;
        const auto id = item.id;
        if (event.type() == QEvent::MouseButtonRelease) {
            editor_.cancel();
            select(id);
            return true;
        }
        if (event.type() != QEvent::ContextMenu) {
            return false;
        }
        auto &context = static_cast<QContextMenuEvent &>(event);
        tools_.cancel();
        select(id);
        const auto names = editor_.route() ? std::array{QStringLiteral("routeContextMenu"),
                                                        QStringLiteral("editRoute"),
                                                        QStringLiteral("moveRoute"),
                                                        QStringLiteral("deleteRoute")}
                                           : std::array{QStringLiteral("polygonContextMenu"),
                                                        QStringLiteral("editPolygon"),
                                                        QStringLiteral("movePolygon"),
                                                        QStringLiteral("deletePolygon")};
        Modal<QMenu> menu(&map);
        menu->setObjectName(names[0]);
        auto *edit = menu->addAction(uiIcon(UiIcon::Edit), tr("Edit"));
        edit->setObjectName(names[1]);
        auto *move = menu->addAction(uiIcon(UiIcon::Move), tr("Move"));
        move->setObjectName(names[2]);
        auto *remove = menu->addAction(uiIcon(UiIcon::Delete), tr("Delete ..."));
        remove->setObjectName(names[3]);
        edit->setProperty("menuOrder", -3);
        move->setProperty("menuOrder", -2);
        remove->setProperty("menuOrder", -1);
        sortMenuActions(*menu);
        connect(edit, &QAction::triggered, this, [this, id] {
            const auto currentObject = objects_.find(id);
            if (currentObject) {
                selection_.clear();
                editor_.edit(*currentObject);
                editingVertices_ = true;
                emit changed();
            }
        });
        const auto anchor = worldPoint(QPointF(context.pos()), view_);
        connect(move, &QAction::triggered, this, [this, id, anchor] {
            movingObject_ = objects_.find(id);
            if (movingObject_ && map_) {
                anchor_ = anchor;
                selection_.clear();
                map_->setCursor(Qt::CrossCursor);
                map_->setFocus();
                emit changed();
            }
        });
        connect(remove, &QAction::triggered, this, [this, id] {
            deleteObject(objects_, id, &editor_);
        });
        tools_.populateContextMenu(item, *menu);
        menu->exec(context.globalPos());
        return true;
    }
    QList<MapItem> itemsAt(const Viewport &view, QPointF pointer) const override
    {
        QList<MapItem> result;
        for (const auto &record : objects_.items()) {
            if (record.type != type()) {
                continue;
            }
            const auto points = vertices(record.geometry);
            const QFontMetrics metrics(QGuiApplication::font());
            const auto text = label(record, points);
            const auto anchor = labelAnchor(points, metrics, text, view);
            bool hit = mapLabelBounds(metrics, anchor, text).contains(pointer);
            if (editor_.route()) {
                for (int wrap = -1; wrap <= 1 && !hit; ++wrap) {
                    auto line = screenPolygon(unwrappedRoute(points), view);
                    line.translate(wrap * view.scale(), 0);
                    hit = lineDistance(line, pointer) <= editor_.lineWidth / 2 + 5;
                }
            } else {
                hit = hit || points.containsPoint(worldPoint(pointer, view), Qt::OddEvenFill);
            }
            if (!hit) {
                continue;
            }
            MapItem item;
            item.owner = owner();
            item.id = record.id;
            item.label = record.title;
            item.type = typeName();
            item.kind = editor_.route() ? MapItem::Kind::Line : MapItem::Kind::Polygon;
            result.append(std::move(item));
        }
        return result;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        for (const auto &item : objects_.items()) {
            if (item.type != type() || (editor_.draft() && editor_.draft()->id == item.id) ||
                (movingObject_ && movingObject_->id == item.id)) {
                continue;
            }
            const auto points = vertices(item.geometry);
            draw(painter, points, view, QColor(item.color), item.restricted);
            const auto text = label(item, points);
            paintMapLabel(painter,
                          labelAnchor(points, painter.fontMetrics(), text, view),
                          text,
                          selection_.item() && selection_.item()->owner == owner() &&
                              selection_.item()->id == item.id);
        }
        if (editor_.draft() && moving_ < 0) {
            draw(painter,
                 vertices(editor_.draft()->geometry),
                 view,
                 QColor(editor_.draft()->color),
                 editor_.draft()->restricted);
        }
    }
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const override
    {
        if (drawing_) {
            auto measured = points_;
            measured.append(worldPoint(pointer, view));
            draw(painter, measured, view, QColor("#1e88e5"));
            dots(painter, screenPolygon(points_, view), QColor("#1e88e5"));
            lengthLabels(painter, measured, view);
        } else if (movingObject_) {
            draw(painter,
                 movedPoints(worldPoint(pointer, view)),
                 view,
                 QColor(movingObject_->color),
                 movingObject_->restricted);
        } else if (editor_.draft() && editingVertices_) {
            auto measured = vertices(editor_.draft()->geometry);
            // The draft can change while a point moves. Check the index on each use.
            const bool movable = moving_ >= 0 && moving_ < measured.size();
            if (movable) {
                measured[moving_] = worldPoint(pointer, view);
                draw(painter,
                     measured,
                     view,
                     QColor(editor_.draft()->color),
                     editor_.draft()->restricted);
            }
            dots(painter, screenPolygon(measured, view), QColor(editor_.draft()->color));
            lengthLabels(painter, measured, view);
        }
        if (guidesActive()) {
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(QColor(0, 0, 0, qRound(editor_.guideOpacity * 255)), 1));
            painter.drawLine(QPointF(0, pointer.y()), QPointF(view.size.width(), pointer.y()));
            painter.drawLine(QPointF(pointer.x(), 0), QPointF(pointer.x(), view.size.height()));
        }
    }

private:
    QString type() const
    {
        return editor_.route() ? QStringLiteral("route") : QStringLiteral("polygon");
    }
    QString owner() const
    {
        return editor_.route() ? QStringLiteral("org.swak.routes")
                               : QStringLiteral("org.swak.polygons");
    }
    QString typeName() const
    {
        return editor_.route() ? tr("Route") : tr("Area");
    }
    QPolygonF vertices(const QJsonObject &geometry) const
    {
        return editor_.route() ? routePoints(geometry) : polygonVertices(geometry);
    }
    QString validate(const QPolygonF &points) const
    {
        return editor_.route() ? validateRoute(points) : validatePolygon(points);
    }
    QString label(const UserObject &item, const QPolygonF &points) const
    {
        if (!editor_.route()) {
            return item.title;
        }
        const auto length = formatDistance(routeLength(points));
        return item.title.isEmpty() ? length : item.title + QStringLiteral(" · ") + length;
    }
    // Areas show the label at the center. Routes show it beside the middle point.
    QPointF labelAnchor(const QPolygonF &points,
                        const QFontMetrics &metrics,
                        const QString &text,
                        const Viewport &view) const
    {
        if (editor_.route()) {
            const auto line = unwrappedRoute(points);
            return line.isEmpty() ? QPointF{}
                                  : screenPoint(line.at(line.size() / 2), view) + QPointF(6, -8);
        }
        const auto shown = metrics.elidedText(text, Qt::ElideRight, 240);
        return screenPoint(polygonLabelPoint(points), view) +
               QPointF(-metrics.horizontalAdvance(shown) / 2.0, metrics.ascent() / 2.0);
    }
    void lengthLabels(QPainter &painter, const QPolygonF &points, const Viewport &view) const
    {
        const auto count = points.size();
        if (count < 2) {
            return;
        }
        const auto line = editor_.route() ? unwrappedRoute(points) : points;
        const auto segments = editor_.route() || count == 2 ? count - 1 : count;
        for (qsizetype i = 0; i < segments; ++i) {
            const auto &a = line[i], &b = line[(i + 1) % count];
            const auto start = screenPoint(a, view), end = screenPoint(b, view);
            if (QLineF(start, end).length() < 1) {
                continue;
            }
            paintMapLabel(
                painter, (start + end) / 2 + QPointF(4, -8), formatDistance(groundDistance(a, b)));
        }
        if (editor_.route() && count > 2) {
            paintMapLabel(painter,
                          screenPoint(line.last(), view) + QPointF(8, 16),
                          tr("Total: %1").arg(formatDistance(routeLength(points))));
        }
    }
    bool near(QPointF a, QPointF b) const
    {
        return std::hypot(a.x() - b.x(), a.y() - b.y()) <= editor_.radius + 5;
    }
    void resetPointer()
    {
        drawing_ = false;
        editingVertices_ = false;
        moving_ = -1;
        movingObject_.reset();
        skipMoveRelease_ = false;
        points_.clear();
        if (map_) {
            map_->unsetCursor();
        }
    }
    void error(const QString &message)
    {
        editor_.reportError(message);
        logMessage(Severity::Warning,
                   Urgency::Attention,
                   editor_.route() ? QStringLiteral("swak.routes")
                                   : QStringLiteral("swak.polygons"),
                   message);
    }
    void addPoint(const Viewport &view, QWidget &map)
    {
        map.setCursor(Qt::CrossCursor);
        if (!editor_.route() && points_.size() >= 3 &&
            near(screenPoint(points_.first(), view), pointer_)) {
            complete(points_);
            return;
        }
        auto point = worldPoint(pointer_, view);
        if (editor_.route()) {
            point.rx() -= std::floor(point.x());
        }
        if (!QRectF(0, 0, 1, 1).contains(point)) {
            return;
        }
        if (editor_.route() ? !points_.isEmpty() && points_.last() == point
                            : points_.contains(point)) {
            error(editor_.route() ? tr("Choose a different route point.")
                                  : tr("Choose a different polygon corner."));
            return;
        }
        const int limit = editor_.route() ? MaxRoutePoints : MaxPolygonVertices;
        if (points_.size() >= limit) {
            error(editor_.route() ? tr("Use at most %1 route points.").arg(limit)
                                  : tr("Use at most 64 polygon corners."));
            return;
        }
        points_.append(point);
        emit overlayChanged();
    }
    void complete(const QPolygonF &candidate)
    {
        const auto problem = validate(candidate);
        if (!problem.isEmpty()) {
            error(problem);
            return;
        }
        UserObject item;
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.type = type();
        item.title = editor_.route() ? tr("New Route") : tr("New Area");
        item.color = QStringLiteral("#1e88e5");
        item.geometry = polygonGeometry(candidate);
        resetPointer();
        editor_.edit(item);
        emit changed();
    }
    QPolygonF movedPoints(QPointF point) const
    {
        auto points = vertices(movingObject_->geometry);
        points.translate(point - anchor_);
        if (editor_.route()) {
            for (auto &p : points) {
                p.setX(p.x() - std::floor(p.x()));
            }
        }
        return points;
    }
    void draw(QPainter &painter,
              const QPolygonF &points,
              const Viewport &view,
              const QColor &color,
              bool restricted = false) const
    {
        painter.setPen(QPen(color, editor_.lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (editor_.route()) {
            painter.setBrush(Qt::NoBrush);
            const auto line = screenPolygon(unwrappedRoute(points), view);
            for (int wrap = -1; wrap <= 1; ++wrap) {
                painter.drawPolyline(line.translated(wrap * view.scale(), 0));
            }
            return;
        }
        const auto polygon = screenPolygon(points, view);
        auto fill = color;
        fill.setAlpha(qRound(editor_.fillOpacity * 255));
        painter.setBrush(polygon.size() >= 3 ? QBrush(fill) : Qt::NoBrush);
        if (polygon.size() >= 3) {
            painter.drawPolygon(polygon);
            if (restricted) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QBrush(color, Qt::BDiagPattern));
                painter.drawPolygon(polygon);
            }
        } else {
            painter.drawPolyline(polygon);
        }
    }
    void dots(QPainter &painter, const QPolygonF &points, const QColor &color) const
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (const auto &point : points) {
            painter.drawEllipse(point, editor_.radius, editor_.radius);
        }
    }
    void vertexMenu(QContextMenuEvent &context, const Viewport &view, QWidget &map)
    {
        const auto points = vertices(editor_.draft()->geometry);
        int index = -1;
        for (qsizetype i = 0; i < points.size(); ++i) {
            if (near(screenPoint(points.at(i), view), context.pos())) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            return;
        }
        auto reduced = points;
        reduced.removeAt(index);
        Modal<QMenu> menu(&map);
        menu->setObjectName(editor_.route() ? QStringLiteral("routePointMenu")
                                            : QStringLiteral("polygonVertexMenu"));
        auto *move = menu->addAction(uiIcon(UiIcon::Move), tr("Move"));
        move->setObjectName(editor_.route() ? QStringLiteral("moveRoutePoint")
                                            : QStringLiteral("movePolygonVertex"));
        move->setProperty("menuOrder", -1);
        auto *remove = menu->addAction(uiIcon(UiIcon::Delete), tr("Delete ..."));
        remove->setObjectName(editor_.route() ? QStringLiteral("deleteRoutePoint")
                                              : QStringLiteral("deletePolygonVertex"));
        remove->setEnabled(validate(reduced).isEmpty());
        connect(move, &QAction::triggered, this, [this, index, &map] {
            if (!editor_.draft() || index >= vertices(editor_.draft()->geometry).size()) {
                return;
            }
            moving_ = index;
            map.setCursor(Qt::CrossCursor);
            map.setFocus();
            emit changed();
        });
        connect(remove, &QAction::triggered, this, [this, reduced] {
            if (confirmDeletion(
                    &editor_, editor_.route() ? tr("this route point") : tr("this area corner"))) {
                editor_.setGeometry(polygonGeometry(reduced));
            }
        });
        menu->exec(context.globalPos());
    }
    void select(const QString &id)
    {
        const auto record = objects_.find(id);
        if (!record || record->type != type()) {
            return;
        }
        const auto points = vertices(record->geometry);
        MapItem item;
        item.kind = editor_.route() ? MapItem::Kind::Line : MapItem::Kind::Polygon;
        item.owner = owner();
        item.id = id;
        item.label = record->title;
        item.type = typeName();
        item.icon = uiIcon(editor_.route() ? UiIcon::Route : UiIcon::Area);
        item.description = record->description;
        item.color = QColor(record->color);
        item.lineWidth = editor_.lineWidth;
        item.outlineStyle = Qt::SolidLine;
        if (editor_.route()) {
            item.path.addPolygon(unwrappedRoute(points));
        } else {
            item.path.addPolygon(points);
            item.path.closeSubpath();
        }
        item.properties = record->properties;
        item.properties.insert(QStringLiteral("ID"), id);
        if (editor_.route()) {
            item.properties.insert(QStringLiteral("Length"), formatDistance(routeLength(points)));
            item.properties.insert(tr("Points"), QString::number(points.size()));
        } else {
            item.properties.insert(QStringLiteral("Area"), formatArea(polygonArea(points)));
            item.properties.insert(tr("Restricted"), record->restricted ? tr("Yes") : tr("No"));
        }
        store_.select({});
        selection_.select(std::move(item));
    }
    bool drawing_ = false;
    bool editingVertices_ = false;
    int moving_ = -1;
    bool skipMoveRelease_ = false;
    std::optional<UserObject> movingObject_;
    QPointF anchor_;
    QPolygonF points_;
    QPointF pointer_;
    Viewport view_;
    QPointer<QWidget> map_;
    UserObjects &objects_;
    MapSelection &selection_;
    Store &store_;
    MapTools &tools_;
    PolygonEditor &editor_;
};
} // namespace
class PolygonPlugin final : public QObject,
                            public Plugin,
                            public PanePlugin,
                            public MenuPlugin,
                            public ToolbarPlugin,
                            public MapExtension
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "polygons/MANIFEST")
    Q_INTERFACES(
        swak::Plugin swak::PanePlugin swak::MenuPlugin swak::ToolbarPlugin swak::MapExtension)
public:
    QString name() const override
    {
        return tr("Edit area");
    }
    QString mapId() const override
    {
        return QStringLiteral("org.swak.polygons");
    }
    QString mapName() const override
    {
        return tr("Areas");
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
    QAction *primaryAction(QWidget *pane) override
    {
        return pane->findChild<QAction *>(QStringLiteral("polygonModeAction"));
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return createEditor(services, parent, Outline::Area);
    }
    QList<PluginPane> additionalPanes(PluginServices &services, QWidget *parent) override
    {
        auto *editor = createEditor(services, parent, Outline::Route);
        routeEditors_.insert(services.mapTools, editor);
        return {{QStringLiteral("org.swak.routes.pane"), tr("Edit route"), editor}};
    }
    bool rootActions() const override
    {
        return true;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        addRouteAction(services, [&menu](QAction *action) {
            menu.addAction(action);
        });
    }
    void populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *) override
    {
        addRouteAction(services, [&toolbar](QAction *action) {
            toolbar.addAction(action);
        });
    }

private:
    QWidget *createEditor(PluginServices &services, QWidget *parent, Outline outline)
    {
        if (!services.objects || !services.selection || !services.mapTools ||
            !services.collections) {
            return new QWidget(parent);
        }
        const bool route = outline == Outline::Route;
        auto *editor = new PolygonEditor(*services.objects, *services.collections, parent, outline);
        auto *tool = new PolygonTool(services, *editor);
        services.mapTools->add(tool);
        auto *action = new QAction(
            uiIcon(route ? UiIcon::Route : UiIcon::Area), route ? tr("Route") : tr("Area"), editor);
        action->setObjectName(route ? QStringLiteral("routeModeAction")
                                    : QStringLiteral("polygonModeAction"));
        // Route follows Line in the creation section.
        setToolbarSection(*action, ToolbarSection::Creation, route ? 4 : 1);
        action->setCheckable(true);
        const auto updateAction = [action, editor, tool, route] {
            action->setChecked(!tool->statusText().isEmpty());
            const auto key = QKeySequence(editor->createKey()).toString(QKeySequence::NativeText);
            action->setToolTip(route ? tr("Route mode (%1)").arg(key)
                                     : tr("Area mode (%1)").arg(key));
        };
        connect(tool, &MapTool::changed, action, updateAction);
        connect(editor, &ObjectEditor::configurationChanged, action, updateAction);
        updateAction();
        connect(action, &QAction::triggered, tool, [tool] {
            if (!tool->statusText().isEmpty()) {
                tool->cancel();
            } else {
                emit tool->activationRequested();
            }
        });
        QTimer::singleShot(0, editor, [editor] {
            emit editor->visibilityRequested(false);
        });
        return editor;
    }
    void addRouteAction(const PluginServices &services, const std::function<void(QAction *)> &add)
    {
        if (const auto editor = routeEditors_.value(services.mapTools)) {
            if (auto *action = editor->findChild<QAction *>(QStringLiteral("routeModeAction"))) {
                add(action);
            }
        }
    }
    QHash<MapTools *, QPointer<QWidget>> routeEditors_;
};
} // namespace swak
#include "polygons.moc"
