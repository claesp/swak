#include "core/interfaces.h"
#include "core/tiles.h"
#include "core/data.h"
#include <QHideEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QPointer>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
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
#include <QKeySequence>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QResizeEvent>
#include "core/map_selection.h"
#include <array>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace swak {
namespace {
const std::array<QString, 14> KeyPaths{QStringLiteral("panNorthKey"), QStringLiteral("panWestKey"), QStringLiteral("panSouthKey"), QStringLiteral("panEastKey"), QStringLiteral("zoomInKey"), QStringLiteral("zoomOutKey"), QStringLiteral("resetViewKey"), QStringLiteral("zoomPlusKey"), QStringLiteral("zoomEqualKey"), QStringLiteral("zoomMinusKey"),
    QStringLiteral("stepWestKey"), QStringLiteral("stepEastKey"), QStringLiteral("stepNorthKey"), QStringLiteral("stepSouthKey")};
const std::array<int, 14> DefaultKeys{Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_E, Qt::Key_Home, Qt::Key_Plus, Qt::Key_Equal, Qt::Key_Minus, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down};
int navigationKey(const QString &text)
{
    if (text.isEmpty() || text.size() > 32) return 0;
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].keyboardModifiers() != Qt::NoModifier) return 0;
    const int key = sequence[0].key();
    return ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F12) || (key >= Qt::Key_Left && key <= Qt::Key_Down) || key == Qt::Key_Home || key == Qt::Key_Plus || key == Qt::Key_Equal || key == Qt::Key_Minus) ? key : 0;
}
}
class MapWidget final : public QOpenGLWidget, public SettingsParticipant, protected QOpenGLFunctions {
public:
    MapWidget(Store &store, QList<MapLayer *> layers, QWidget *parent)
        : QOpenGLWidget(parent), store_(store), layers_(std::move(layers))
    {
        panTimer_.setInterval(16);
        panTimer_.setTimerType(Qt::PreciseTimer);
        connect(&panTimer_, &QTimer::timeout, this, [this] {
            const double seconds = std::min(static_cast<double>(panClock_.restart()) / 1000.0, 0.1);
            view_.center += panDirection() * (panSpeed_ * seconds / view_.scale());
            clampCenter();
            update();
        });
        connect(&tiles_, &TileRenderer::changed, this, QOverload<>::of(&MapWidget::update));
        setObjectName(QStringLiteral("map"));
        setMinimumSize(320, 240);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("Event map"));
        setAccessibleDescription(tr("Drag to pan. Double-click to center. Use the wheel to zoom. Change keyboard controls in File, Settings, Keybindings. Right-click for map layers and information."));
        setMouseTracking(true);
        cacheLabel_ = new QLabel(this);
        cacheLabel_->setObjectName(QStringLiteral("mapCacheSize"));
        cacheLabel_->setTextFormat(Qt::PlainText);
        cacheLabel_->setStyleSheet(QStringLiteral("color: #c7d7e5;"));
        cacheLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cacheLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        cacheLabel_->setToolTip(tr("Local tile files for the active HTTPS cache. Cached tiles survive restarts until they expire or the disk limit removes them."));
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
        connect(&store_, &Store::changed, this, [this] {
            if (selection_ && selectedSource_ == QStringLiteral("cot")) selection_->clear();
            update();
        });
        connect(&store_, &Store::selectionChanged, this, [this] {
            const auto &events = store_.events();
            const auto event = std::find_if(events.begin(), events.end(), [this](const Event &item) {
                return item.uid == store_.selected();
            });
            if (event != events.end()) { view_.center = project(event->latitude, event->longitude); clampCenter(); }
            update();
        });
    }
    void setSelection(MapSelection *selection)
    {
        selection_ = selection;
        if (selection_) connect(selection_, &MapSelection::changed, this, qOverload<>(&QWidget::update));
    }
    void setDataSources(DataSources *sources)
    {
        dataSources_ = sources;
        if (sources) connect(sources, &DataSources::contentChanged, this, [this] {
            if (selection_ && selectedSource_ == QStringLiteral("data")) selection_->clear();
            update();
        });
    }
    void setMapSources(MapSources *sources)
    {
        if (sources_) disconnect(sources_, nullptr, this, nullptr);
        sources_ = sources;
        const auto apply = [this] {
            if (selection_ && selectedSource_ == QStringLiteral("vector")) selection_->clear();
            const auto source = sources_->activeSource();
            normalizeZoom();
            clampCenter();
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
                keySetting(QStringLiteral("panNorthKey"), tr("Pan north")),
                keySetting(QStringLiteral("panWestKey"), tr("Pan west")),
                keySetting(QStringLiteral("panSouthKey"), tr("Pan south")),
                keySetting(QStringLiteral("panEastKey"), tr("Pan east")),
                keySetting(QStringLiteral("zoomInKey"), tr("Zoom in")),
                keySetting(QStringLiteral("zoomOutKey"), tr("Zoom out")),
                keySetting(QStringLiteral("resetViewKey"), tr("Reset map view")),
                keySetting(QStringLiteral("zoomPlusKey"), tr("Zoom in (alternate)")),
                keySetting(QStringLiteral("zoomEqualKey"), tr("Zoom in (second alternate)")),
                keySetting(QStringLiteral("zoomMinusKey"), tr("Zoom out (alternate)")),
                keySetting(QStringLiteral("stepWestKey"), tr("Step west")),
                keySetting(QStringLiteral("stepEastKey"), tr("Step east")),
                keySetting(QStringLiteral("stepNorthKey"), tr("Step north")),
                keySetting(QStringLiteral("stepSouthKey"), tr("Step south")),
                numberSetting(QStringLiteral("panSpeed"), tr("Keyboard pan speed (pixels per second)"), 30, 2000),
                booleanSetting(QStringLiteral("wheelZoom"), tr("Mouse wheel zoom")),
                booleanSetting(QStringLiteral("wheelReverse"), tr("Reverse mouse wheel zoom")),
                numberSetting(QStringLiteral("wheelSteps"), tr("Zoom levels per wheel step"), 1, 4),
                numberSetting(QStringLiteral("prefetchMargin"), tr("Nearby tile rows (0 disables preloading)"), 0, 2),
                numberSetting(QStringLiteral("tileCacheMiB"), tr("Tile memory cache (MiB)"), 64, 512),
                numberSetting(QStringLiteral("diskCacheMiB"), tr("Tile disk cache (MiB)"), 16, 4096),
                numberSetting(QStringLiteral("missingOpacity"), tr("Missing tile marker opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("opacity"), tr("Provider opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("attributionOpacity"), tr("Attribution background opacity"), 0, 1, 2),
                booleanSetting(QStringLiteral("smooth"), tr("Smooth image scaling")),
                booleanSetting(QStringLiteral("antialias"), tr("Smooth vector edges"))};
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        return readKeys(values) ? QString{} : tr("Use distinct single keys without modifiers. Use letters, digits, arrows, Home, +, =, -, or F1 through F12. Leave optional keys empty to disable them.");
    }
    QJsonObject savePreferences() const override
    {
        QJsonObject values{{QStringLiteral("informationGeometry"), QString::fromLatin1((information_ ? information_->saveGeometry() : informationGeometry_).toBase64())}, {QStringLiteral("centerX"), view_.center.x()}, {QStringLiteral("centerY"), view_.center.y()},
                {QStringLiteral("zoom"), view_.zoom}, {QStringLiteral("tileCacheMiB"), tileCacheMiB_}, {QStringLiteral("diskCacheMiB"), diskCacheMiB_}, {QStringLiteral("opacity"), opacity_},
                {QStringLiteral("missingOpacity"), missingOpacity_},
                {QStringLiteral("attributionOpacity"), attributionOpacity_}, {QStringLiteral("smooth"), smooth_}, {QStringLiteral("antialias"), antialias_},
                {QStringLiteral("panSpeed"), panSpeed_}, {QStringLiteral("wheelZoom"), wheelZoom_},
                {QStringLiteral("wheelReverse"), wheelReverse_}, {QStringLiteral("wheelSteps"), wheelSteps_},
                {QStringLiteral("prefetchMargin"), prefetchMargin_}};
        for (size_t i = 0; i < KeyPaths.size(); ++i) values.insert(KeyPaths[i], QKeySequence(keys_[i]).toString(QKeySequence::PortableText));
        return values;
    }
    void restorePreferences(const QJsonObject &values) override
    {
        stopPanning();
        keys_ = readKeys(values).value_or(DefaultKeys);
        panSpeed_ = preferenceNumber(values, QStringLiteral("panSpeed"), 300, 30, 2000);
        wheelZoom_ = values.value(QStringLiteral("wheelZoom")).toBool(true);
        wheelReverse_ = values.value(QStringLiteral("wheelReverse")).toBool(false);
        wheelSteps_ = static_cast<int>(preferenceNumber(values, QStringLiteral("wheelSteps"), 1, 1, 4));
        prefetchMargin_ = static_cast<int>(preferenceNumber(values, QStringLiteral("prefetchMargin"), 1, 0, 2));
        tiles_.setPrefetchMargin(prefetchMargin_);
        informationGeometry_ = QByteArray::fromBase64(values.value(QStringLiteral("informationGeometry")).toString().left(8192).toLatin1());
        view_.center = {preferenceNumber(values, QStringLiteral("centerX"), 0.5, 0, 1),
                        preferenceNumber(values, QStringLiteral("centerY"), 0.5, 0, 1)};
        view_.zoom = preferenceNumber(values, QStringLiteral("zoom"), 1, 0, 19);
        normalizeZoom();
        clampCenter();
        wheelRemainder_ = 0;
        tileCacheMiB_ = static_cast<int>(preferenceNumber(values, QStringLiteral("tileCacheMiB"), 256, 64, 512));
        tiles_.setMemoryLimit(tileCacheMiB_);
        diskCacheMiB_ = static_cast<int>(preferenceNumber(values, QStringLiteral("diskCacheMiB"), 128, 16, 4096));
        tiles_.setDiskLimit(diskCacheMiB_);
        missingOpacity_ = preferenceNumber(values, QStringLiteral("missingOpacity"), 0.2, 0, 1);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 1, 0, 1);
        attributionOpacity_ = preferenceNumber(values, QStringLiteral("attributionOpacity"), 0.55, 0, 1);
        smooth_ = values.value(QStringLiteral("smooth")).toBool(true);
        antialias_ = values.value(QStringLiteral("antialias")).toBool(true);
        update();
    }
protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const QPoint position = event->reason() == QContextMenuEvent::Keyboard ? rect().center() : event->pos();
        const auto features = tiles_.featuresAt(position);
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("mapContextMenu"));
        auto *information = menu.addAction(tr("Map information"));
        information->setObjectName(QStringLiteral("mapInformation"));
        information->setEnabled(!features.isEmpty());
        connect(information, &QAction::triggered, this, [this, features] { showInformation(features); });
        auto names = tiles_.layers();
        if (!names.isEmpty() && sources_) {
            auto *layers = menu.addMenu(tr("Map layers"));
            const QCollator collator;
            std::sort(names.begin(), names.end(), [&](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
            for (const auto &name : names) {
                auto *action = layers->addAction(name);
                action->setCheckable(true);
                action->setChecked(vectorLayerVisible(sources_->activeSource(), name));
                connect(action, &QAction::triggered, this, [this, name](bool visible) {
                    auto source = sources_->activeSource();
                    if (source.isEmpty()) return;
                    auto visibility = source.value(QStringLiteral("visibleLayers")).toObject();
                    visibility.insert(name, visible);
                    source.insert(QStringLiteral("visibleLayers"), visibility);
                    sources_->put(sources_->activeId(), source);
                });
            }
        }
        menu.exec(mapToGlobal(position));
        event->accept();
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::WindowDeactivate) stopPanning();
        return QOpenGLWidget::event(event);
    }
    void focusOutEvent(QFocusEvent *event) override { stopPanning(); QOpenGLWidget::focusOutEvent(event); }
    void hideEvent(QHideEvent *event) override { stopPanning(); tiles_.stop(); QOpenGLWidget::hideEvent(event); }
    void resizeEvent(QResizeEvent *event) override
    {
        QOpenGLWidget::resizeEvent(event);
        view_.size = size(); normalizeZoom(); clampCenter(); update();
    }
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
            if (!tilesPainted && layer->order() > 0) { tiles_.paint(painter, view_, opacity_, smooth_, missingOpacity_); tilesPainted = true; if (dataSources_) dataSources_->paint(painter, view_); }
            painter.save();
            layer->paint(painter, view_, store_);
            painter.restore();
        }
        if (!tilesPainted) { tiles_.paint(painter, view_, opacity_, smooth_, missingOpacity_); if (dataSources_) dataSources_->paint(painter, view_); }
        if (selection_) selection_->paint(painter, view_);
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        painter.setPen(QColor("#c7d7e5"));
        const QPointF coord = view_.coordinate(cursor_);
        const bool onWorld = coord.x() >= -180 && coord.x() <= 180 && coord.y() >= -85.051129 && coord.y() <= 85.051129;
        const QString position = onWorld ? tr("%1°, %2°").arg(coord.y(), 0, 'f', 4).arg(coord.x(), 0, 'f', 4)
                                         : tr("Pointer outside map");
        painter.drawText(QRect(16, height() - 30, width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter,
                         tr("%1  |  Zoom %2").arg(position).arg(view_.zoom, 0, 'f', 0));
        const auto cacheBytes = tiles_.diskCacheBytes();
        cacheLabel_->setText(cacheBytes < 0 ? tr("Map cache: inactive")
            : tr("Map cache: %1").arg(QLocale().formattedDataSize(cacheBytes)));
        cacheLabel_->adjustSize();
        cacheLabel_->move(width() - 12 - cacheLabel_->width(), height() - 32 - cacheLabel_->height());
        QStringList credits;
        if (!source.isEmpty()) credits.append(source.value(QStringLiteral("attribution")).toString());
        if (dataSources_) credits.append(dataSources_->attributions());
        attribution_->setVisible(!credits.isEmpty());
        if (!credits.isEmpty()) {
            QString text;
            QString tooltip;
            int naturalWidth = 0;
            for (auto credit : credits) {
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
                int creditWidth = attribution_->fontMetrics().horizontalAdvance(credit);
                QString line = credit.toHtmlEscaped();
                const bool linked = !urls.isEmpty();
                if (linked) {
                    line = QStringLiteral("<a style=\"color: white\" href=\"%1\">%2</a>")
                        .arg(urls.first().toString(QUrl::FullyEncoded).toHtmlEscaped(), line);
                    for (qsizetype i = 1; i < urls.size(); ++i) {
                        const auto label = tr("Source %1").arg(i + 1);
                        line += QStringLiteral(" | <a style=\"color: white\" href=\"%1\">%2</a>")
                            .arg(urls.at(i).toString(QUrl::FullyEncoded).toHtmlEscaped(), label.toHtmlEscaped());
                        creditWidth += attribution_->fontMetrics().horizontalAdvance(QStringLiteral(" | ") + label);
                    }
                }
                if (!text.isEmpty()) text += QStringLiteral("<br>");
                text += line;
                naturalWidth = std::max(naturalWidth, creditWidth);
                if (tooltip.isEmpty() && linked) tooltip = urls.first().toDisplayString();
            }
            if (source.value(QStringLiteral("ignoreTls")).toBool(false)) {
                const auto warning = tr("HTTPS certificate checks disabled");
                text += QStringLiteral("<br>") + warning.toHtmlEscaped();
                naturalWidth = std::max(naturalWidth, attribution_->fontMetrics().horizontalAdvance(warning));
            }
            if (!tiles_.status().isEmpty()) text += QStringLiteral("<br>") + tiles_.status().toHtmlEscaped();
            attribution_->setText(text);
            attribution_->setToolTip(tooltip);
            const auto style = QStringLiteral("QLabel { color: white; background-color: rgba(16,32,44,%1); padding: 4px; }")
                .arg(qRound(attributionOpacity_ * 255));
            if (attribution_->styleSheet() != style) attribution_->setStyleSheet(style);
            naturalWidth = std::max(naturalWidth, attribution_->fontMetrics().horizontalAdvance(tiles_.status()));
            const int panelWidth = std::min(width() - 24, naturalWidth + 16);
            const int panelHeight = attribution_->heightForWidth(panelWidth);
            attribution_->setGeometry(width() - 12 - panelWidth, cacheLabel_->y() - 2 - panelHeight, panelWidth, panelHeight);
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
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) { QOpenGLWidget::mouseDoubleClickEvent(event); return; }
        view_.center += (event->position() - QPointF(width() / 2.0, height() / 2.0)) / view_.scale();
        clampCenter();
        dragging_ = false;
        moved_ = true;
        cursor_ = event->position();
        unsetCursor();
        event->accept();
        update();
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
        if (!closest.isEmpty()) {
            const auto selectedEvent = std::find_if(store_.events().begin(), store_.events().end(), [&](const Event &item) { return item.uid == closest; });
            if (selectedEvent != store_.events().end() && selection_) {
                MapItem item; item.label = selectedEvent->callsign.isEmpty() ? selectedEvent->uid : selectedEvent->callsign; item.type = selectedEvent->type;
                item.point = project(selectedEvent->latitude, selectedEvent->longitude); item.color = Qt::white;
                item.properties = {{QStringLiteral("UID"), selectedEvent->uid}, {QStringLiteral("How"), selectedEvent->how},
                    {QStringLiteral("Latitude"), selectedEvent->latitude}, {QStringLiteral("Longitude"), selectedEvent->longitude},
                    {QStringLiteral("Altitude"), selectedEvent->altitude}, {QStringLiteral("Circular error"), selectedEvent->circularError}, {QStringLiteral("Linear error"), selectedEvent->linearError},
                    {QStringLiteral("Time"), selectedEvent->time.toString(Qt::ISODateWithMs)}, {QStringLiteral("Start"), selectedEvent->start.toString(Qt::ISODateWithMs)}, {QStringLiteral("Stale"), selectedEvent->stale.toString(Qt::ISODateWithMs)}};
                selectedSource_ = QStringLiteral("cot"); selection_->select(std::move(item));
            }
        } else {
            auto item = dataSources_ ? dataSources_->itemAt(view_, event->position()) : std::optional<MapItem>{};
            selectedSource_ = QStringLiteral("data");
            if (!item) { item = tiles_.itemAt(event->position()); selectedSource_ = QStringLiteral("vector"); }
            if (selection_) { if (item) selection_->select(std::move(*item)); else selection_->clear(); }
        }
        store_.select(closest);
        update();
    }
    void wheelEvent(QWheelEvent *event) override
    {
        if (!wheelZoom_ || !QRectF(rect()).contains(event->position())) { wheelRemainder_ = 0; event->ignore(); return; }
        if (event->phase() == Qt::ScrollBegin) wheelRemainder_ = 0;
        const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
        wheelRemainder_ += std::clamp(delta, -1200, 1200);
        const int steps = wheelRemainder_ / 120;
        wheelRemainder_ %= 120;
        if (steps) zoomAt(event->position(), steps * wheelSteps_ * (wheelReverse_ ? -1 : 1));
        if (event->phase() == Qt::ScrollEnd) wheelRemainder_ = 0;
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (heldPan_.contains(event->key())) {
            if (!event->isAutoRepeat()) { heldPan_.remove(event->key()); syncPanTimer(); }
            event->accept();
            return;
        }
        QOpenGLWidget::keyReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
        const bool plainKey = modifiers == Qt::NoModifier || (event->key() == Qt::Key_Plus && modifiers == Qt::ShiftModifier);
        if (plainKey && std::find(keys_.begin(), keys_.begin() + 4, event->key()) != keys_.begin() + 4) {
            if (!event->isAutoRepeat()) { heldPan_.insert(event->key()); syncPanTimer(); }
            event->accept();
            return;
        }
        if (!plainKey) {
            QOpenGLWidget::keyPressEvent(event); return;
        }
        const auto match = std::find(keys_.begin() + 4, keys_.end(), event->key());
        if (match == keys_.end()) { QOpenGLWidget::keyPressEvent(event); return; }
        switch (std::distance(keys_.begin(), match)) {
        case 4: case 7: case 8: zoomAt(rect().center(), 1); break;
        case 5: case 9: zoomAt(rect().center(), -1); break;
        case 6: view_.center = {0.5, 0.5}; view_.zoom = 1; normalizeZoom(); wheelRemainder_ = 0; break;
        case 10: view_.center.rx() -= 64 / view_.scale(); break;
        case 11: view_.center.rx() += 64 / view_.scale(); break;
        case 12: view_.center.ry() -= 64 / view_.scale(); break;
        case 13: view_.center.ry() += 64 / view_.scale(); break;
        default: break;
        }
        event->accept();
        clampCenter();
        update();
    }
private:
    static std::optional<std::array<int, 14>> readKeys(const QJsonObject &values)
    {
        std::array<int, 14> keys{};
        QSet<int> used;
        for (size_t i = 0; i < KeyPaths.size(); ++i) {
            const auto text = values.value(KeyPaths[i]).toString(QKeySequence(DefaultKeys[i]).toString(QKeySequence::PortableText));
            if (i >= 4 && text.isEmpty()) continue;
            const int key = navigationKey(text);
            // Keep older custom pan keys when new defaults overlap them.
            if (i >= 4 && !values.contains(KeyPaths[i]) && used.contains(key)) continue;
            if (!key || used.contains(key)) return std::nullopt;
            keys[i] = key;
            used.insert(key);
        }
        return keys;
    }
    QPointF panDirection() const
    {
        const double x = int(heldPan_.contains(keys_[3])) - int(heldPan_.contains(keys_[1]));
        const double y = int(heldPan_.contains(keys_[2])) - int(heldPan_.contains(keys_[0]));
        const double length = std::hypot(x, y);
        return length > 0 ? QPointF(x / length, y / length) : QPointF{};
    }
    void syncPanTimer()
    {
        if (panDirection().isNull()) { panTimer_.stop(); return; }
        if (!panTimer_.isActive()) { panClock_.start(); panTimer_.start(); }
    }
    void stopPanning() { heldPan_.clear(); panTimer_.stop(); }
    void showInformation(const QList<QJsonObject> &features)
    {
        if (information_) information_->close();
        auto *dialog = new QDialog(this);
        information_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("mapInformationDialog"));
        dialog->setWindowTitle(tr("Map information"));
        dialog->resize(520, 420);
        auto *layout = new QVBoxLayout(dialog);
        auto *text = new QPlainTextEdit(dialog);
        text->setObjectName(QStringLiteral("mapFeatureProperties"));
        text->setReadOnly(true);
        QString details;
        const QCollator collator;
        for (const auto &feature : features) {
            details += tr("Layer: %1\nFeature ID: %2\n").arg(feature.value(QStringLiteral("layer")).toString(), feature.value(QStringLiteral("id")).toString());
            const auto properties = feature.value(QStringLiteral("properties")).toObject();
            auto keys = properties.keys();
            std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
            for (const auto &key : keys) {
                const auto value = properties.value(key);
                details += key + QStringLiteral(": ") + (value.isString() ? value.toString() : value.isBool() ?
                    (value.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : QString::number(value.toDouble(), 'g', 16)) + QLatin1Char('\n');
            }
            details += QLatin1Char('\n');
        }
        text->setPlainText(details);
        layout->addWidget(text);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        if (!informationGeometry_.isEmpty()) dialog->restoreGeometry(informationGeometry_);
        connect(dialog, &QDialog::finished, this, [this, dialog] { informationGeometry_ = dialog->saveGeometry(); });
        dialog->show();
    }
    std::pair<int, int> zoomLimits() const
    {
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        const int fit = std::clamp(static_cast<int>(std::ceil(std::log2(std::max(width(), height()) / 256.0))), 0, 19);
        const int minimum = std::max(fit, source.value(QStringLiteral("minZoom")).toInt());
        const int maximum = std::max(minimum, source.isEmpty() ? 19 : source.value(QStringLiteral("maxZoom")).toInt());
        return {minimum, maximum};
    }
    void normalizeZoom()
    {
        const auto [minimum, maximum] = zoomLimits();
        view_.zoom = std::clamp(std::round(view_.zoom), static_cast<double>(minimum), static_cast<double>(maximum));
    }
    void clampCenter()
    {
        const double halfWidth = std::min(0.5, width() / (2 * view_.scale()));
        const double halfHeight = std::min(0.5, height() / (2 * view_.scale()));
        view_.center.setX(std::clamp(view_.center.x(), halfWidth, 1 - halfWidth));
        view_.center.setY(std::clamp(view_.center.y(), halfHeight, 1 - halfHeight));
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
    MapSelection *selection_ = nullptr;
    QString selectedSource_;
    QTimer panTimer_;
    QElapsedTimer panClock_;
    QSet<int> heldPan_;
    std::array<int, 14> keys_ = DefaultKeys;
    double panSpeed_ = 300;
    bool wheelZoom_ = true;
    bool wheelReverse_ = false;
    int wheelSteps_ = 1;
    int prefetchMargin_ = 1;
    QPointer<QDialog> information_;
    QByteArray informationGeometry_;
    MapSources *sources_ = nullptr;
    TileRenderer tiles_;
    QLabel *attribution_;
    QLabel *cacheLabel_;
    double attributionOpacity_ = 0.55;
    double opacity_ = 1;
    int tileCacheMiB_ = 256;
    int diskCacheMiB_ = 128;
    double missingOpacity_ = 0.2;
    int wheelRemainder_ = 0;
    bool smooth_ = true;
    bool antialias_ = true;
    Store &store_;
    DataSources *dataSources_ = nullptr;
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
        widget->setDataSources(services.data);
        widget->setSelection(services.selection);
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
