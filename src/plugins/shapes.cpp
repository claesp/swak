#include "core/delete_confirmation.h"
#include "core/interfaces.h"
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
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolBar>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
namespace swak {
namespace {
enum class ShapeMode
{
    Circle,
    Distance,
    Line
};
QString shapeType(ShapeMode mode)
{
    return mode == ShapeMode::Circle ? QStringLiteral("range-circle") : QStringLiteral("line");
}
QString shapeName(ShapeMode mode)
{
    return mode == ShapeMode::Circle     ? QObject::tr("Circle")
           : mode == ShapeMode::Distance ? QObject::tr("Distance")
                                         : QObject::tr("Line");
}
QPointF lineEnd(QPointF first, QPointF second)
{
    second.setX(second.x() - std::round(second.x() - first.x()));
    return second;
}
QPointF screen(QPointF point, const Viewport &view)
{
    return (point - view.center) * view.scale() +
           QPointF(view.size.width() / 2, view.size.height() / 2);
}
QPointF world(QPointF point, const Viewport &view)
{
    return view.center +
           (point - QPointF(view.size.width() / 2, view.size.height() / 2)) / view.scale();
}
class ShapesEditor final : public ObjectEditor, public SettingsParticipant
{
public:
    ShapesEditor(UserObjects &objects,
                 MarkerCollections &collections,
                 QWidget *parent,
                 bool isCircle = true)
        : ObjectEditor(objects, collections, parent), circles(isCircle)
    {
        setObjectName(circles ? QStringLiteral("shapesEditor") : QStringLiteral("linesEditor"));
    }
    QString preferenceId() const override
    {
        return circles ? QStringLiteral("org.swak.shapes") : QStringLiteral("org.swak.lines");
    }
    QString preferenceFallbackId() const override
    {
        return QStringLiteral("org.swak.shapes");
    }
    QString settingsTitle() const override
    {
        return circles ? tr("Circles") : tr("Lines");
    }
    QList<SettingField> settingsFields() const override
    {
        QList<SettingField> fields;
        if (circles) {
            fields.append(keySetting(QStringLiteral("circleKey"), tr("Create circle")));
        } else {
            fields.append(keySetting(QStringLiteral("lineKey"), tr("Create line")));
            fields.append(keySetting(QStringLiteral("distanceKey"), tr("Measure distance")));
            fields.append(numberSetting(
                QStringLiteral("holdSeconds"), tr("Measurement duration (seconds)"), 1, 60));
            fields.append(numberSetting(
                QStringLiteral("fadeMs"), tr("Measurement fade (milliseconds)"), 100, 5000));
        }
        fields.append(
            numberSetting(QStringLiteral("lineWidth"), tr("Outline width (pixels)"), 1, 6));
        fields.append(
            numberSetting(QStringLiteral("radius"), tr("Control point radius (pixels)"), 1, 6));
        return fields;
    }

    QJsonObject savePreferences() const override
    {
        auto values = editorState();
        const QStringList names{
            QStringLiteral("circleKey"), QStringLiteral("distanceKey"), QStringLiteral("lineKey")};
        for (int i = 0; i < 3; ++i) {
            values.insert(names[i], QKeySequence(keys[i]).toString(QKeySequence::PortableText));
        }
        values.insert(QStringLiteral("shapeShortcutVersion"), 1);
        values.insert(QStringLiteral("holdSeconds"), holdSeconds);
        values.insert(QStringLiteral("fadeMs"), fadeMs);
        values.insert(QStringLiteral("lineWidth"), width);
        values.insert(QStringLiteral("radius"), radius);
        return values;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        QSet<int> used;
        for (const auto &name : {QStringLiteral("circleKey"),
                                 QStringLiteral("distanceKey"),
                                 QStringLiteral("lineKey")}) {
            if (circles != (name == QStringLiteral("circleKey"))) {
                continue;
            }
            const auto sequence =
                QKeySequence::fromString(values.value(name).toString(), QKeySequence::PortableText);
            if (sequence.count() != 1 ||
                (sequence[0].keyboardModifiers() != Qt::NoModifier &&
                 sequence[0].keyboardModifiers() != Qt::ShiftModifier) ||
                sequence[0].key() < Qt::Key_A || sequence[0].key() > Qt::Key_Z ||
                used.contains(sequence[0].toCombined())) {
                return tr("Use different letters, with optional Shift.");
            }
            used.insert(sequence[0].toCombined());
        }
        return {};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        auto merged = savePreferences();
        for (auto it = values.begin(); it != values.end(); ++it) {
            merged.insert(it.key(), it.value());
        }
        if (values.value(QStringLiteral("shapeShortcutVersion")).toInt() < 1) {
            const QStringList names{QStringLiteral("circleKey"),
                                    QStringLiteral("distanceKey"),
                                    QStringLiteral("lineKey")};
            const QStringList defaults{
                QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("L")};
            for (int i = 0; i < names.size(); ++i) {
                if (merged.value(names[i]).toString() == defaults[i]) {
                    merged.insert(names[i], QStringLiteral("Shift+") + defaults[i]);
                }
            }
        }
        if (validateSettings(merged).isEmpty()) {
            const QStringList names{QStringLiteral("circleKey"),
                                    QStringLiteral("distanceKey"),
                                    QStringLiteral("lineKey")};
            for (int i = 0; i < 3; ++i) {
                keys[i] = QKeySequence::fromString(merged.value(names[i]).toString(),
                                                   QKeySequence::PortableText)[0]
                              .toCombined();
            }
        }
        holdSeconds = preferenceInteger(values, QStringLiteral("holdSeconds"), 10, 1, 60);
        fadeMs = preferenceInteger(values, QStringLiteral("fadeMs"), 1000, 100, 5000);
        width = preferenceNumber(values, QStringLiteral("lineWidth"), 2, 1, 6);
        radius = preferenceNumber(values, QStringLiteral("radius"), 3, 1, 6);
        restoreEditorState(values);
        emit configurationChanged();
    }
    const bool circles;
    std::array<int, 3> keys{QKeyCombination(Qt::ShiftModifier, Qt::Key_C).toCombined(),
                            QKeyCombination(Qt::ShiftModifier, Qt::Key_D).toCombined(),
                            QKeyCombination(Qt::ShiftModifier, Qt::Key_L).toCombined()};
    int holdSeconds = 10, fadeMs = 1000;
    double width = 2, radius = 3;
};
class ShapesTool final : public MapTool
{
public:
    ShapesTool(PluginServices &services, ShapesEditor &editor)
        : MapTool(&editor), objects_(*services.objects), selection_(*services.selection),
          tools_(*services.mapTools), store_(services.store), editor_(editor),
          mode_(editor.circles ? ShapeMode::Circle : ShapeMode::Line)
    {
        setObjectName(editor.circles ? QStringLiteral("shapesTool") : QStringLiteral("linesTool"));
        clock_.start();
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] {
            const auto now = clock_.elapsed();
            measurements_.removeIf([&](const Measurement &m) {
                return now - m.time >= editor_.holdSeconds * 1000 + editor_.fadeMs;
            });
            if (measurements_.isEmpty()) {
                timer_.stop();
            }
            emit changed();
        });
        connect(&editor_, &ObjectEditor::applied, this, [this](const QString &id) {
            reset();
            select(id);
            if (map_) {
                map_->setFocus();
            }
            emit changed();
        });
        connect(&editor_, &ObjectEditor::cancelled, this, [this] {
            reset();
            emit changed();
        });
        connect(&editor_, &ObjectEditor::configurationChanged, this, [this] {
            emit changed();
            emit overlayChanged();
        });
        connect(&objects_, &UserObjects::changed, this, [this] {
            if (selection_.item() && selection_.item()->owner == owner()) {
                const auto id = selection_.item()->id;
                if (const auto item = objects_.find(id)) {
                    if (supported(*item)) {
                        select(id);
                    }
                } else {
                    selection_.clear();
                }
            }
            emit itemsChanged();
            emit changed();
        });
        connect(&objects_, &UserObjects::restored, this, [this](const QString &id) {
            const auto item = objects_.find(id);
            if (item && supported(*item)) {
                cancel();
                select(id);
            }
        });
    }
    void request(ShapeMode mode)
    {
        if (interactionActive() && mode_ == mode) {
            cancel();
        } else {
            mode_ = mode;
            emit activationRequested();
        }
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
        return drawing_ || moving_.has_value() || movingPoint_ >= 0 || editor_.draft().has_value();
    }
    bool activeMode(ShapeMode mode) const
    {
        return mode == mode_ && guidesActive();
    }
    bool guidesActive() const override
    {
        return drawing_ || moving_.has_value() || movingPoint_ >= 0;
    }
    QString statusText() const override
    {
        return guidesActive() ? QKeySequence(editor_.keys[static_cast<size_t>(mode_)])
                                    .toString(QKeySequence::NativeText)
                              : QString{};
    }
    bool cancel() override
    {
        const bool active = interactionActive();
        reset();
        editor_.cancel();
        if (active) {
            emit changed();
        }
        return active;
    }
    bool dismiss() override
    {
        if (movingPoint_ >= 0) {
            movingPoint_ = -1;
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
        const auto selected = objects_.find(selection_.item()->id);
        if (!selected || !supported(*selected)) {
            return false;
        }
        deleteObject(objects_, selection_.item()->id, &editor_);
        return true;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &map) override
    {
        view_ = view;
        map_ = &map;
        if (event.type() == QEvent::KeyPress) {
            auto &key = static_cast<QKeyEvent &>(event);
            for (size_t i = 0; i < 3; ++i) {
                if ((editor_.circles == (i == 0)) &&
                    key.keyCombination().toCombined() == editor_.keys[i]) {
                    if (!key.isAutoRepeat()) {
                        request(static_cast<ShapeMode>(i));
                    }
                    return true;
                }
            }
            if (key.modifiers() != Qt::NoModifier) {
                return false;
            }
            if (key.key() == Qt::Key_Escape) {
                return dismiss();
            }
            if (key.key() == Qt::Key_Delete || key.key() == Qt::Key_Backspace) {
                return key.isAutoRepeat() || deleteSelection();
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
        if (event.type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
            pointer_ = static_cast<QMouseEvent &>(event).position();
            const auto point = world(pointer_, view);
            if (drawing_) {
                if (!QRectF(0, 0, 1, 1).contains(point)) {
                    return true;
                }
                if (!first_) {
                    first_ = point;
                    emit overlayChanged();
                    return true;
                }
                const QPolygonF points{*first_, point};
                const auto geometry = polygonGeometry(points);
                const auto error = validateTwoPointShape(shapeType(mode_), geometry);
                if (!error.isEmpty()) {
                    editor_.reportError(error);
                    return true;
                }
                if (mode_ == ShapeMode::Distance) {
                    if (measurements_.size() >= 64) {
                        measurements_.removeFirst();
                    }
                    measurements_.append({points, clock_.elapsed()});
                    reset();
                    timer_.start();
                } else {
                    UserObject item;
                    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    item.type = shapeType(mode_);
                    item.title = mode_ == ShapeMode::Circle ? tr("New Circle") : tr("New Line");
                    item.color = QStringLiteral("#1e88e5");
                    item.geometry = geometry;
                    reset();
                    editor_.edit(item);
                }
                emit changed();
                return true;
            }
            if (moving_) {
                auto item = *moving_;
                item.geometry = polygonGeometry(movedPoints(point));
                const auto error = objects_.put(item);
                if (error.isEmpty()) {
                    reset();
                    select(item.id);
                    emit changed();
                } else {
                    editor_.reportError(error);
                }
                return true;
            }
            if (movingPoint_ >= 0 && editor_.draft()) {
                if (editor_.setGeometry(polygonGeometry(editedPoints(point)))) {
                    movingPoint_ = -1;
                    map.unsetCursor();
                    emit changed();
                }
                return true;
            }
            if (editor_.draft()) {
                return true;
            }
        }
        if (event.type() == QEvent::ContextMenu && interactionActive()) {
            if (editor_.draft() && movingPoint_ < 0) {
                controlMenu(static_cast<QContextMenuEvent &>(event), view, map);
            }
            return true;
        }
        return false;
    }
    QList<MapItem> itemsAt(const Viewport &view, QPointF pointer) const override
    {
        QList<MapItem> result;
        for (const auto &item : objects_.items()) {
            if (supported(item)) {
                const auto points = polygonVertices(item.geometry);
                bool hit = false;
                if (item.type == QStringLiteral("range-circle")) {
                    hit = QLineF(screen(points[0], view), pointer).length() <=
                          QLineF(points[0], points[1]).length() * view.scale();
                } else {
                    for (int wrap = -1; wrap <= 1; ++wrap) {
                        const auto offset = QPointF(wrap * view.scale(), 0);
                        const auto a = screen(points[0], view) + offset,
                                   b = screen(lineEnd(points[0], points[1]), view) + offset,
                                   d = b - a;
                        const auto length = QPointF::dotProduct(d, d);
                        const auto t =
                            length > 0
                                ? std::clamp(QPointF::dotProduct(pointer - a, d) / length, 0.0, 1.0)
                                : 0;
                        hit = hit || QLineF(pointer, a + t * d).length() <= 7;
                    }
                }
                const QFontMetrics metrics(QGuiApplication::font());
                const auto a = screen(points[0], view);
                if (item.type == QStringLiteral("range-circle")) {
                    const auto title = metrics.elidedText(item.title, Qt::ElideRight, 240);
                    hit = hit || mapLabelBounds(metrics,
                                                a + QPointF(-metrics.horizontalAdvance(title) / 2.0,
                                                            metrics.ascent() / 2.0),
                                                item.title)
                                     .contains(pointer);
                } else {
                    for (int wrap = -1; wrap <= 1; ++wrap) {
                        const auto baseline =
                            (a + screen(lineEnd(points[0], points[1]), view)) / 2 +
                            QPointF(wrap * view.scale() + 4, -8);
                        hit =
                            hit || mapLabelBounds(metrics, baseline, lineLabel(points, item.title))
                                       .contains(pointer);
                    }
                }
                if (hit) {
                    result.append(mapItem(item));
                }
            }
        }
        return result;
    }
    bool interact(const MapItem &item, QEvent &event, QWidget &map) override
    {
        if (item.owner != owner()) {
            return false;
        }
        const auto record = objects_.find(item.id);
        if (!record || !supported(*record)) {
            return false;
        }
        if (event.type() == QEvent::MouseButtonRelease) {
            editor_.cancel();
            select(item.id);
            return true;
        }
        if (event.type() != QEvent::ContextMenu) {
            return false;
        }
        tools_.cancel();
        map_ = &map;
        select(item.id);
        const auto context = static_cast<QContextMenuEvent &>(event).globalPos();
        Modal<QMenu> menu(&map);
        menu->setObjectName(QStringLiteral("shapeContextMenu"));
        auto *edit = menu->addAction(uiIcon(UiIcon::Edit), tr("Edit"));
        edit->setObjectName(QStringLiteral("editShape"));
        auto *move = menu->addAction(uiIcon(UiIcon::Move), tr("Move"));
        move->setObjectName(QStringLiteral("moveShape"));
        auto *remove = menu->addAction(uiIcon(UiIcon::Delete), tr("Delete ..."));
        remove->setObjectName(QStringLiteral("deleteShape"));
        edit->setProperty("menuOrder", -2);
        remove->setProperty("menuOrder", -1);
        sortMenuActions(*menu);
        connect(edit, &QAction::triggered, this, [this, id = item.id] {
            if (const auto found = objects_.find(id)) {
                selection_.clear();
                mode_ = editor_.circles ? ShapeMode::Circle : ShapeMode::Line;
                editor_.edit(*found);
                emit changed();
            }
        });
        connect(move, &QAction::triggered, this, [this, id = item.id, context] {
            moving_ = objects_.find(id);
            anchor_ = world(map_->mapFromGlobal(context), view_);
            if (moving_) {
                mode_ = moving_->type == QStringLiteral("range-circle") ? ShapeMode::Circle
                                                                        : ShapeMode::Line;
                selection_.clear();
                map_->setCursor(Qt::CrossCursor);
                map_->setFocus();
                emit changed();
            }
        });
        connect(remove, &QAction::triggered, this, [this, id = item.id] {
            deleteObject(objects_, id, &editor_);
        });
        menu->exec(context);
        return true;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        for (const auto &item : objects_.items()) {
            if (supported(item) && (!moving_ || moving_->id != item.id) &&
                (!editor_.draft() || editor_.draft()->id != item.id)) {
                draw(painter,
                     polygonVertices(item.geometry),
                     item.type,
                     QColor(item.color),
                     item.title,
                     view,
                     selection_.item() && selection_.item()->owner == owner() &&
                         selection_.item()->id == item.id,
                     item.restricted);
            }
        }
        if (editor_.draft() && movingPoint_ < 0) {
            draw(painter,
                 polygonVertices(editor_.draft()->geometry),
                 editor_.draft()->type,
                 QColor(editor_.draft()->color),
                 editor_.draft()->title,
                 view,
                 false,
                 editor_.draft()->restricted);
        }
        const auto now = clock_.elapsed();
        for (const auto &measurement : measurements_) {
            painter.save();
            painter.setOpacity(std::clamp(
                1.0 - static_cast<double>(now - measurement.time - editor_.holdSeconds * 1000) /
                          editor_.fadeMs,
                0.0,
                1.0));
            draw(painter, measurement.points, QStringLiteral("line"), QColor("#1e88e5"), {}, view);
            painter.restore();
        }
    }
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const override
    {
        const auto point = world(pointer, view);
        if (drawing_ && first_) {
            const QPolygonF points{*first_, point};
            draw(painter, points, shapeType(mode_), QColor("#1e88e5"), {}, view);
            dots(painter, points, QColor("#1e88e5"), view);
            if (mode_ == ShapeMode::Circle) {
                paintMapLabel(painter,
                              (screen(*first_, view) + screen(point, view)) / 2 + QPointF(4, -8),
                              tr("Radius: %1").arg(formatDistance(groundDistance(*first_, point))));
            }
        }
        if (moving_) {
            draw(painter,
                 movedPoints(point),
                 moving_->type,
                 QColor(moving_->color),
                 moving_->title,
                 view,
                 false,
                 moving_->restricted);
        }
        if (editor_.draft()) {
            const auto points = movingPoint_ >= 0 ? editedPoints(point)
                                                  : polygonVertices(editor_.draft()->geometry);
            if (movingPoint_ >= 0) {
                draw(painter,
                     points,
                     editor_.draft()->type,
                     QColor(editor_.draft()->color),
                     editor_.draft()->title,
                     view,
                     false,
                     editor_.draft()->restricted);
            }
            dots(painter, points, QColor(editor_.draft()->color), view);
        }
        if (guidesActive()) {
            painter.setPen(QPen(QColor(0, 0, 0, 128), 1));
            painter.drawLine(QPointF(0, pointer.y()), QPointF(view.size.width(), pointer.y()));
            painter.drawLine(QPointF(pointer.x(), 0), QPointF(pointer.x(), view.size.height()));
        }
    }

private:
    struct Measurement
    {
        QPolygonF points;
        qint64 time = 0;
    };
    static QString owner()
    {
        return QStringLiteral("org.swak.shapes");
    }
    bool supported(const UserObject &item) const
    {
        return item.type ==
               (editor_.circles ? QStringLiteral("range-circle") : QStringLiteral("line"));
    }
    void reset()
    {
        drawing_ = false;
        first_.reset();
        moving_.reset();
        movingPoint_ = -1;
        if (map_) {
            map_->unsetCursor();
        }
    }
    QPolygonF movedPoints(QPointF point) const
    {
        auto points = polygonVertices(moving_->geometry);
        points.translate(point - anchor_);
        if (moving_->type == QStringLiteral("line")) {
            std::transform(points.begin(), points.end(), points.begin(), [](QPointF p) {
                p.setX(p.x() - std::floor(p.x()));
                return p;
            });
        }
        return points;
    }
    QPolygonF editedPoints(QPointF point) const
    {
        auto points = polygonVertices(editor_.draft()->geometry);
        if (movingPoint_ == 0 && editor_.draft()->type == QStringLiteral("range-circle")) {
            points[1] += point - points[0];
        }
        points[movingPoint_] = point;
        return points;
    }
    void dots(QPainter &painter, const QPolygonF &points, QColor color, const Viewport &view) const
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (const auto &point : points) {
            painter.drawEllipse(screen(point, view), editor_.radius, editor_.radius);
        }
    }
    static QString lineLabel(const QPolygonF &points, const QString &title)
    {
        const auto distance = formatDistance(groundDistance(points[0], points[1]));
        return title.isEmpty() ? distance : title + QStringLiteral(" · ") + distance;
    }
    void draw(QPainter &painter,
              const QPolygonF &points,
              const QString &type,
              QColor color,
              const QString &title,
              const Viewport &view,
              bool selected = false,
              bool restricted = false) const
    {
        if (points.size() != 2) {
            return;
        }
        const auto a = screen(points[0], view), b = screen(points[1], view);
        painter.setPen(QPen(color, editor_.width));
        painter.setBrush(Qt::NoBrush);
        if (type == QStringLiteral("range-circle")) {
            auto fill = color;
            fill.setAlphaF(static_cast<float>(tools_.areaFillOpacity()));
            painter.setBrush(fill);
            const auto radius = QLineF(a, b).length();
            painter.drawEllipse(a, radius, radius);
            if (restricted) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QBrush(color, Qt::BDiagPattern));
                painter.drawEllipse(a, radius, radius);
            }
            const auto label = painter.fontMetrics().elidedText(title, Qt::ElideRight, 240);
            paintMapLabel(painter,
                          a + QPointF(-painter.fontMetrics().horizontalAdvance(label) / 2.0,
                                      painter.fontMetrics().ascent() / 2.0),
                          label,
                          selected);
        } else {
            for (int wrap = -1; wrap <= 1; ++wrap) {
                const auto offset = QPointF(wrap * view.scale(), 0);
                const auto end = screen(lineEnd(points[0], points[1]), view);
                painter.drawLine(a + offset, end + offset);
                paintMapLabel(painter,
                              (a + end) / 2 + offset + QPointF(4, -8),
                              lineLabel(points, title),
                              selected);
            }
        }
    }
    MapItem mapItem(const UserObject &record) const
    {
        const auto points = polygonVertices(record.geometry);
        MapItem item;
        item.owner = owner();
        item.id = record.id;
        item.label = record.title;
        item.description = record.description;
        item.color = QColor(record.color);
        item.lineWidth = editor_.width;
        item.outlineStyle = Qt::SolidLine;
        item.properties = record.properties;
        item.properties.insert(QStringLiteral("ID"), record.id);
        if (record.type == QStringLiteral("range-circle")) {
            item.kind = MapItem::Kind::Polygon;
            item.icon = uiIcon(UiIcon::Circle);
            item.type = tr("Circle");
            item.properties.insert(tr("Restricted"), record.restricted ? tr("Yes") : tr("No"));
            const auto boundary = circleBoundary(points[0], points[1]);
            const auto radius = QLineF(points[0], points[1]).length();
            item.path.addEllipse(points[0], radius, radius);
            item.properties.insert(QStringLiteral("Area"), formatArea(polygonArea(boundary)));
        } else {
            item.kind = MapItem::Kind::Line;
            item.icon = uiIcon(UiIcon::Line);
            item.type = tr("Line");
            item.path.moveTo(points[0]);
            item.path.lineTo(lineEnd(points[0], points[1]));
            item.properties.insert(QStringLiteral("Distance"),
                                   formatDistance(groundDistance(points[0], points[1])));
        }
        return item;
    }
    void select(const QString &id)
    {
        if (const auto item = objects_.find(id); item && supported(*item)) {
            store_.select({});
            selection_.select(mapItem(*item));
        }
    }
    void controlMenu(QContextMenuEvent &event, const Viewport &view, QWidget &map)
    {
        const auto points = polygonVertices(editor_.draft()->geometry);
        for (int i = 0; i < 2; ++i) {
            if (QLineF(screen(points[i], view), event.pos()).length() <= editor_.radius + 5) {
                Modal<QMenu> menu(&map);
                auto *move = menu->addAction(uiIcon(UiIcon::Move), tr("Move"));
                move->setObjectName(QStringLiteral("moveShapePoint"));
                auto *remove = menu->addAction(uiIcon(UiIcon::Delete), tr("Delete ..."));
                remove->setEnabled(false);
                remove->setToolTip(tr("This shape needs both control points."));
                connect(move, &QAction::triggered, this, [this, i] {
                    movingPoint_ = i;
                    map_->setCursor(Qt::CrossCursor);
                    map_->setFocus();
                    emit overlayChanged();
                });
                menu->exec(event.globalPos());
                return;
            }
        }
    }
    UserObjects &objects_;
    MapSelection &selection_;
    MapTools &tools_;
    Store &store_;
    ShapesEditor &editor_;
    ShapeMode mode_ = ShapeMode::Circle;
    bool drawing_ = false;
    std::optional<QPointF> first_;
    std::optional<UserObject> moving_;
    int movingPoint_ = -1;
    QPointF pointer_, anchor_;
    Viewport view_;
    QPointer<QWidget> map_;
    QList<Measurement> measurements_;
    QElapsedTimer clock_;
    QTimer timer_;
};
} // namespace
class ShapesPlugin final : public QObject,
                           public Plugin,
                           public PanePlugin,
                           public MenuPlugin,
                           public ToolbarPlugin,
                           public MapExtension
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "shapes/MANIFEST")
    Q_INTERFACES(
        swak::Plugin swak::PanePlugin swak::MenuPlugin swak::ToolbarPlugin swak::MapExtension)
public:
    QString name() const override
    {
        return tr("Circles");
    }
    QString mapId() const override
    {
        return QStringLiteral("org.swak.shapes");
    }
    QString mapName() const override
    {
        return name();
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return createEditor(services, parent, true);
    }
    QList<PluginPane> additionalPanes(PluginServices &services, QWidget *parent) override
    {
        return {{QStringLiteral("org.swak.lines.pane"),
                 tr("Lines"),
                 createEditor(services, parent, false)}};
    }
    QWidget *createEditor(PluginServices &services, QWidget *parent, bool circles)
    {
        if (!services.objects || !services.selection || !services.mapTools ||
            !services.collections) {
            return new QWidget(parent);
        }
        auto *editor = new ShapesEditor(*services.objects, *services.collections, parent, circles);
        auto *tool = new ShapesTool(services, *editor);
        services.mapTools->add(tool);
        editors_[services.mapTools].append(editor);
        connect(editor,
                &QObject::destroyed,
                this,
                [this, tools = services.mapTools, object = static_cast<QObject *>(editor)] {
                    auto &entries = editors_[tools];
                    entries.removeIf([object](const auto &entry) {
                        return entry.isNull() || entry.data() == object;
                    });
                    if (entries.isEmpty()) {
                        editors_.remove(tools);
                    }
                });
        const std::array<UiIcon, 3> icons{UiIcon::Circle, UiIcon::Distance, UiIcon::Line};
        for (int i = 0; i < 3; ++i) {
            if (circles != (i == 0)) {
                continue;
            }
            const auto mode = static_cast<ShapeMode>(i);
            auto *action =
                new QAction(uiIcon(icons[static_cast<size_t>(i)]), shapeName(mode), editor);
            action->setObjectName(QStringLiteral("shapeMode%1").arg(i));
            setToolbarSection(*action,
                              mode == ShapeMode::Distance ? ToolbarSection::Measurement
                                                          : ToolbarSection::Creation,
                              mode == ShapeMode::Circle ? 2 : 3);
            action->setCheckable(true);
            const auto sync = [action, editor, tool, mode, i] {
                action->setChecked(tool->activeMode(mode));
                action->setToolTip(
                    shapeName(mode) +
                    QStringLiteral(" (%1)").arg(QKeySequence(editor->keys[static_cast<size_t>(i)])
                                                    .toString(QKeySequence::NativeText)));
            };
            connect(tool, &MapTool::changed, action, sync);
            connect(editor, &ObjectEditor::configurationChanged, action, sync);
            sync();
            connect(action, &QAction::triggered, tool, [tool, mode] {
                tool->request(mode);
            });
        }
        QTimer::singleShot(0, editor, [editor] {
            emit editor->visibilityRequested(false);
        });
        return editor;
    }
    bool rootActions() const override
    {
        return true;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        for (const auto &editor : editors_.value(services.mapTools)) {
            addActions(editor, [&menu](QAction *action) {
                menu.addAction(action);
            });
        }
    }
    void populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *) override
    {
        for (const auto &editor : editors_.value(services.mapTools)) {
            addActions(editor, [&toolbar](QAction *action) {
                toolbar.addAction(action);
            });
        }
    }

private:
    QHash<MapTools *, QList<QPointer<QObject>>> editors_;
    static void addActions(QObject *root, const std::function<void(QAction *)> &add)
    {
        if (!root) {
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (auto *action = root->findChild<QAction *>(QStringLiteral("shapeMode%1").arg(i))) {
                add(action);
            }
        }
    }
};
} // namespace swak
#include "shapes.moc"
