#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/marker_collections.h"
#include "core/marker_symbols.h"
#include "core/menu_order.h"
#include "core/object_editor.h"
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
#include <utility>
namespace swak {
namespace {
QPointF location(const UserObject &item)
{
    return {item.geometry.value(QStringLiteral("x")).toDouble(),
            item.geometry.value(QStringLiteral("y")).toDouble()};
}
void setLocation(UserObject &item, QPointF point)
{
    item.geometry = {{QStringLiteral("x"), point.x()}, {QStringLiteral("y"), point.y()}};
}
QPointF screen(QPointF point, const Viewport &view)
{
    return (point - view.center) * view.scale() +
           QPointF(view.size.width() / 2, view.size.height() / 2);
}
class MarkerEditor final : public ObjectEditor, public SettingsParticipant
{
public:
    MarkerEditor(UserObjects &objects, MarkerCollections &collections, QWidget *parent)
        : ObjectEditor(objects, collections, parent)
    {
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.markers");
    }
    QString settingsTitle() const override
    {
        return tr("Markers");
    }
    QList<SettingField> settingsFields() const override
    {
        return {keySetting(QStringLiteral("createKey"), tr("Create marker")),
                numberSetting(QStringLiteral("radius"), tr("Marker radius (pixels)"), 3, 24),
                textSetting(QStringLiteral("color"), tr("Default marker color (#RRGGBB)"), 7),
                numberSetting(QStringLiteral("guideOpacity"), tr("Guide line opacity"), 0, 1, 2)};
    }
    QJsonObject savePreferences() const override
    {
        auto state = editorState();
        state.insert(QStringLiteral("guideStyleVersion"), 2);
        state.insert(QStringLiteral("createKey"),
                     QKeySequence(key_).toString(QKeySequence::PortableText));
        state.insert(QStringLiteral("radius"), radius_);
        state.insert(QStringLiteral("color"), color_.name());
        state.insert(QStringLiteral("guideOpacity"), guideOpacity_);
        return state;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        if (!readKey(values)) {
            return tr("Use one letter without modifiers to create a marker.");
        }
        const auto color =
            values.value(QStringLiteral("color")).toString(QStringLiteral("#ffc857"));
        if (color.size() != 7 || !color.startsWith('#') || !QColor::isValidColorName(color)) {
            return tr("Use a color in #RRGGBB format.");
        }
        return {};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const int key = readKey(values);
        key_ = key ? key : Qt::Key_M;
        radius_ = preferenceNumber(values, QStringLiteral("radius"), 7, 3, 24);
        guideOpacity_ = preferenceNumber(values, QStringLiteral("guideOpacity"), 0.5, 0, 1);
        if (!values.contains(QStringLiteral("guideStyleVersion")) && guideOpacity_ == 0.6) {
            guideOpacity_ = 0.5;
        }
        const auto color =
            preferenceText(values, QStringLiteral("color"), QStringLiteral("#ffc857"), 7);
        color_ = QColor::isValidColorName(color) ? QColor(color) : QColor("#ffc857");
        restoreEditorState(values);
        emit configurationChanged();
    }
    double radius() const
    {
        return radius_;
    }
    QColor color() const
    {
        return color_;
    }
    double guideOpacity() const
    {
        return guideOpacity_;
    }
    int createKey() const
    {
        return key_;
    }

private:
    static int readKey(const QJsonObject &values)
    {
        const auto text = values.value(QStringLiteral("createKey")).toString(QStringLiteral("M"));
        if (text.size() > 32) {
            return 0;
        }
        const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (key.count() != 1 || key[0].keyboardModifiers() != Qt::NoModifier) {
            return 0;
        }
        return key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z
                   ? static_cast<int>(key[0].key())
                   : 0;
    }
    int key_ = Qt::Key_M;
    double radius_ = 7;
    QColor color_{"#ffc857"};
    double guideOpacity_ = 0.5;
};
class MarkerTool final : public MapTool
{
public:
    MarkerTool(PluginServices &services, MarkerEditor &editor)
        : MapTool(&editor), objects_(*services.objects), selection_(*services.selection),
          store_(services.store), editor_(editor), collections_(*services.collections),
          tools_(services.mapTools)
    {
        connect(&collections_, &MarkerCollections::changed, this, [this] {
            if (selection_.item() && selection_.item()->owner == owner()) {
                select(selection_.item()->id);
            }
            emit changed();
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
            if (mode_ == Mode::Moving && !objects_.find(moving_.id)) {
                cancel();
            }
            emit changed();
        });
        connect(&objects_, &UserObjects::restored, this, [this](const QString &id) {
            const auto item = objects_.find(id);
            if (item && item->type == QStringLiteral("circle")) {
                cancel();
                select(id);
            }
        });
        connect(&editor_, &ObjectEditor::applied, this, [this](const QString &id) {
            select(id);
            if (map_ && map_->isVisible()) {
                map_->window()->activateWindow();
                map_->setFocus(Qt::OtherFocusReason);
            }
            emit changed();
        });
        connect(&editor_, &ObjectEditor::configurationChanged, this, [this] {
            emit overlayChanged();
            emit itemsChanged();
            if (selection_.item() && selection_.item()->owner == owner()) {
                select(selection_.item()->id);
            }
            emit changed();
        });
        connect(&editor_, &ObjectEditor::cancelled, this, [this] {
            emit changed();
        });
    }
    void activate(QWidget &map) override
    {
        cancel();
        map_ = &map;
        mode_ = Mode::Placing;
        selection_.clear();
        store_.select({});
        pointer_ = map.mapFromGlobal(QCursor::pos());
        map.setCursor(Qt::CrossCursor);
        emit changed();
    }
    bool interactionActive() const override
    {
        return mode_ != Mode::Idle;
    }
    bool guidesActive() const override
    {
        return mode_ != Mode::Idle;
    }
    QString statusText() const override
    {
        return mode_ == Mode::Idle ? QString{} : QStringLiteral("M");
    }
    bool cancel() override
    {
        const bool active = mode_ != Mode::Idle;
        const auto restore = mode_ == Mode::Moving ? moving_.id : QString{};
        mode_ = Mode::Idle;
        if (active && map_) {
            map_->unsetCursor();
        }
        const bool editing = editor_.cancel();
        if (!restore.isEmpty()) {
            select(restore);
        }
        if (active || editing) {
            emit changed();
        }
        return active || editing;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &map) override
    {
        map_ = &map;
        if (event.type() == QEvent::KeyPress) {
            auto &key = static_cast<QKeyEvent &>(event);
            if (key.modifiers() != Qt::NoModifier) {
                return false;
            }
            if (key.key() == Qt::Key_Escape) {
                return cancel();
            }
            if (key.key() == editor_.createKey()) {
                if (!key.isAutoRepeat()) {
                    if (mode_ != Mode::Idle) {
                        cancel();
                        return true;
                    }
                    emit activationRequested();
                }
                return true;
            }
            if (mode_ == Mode::Idle &&
                (key.key() == Qt::Key_Delete || key.key() == Qt::Key_Backspace) &&
                selection_.item() && selection_.item()->owner == owner()) {
                return key.isAutoRepeat() || deleteSelection();
            }
        }
        if (event.type() == QEvent::MouseMove) {
            pointer_ = static_cast<QMouseEvent &>(event).position();
            if (mode_ != Mode::Idle) {
                emit overlayChanged();
                return true;
            }
        }
        if (event.type() == QEvent::MouseButtonPress ||
            event.type() == QEvent::MouseButtonDblClick) {
            if (mode_ != Mode::Idle &&
                static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
                return true;
            }
        }
        if (event.type() == QEvent::MouseButtonRelease) {
            auto &mouse = static_cast<QMouseEvent &>(event);
            if (mouse.button() != Qt::LeftButton) {
                return false;
            }
            pointer_ = mouse.position();
            if (mode_ != Mode::Idle) {
                const auto point = view.center + (pointer_ - QPointF(view.size.width() / 2,
                                                                     view.size.height() / 2)) /
                                                     view.scale();
                if (!QRectF(0, 0, 1, 1).contains(point)) {
                    return true;
                }
                const bool creating = mode_ == Mode::Placing;
                mode_ = Mode::Idle;
                map.unsetCursor();
                if (creating) {
                    if (editor_.newCollection().isEmpty()) {
                        logMessage(Severity::Warning,
                                   Urgency::Attention,
                                   QStringLiteral("swak.markers"),
                                   tr("Enable a marker collection before placing a marker."));
                        emit changed();
                        return true;
                    }
                    UserObject item;
                    item.collection = editor_.newCollection();
                    item.symbol = editor_.newSymbol();
                    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    item.type = QStringLiteral("circle");
                    item.title = tr("New Marker");
                    item.color = editor_.color().name();
                    if (item.collection != CoreMarkerCollection || !item.symbol.isEmpty()) {
                        if (const auto symbol = collections_.symbol(item.collection, item.symbol)) {
                            item.color = symbol->color.name();
                        }
                    }
                    setLocation(item, point);
                    editor_.edit(item);
                } else {
                    setLocation(moving_, point);
                    const auto error = objects_.put(moving_);
                    if (error.isEmpty()) {
                        select(moving_.id);
                    } else {
                        logMessage(Severity::Error,
                                   Urgency::Attention,
                                   QStringLiteral("swak.markers"),
                                   error);
                    }
                }
                emit changed();
                return true;
            }
            editor_.cancel();
        }
        if (event.type() == QEvent::ContextMenu) {
            if (mode_ != Mode::Idle) {
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
        if (!record || record->type != QStringLiteral("circle")) {
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
        editor_.cancel();
        select(id);
        QMenu menu(&map);
        menu.setObjectName(QStringLiteral("markerContextMenu"));
        auto *edit = menu.addAction(uiIcon(UiIcon::Edit), tr("Edit"));
        edit->setObjectName(QStringLiteral("editMarker"));
        auto *move = menu.addAction(uiIcon(UiIcon::Move), tr("Move"));
        move->setObjectName(QStringLiteral("moveMarker"));
        auto *remove = menu.addAction(uiIcon(UiIcon::Delete), tr("Delete ..."));
        remove->setObjectName(QStringLiteral("deleteMarker"));
        edit->setProperty("menuOrder", -2);
        remove->setProperty("menuOrder", -1);
        sortMenuActions(menu);
        connect(edit, &QAction::triggered, this, [this, id] {
            const auto currentObject = objects_.find(id);
            if (currentObject) {
                selection_.clear();
                editor_.edit(*currentObject);
            }
        });
        connect(move, &QAction::triggered, this, [this, id, &map] {
            const auto currentObject = objects_.find(id);
            if (!currentObject) {
                return;
            }
            moving_ = *currentObject;
            mode_ = Mode::Moving;
            selection_.clear();
            map.setCursor(Qt::CrossCursor);
            emit changed();
        });
        connect(remove, &QAction::triggered, this, [this, id] {
            deleteObject(objects_, id, &editor_);
        });
        if (tools_) {
            tools_->populateContextMenu(item, menu);
        }
        menu.exec(context.globalPos());
        return true;
    }
    QList<MapItem> itemsAt(const Viewport &view, QPointF pointer) const override
    {
        QList<MapItem> result;
        for (const auto &record : objects_.items()) {
            if (record.type != QStringLiteral("circle")) {
                continue;
            }
            if (QLineF(screen(location(record), view), pointer).length() >
                    std::max(8.0, view.markerSize(editor_.radius()) + 5) &&
                !mapLabelBounds(QFontMetrics(QGuiApplication::font()),
                                screen(location(record), view) +
                                    view.markerOffset(QPointF(16, -8), editor_.radius()),
                                record.title)
                     .contains(pointer)) {
                continue;
            }
            MapItem item;
            item.owner = owner();
            item.id = record.id;
            item.label = record.title;
            item.type = tr("Marker");
            if (const auto symbol = collections_.symbol(record.collection, record.symbol)) {
                item.icon = collectionIcon(*symbol, QColor(record.color));
            }
            result.append(std::move(item));
        }
        return result;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        painter.setClipRect(QRectF(QPointF{}, view.size));
        painter.setPen(QPen(QColor("#152028"), 2));
        painter.setBrush(editor_.color());
        for (const auto &item : objects_.items()) {
            if (item.type != QStringLiteral("circle") ||
                (mode_ == Mode::Moving && item.id == moving_.id)) {
                continue;
            }
            painter.setBrush(QColor(item.color));
            paint(painter, item, screen(location(item), view), view);
        }
        if (editor_.draft() && !objects_.find(editor_.draft()->id)) {
            painter.setBrush(QColor(editor_.draft()->color));
            paint(painter, *editor_.draft(), screen(location(*editor_.draft()), view), view);
        }
    }
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const override
    {
        painter.setPen(QPen(QColor("#152028"), 2));
        painter.setBrush(editor_.color());
        if (mode_ != Mode::Idle) {
            if (mode_ == Mode::Moving) {
                painter.setBrush(QColor(moving_.color));
                paint(painter, moving_, pointer, view);
            }
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(QColor(0, 0, 0, qRound(editor_.guideOpacity() * 255)), 1));
            painter.drawLine(QPointF(0, pointer.y()), QPointF(view.size.width(), pointer.y()));
            painter.drawLine(QPointF(pointer.x(), 0), QPointF(pointer.x(), view.size.height()));
        }
    }
    bool deleteSelection() override
    {
        if (mode_ != Mode::Idle || !selection_.item() || selection_.item()->owner != owner()) {
            return false;
        }
        const auto id = selection_.item()->id;
        if (deleteObject(objects_, id, &editor_)) {
            editor_.cancel();
        }
        return true;
    }

private:
    static QString owner()
    {
        return QStringLiteral("org.swak.markers");
    }
    void paint(QPainter &painter, const UserObject &item, QPointF point, const Viewport &view) const
    {
        if (const auto symbol = collections_.symbol(item.collection, item.symbol)) {
            paintCollectionSymbol(
                painter, *symbol, point, view.markerSize(editor_.radius()), QColor(item.color));
        } else {
            paintMarkerSymbol(
                painter, {}, point, view.markerSize(editor_.radius()), QColor(item.color));
        }
        paintMapLabel(painter,
                      point + view.markerOffset(QPointF(16, -8), editor_.radius()),
                      item.title,
                      selection_.item() && selection_.item()->owner == owner() &&
                          selection_.item()->id == item.id);
    }
    void select(const QString &id)
    {
        const auto record = objects_.find(id);
        if (!record || record->type != QStringLiteral("circle")) {
            return;
        }
        MapItem item;
        item.owner = owner();
        item.id = id;
        item.label = record->title;
        item.markerOutline = markerFrame({}, {}, 1);
        if (const auto symbol = collections_.symbol(record->collection, record->symbol)) {
            item.type = symbol->name;
            item.markerOutline = symbol->frame;
        } else {
            item.type = tr("Unavailable marker");
        }
        item.description = record->description;
        item.point = location(*record);
        item.radius = editor_.radius();
        item.symbol = record->symbol;
        item.color = QColor(record->color);
        item.properties = record->properties;
        const auto coordinate = unproject(item.point);
        const auto collectionId = effectiveMarkerCollection(record->collection, record->symbol);
        item.properties.insert(QStringLiteral("Collection"),
                               collections_.collections()
                                   .value(collectionId)
                                   .toObject()
                                   .value(QStringLiteral("name"))
                                   .toString(collectionId));
        item.properties.insert(QStringLiteral("Collection ID"), collectionId);
        item.properties.insert(QStringLiteral("ID"), id);
        item.properties.insert(QStringLiteral("Latitude"), coordinate.y());
        item.properties.insert(QStringLiteral("Longitude"), coordinate.x());
        store_.select({});
        selection_.select(std::move(item));
    }
    enum class Mode
    {
        Idle,
        Placing,
        Moving
    };
    Mode mode_ = Mode::Idle;
    UserObject moving_;
    QPointF pointer_;
    QPointer<QWidget> map_;
    UserObjects &objects_;
    MapSelection &selection_;
    Store &store_;
    MarkerEditor &editor_;
    MarkerCollections &collections_;
    MapTools *tools_;
};
} // namespace
class MarkerPlugin final : public QObject,
                           public Plugin,
                           public PanePlugin,
                           public MapExtension,
                           public MarkerCollectionProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "markers/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MapExtension swak::MarkerCollectionProvider)
public:
    QJsonObject markerCollection() const override
    {
        return coreMarkerCollection();
    }
    QString name() const override
    {
        return tr("Edit marker");
    }
    QAction *primaryAction(QWidget *pane) override
    {
        return pane->findChild<QAction *>(QStringLiteral("markerModeAction"));
    }
    QString mapId() const override
    {
        return QStringLiteral("org.swak.markers");
    }
    QString mapName() const override
    {
        return tr("Markers");
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        if (!services.objects || !services.selection || !services.mapTools ||
            !services.collections) {
            return new QWidget(parent);
        }
        auto *editor = new MarkerEditor(*services.objects, *services.collections, parent);
        auto *tool = new MarkerTool(services, *editor);
        services.mapTools->add(tool);
        auto *action = new QAction(uiIcon(UiIcon::Marker), tr("Marker"), editor);
        action->setObjectName(QStringLiteral("markerModeAction"));
        setToolbarSection(*action, ToolbarSection::Creation, 0);
        action->setCheckable(true);
        const auto updateAction = [action, editor, tool] {
            action->setChecked(!tool->statusText().isEmpty());
            action->setToolTip(
                tr("Marker mode (%1)")
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
#include "markers.moc"
