#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
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
#include <QUuid>

#include <cmath>
#include <optional>
#include <utility>
namespace swak {
namespace {
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
    PolygonEditor(UserObjects &objects, MarkerCollections &collections, QWidget *parent)
        : ObjectEditor(objects, collections, parent)
    {
        setObjectName(QStringLiteral("polygonEditor"));
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.polygons");
    }
    QString settingsTitle() const override
    {
        return tr("Areas");
    }
    QList<SettingField> settingsFields() const override
    {
        return {keySetting(QStringLiteral("createKey"), tr("Create area")),
                numberSetting(QStringLiteral("vertexRadius"), tr("Corner radius (pixels)"), 1, 6),
                numberSetting(QStringLiteral("lineWidth"), tr("Outline width (pixels)"), 1, 6),
                numberSetting(QStringLiteral("fillOpacity"), tr("Fill opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("guideOpacity"), tr("Guide line opacity"), 0, 1, 2)};
    }
    QJsonObject savePreferences() const override
    {
        auto state = editorState();
        state.insert(QStringLiteral("createKey"),
                     QKeySequence(key_).toString(QKeySequence::PortableText));
        state.insert(QStringLiteral("areaShortcutVersion"), 2);
        state.insert(QStringLiteral("vertexRadius"), radius);
        state.insert(QStringLiteral("lineWidth"), lineWidth);
        state.insert(QStringLiteral("fillOpacity"), fillOpacity);
        state.insert(QStringLiteral("guideOpacity"), guideOpacity);
        return state;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        return readKey(values) ? QString{}
                               : tr("Use one letter without modifiers to create a polygon.");
    }
    void restorePreferences(const QJsonObject &values) override
    {
        key_ = readKey(values);
        if (!key_) {
            key_ = Qt::Key_A;
        }
        radius = preferenceNumber(values, QStringLiteral("vertexRadius"), 3, 1, 6);
        lineWidth = preferenceNumber(values, QStringLiteral("lineWidth"), 2, 1, 6);
        fillOpacity = preferenceNumber(values, QStringLiteral("fillOpacity"), 0.3, 0, 1);
        guideOpacity = preferenceNumber(values, QStringLiteral("guideOpacity"), 0.5, 0, 1);
        restoreEditorState(values);
        emit configurationChanged();
    }
    int createKey() const
    {
        return key_;
    }
    double radius = 3, lineWidth = 2, fillOpacity = 0.3, guideOpacity = 0.5;

private:
    static int readKey(const QJsonObject &values)
    {
        auto text = preferenceText(values, QStringLiteral("createKey"), QStringLiteral("A"), 32);
        if (values.value(QStringLiteral("areaShortcutVersion")).toInt() < 2 &&
            text == QStringLiteral("P")) {
            text = QStringLiteral("A");
        }
        const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (key.count() != 1 || key[0].keyboardModifiers() != Qt::NoModifier) {
            return 0;
        }
        return key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z
                   ? static_cast<int>(key[0].key())
                   : 0;
    }
    int key_ = Qt::Key_A;
};
class PolygonTool final : public MapTool
{
public:
    PolygonTool(PluginServices &services, PolygonEditor &editor)
        : MapTool(&editor), objects_(*services.objects), selection_(*services.selection),
          store_(services.store), tools_(*services.mapTools), editor_(editor)
    {
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
            if (item && item->type == QStringLiteral("polygon")) {
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
        return drawing_ || moving_ >= 0 || editor_.draft().has_value();
    }
    std::optional<double> areaFillOpacity() const override
    {
        return editor_.fillOpacity;
    }
    bool coordinateGridActive() const override
    {
        return drawing_ || moving_ >= 0;
    }
    QString statusText() const override
    {
        return drawing_ || moving_ >= 0 ? QStringLiteral("A") : QString{};
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
        objects_.remove(selection_.item()->id);
        return true;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &map) override
    {
        map_ = &map;
        if (event.type() == QEvent::KeyPress) {
            auto &key = static_cast<QKeyEvent &>(event);
            if (key.modifiers() != Qt::NoModifier) {
                return false;
            }
            if (key.key() == editor_.createKey()) {
                if (!key.isAutoRepeat()) {
                    if (interactionActive()) {
                        cancel();
                    } else {
                        emit activationRequested();
                    }
                }
                return true;
            }
            if (key.key() == Qt::Key_Escape) {
                if (moving_ >= 0) {
                    moving_ = -1;
                    map.unsetCursor();
                    emit changed();
                    return true;
                }
                return cancel();
            }
            if (drawing_ && (key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter)) {
                if (!key.isAutoRepeat() && points_.size() >= 2) {
                    auto candidate = points_;
                    if (candidate.size() == 2) {
                        candidate.append(worldPoint(pointer_, view));
                    }
                    complete(candidate);
                }
                return true;
            }
            if (key.key() == Qt::Key_Delete || key.key() == Qt::Key_Backspace) {
                return deleteSelection();
            }
        }
        if (event.type() == QEvent::MouseMove) {
            pointer_ = static_cast<QMouseEvent &>(event).position();
            if (drawing_ || moving_ >= 0) {
                emit overlayChanged();
                return true;
            }
        }
        if ((event.type() == QEvent::MouseButtonPress ||
             event.type() == QEvent::MouseButtonDblClick) &&
            (drawing_ || moving_ >= 0) &&
            static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
            return true;
        }
        if (event.type() == QEvent::MouseButtonRelease) {
            auto &mouse = static_cast<QMouseEvent &>(event);
            if (mouse.button() != Qt::LeftButton) {
                return false;
            }
            pointer_ = mouse.position();
            if (drawing_) {
                map.setCursor(Qt::CrossCursor);
                if (points_.size() >= 3 && near(screenPoint(points_.first(), view), pointer_)) {
                    complete(points_);
                } else {
                    const auto point = worldPoint(pointer_, view);
                    if (!QRectF(0, 0, 1, 1).contains(point)) {
                        return true;
                    }
                    if (points_.contains(point)) {
                        error(tr("Choose a different polygon corner."));
                        return true;
                    }
                    if (points_.size() >= MaxPolygonVertices) {
                        error(tr("Use at most 64 polygon corners."));
                        return true;
                    }
                    points_.append(point);
                    emit overlayChanged();
                }
                return true;
            }
            if (moving_ >= 0 && editor_.draft()) {
                auto candidate = polygonVertices(editor_.draft()->geometry);
                candidate[moving_] = worldPoint(pointer_, view);
                if (editor_.setGeometry(polygonGeometry(candidate))) {
                    moving_ = -1;
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
            if (drawing_ || moving_ >= 0) {
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
        if (!record || record->type != QStringLiteral("polygon")) {
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
        QMenu menu(&map);
        menu.setObjectName(QStringLiteral("polygonContextMenu"));
        auto *edit = menu.addAction(uiIcon(UiIcon::Edit), tr("Edit"));
        edit->setObjectName(QStringLiteral("editPolygon"));
        auto *remove = menu.addAction(uiIcon(UiIcon::Clear), tr("Delete"));
        remove->setObjectName(QStringLiteral("deletePolygon"));
        connect(edit, &QAction::triggered, this, [this, id] {
            const auto currentObject = objects_.find(id);
            if (currentObject) {
                selection_.clear();
                editor_.edit(*currentObject);
                editingVertices_ = true;
                emit changed();
            }
        });
        connect(remove, &QAction::triggered, this, [this, id] {
            objects_.remove(id);
        });
        tools_.populateContextMenu(item, menu);
        menu.exec(context.globalPos());
        return true;
    }
    QList<MapItem> itemsAt(const Viewport &view, QPointF pointer) const override
    {
        QList<MapItem> result;
        for (const auto &record : objects_.items()) {
            if (record.type != QStringLiteral("polygon")) {
                continue;
            }
            const QFontMetrics metrics(QGuiApplication::font());
            const auto text = metrics.elidedText(record.title, Qt::ElideRight, 240);
            const auto baseline =
                screenPoint(polygonLabelPoint(polygonVertices(record.geometry)), view) +
                QPointF(-metrics.horizontalAdvance(text) / 2.0, metrics.ascent() / 2.0);
            if (!polygonVertices(record.geometry)
                     .containsPoint(worldPoint(pointer, view), Qt::OddEvenFill) &&
                !mapLabelBounds(metrics, baseline, record.title).contains(pointer)) {
                continue;
            }
            MapItem item;
            item.owner = owner();
            item.id = record.id;
            item.label = record.title;
            item.type = tr("Area");
            item.kind = MapItem::Kind::Polygon;
            result.append(std::move(item));
        }
        return result;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        for (const auto &item : objects_.items()) {
            if (item.type != QStringLiteral("polygon") ||
                (editor_.draft() && editor_.draft()->id == item.id)) {
                continue;
            }
            const auto points = polygonVertices(item.geometry);
            draw(painter, screenPolygon(points, view), QColor(item.color));
            const auto text = painter.fontMetrics().elidedText(item.title, Qt::ElideRight, 240);
            const auto baseline = screenPoint(polygonLabelPoint(points), view) +
                                  QPointF(-painter.fontMetrics().horizontalAdvance(text) / 2.0,
                                          painter.fontMetrics().ascent() / 2.0);
            paintMapLabel(painter,
                          baseline,
                          item.title,
                          selection_.item() && selection_.item()->owner == owner() &&
                              selection_.item()->id == item.id);
        }
        if (editor_.draft() && moving_ < 0) {
            draw(painter,
                 screenPolygon(polygonVertices(editor_.draft()->geometry), view),
                 QColor(editor_.draft()->color));
        }
    }
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const override
    {
        if (drawing_) {
            auto polygon = screenPolygon(points_, view);
            polygon.append(pointer);
            draw(painter, polygon, QColor("#1e88e5"));
            dots(painter, screenPolygon(points_, view), QColor("#1e88e5"));
        } else if (editor_.draft() && editingVertices_) {
            auto polygon = screenPolygon(polygonVertices(editor_.draft()->geometry), view);
            if (moving_ >= 0) {
                polygon[moving_] = pointer;
                draw(painter, polygon, QColor(editor_.draft()->color));
            }
            dots(painter, polygon, QColor(editor_.draft()->color));
        }
        if (drawing_ || moving_ >= 0) {
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(QColor(0, 0, 0, qRound(editor_.guideOpacity * 255)), 1));
            painter.drawLine(QPointF(0, pointer.y()), QPointF(view.size.width(), pointer.y()));
            painter.drawLine(QPointF(pointer.x(), 0), QPointF(pointer.x(), view.size.height()));
        }
    }

private:
    static QString owner()
    {
        return QStringLiteral("org.swak.polygons");
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
        points_.clear();
        if (map_) {
            map_->unsetCursor();
        }
    }
    void error(const QString &message)
    {
        editor_.reportError(message);
        logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.polygons"), message);
    }
    void complete(const QPolygonF &candidate)
    {
        const auto problem = validatePolygon(candidate);
        if (!problem.isEmpty()) {
            error(problem);
            return;
        }
        UserObject item;
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.type = QStringLiteral("polygon");
        item.title = tr("New Area");
        item.color = QStringLiteral("#1e88e5");
        item.geometry = polygonGeometry(candidate);
        resetPointer();
        editor_.edit(item);
        emit changed();
    }
    void draw(QPainter &painter, const QPolygonF &points, const QColor &color) const
    {
        auto fill = color;
        fill.setAlpha(qRound(editor_.fillOpacity * 255));
        painter.setPen(QPen(color, editor_.lineWidth));
        painter.setBrush(points.size() >= 3 ? QBrush(fill) : Qt::NoBrush);
        if (points.size() >= 3) {
            painter.drawPolygon(points);
        } else {
            painter.drawPolyline(points);
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
        const auto points = polygonVertices(editor_.draft()->geometry);
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
        QMenu menu(&map);
        menu.setObjectName(QStringLiteral("polygonVertexMenu"));
        auto *move = menu.addAction(uiIcon(UiIcon::Move), tr("Move"));
        move->setObjectName(QStringLiteral("movePolygonVertex"));
        auto *remove = menu.addAction(uiIcon(UiIcon::Clear), tr("Delete"));
        remove->setObjectName(QStringLiteral("deletePolygonVertex"));
        remove->setEnabled(validatePolygon(reduced).isEmpty());
        connect(move, &QAction::triggered, this, [this, index, &map] {
            if (!editor_.draft() || index >= polygonVertices(editor_.draft()->geometry).size()) {
                return;
            }
            moving_ = index;
            map.setCursor(Qt::CrossCursor);
            map.setFocus();
            emit changed();
        });
        connect(remove, &QAction::triggered, this, [this, reduced] {
            editor_.setGeometry(polygonGeometry(reduced));
        });
        menu.exec(context.globalPos());
    }
    void select(const QString &id)
    {
        const auto record = objects_.find(id);
        if (!record || record->type != QStringLiteral("polygon")) {
            return;
        }
        const auto points = polygonVertices(record->geometry);
        MapItem item;
        item.kind = MapItem::Kind::Polygon;
        item.owner = owner();
        item.id = id;
        item.label = record->title;
        item.type = tr("Area");
        item.description = record->description;
        item.color = QColor(record->color);
        item.lineWidth = editor_.lineWidth;
        item.outlineStyle = Qt::SolidLine;
        item.path.addPolygon(points);
        item.path.closeSubpath();
        item.properties = record->properties;
        item.properties.insert(QStringLiteral("ID"), id);
        item.properties.insert(QStringLiteral("Area"), formatArea(polygonArea(points)));
        store_.select({});
        selection_.select(std::move(item));
    }
    bool drawing_ = false;
    bool editingVertices_ = false;
    int moving_ = -1;
    QPolygonF points_;
    QPointF pointer_;
    QPointer<QWidget> map_;
    UserObjects &objects_;
    MapSelection &selection_;
    Store &store_;
    MapTools &tools_;
    PolygonEditor &editor_;
};
} // namespace
class PolygonPlugin final : public QObject, public Plugin, public PanePlugin, public MapExtension
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "polygons/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MapExtension)
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
        if (!services.objects || !services.selection || !services.mapTools ||
            !services.collections) {
            return new QWidget(parent);
        }
        auto *editor = new PolygonEditor(*services.objects, *services.collections, parent);
        auto *tool = new PolygonTool(services, *editor);
        services.mapTools->add(tool);
        auto *action = new QAction(uiIcon(UiIcon::Area), tr("Area"), editor);
        action->setObjectName(QStringLiteral("polygonModeAction"));
        setToolbarSection(*action, ToolbarSection::Creation, 1);
        action->setCheckable(true);
        const auto updateAction = [action, editor, tool] {
            action->setChecked(!tool->statusText().isEmpty());
            action->setToolTip(
                tr("Area mode (%1)")
                    .arg(QKeySequence(editor->createKey()).toString(QKeySequence::NativeText)));
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
};
} // namespace swak
#include "polygons.moc"
