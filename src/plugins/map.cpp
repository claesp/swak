#include "core/interfaces.h"
#include "core/tiles.h"
#include <QHideEvent>
#include "core/projection.h"
#include "core/store.h"
#include "core/preferences.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace swak {
class MapWidget final : public QOpenGLWidget, public SettingsParticipant, public MapConsumer, protected QOpenGLFunctions {
public:
    MapWidget(Store &store, QList<MapLayer *> layers, QWidget *parent)
        : QOpenGLWidget(parent), store_(store), layers_(std::move(layers))
    {
        connect(&tiles_, &TileRenderer::changed, this, QOverload<>::of(&MapWidget::update));
        setObjectName(QStringLiteral("map"));
        setMinimumSize(320, 240);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("Event map"));
        setAccessibleDescription(tr("Drag to pan. Use the wheel or plus and minus keys to zoom. Press Home to reset."));
        setMouseTracking(true);
        std::sort(layers_.begin(), layers_.end(), [](const auto *a, const auto *b) { return a->order() < b->order(); });
        connect(&store_, &Store::changed, this, QOverload<>::of(&MapWidget::update));
        connect(&store_, &Store::selectionChanged, this, [this] {
            const auto &events = store_.events();
            const auto event = std::find_if(events.begin(), events.end(), [this](const Event &item) {
                return item.uid == store_.selected();
            });
            if (event != events.end()) view_.center = project(event->latitude, event->longitude);
            update();
        });
    }
    void setMapSources(MapSources *sources) override
    {
        if (sources_) disconnect(sources_, nullptr, this, nullptr);
        sources_ = sources;
        const auto apply = [this] {
            const auto source = sources_->activeSource();
            tiles_.setSource(sources_->provider(source), source);
            update();
        };
        connect(sources_, &MapSources::changed, this, apply);
        apply();
    }
    QString preferenceId() const override { return QStringLiteral("org.swak.map"); }
    QString settingsTitle() const override { return tr("Map"); }
    QList<SettingField> settingsFields() const override
    {
        return {numberSetting(QStringLiteral("centerX"), tr("Horizontal center (0–1)"), 0, 1, 6),
                numberSetting(QStringLiteral("centerY"), tr("Vertical center (0–1)"), 0, 1, 6),
                numberSetting(QStringLiteral("zoom"), tr("Zoom"), 0, 19, 2),
                numberSetting(QStringLiteral("opacity"), tr("Provider opacity"), 0, 1, 2),
                booleanSetting(QStringLiteral("smooth"), tr("Smooth image scaling")),
                booleanSetting(QStringLiteral("antialias"), tr("Smooth vector edges"))};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("centerX"), view_.center.x()}, {QStringLiteral("centerY"), view_.center.y()},
                {QStringLiteral("zoom"), view_.zoom}, {QStringLiteral("opacity"), opacity_},
                {QStringLiteral("smooth"), smooth_}, {QStringLiteral("antialias"), antialias_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        view_.center = {preferenceNumber(values, QStringLiteral("centerX"), 0.5, 0, 1),
                        preferenceNumber(values, QStringLiteral("centerY"), 0.5, 0, 1)};
        view_.zoom = preferenceNumber(values, QStringLiteral("zoom"), 1, 0, 19);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 1, 0, 1);
        smooth_ = values.value(QStringLiteral("smooth")).toBool(true);
        antialias_ = values.value(QStringLiteral("antialias")).toBool(true);
        update();
    }
protected:
    void hideEvent(QHideEvent *event) override { tiles_.stop(); QOpenGLWidget::hideEvent(event); }
    void initializeGL() override { initializeOpenGLFunctions(); }
    void paintGL() override
    {
        glClearColor(0.04F, 0.09F, 0.13F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        view_.size = size();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, antialias_);
        bool tilesPainted = false;
        for (const auto *layer : layers_) {
            if (!tilesPainted && layer->order() > 0) { tiles_.paint(painter, view_, opacity_, smooth_); tilesPainted = true; }
            painter.save();
            layer->paint(painter, view_, store_);
            painter.restore();
        }
        if (!tilesPainted) tiles_.paint(painter, view_, opacity_, smooth_);
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        painter.setPen(QColor("#c7d7e5"));
        painter.drawText(QRect(16, 12, width() - 32, 44), Qt::AlignLeft | Qt::AlignTop,
                         (source.isEmpty() ? tr("OFFLINE MAP | Schematic land outlines") : source.value(QStringLiteral("name")).toString()) +
                         tr("\nDrag to pan · Scroll to zoom · Home to reset"));
        const QPointF coord = view_.coordinate(cursor_);
        const bool onWorld = coord.x() >= -180 && coord.x() <= 180 && coord.y() >= -85.051129 && coord.y() <= 85.051129;
        const QString position = onWorld ? tr("%1°, %2°").arg(coord.y(), 0, 'f', 4).arg(coord.x(), 0, 'f', 4)
                                         : tr("Pointer outside map");
        painter.drawText(QRect(16, height() - 30, width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter,
                         tr("%1  |  Zoom %2  |  %3 events").arg(position).arg(view_.zoom, 0, 'f', 1).arg(store_.rowCount()));
        if (store_.rowCount() == 0) {
            painter.drawText(rect(), Qt::AlignCenter, tr("No events yet.\nGenerate an event or start a receiver."));
        }
        if (!source.isEmpty()) {
            QString credit = source.value(QStringLiteral("attribution")).toString();
            if (source.value(QStringLiteral("ignoreTls")).toBool(false)) credit += tr("\nHTTPS certificate checks disabled");
            if (!tiles_.status().isEmpty()) credit += QLatin1Char('\n') + tiles_.status();
            const int textHeight = painter.fontMetrics().boundingRect(QRect(0, 0, width() - 24, 300), Qt::TextWordWrap, credit).height();
            const QRect panel(0, height() - 38 - textHeight, width(), textHeight + 8);
            painter.fillRect(panel, QColor("#10202c"));
            painter.setPen(Qt::white);
            painter.drawText(panel.adjusted(12, 4, -12, -4), Qt::TextWordWrap, credit);
        }
        if (hasFocus()) {
            painter.setPen(QPen(QColor("#5fd6ff"), 2));
            painter.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        dragging_ = true;
        moved_ = false;
        last_ = event->position();
        press_ = last_;
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        cursor_ = event->position();
        if (dragging_) {
            moved_ = moved_ || (cursor_ - press_).manhattanLength() > 4;
            view_.center -= (cursor_ - last_) / view_.scale();
            clampCenter();
            last_ = cursor_;
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        dragging_ = false;
        unsetCursor();
        if (moved_) return;
        QString closest;
        double distance = 14;
        for (const auto &item : store_.events()) {
            if (item.start > QDateTime::currentDateTimeUtc()) continue;
            const auto delta = view_.screen(item.latitude, item.longitude) - event->position();
            const double candidate = std::hypot(delta.x(), delta.y());
            if (candidate < distance) { distance = candidate; closest = item.uid; }
        }
        store_.select(closest);
        update();
    }
    void wheelEvent(QWheelEvent *event) override
    {
        zoomAt(event->position(), event->angleDelta().y() / 480.0);
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Plus: case Qt::Key_Equal: zoomAt(rect().center(), 0.5); break;
        case Qt::Key_Minus: zoomAt(rect().center(), -0.5); break;
        case Qt::Key_Home: view_.center = {0.5, 0.5}; view_.zoom = 1; break;
        case Qt::Key_Left: view_.center.rx() -= 64 / view_.scale(); break;
        case Qt::Key_Right: view_.center.rx() += 64 / view_.scale(); break;
        case Qt::Key_Up: view_.center.ry() -= 64 / view_.scale(); break;
        case Qt::Key_Down: view_.center.ry() += 64 / view_.scale(); break;
        default: QOpenGLWidget::keyPressEvent(event); return;
        }
        clampCenter();
        update();
    }
private:
    void clampCenter()
    {
        view_.center.setX(std::clamp(view_.center.x(), 0.0, 1.0));
        view_.center.setY(std::clamp(view_.center.y(), 0.0, 1.0));
    }
    void zoomAt(QPointF anchor, double delta)
    {
        const QPointF offset = anchor - QPointF(width() / 2.0, height() / 2.0);
        const QPointF before = view_.center + offset / view_.scale();
        view_.zoom = std::clamp(view_.zoom + delta, 0.0, 19.0);
        view_.center = before - offset / view_.scale();
        clampCenter();
        update();
    }
    MapSources *sources_ = nullptr;
    TileRenderer tiles_;
    double opacity_ = 1;
    bool smooth_ = true;
    bool antialias_ = true;
    Store &store_;
    QList<MapLayer *> layers_;
    Viewport view_;
    QPointF cursor_;
    QPointF last_;
    QPointF press_;
    bool dragging_ = false;
    bool moved_ = false;
};
class MapPlugin final : public QObject, public ViewPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.View/1.0")
    Q_INTERFACES(swak::ViewPlugin)
public:
    QString name() const override { return tr("OpenGL map"); }
    QWidget *create(Store &store, const QList<MapLayer *> &layers, QWidget *parent) override
    { return new MapWidget(store, layers, parent); }
};
}
#include "map.moc"
