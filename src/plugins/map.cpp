#include "core/map_label.h"
#include "core/ui_icons.h"
#include "core/interfaces.h"
#include "core/tiles.h"
#include "core/map_tools.h"
#include "core/map_grid.h"
#include "core/marker_symbols.h"
#include "core/single_line_label.h"
#include "core/data.h"
#include <QHideEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QFrame>
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
const std::array<QString, 15> KeyPaths{QStringLiteral("panNorthKey"), QStringLiteral("panWestKey"), QStringLiteral("panSouthKey"), QStringLiteral("panEastKey"), QStringLiteral("zoomInKey"), QStringLiteral("zoomOutKey"), QStringLiteral("resetViewKey"), QStringLiteral("zoomPlusKey"), QStringLiteral("zoomEqualKey"), QStringLiteral("zoomMinusKey"),
    QStringLiteral("stepWestKey"), QStringLiteral("stepEastKey"), QStringLiteral("stepNorthKey"), QStringLiteral("stepSouthKey"), QStringLiteral("pauseMarkersKey")};
const std::array<int, 15> DefaultKeys{Qt::Key_Up, Qt::Key_Left, Qt::Key_Down, Qt::Key_Right, Qt::Key_Q, Qt::Key_E, Qt::Key_Home, Qt::Key_Plus, Qt::Key_Equal, Qt::Key_Minus, 0, 0, 0, 0, Qt::Key_Space};
int navigationKey(const QString &text)
{
    if (text.isEmpty() || text.size() > 32) return 0;
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].keyboardModifiers() != Qt::NoModifier) return 0;
    const int key = sequence[0].key();
    return ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F12) || (key >= Qt::Key_Left && key <= Qt::Key_Down) || key == Qt::Key_Space || key == Qt::Key_Home || key == Qt::Key_Plus || key == Qt::Key_Equal || key == Qt::Key_Minus) ? key : 0;
}
}
class ToolOverlay final : public QWidget {
public:
    ToolOverlay(MapTools &tools, const Viewport &view, QWidget *parent) : QWidget(parent), tools_(tools), view_(view) {
        setObjectName(QStringLiteral("mapToolOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        connect(&tools, &MapTools::changed, this, qOverload<>(&ToolOverlay::update));
        connect(&tools, &MapTools::overlayChanged, this, qOverload<>(&ToolOverlay::repaint));
    }
protected:
    void paintEvent(QPaintEvent *) override { QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); tools_.paintOverlay(painter, view_, mapFromGlobal(QCursor::pos())); }
private:
    MapTools &tools_;
    const Viewport &view_;
};
class MapWidget final : public QOpenGLWidget, public SettingsParticipant, public StatusProvider, protected QOpenGLFunctions {
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
        connect(&store_.motion(), &TrackMotion::changed, this, [this] { updateStatus(); update(); });
        connect(&store_, &Store::changed, this, [this] {
            if (store_.motion().paused()) { update(); return; }
            if (selection_ && selectedSource_ == QStringLiteral("cot") && selection_->item()) {
                auto item = *selection_->item();
                const auto &events = store_.events();
                const auto found = std::find_if(events.cbegin(), events.cend(), [&item](const Event &event) { return event.uid == item.id; });
                if (found == events.cend()) selection_->clear();
                else {
                    item.type = found->type; item.properties.insert(QStringLiteral("How"), found->how);
                    item.point = project(found->latitude, found->longitude); item.label = found->callsign.isEmpty() ? found->uid : found->callsign;
                    item.properties.insert(QStringLiteral("Latitude"), found->latitude); item.properties.insert(QStringLiteral("Longitude"), found->longitude);
                    item.properties.insert(QStringLiteral("Time"), found->time.toString(Qt::ISODateWithMs)); item.properties.insert(QStringLiteral("Stale"), found->stale.toString(Qt::ISODateWithMs));
                    selection_->select(std::move(item));
                }
            }
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
            tiles_.setSource(sources_->provider(source), source, sources_->credential(sources_->activeId()));
            update();
        };
        connect(sources_, &MapSources::changed, this, apply);
        apply();
    }
    void setMapTools(MapTools *tools) {
        tools_ = tools;
        if (tools_) connect(tools_, &MapTools::activationRequested, this, [this](MapTool *tool) {
            tools_->cancel(); tool->activate(*this); setFocus(Qt::OtherFocusReason);
        });
        if (tools_) { overlay_ = new ToolOverlay(*tools_, view_, this); overlay_->setGeometry(rect()); overlay_->show(); attribution_->raise(); }
        if (tools_) connect(tools_, &MapTools::changed, this, qOverload<>(&MapWidget::update));
    }
    QWidget *createStatusWidget(QWidget *parent) override
    {
        auto *status = new QWidget(parent);
        status->setObjectName(QStringLiteral("mapStatusSections"));
        auto *layout = new QHBoxLayout(status);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        const auto section = [status, layout](const QString &id, const QString &name, bool separator, int stretch) {
            auto *container = new QWidget(status);
            auto *row = new QHBoxLayout(container);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(6);
            if (separator) {
                auto *line = new QFrame(container);
                line->setObjectName(QStringLiteral("mapStatusSeparator"));
                line->setFrameShape(QFrame::VLine);
                line->setFrameShadow(QFrame::Sunken);
                row->addWidget(line);
            }
            auto *label = new SingleLineLabel(container);
            label->setObjectName(id);
            label->setAccessibleName(name);
            label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            row->addWidget(label);
            layout->addWidget(container, stretch);
            return label;
        };
        cacheStatus_ = section(QStringLiteral("mapStatus"), tr("Map cache"), false, 0);
        pauseStatus_ = section(QStringLiteral("mapPauseStatus"), tr("CoT position updates"), true, 0);
        zoomStatus_ = section(QStringLiteral("mapZoomStatus"), tr("Map zoom"), true, 0);
        pointerStatus_ = section(QStringLiteral("mapPointerStatus"), tr("Map pointer coordinates"), true, 1);
        tileStatus_ = section(QStringLiteral("mapTileStatus"), tr("Map tile status"), true, 1);
        tlsStatus_ = section(QStringLiteral("mapTlsStatus"), tr("Map HTTPS warning"), true, 0);
        updateStatus();
        return status;
    }

    QString preferenceId() const override { return QStringLiteral("org.swak.map"); }
    QString settingsTitle() const override { return tr("Map"); }
    QList<SettingField> settingsFields() const override
    {
        return {numberSetting(QStringLiteral("centerX"), tr("Horizontal center (0–1)"), 0, 1, 6),
                numberSetting(QStringLiteral("centerY"), tr("Vertical center (0–1)"), 0, 1, 6),
                numberSetting(QStringLiteral("zoom"), tr("Zoom"), zoomLimits().first, zoomLimits().second),
                keySetting(QStringLiteral("pauseMarkersKey"), tr("Pause or resume CoT positions")),
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
                booleanSetting(QStringLiteral("gridVisible"), tr("Coordinate grid in marker mode")),
                numberSetting(QStringLiteral("gridOpacity"), tr("Coordinate grid opacity"), 0, 1, 2),
                numberSetting(QStringLiteral("gridSpacing"), tr("Coordinate grid spacing (pixels)"), 80, 400),
                booleanSetting(QStringLiteral("smooth"), tr("Smooth image scaling")),
                booleanSetting(QStringLiteral("antialias"), tr("Smooth vector edges"))};
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        return readKeys(values) ? QString{} : tr("Use distinct single keys without modifiers. Use letters, digits, arrows, Space, Home, +, =, -, or F1 through F12. Leave optional keys empty to disable them.");
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
        values.insert(QStringLiteral("navigationKeysVersion"), 2);
        values.insert(QStringLiteral("gridStyleVersion"), 2); values.insert(QStringLiteral("gridVisible"), gridVisible_); values.insert(QStringLiteral("gridOpacity"), gridOpacity_); values.insert(QStringLiteral("gridSpacing"), gridSpacing_);
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
        gridVisible_ = values.value(QStringLiteral("gridVisible")).toBool(true);
        gridOpacity_ = preferenceNumber(values, QStringLiteral("gridOpacity"), 0.5, 0, 1);
        if (!values.contains(QStringLiteral("gridStyleVersion")) && gridOpacity_ == 0.2) gridOpacity_ = 0.5;
        gridSpacing_ = preferenceNumber(values, QStringLiteral("gridSpacing"), 140, 80, 400);
        smooth_ = values.value(QStringLiteral("smooth")).toBool(true);
        antialias_ = values.value(QStringLiteral("antialias")).toBool(true);
        update();
    }
protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (tools_ && tools_->handle(*event, view_, *this)) { event->accept(); return; }
        const QPoint position = event->reason() == QContextMenuEvent::Keyboard ? rect().center() : event->pos();
        if (chooseObject(position, *event)) { event->accept(); return; }
        const auto features = tiles_.featuresAt(position);
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("mapContextMenu"));
        auto *information = menu.addAction(tr("Map information"));
        information->setObjectName(QStringLiteral("mapInformation"));
        information->setEnabled(!features.isEmpty());
        connect(information, &QAction::triggered, this, [this, features] { showInformation(features); });
        addLayerMenu(menu);
        menu.exec(mapToGlobal(position));
        event->accept();
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::WindowDeactivate) stopPanning();
        return QOpenGLWidget::event(event);
    }
    void focusOutEvent(QFocusEvent *event) override { stopPanning(); QOpenGLWidget::focusOutEvent(event); }
    void hideEvent(QHideEvent *event) override { stopPanning(); if (tools_) tools_->cancel(); tiles_.stop(); QOpenGLWidget::hideEvent(event); }
    void resizeEvent(QResizeEvent *event) override
    {
        QOpenGLWidget::resizeEvent(event);
        if (overlay_) overlay_->setGeometry(rect());
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
        const auto paintBase = [&] {
            tiles_.paint(painter, view_, opacity_, smooth_, missingOpacity_);
            if (gridVisible_ && tools_ && tools_->coordinateGridActive()) paintMapGrid(painter, view_, gridOpacity_, gridSpacing_);
            if (dataSources_) dataSources_->paint(painter, view_, selection_ && selection_->item() ? &*selection_->item() : nullptr);
        };
        bool tilesPainted = false;
        for (const auto *layer : layers_) {
            if (!tilesPainted && layer->order() > 0) { paintBase(); tilesPainted = true; }
            painter.save();
            layer->paint(painter, view_, store_);
            painter.restore();
        }
        if (!tilesPainted) paintBase();
        if (overlay_) overlay_->update();
        if (tools_) tools_->paint(painter, view_);
        if (selection_) {
            if (selectedSource_ == QStringLiteral("cot") && selection_->item()) {
                const auto visuals = store_.motion().snapshot();
                const auto found = std::find_if(visuals.cbegin(), visuals.cend(), [this](const auto &visual) { return visual.event.uid == selection_->item()->id && !visual.expired; });
                if (found != visuals.cend()) selection_->paint(painter, view_, found->world, found->opacity);
            } else selection_->paint(painter, view_);
        }
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        painter.setPen(QColor("#c7d7e5"));
        updateStatus();
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
            attribution_->setText(text);
            attribution_->setToolTip(tooltip);
            const auto style = QStringLiteral("QLabel { color: white; background-color: rgba(16,32,44,%1); padding: 4px; }")
                .arg(qRound(attributionOpacity_ * 255));
            if (attribution_->styleSheet() != style) attribution_->setStyleSheet(style);
            const int panelWidth = std::min(width() - 24, naturalWidth + 16);
            const int panelHeight = attribution_->heightForWidth(panelWidth);
            attribution_->setGeometry(width() - 12 - panelWidth, height() - 8 - panelHeight, panelWidth, panelHeight);
        }
        if (hasFocus()) {
            painter.setPen(QPen(QColor("#5fd6ff"), 2));
            painter.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (tools_ && tools_->handle(*event, view_, *this)) { dragging_ = false; moved_ = false; event->accept(); return; }
        if (event->button() != Qt::LeftButton) return;
        dragging_ = true;
        moved_ = false;
        last_ = event->position();
        press_ = last_;
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (tools_ && tools_->handle(*event, view_, *this)) { event->accept(); return; }
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
        if (tools_ && tools_->handle(*event, view_, *this)) { updateStatus(); event->accept(); return; }
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
        if (tools_ && tools_->handle(*event, view_, *this)) { event->accept(); update(); return; }
        if (!chooseObject(event->position(), *event)) {
            if (selection_) selection_->clear();
            store_.select({});
        }
        update();
    }
    void addLayerMenu(QMenu &menu)
    {
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
    }
    struct Hit {
        MapItem item;
        QString source;
    };
    QList<Hit> objectsAt(QPointF position) const
    {
        QList<Hit> hits;
        if (tools_) for (const auto &item : tools_->itemsAt(view_, position)) hits.append({item, QStringLiteral("local")});
        const auto now = QDateTime::currentDateTimeUtc();
        int labels = 0;
        for (const auto &visual : store_.motion().snapshot()) {
            const auto &message = visual.event;
            const auto screen = (visual.world - view_.center) * view_.scale() + QPointF(view_.size.width() / 2, view_.size.height() / 2);
            if (message.start > now) continue;
            const bool visible = QRectF(QPointF{}, view_.size).adjusted(-12, -12, 12, 12).contains(screen);
            const bool labelVisible = visible && (message.uid == store_.selected() || labels++ < 100);
            const auto label = (message.callsign.isEmpty() ? message.uid : message.callsign).left(32);
            const bool labelHit = labelVisible && mapLabelBounds(fontMetrics(), screen + QPointF(16, -8), label).contains(position);
            if (visual.expired || visual.opacity < 0.15 || message.stale <= now || (QLineF(screen, position).length() >= 14 && !labelHit)) continue;
            MapItem item; item.owner = QStringLiteral("org.swak.events"); item.label = message.callsign.isEmpty() ? message.uid : message.callsign; item.type = message.type;
            item.point = project(message.latitude, message.longitude); item.color = Qt::white;
            item.symbol = markerSymbolForCot(message.type);
            item.properties = {{QStringLiteral("UID"), message.uid}, {QStringLiteral("How"), message.how},
                {QStringLiteral("Latitude"), message.latitude}, {QStringLiteral("Longitude"), message.longitude},
                {QStringLiteral("Altitude"), message.altitude}, {QStringLiteral("Circular error"), message.circularError}, {QStringLiteral("Linear error"), message.linearError},
                {QStringLiteral("Time"), message.time.toString(Qt::ISODateWithMs)}, {QStringLiteral("Start"), message.start.toString(Qt::ISODateWithMs)}, {QStringLiteral("Stale"), message.stale.toString(Qt::ISODateWithMs)}};
            item.id = message.uid;
            hits.append({std::move(item), QStringLiteral("cot")});
        }
        if (dataSources_) for (const auto &item : dataSources_->itemsAt(view_, position)) hits.append({item, QStringLiteral("data")});
        for (const auto &item : tiles_.itemsAt(position)) hits.append({item, QStringLiteral("vector")});
        return hits;
    }
    void closeOnContentChange(QMenu &menu)
    {
        connect(&store_, &QAbstractItemModel::rowsRemoved, &menu, &QMenu::close);
        connect(&store_, &QAbstractItemModel::modelReset, &menu, &QMenu::close);
        if (tools_) connect(tools_, &MapTools::itemsChanged, &menu, &QMenu::close);
        if (dataSources_) connect(dataSources_, &DataSources::contentChanged, &menu, &QMenu::close);
        if (sources_) connect(sources_, &MapSources::changed, &menu, &QMenu::close);
    }
    void selectHit(const Hit &hit)
    {
        selectedSource_ = hit.source;
        if (selection_) selection_->select(hit.item);
        store_.select(hit.source == QStringLiteral("cot") ? hit.item.id : QString{});
    }
    bool chooseObject(QPointF position, QEvent &event)
    {
        const auto hits = objectsAt(position);
        if (hits.isEmpty()) return false;
        qsizetype chosen = 0;
        if (hits.size() > 1) {
            QMenu menu(this); menu.setObjectName(QStringLiteral("mapObjectChooser")); menu.setToolTipsVisible(true);
            chosen = -1;
            closeOnContentChange(menu);
            // Build each page only when it opens. Dense overlaps keep a small menu.
            const auto fillPage = [&](auto &&self, QMenu *page, qsizetype start) -> void {
                const auto end = std::min(start + 30, hits.size());
                for (auto i = start; i < end; ++i) {
                    const auto &hit = hits.at(i);
                    auto label = hit.item.label.isEmpty() ? tr("Unnamed object") : hit.item.label;
                    auto detail = hit.item.type;
                    if (hit.source == QStringLiteral("cot")) detail += tr(" / CoT message");
                    if (hit.source == QStringLiteral("data")) detail += tr(" / Data provider");
                    if (hit.source == QStringLiteral("vector")) detail += tr(" / Map feature");
                    const auto text = label.left(100);
                    auto *action = page->addAction(hit.item.icon.isNull() ? uiIcon(hit.item.kind == MapItem::Kind::Polygon ? UiIcon::Area : UiIcon::Marker) : hit.item.icon, QString(text).replace('&', QStringLiteral("&&")));
                    action->setToolTip(label + QStringLiteral(" (%1)").arg(detail));
                    action->setData(hit.item.id);
                    connect(action, &QAction::triggered, &menu, [&, i] { chosen = i; menu.close(); });
                }
                if (end < hits.size()) {
                    auto *more = page->addMenu(tr("More objects")); more->setToolTipsVisible(true);
                    connect(more, &QMenu::aboutToShow, &menu, [&, more, end] { if (more->actions().isEmpty()) self(self, more, end); });
                }
            };
            fillPage(fillPage, &menu, 0);
            menu.exec(mapToGlobal(position.toPoint()));
            if (chosen < 0) return true;
        }
        const auto &hit = hits.at(chosen);
        if (hit.source == QStringLiteral("local")) {
            selectedSource_.clear();
            if (tools_) tools_->interact(hit.item, event, *this);
        } else if (event.type() == QEvent::ContextMenu) {
            QMenu menu(this); menu.setObjectName(QStringLiteral("mapItemContextMenu"));
            auto *information = menu.addAction(tr("Information")); information->setObjectName(QStringLiteral("mapItemInformation"));
            if (tools_) tools_->populateContextMenu(hit.item, menu);
            if (hit.source == QStringLiteral("vector")) {
                menu.setObjectName(QStringLiteral("mapContextMenu")); information->setObjectName(QStringLiteral("mapInformation"));
                information->setText(tr("Map information"));
                addLayerMenu(menu);
                connect(information, &QAction::triggered, &menu, [this, hit] {
                    auto properties = hit.item.properties; properties.insert(QStringLiteral("name"), hit.item.label);
                    properties.insert(QStringLiteral("description"), hit.item.description);
                    showInformation({QJsonObject{{QStringLiteral("layer"), hit.item.type}, {QStringLiteral("id"), properties.value(QStringLiteral("Feature ID"))}, {QStringLiteral("properties"), properties}}});
                });
            } else connect(information, &QAction::triggered, &menu, [this, hit] { selectHit(hit); });
            menu.exec(mapToGlobal(position.toPoint()));
        } else selectHit(hit);
        return true;
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
        if (event->key() == keys_[14] && event->modifiers() == Qt::NoModifier) {
            if (!event->isAutoRepeat()) store_.motion().setPaused(!store_.motion().paused());
            event->accept(); return;
        }
        if (tools_ && tools_->handle(*event, view_, *this)) { stopPanning(); event->accept(); update(); return; }
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
    void updateStatus()
    {
        const auto source = sources_ ? sources_->activeSource() : QJsonObject{};
        const QPointF coord = view_.coordinate(cursor_);
        const bool onWorld = coord.x() >= -180 && coord.x() <= 180 && coord.y() >= -85.051129 && coord.y() <= 85.051129;
        const QString position = onWorld ? tr("%1°, %2°").arg(coord.y(), 0, 'f', 4).arg(coord.x(), 0, 'f', 4)
                                         : tr("Pointer outside map");
        const auto setText = [](const QPointer<QLabel> &label, const QString &text, bool optional = false) {
            if (!label) return;
            if (label->text() != text) label->setText(text);
            if (optional) label->parentWidget()->setVisible(!text.isEmpty());
        };
        const auto cacheBytes = tiles_.diskCacheBytes();
        setText(cacheStatus_, cacheBytes < 0 ? tr("Map cache: inactive") : tr("Map cache: %1").arg(QLocale().formattedDataSize(cacheBytes)));
        setText(pauseStatus_, store_.motion().paused() ? tr("CoT positions paused") : QString{}, true);
        setText(zoomStatus_, tr("Zoom %1").arg(view_.zoom, 0, 'f', 0));
        setText(pointerStatus_, position);
        setText(tileStatus_, tiles_.status(), true);
        setText(tlsStatus_, source.value(QStringLiteral("ignoreTls")).toBool(false) ? tr("HTTPS certificate checks disabled") : QString{}, true);
    }

    static std::optional<std::array<int, 15>> readKeys(QJsonObject values)
    {
        if (values.value(QStringLiteral("navigationKeysVersion")).toInt() < 2) {
            const std::array<int, 4> oldPan{Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D};
            const std::array<int, 4> oldStep{Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down};
            for (size_t i = 0; i < 4; ++i) {
                if (values.value(KeyPaths[i]).toString() == QKeySequence(oldPan[i]).toString(QKeySequence::PortableText))
                    values.insert(KeyPaths[i], QKeySequence(DefaultKeys[i]).toString(QKeySequence::PortableText));
                if (values.value(KeyPaths[i + 10]).toString() == QKeySequence(oldStep[i]).toString(QKeySequence::PortableText)) values.insert(KeyPaths[i + 10], QString{});
            }
        }
        std::array<int, 15> keys{};
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
    std::array<int, 15> keys_ = DefaultKeys;
    double panSpeed_ = 300;
    bool wheelZoom_ = true;
    bool wheelReverse_ = false;
    int wheelSteps_ = 1;
    int prefetchMargin_ = 1;
    QPointer<QDialog> information_;
    QByteArray informationGeometry_;
    ToolOverlay *overlay_ = nullptr;
    bool gridVisible_ = true;
    double gridOpacity_ = 0.5;
    double gridSpacing_ = 140;
    MapTools *tools_ = nullptr;
    MapSources *sources_ = nullptr;
    TileRenderer tiles_;
    QLabel *attribution_;
    QPointer<QLabel> pauseStatus_, cacheStatus_, zoomStatus_, pointerStatus_, tileStatus_, tlsStatus_;
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
        widget->setMapTools(services.mapTools);
        return widget;
    }
    void populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *) override
    {
        auto *mapSources = &services.maps;
        auto *mapButton = new QToolButton(&toolbar);
        mapButton->setObjectName(QStringLiteral("mapProviderButton"));
        mapButton->setAccessibleName(tr("Select map provider"));
        mapButton->setPopupMode(QToolButton::InstantPopup);
        mapButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mapButton->setIcon(uiIcon(UiIcon::Map));
        auto *mapMenu = new QMenu(mapButton);
        mapMenu->setObjectName(QStringLiteral("mapProviderMenu"));
        mapButton->setMenu(mapMenu);
        toolbar.addWidget(mapButton);
        const auto updateMapButton = [mapButton, mapSources] {
            const auto source = mapSources->activeSource();
            mapButton->setText(source.isEmpty() ? tr("No map provider") : source.value(QStringLiteral("name")).toString());
            mapButton->setToolTip(tr("Map provider: %1").arg(mapButton->text()));
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
