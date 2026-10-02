#include "core/interfaces.h"
#include "core/tiles.h"
#include <QHideEvent>
#include <QLabel>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QUrl>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QStyle>
#include <QCollator>
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
class MapWidget final : public QOpenGLWidget, public SettingsParticipant, protected QOpenGLFunctions {
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
        attribution_ = new QLabel(this);
        attribution_->setObjectName(QStringLiteral("mapAttribution"));
        attribution_->setAlignment(Qt::AlignRight);
        attribution_->setWordWrap(true);
        attribution_->setTextFormat(Qt::RichText);
        attribution_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        attribution_->setFocusPolicy(Qt::StrongFocus);
        connect(attribution_, &QLabel::linkActivated, this, [](const QString &link) {
            const QUrl url(link, QUrl::StrictMode);
            if (url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
                && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")))
                QDesktopServices::openUrl(url);
        });
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
    void setMapSources(MapSources *sources)
    {
        if (sources_) disconnect(sources_, nullptr, this, nullptr);
        sources_ = sources;
        const auto apply = [this] {
            const auto source = sources_->activeSource();
            normalizeZoom();
            wheelRemainder_ = 0;
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
                numberSetting(QStringLiteral("zoom"), tr("Zoom"), zoomLimits().first, zoomLimits().second),
                numberSetting(QStringLiteral("missingOpacity"), tr("Missing tile marker opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("opacity"), tr("Provider opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("attributionOpacity"), tr("Attribution background opacity"), 0, 1, 2),
                booleanSetting(QStringLiteral("smooth"), tr("Smooth image scaling")),
                booleanSetting(QStringLiteral("antialias"), tr("Smooth vector edges"))};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("centerX"), view_.center.x()}, {QStringLiteral("centerY"), view_.center.y()},
                {QStringLiteral("zoom"), view_.zoom}, {QStringLiteral("opacity"), opacity_},
                {QStringLiteral("missingOpacity"), missingOpacity_},
                {QStringLiteral("attributionOpacity"), attributionOpacity_}, {QStringLiteral("smooth"), smooth_}, {QStringLiteral("antialias"), antialias_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        view_.center = {preferenceNumber(values, QStringLiteral("centerX"), 0.5, 0, 1),
                        preferenceNumber(values, QStringLiteral("centerY"), 0.5, 0, 1)};
        view_.zoom = preferenceNumber(values, QStringLiteral("zoom"), 1, 0, 19);
        normalizeZoom();
        wheelRemainder_ = 0;
        missingOpacity_ = preferenceNumber(values, QStringLiteral("missingOpacity"), 0.2, 0, 1);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 1, 0, 1);
        attributionOpacity_ = preferenceNumber(values, QStringLiteral("attributionOpacity"), 0.55, 0, 1);
        smooth_ = values.value(QStringLiteral("smooth")).toBool(true);
        antialias_ = values.value(QStringLiteral("antialias")).toBool(true);
        update();
    }
protected:
    void hideEvent(QHideEvent *event) override { tiles_.stop(); QOpenGLWidget::hideEvent(event); }
    void initializeGL() override { initializeOpenGLFunctions(); }
    void paintGL() override
    {
        glClearColor(static_cast<float>(qRed(MapBackgroundRgb)) / 255,
            static_cast<float>(qGreen(MapBackgroundRgb)) / 255, static_cast<float>(qBlue(MapBackgroundRgb)) / 255, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        view_.size = size();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, antialias_);
        bool tilesPainted = false;
        for (const auto *layer : layers_) {
            if (!tilesPainted && layer->order() > 0) { tiles_.paint(painter, view_, opacity_, smooth_, missingOpacity_); tilesPainted = true; }
            painter.save();
            layer->paint(painter, view_, store_);
            painter.restore();
        }
        if (!tilesPainted) tiles_.paint(painter, view_, opacity_, smooth_, missingOpacity_);
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        painter.setPen(QColor("#c7d7e5"));
        const QPointF coord = view_.coordinate(cursor_);
        const bool onWorld = coord.x() >= -180 && coord.x() <= 180 && coord.y() >= -85.051129 && coord.y() <= 85.051129;
        const QString position = onWorld ? tr("%1°, %2°").arg(coord.y(), 0, 'f', 4).arg(coord.x(), 0, 'f', 4)
                                         : tr("Pointer outside map");
        painter.drawText(QRect(16, height() - 30, width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter,
                         tr("%1  |  Zoom %2  |  %3 events").arg(position).arg(view_.zoom, 0, 'f', 0).arg(store_.rowCount()));
        attribution_->setVisible(!source.isEmpty());
        if (!source.isEmpty()) {
            QString credit = source.value(QStringLiteral("attribution")).toString();
            static const QRegularExpression links(QStringLiteral("https?://[^\\s|<>]+"));
            QList<QUrl> urls;
            auto matches = links.globalMatch(credit);
            while (matches.hasNext()) {
                auto candidate = matches.next().captured();
                while (!candidate.isEmpty() && (QStringLiteral(".,;:!?").contains(candidate.back())
                    || (candidate.endsWith(')') && candidate.count(')') > candidate.count('('))
                    || (candidate.endsWith(']') && candidate.count(']') > candidate.count('[')))) candidate.chop(1);
                const QUrl url(candidate, QUrl::StrictMode);
                if (url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()) urls.append(url);
            }
            credit.remove(links);
            credit = credit.trimmed();
            while (!credit.isEmpty() && QStringLiteral("|(").contains(credit.back())) {
                credit.chop(1);
                credit = credit.trimmed();
            }
            if (credit.isEmpty()) credit = tr("Map attribution");
            int naturalWidth = attribution_->fontMetrics().horizontalAdvance(credit);
            QString text = credit.toHtmlEscaped();
            const bool linked = !urls.isEmpty();
            if (linked) {
                text = QStringLiteral("<a style=\"color: white\" href=\"%1\">%2</a>")
                    .arg(urls.first().toString(QUrl::FullyEncoded).toHtmlEscaped(), text);
                for (qsizetype i = 1; i < urls.size(); ++i) {
                    const auto label = tr("Source %1").arg(i + 1);
                    text += QStringLiteral(" | <a style=\"color: white\" href=\"%1\">%2</a>")
                        .arg(urls.at(i).toString(QUrl::FullyEncoded).toHtmlEscaped(), label.toHtmlEscaped());
                    naturalWidth += attribution_->fontMetrics().horizontalAdvance(QStringLiteral(" | ") + label);
                }
            }
            if (source.value(QStringLiteral("ignoreTls")).toBool(false)) {
                const auto warning = tr("HTTPS certificate checks disabled");
                text += QStringLiteral("<br>") + warning.toHtmlEscaped();
                naturalWidth = std::max(naturalWidth, attribution_->fontMetrics().horizontalAdvance(warning));
            }
            if (!tiles_.status().isEmpty()) text += QStringLiteral("<br>") + tiles_.status().toHtmlEscaped();
            attribution_->setText(text);
            attribution_->setToolTip(linked ? urls.first().toDisplayString() : QString{});
            const auto style = QStringLiteral("QLabel { color: white; background-color: rgba(16,32,44,%1); padding: 4px; }")
                .arg(qRound(attributionOpacity_ * 255));
            if (attribution_->styleSheet() != style) attribution_->setStyleSheet(style);
            naturalWidth = std::max(naturalWidth, attribution_->fontMetrics().horizontalAdvance(tiles_.status()));
            const int panelWidth = std::min(width() - 24, naturalWidth + 16);
            const int panelHeight = attribution_->heightForWidth(panelWidth);
            attribution_->setGeometry(width() - 12 - panelWidth, height() - 34 - panelHeight, panelWidth, panelHeight);
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
        if (event->phase() == Qt::ScrollBegin) wheelRemainder_ = 0;
        const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
        wheelRemainder_ += std::clamp(delta, -1200, 1200);
        const int steps = wheelRemainder_ / 120;
        wheelRemainder_ %= 120;
        if (steps) zoomAt(event->position(), steps);
        if (event->phase() == Qt::ScrollEnd) wheelRemainder_ = 0;
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Plus: case Qt::Key_Equal: zoomAt(rect().center(), 1); break;
        case Qt::Key_Minus: zoomAt(rect().center(), -1); break;
        case Qt::Key_Home: view_.center = {0.5, 0.5}; view_.zoom = 1; normalizeZoom(); wheelRemainder_ = 0; break;
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
    std::pair<int, int> zoomLimits() const
    {
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        return source.isEmpty() ? std::pair{0, 19} : std::pair{source.value(QStringLiteral("minZoom")).toInt(),
            source.value(QStringLiteral("maxZoom")).toInt()};
    }
    void normalizeZoom()
    {
        const auto [minimum, maximum] = zoomLimits();
        view_.zoom = std::clamp(std::round(view_.zoom), static_cast<double>(minimum), static_cast<double>(maximum));
    }
    void clampCenter()
    {
        view_.center.setX(std::clamp(view_.center.x(), 0.0, 1.0));
        view_.center.setY(std::clamp(view_.center.y(), 0.0, 1.0));
    }
    void zoomAt(QPointF anchor, int delta)
    {
        const QPointF offset = anchor - QPointF(width() / 2.0, height() / 2.0);
        const QPointF before = view_.center + offset / view_.scale();
        view_.zoom += delta;
        normalizeZoom();
        view_.center = before - offset / view_.scale();
        clampCenter();
        update();
    }
    MapSources *sources_ = nullptr;
    TileRenderer tiles_;
    QLabel *attribution_;
    double attributionOpacity_ = 0.55;
    double opacity_ = 1;
    double missingOpacity_ = 0.2;
    int wheelRemainder_ = 0;
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
class MapPlugin final : public QObject, public Plugin, public ViewPlugin, public ToolbarPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "map/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::ViewPlugin swak::ToolbarPlugin)
public:
    QString name() const override { return tr("OpenGL map"); }
    QWidget *create(PluginServices &services, const QList<MapLayer *> &layers, QWidget *parent) override
    {
        auto *widget = new MapWidget(services.store, layers, parent);
        widget->setMapSources(&services.maps);
        return widget;
    }
    void populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *) override
    {
        auto *mapSources = &services.maps;
        auto *mapButton = new QToolButton(&toolbar);
        mapButton->setObjectName(QStringLiteral("mapProviderButton"));
        mapButton->setAccessibleName(tr("Select map provider"));
        mapButton->setPopupMode(QToolButton::InstantPopup);
        mapButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mapButton->setIcon(QIcon::fromTheme(QStringLiteral("globe"), toolbar.style()->standardIcon(QStyle::SP_DirIcon)));
        auto *mapMenu = new QMenu(mapButton);
        mapMenu->setObjectName(QStringLiteral("mapProviderMenu"));
        mapButton->setMenu(mapMenu);
        toolbar.addWidget(mapButton);
        const auto updateMapButton = [mapButton, mapSources] {
            const auto source = mapSources->activeSource();
            mapButton->setText(source.isEmpty() ? tr("No map provider") : source.value(QStringLiteral("name")).toString());
        };
        connect(mapSources, &MapSources::changed, mapButton, updateMapButton);
        connect(mapMenu, &QMenu::aboutToShow, mapMenu, [mapMenu, mapSources] {
            mapMenu->clear();
            auto *clearProvider = mapMenu->addAction(tr("No map provider"));
            clearProvider->setCheckable(true);
            clearProvider->setChecked(mapSources->activeSource().isEmpty());
            connect(clearProvider, &QAction::triggered, mapSources, [mapSources] { mapSources->select({}); });
            const auto sources = mapSources->sources();
            auto ids = sources.keys();
            const QCollator collator;
            std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
                return collator.compare(sources.value(a).toObject().value(QStringLiteral("name")).toString(),
                    sources.value(b).toObject().value(QStringLiteral("name")).toString()) < 0;
            });
            for (const auto &id : ids) {
                const auto source = sources.value(id).toObject();
                const auto *provider = mapSources->provider(source);
                auto *action = mapMenu->addAction(source.value(QStringLiteral("name")).toString());
                action->setData(id);
                action->setEnabled(provider && provider->validate(source).isEmpty());
                action->setCheckable(true);
                action->setChecked(mapSources->networkEnabled() && mapSources->activeId() == id);
                connect(action, &QAction::triggered, mapSources, [mapSources, id] { mapSources->select(id); });
            }
            mapMenu->addSeparator();
            const auto *manage = mapMenu->addAction(tr("Map Providers"));
            connect(manage, &QAction::triggered, mapSources, &MapSources::managementRequested);
        });
        updateMapButton();
    }
};
}
#include "map.moc"
