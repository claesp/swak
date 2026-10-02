#include "core/interfaces.h"
#include "core/maps.h"
#include "core/map_tools.h"
#include "core/object_editor.h"
#include "core/store.h"
#include "core/logging.h"
#include <QMouseEvent>
#include <QCursor>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QMenu>
#include <QKeySequence>
#include <QUuid>
#include <QTimer>
#include <cmath>
namespace swak {
namespace {
QPointF location(const UserObject &item) { return {item.geometry.value(QStringLiteral("x")).toDouble(), item.geometry.value(QStringLiteral("y")).toDouble()}; }
void setLocation(UserObject &item, QPointF point) { item.geometry = {{QStringLiteral("x"), point.x()}, {QStringLiteral("y"), point.y()}}; }
QPointF screen(QPointF point, const Viewport &view) { return (point - view.center) * view.scale() + QPointF(view.size.width() / 2, view.size.height() / 2); }
class MarkerEditor final : public ObjectEditor, public SettingsParticipant {
public:
    MarkerEditor(UserObjects &objects, QWidget *parent) : ObjectEditor(objects, parent) {}
    QString preferenceId() const override { return QStringLiteral("org.swak.markers"); }
    QString settingsTitle() const override { return tr("Markers"); }
    QList<SettingField> settingsFields() const override {
        return {keySetting(QStringLiteral("createKey"), tr("Create marker")), numberSetting(QStringLiteral("radius"), tr("Circle radius (pixels)"), 3, 24),
            textSetting(QStringLiteral("color"), tr("Circle color (#RRGGBB)"), 7), numberSetting(QStringLiteral("guideOpacity"), tr("Guide line opacity"), 0, 1, 2)};
    }
    QJsonObject savePreferences() const override {
        auto state = editorState(); state.insert(QStringLiteral("guideStyleVersion"), 2); state.insert(QStringLiteral("createKey"), QKeySequence(key_).toString(QKeySequence::PortableText));
        state.insert(QStringLiteral("radius"), radius_); state.insert(QStringLiteral("color"), color_.name()); state.insert(QStringLiteral("guideOpacity"), guideOpacity_); return state;
    }
    QString validateSettings(const QJsonObject &values) const override {
        if (!readKey(values)) return tr("Use one letter without modifiers to create a marker.");
        const auto color = values.value(QStringLiteral("color")).toString(QStringLiteral("#ffc857"));
        if (color.size() != 7 || !color.startsWith('#') || !QColor::isValidColorName(color)) return tr("Use a color in #RRGGBB format.");
        return {};
    }
    void restorePreferences(const QJsonObject &values) override {
        const int key = readKey(values); key_ = key ? key : Qt::Key_M;
        radius_ = preferenceNumber(values, QStringLiteral("radius"), 7, 3, 24);
        guideOpacity_ = preferenceNumber(values, QStringLiteral("guideOpacity"), 0.5, 0, 1);
        if (!values.contains(QStringLiteral("guideStyleVersion")) && guideOpacity_ == 0.6) guideOpacity_ = 0.5;
        const auto color = preferenceText(values, QStringLiteral("color"), QStringLiteral("#ffc857"), 7);
        color_ = QColor::isValidColorName(color) ? QColor(color) : QColor("#ffc857"); restoreEditorState(values); emit configurationChanged();
    }
    double radius() const { return radius_; }
    QColor color() const { return color_; }
    double guideOpacity() const { return guideOpacity_; }
    int createKey() const { return key_; }
private:
    static int readKey(const QJsonObject &values) {
        const auto text = values.value(QStringLiteral("createKey")).toString(QStringLiteral("M")); if (text.size() > 32) return 0;
        const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (key.count() != 1 || key[0].keyboardModifiers() != Qt::NoModifier) return 0;
        return key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z ? static_cast<int>(key[0].key()) : 0;
    }
    int key_ = Qt::Key_M;
    double radius_ = 7;
    QColor color_{"#ffc857"};
    double guideOpacity_ = 0.5;
};
class MarkerTool final : public MapTool {
public:
    MarkerTool(PluginServices &services, MarkerEditor &editor) : MapTool(&editor), objects_(*services.objects), selection_(*services.selection), store_(services.store), editor_(editor)
    {
        connect(&objects_, &UserObjects::changed, this, [this] {
            if (selection_.item() && selection_.item()->owner == owner()) {
                const auto id = selection_.item()->id;
                if (objects_.find(id)) select(id); else selection_.clear();
            }
            if (mode_ == Mode::Moving && !objects_.find(moving_.id)) cancel();
            emit changed();
        });
        connect(&objects_, &UserObjects::restored, this, [this](const QString &id) {
            const auto item = objects_.find(id); if (item && item->type == QStringLiteral("circle")) { cancel(); select(id); }
        });
        connect(&editor_, &ObjectEditor::applied, this, [this](const QString &id) { select(id); emit changed(); });
        connect(&editor_, &ObjectEditor::configurationChanged, this, [this] {
            if (selection_.item() && selection_.item()->owner == owner()) select(selection_.item()->id);
            emit changed();
        });
        connect(&editor_, &ObjectEditor::cancelled, this, [this] { emit changed(); });
    }
    bool coordinateGridActive() const override { return mode_ != Mode::Idle; }
    QString statusText() const override { return mode_ == Mode::Idle ? QString{} : QStringLiteral("M"); }
    bool cancel() override
    {
        const bool active = mode_ != Mode::Idle;
        const auto restore = mode_ == Mode::Moving ? moving_.id : QString{};
        mode_ = Mode::Idle; if (active && map_) map_->unsetCursor();
        const bool editing = editor_.cancel();
        if (!restore.isEmpty()) select(restore);
        if (active || editing) emit changed();
        return active || editing;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &map) override
    {
        map_ = &map;
        if (event.type() == QEvent::KeyPress) {
            auto &key = static_cast<QKeyEvent &>(event);
            if (key.modifiers() != Qt::NoModifier) return false;
            if (key.key() == Qt::Key_Escape) return cancel();
            if (key.key() == editor_.createKey()) {
                if (!key.isAutoRepeat()) {
                    if (mode_ != Mode::Idle) { cancel(); return true; }
                    cancel(); mode_ = Mode::Placing; selection_.clear(); store_.select({}); pointer_ = map.mapFromGlobal(QCursor::pos()); map.setCursor(Qt::CrossCursor); emit changed(); }
                return true;
            }
            if (mode_ == Mode::Idle && (key.key() == Qt::Key_Delete || key.key() == Qt::Key_Backspace)
                && selection_.item() && selection_.item()->owner == owner()) {
                return deleteSelection();
            }
        }
        if (event.type() == QEvent::MouseMove) {
            pointer_ = static_cast<QMouseEvent &>(event).position();
            if (mode_ != Mode::Idle) { emit overlayChanged(); return true; }
        }
        if (event.type() == QEvent::MouseButtonPress || event.type() == QEvent::MouseButtonDblClick) {
            if (mode_ != Mode::Idle && static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) return true;
        }
        if (event.type() == QEvent::MouseButtonRelease) {
            auto &mouse = static_cast<QMouseEvent &>(event); if (mouse.button() != Qt::LeftButton) return false;
            pointer_ = mouse.position();
            if (mode_ != Mode::Idle) {
                const auto point = view.center + (pointer_ - QPointF(view.size.width() / 2, view.size.height() / 2)) / view.scale();
                if (!QRectF(0, 0, 1, 1).contains(point)) return true;
                const bool creating = mode_ == Mode::Placing; mode_ = Mode::Idle; map.unsetCursor();
                if (creating) {
                    UserObject item; item.id = QUuid::createUuid().toString(QUuid::WithoutBraces); item.type = QStringLiteral("circle"); item.title = tr("New Marker"); setLocation(item, point);
                    editor_.edit(item);
                } else {
                    setLocation(moving_, point);
                    const auto error = objects_.put(moving_);
                    if (error.isEmpty()) select(moving_.id);
                    else logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.markers"), error);
                }
                emit changed(); return true;
            }
            const auto id = hit(view, pointer_);
            if (!id.isEmpty()) { editor_.cancel(); select(id); return true; }
            editor_.cancel();
        }
        if (event.type() == QEvent::ContextMenu) {
            if (mode_ != Mode::Idle) return true;
            auto &context = static_cast<QContextMenuEvent &>(event);
            pointer_ = context.pos();
            const auto id = hit(view, context.reason() == QContextMenuEvent::Keyboard ? map.rect().center() : context.pos());
            if (id.isEmpty()) return false;
            editor_.cancel(); select(id);
            QMenu menu(&map); menu.setObjectName(QStringLiteral("markerContextMenu"));
            auto *edit = menu.addAction(tr("Edit")); edit->setObjectName(QStringLiteral("editMarker"));
            auto *move = menu.addAction(tr("Move")); move->setObjectName(QStringLiteral("moveMarker"));
            auto *remove = menu.addAction(tr("Delete")); remove->setObjectName(QStringLiteral("deleteMarker"));
            connect(edit, &QAction::triggered, this, [this, id] { const auto item = objects_.find(id); if (item) { selection_.clear(); editor_.edit(*item); } });
            connect(move, &QAction::triggered, this, [this, id, &map] {
                const auto item = objects_.find(id); if (!item) return;
                moving_ = *item; mode_ = Mode::Moving; selection_.clear(); map.setCursor(Qt::CrossCursor); emit changed();
            });
            connect(remove, &QAction::triggered, this, [this, id] { objects_.remove(id); });
            menu.exec(context.globalPos()); return true;
        }
        return false;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        painter.setClipRect(QRectF(QPointF{}, view.size));
        painter.setPen(QPen(QColor("#152028"), 2)); painter.setBrush(editor_.color());
        for (const auto &item : objects_.items()) {
            if (item.type != QStringLiteral("circle") || (mode_ == Mode::Moving && item.id == moving_.id)) continue;
            painter.drawEllipse(screen(location(item), view), editor_.radius(), editor_.radius());
        }
        if (editor_.draft() && !objects_.find(editor_.draft()->id)) painter.drawEllipse(screen(location(*editor_.draft()), view), editor_.radius(), editor_.radius());
    }
    void paintOverlay(QPainter &painter, const Viewport &view) const override
    {
        painter.setPen(QPen(QColor("#152028"), 2)); painter.setBrush(editor_.color());
        if (mode_ != Mode::Idle) {
            if (mode_ == Mode::Moving) painter.drawEllipse(pointer_, editor_.radius(), editor_.radius());
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(QColor(0, 0, 0, qRound(editor_.guideOpacity() * 255)), 1));
            painter.drawLine(QPointF(0, pointer_.y()), QPointF(view.size.width(), pointer_.y()));
            painter.drawLine(QPointF(pointer_.x(), 0), QPointF(pointer_.x(), view.size.height()));
        }
    }
    bool deleteSelection() override {
        if (mode_ != Mode::Idle || !selection_.item() || selection_.item()->owner != owner()) return false;
        const auto id = selection_.item()->id; editor_.cancel(); objects_.remove(id); return true;
    }
private:
    static QString owner() { return QStringLiteral("org.swak.markers"); }
    QString hit(const Viewport &view, QPointF pointer) const {
        QString id; double distance = editor_.radius() + 5;
        for (const auto &item : objects_.items()) {
            if (item.type != QStringLiteral("circle")) continue;
            const auto offset = screen(location(item), view) - pointer; const double candidate = std::hypot(offset.x(), offset.y());
            if (candidate < distance) { distance = candidate; id = item.id; }
        }
        return id;
    }
    void select(const QString &id) {
        const auto record = objects_.find(id); if (!record || record->type != QStringLiteral("circle")) return;
        MapItem item; item.owner = owner(); item.id = id; item.label = record->title; item.type = tr("Marker"); item.description = record->description;
        item.point = location(*record); item.radius = editor_.radius(); item.color = editor_.color(); item.properties = record->properties;
        const auto coordinate = unproject(item.point);
        item.properties.insert(QStringLiteral("ID"), id); item.properties.insert(QStringLiteral("Latitude"), coordinate.y()); item.properties.insert(QStringLiteral("Longitude"), coordinate.x());
        store_.select({}); selection_.select(std::move(item));
    }
    enum class Mode { Idle, Placing, Moving };
    Mode mode_ = Mode::Idle;
    UserObject moving_;
    QPointF pointer_;
    QPointer<QWidget> map_;
    UserObjects &objects_;
    MapSelection &selection_;
    Store &store_;
    MarkerEditor &editor_;
};
}
class MarkerPlugin final : public QObject, public Plugin, public PanePlugin, public MapExtension {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "markers/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MapExtension)
public:
    QString name() const override { return tr("Edit marker"); }
    QString mapId() const override { return QStringLiteral("org.swak.markers"); }
    QString mapName() const override { return tr("Markers"); }
    Kind mapKind() const override { return Kind::Tool; }
    QWidget *create(PluginServices &services, QWidget *parent) override {
        if (!services.objects || !services.selection || !services.mapTools) return new QWidget(parent);
        auto *editor = new MarkerEditor(*services.objects, parent);
        services.mapTools->add(new MarkerTool(services, *editor));
        QTimer::singleShot(0, editor, [editor] { emit editor->visibilityRequested(false); });
        return editor;
    }
};
}
#include "markers.moc"
