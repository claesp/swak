#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/measure.h"
#include "core/modal.h"
#include "core/provider_table.h"
#include "core/routing.h"
#include "core/settings_dialog.h"
#include "core/ui_icons.h"
#include "core/user_objects.h"

#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

namespace swak {
class RouteTool final : public MapTool
{
public:
    explicit RouteTool(QObject *parent) : MapTool(parent) {}
    std::function<void(QPointF)> picked;
    bool picking = false;
    QPointer<QWidget> map;
    QPainterPath path;
    double width = 3;
    QString attribution;
    void activate(QWidget &target) override
    {
        picking = true;
        // The cross hair shows that the next map click sets a route point, as in Marker mode.
        map = &target;
        target.setCursor(Qt::CrossCursor);
        target.setFocus(Qt::OtherFocusReason);
        emit overlayChanged();
    }
    bool guidesActive() const override
    {
        return picking;
    }
    bool interactionActive() const override
    {
        return picking;
    }
    bool cancel() override
    {
        if (!picking) {
            return false;
        }
        picking = false;
        if (map) {
            map->unsetCursor();
        }
        emit overlayChanged();
        return true;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &) override
    {
        if (!picking) {
            return false;
        }
        if (event.type() == QEvent::KeyPress &&
            static_cast<QKeyEvent &>(event).key() == Qt::Key_Escape) {
            return cancel();
        }
        if (event.type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
            return true;
        }
        if (event.type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent &>(event).button() == Qt::LeftButton) {
            const auto point = view.coordinate(static_cast<QMouseEvent &>(event).position());
            if (std::abs(point.x()) <= 180 && std::abs(point.y()) <= 85.051129) {
                cancel();
                if (picked) {
                    picked(point);
                }
            }
            return true;
        }
        return false;
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        if (path.isEmpty()) {
            return;
        }
        QTransform transform;
        transform.translate(view.size.width() / 2, view.size.height() / 2);
        transform.scale(view.scale(), view.scale());
        transform.translate(-view.center.x(), -view.center.y());
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#8b5cf6"), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(transform.map(path));
        painter.restore();
        painter.save();
        painter.setPen(Qt::white);
        const QRectF attributionBox(
            80, view.size.height() - 80, std::max(1.0, view.size.width() - 160), 45);
        const auto textBounds =
            painter.boundingRect(attributionBox, Qt::AlignCenter | Qt::TextWordWrap, attribution);
        painter.fillRect(textBounds.adjusted(-5, -3, 5, 3), QColor(0, 0, 0, 140));
        painter.drawText(attributionBox, Qt::AlignCenter | Qt::TextWordWrap, attribution);
        painter.restore();
    }
};
class RoutePane final : public PaneContent,
                        public SettingsParticipant,
                        public SettingsWidgetProvider
{
public:
    RoutePane(PluginServices &services, QWidget *parent)
        : PaneContent(parent), sources_(*services.routes), objects_(services.objects),
          tool_(new RouteTool(this))
    {
        setObjectName(QStringLiteral("routePlanner"));
        if (services.mapTools) {
            services.mapTools->add(tool_);
        }
        auto *layout = new QVBoxLayout(this);
        mode_ = new QComboBox(this);
        mode_->setObjectName(QStringLiteral("routeMode"));
        mode_->setAccessibleName(tr("Travel mode"));
        mode_->addItem(tr("Cycling"), QStringLiteral("cycling"));
        mode_->addItem(tr("Driving"), QStringLiteral("driving"));
        mode_->addItem(tr("Running"), QStringLiteral("running"));
        mode_->addItem(tr("Walking"), QStringLiteral("walking"));
        mode_->setCurrentIndex(1);
        layout->addWidget(mode_);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
            invalidate();
        });
        auto *notice = new QLabel(tr("Choose a start and a destination. Calculate sends them to "
                                     "the route provider selected in Settings."),
                                  this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto *form = new QFormLayout;
        for (int i = 0; i < 2; ++i) {
            auto &endpoint = endpoints_[i];
            const auto name =
                i == 0 ? QStringLiteral("routeStart") : QStringLiteral("routeDestination");
            endpoint.choice = new QComboBox(this);
            endpoint.choice->setObjectName(name);
            endpoint.choice->setAccessibleName(i == 0 ? tr("Start") : tr("Destination"));
            endpoint.choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            endpoint.choice->setMinimumContentsLength(12);
            endpoint.pick = new QToolButton(this);
            endpoint.pick->setObjectName(name + QStringLiteral("Pick"));
            endpoint.pick->setIcon(uiIcon(UiIcon::Target));
            endpoint.pick->setAccessibleName(i == 0 ? tr("Pick the start on the map")
                                                    : tr("Pick the destination on the map"));
            endpoint.pick->setToolTip(endpoint.pick->accessibleName());
            auto *row = new QHBoxLayout;
            row->addWidget(endpoint.choice, 1);
            row->addWidget(endpoint.pick);
            form->addRow(i == 0 ? tr("Start") : tr("Destination"), row);
            connect(endpoint.choice, &QComboBox::activated, this, [this] {
                invalidate();
            });
            connect(endpoint.pick, &QToolButton::clicked, this, [this, i] {
                tool_->picked = [this, i](QPointF point) {
                    endpoints_[i].mapPoint = point;
                    refreshEndpoints();
                    endpoints_[i].choice->setCurrentIndex(
                        endpoints_[i].choice->findData(QStringLiteral("map")));
                    invalidate();
                };
                emit tool_->activationRequested();
            });
        }
        layout->addLayout(form);
        auto *row = new QHBoxLayout;
        auto *calculate = new QPushButton(tr("Calculate"), this);
        calculate->setObjectName(QStringLiteral("calculateRoute"));
        auto *clear = new QPushButton(uiIcon(UiIcon::Clear), tr("Clear ..."), this);
        clear->setObjectName(QStringLiteral("clearRoute"));
        clear->setToolTip(tr("Clear the route, the start, and the destination."));
        row->addWidget(calculate);
        row->addWidget(clear);
        layout->addLayout(row);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("routeStatus"));
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        attribution_ = new QLabel(this);
        attribution_->setTextFormat(Qt::PlainText);
        attribution_->setWordWrap(true);
        layout->addWidget(attribution_);
        layout->addStretch();
        connect(calculate, &QPushButton::clicked, this, [this] {
            if (sources_.busy()) {
                return;
            }
            invalidate();
            const auto start = coordinate(endpoints_[0]);
            const auto destination = coordinate(endpoints_[1]);
            if (!start || !destination) {
                status_->setText(tr("Choose a start and a destination."));
                return;
            }
            status_->setText(tr("Finding a distance-based route ..."));
            sources_.calculate(
                *start, *destination, mode_->currentData().toString(), runningSpeed_);
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            if (!confirmAction(this,
                               tr("Clear"),
                               tr("Clear the route, the start, and the destination?"),
                               tr("Clear"))) {
                return;
            }
            tool_->cancel();
            for (auto &endpoint : endpoints_) {
                endpoint.mapPoint.reset();
                endpoint.choice->setCurrentIndex(0);
            }
            refreshEndpoints();
            invalidate();
        });
        connect(&sources_, &RouteSources::completed, this, [this] {
            tool_->path = {};
            const auto &result = sources_.result();
            status_->setText(result.error.isEmpty()
                                 ? tr("%1. Estimated time: %2 minutes. Server limits can affect "
                                      "shortest-path accuracy.")
                                       .arg(formatDistance(result.meters))
                                       .arg(QLocale().toString(result.seconds / 60, 'f', 1))
                                 : result.error);
            QPointF previous;
            for (int i = 0; i < result.coordinates.size(); ++i) {
                const auto point = project(result.coordinates[i].y(), result.coordinates[i].x());
                if (i == 0 || std::abs(point.x() - previous.x()) > 0.5) {
                    tool_->path.moveTo(point);
                } else {
                    tool_->path.lineTo(point);
                }
                previous = point;
            }
            emit tool_->changed();
        });
        connect(&sources_, &RouteSources::changed, this, [this] {
            refreshAttribution();
            invalidate();
        });
        if (objects_) {
            connect(objects_, &UserObjects::changed, this, [this] {
                refreshEndpoints();
            });
        }
        refreshEndpoints();
        refreshAttribution();
    }
    Qt::DockWidgetArea preferredDockArea() const override
    {
        return Qt::RightDockWidgetArea;
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.route-planner");
    }
    QString settingsTitle() const override
    {
        return tr("Route providers");
    }
    QList<SettingField> settingsFields() const override
    {
        return {};
    }
    QJsonObject savePreferences() const override
    {
        return {{"start", endpointJson(endpoints_[0])},
                {"destination", endpointJson(endpoints_[1])},
                {"mode", mode_->currentData().toString()},
                {"runningSpeed", runningSpeed_},
                {"lineWidth", tool_->width}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto points = values.value("points").toArray();
        for (int i = 0; i < 2; ++i) {
            auto &endpoint = endpoints_[i];
            endpoint.mapPoint.reset();
            QString selected;
            const auto saved = values.value(i == 0 ? "start" : "destination").toObject();
            const auto marker = saved.value("marker").toString();
            const double latitude = saved.value("latitude").toDouble(NAN);
            const double longitude = saved.value("longitude").toDouble(NAN);
            if (!marker.isEmpty() && marker.size() <= 128) {
                selected = QStringLiteral("marker:") + marker;
            } else if (validCoordinate(longitude, latitude)) {
                endpoint.mapPoint = QPointF(longitude, latitude);
                selected = QStringLiteral("map");
            } else if (points.size() == 4) {
                // Earlier versions saved four coordinate fields. Zero pairs were the defaults.
                const double oldLatitude = points[i * 2].toDouble(NAN);
                const double oldLongitude = points[i * 2 + 1].toDouble(NAN);
                if (validCoordinate(oldLongitude, oldLatitude) &&
                    (oldLatitude != 0 || oldLongitude != 0)) {
                    endpoint.mapPoint = QPointF(oldLongitude, oldLatitude);
                    selected = QStringLiteral("map");
                }
            }
            refreshEndpoints();
            const int index = endpoint.choice->findData(selected);
            endpoint.choice->setCurrentIndex(index < 0 ? 0 : index);
        }
        const int mode = mode_->findData(values.value("mode").toString(QStringLiteral("driving")));
        mode_->setCurrentIndex(mode < 0 ? 1 : mode);
        runningSpeed_ = preferenceNumber(values, QStringLiteral("runningSpeed"), 10, 0.5, 25);
        tool_->width = preferenceNumber(values, QStringLiteral("lineWidth"), 3, 1, 8);
        invalidate();
    }
    QWidget *createSettingsWidget(QWidget *parent) override
    {
        auto *widget = new QWidget(parent);
        auto *layout = new QVBoxLayout(widget);
        auto *notice =
            new QLabel(tr("Providers receive route coordinates only after Calculate. The public "
                          "demo has fair-use limits. Use your own server for regular work."),
                       widget);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto *table = new ProviderTable(true, widget);
        table->setObjectName(QStringLiteral("routeProviderTable"));
        layout->addWidget(table);
        // The window Settings dialog can own this widget after the pane is gone.
        // The callbacks use the route service, which outlives both, and never this pane.
        auto &sources = sources_;
        const auto refresh = [&sources, table] {
            const auto selected = table->selectedId();
            table->clearProviders();
            const auto entries = sources.sources();
            auto ids = entries.keys();
            const QCollator collator;
            std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
                return collator.compare(entries[a].toObject().value("name").toString(),
                                        entries[b].toObject().value("name").toString()) < 0;
            });
            for (const auto &id : ids) {
                const auto source = entries[id].toObject();
                const auto *provider = sources.provider(source);
                auto *button =
                    table->append(id,
                                  source.value("name").toString(),
                                  provider ? provider->routeName() : tr("Plugin unavailable"),
                                  source.value("endpoint").toString(),
                                  sources.activeId() == id,
                                  provider != nullptr);
                connect(button, &QAbstractButton::clicked, table, [&sources, table, id] {
                    QTimer::singleShot(0, table, [&sources, id] {
                        sources.select(id);
                    });
                });
            }
            table->selectId(selected);
        };
        connect(&sources, &RouteSources::changed, table, refresh);
        refresh();
        auto *row = new QHBoxLayout;
        for (int operation = 0; operation < 3; ++operation) {
            auto *button = new QPushButton(
                QStringList{tr("Add"), tr("Edit"), tr("Delete ...")}[operation], widget);
            button->setObjectName(QStringList{QStringLiteral("addRouteProvider"),
                                              QStringLiteral("editRouteProvider"),
                                              QStringLiteral("removeRouteProvider")}[operation]);
            row->addWidget(button);
            connect(button, &QPushButton::clicked, widget, [&sources, table, operation, widget] {
                const auto id = operation == 0 ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                               : table->selectedId();
                if (id.isEmpty()) {
                    return;
                }
                if (operation == 2) {
                    if (confirmDeletion(widget, tr("this route provider"))) {
                        sources.remove(id);
                    }
                    return;
                }
                QList<SettingsPage> pages;
                const auto original = sources.sources().value(id).toObject();
                QString error;
                for (const auto *provider : sources.providers()) {
                    if (operation == 1 && provider != sources.provider(original)) {
                        continue;
                    }
                    auto values =
                        operation == 0
                            ? QJsonObject{{"name", ""},
                                          {"endpoint", ""},
                                          {"attribution", "OpenStreetMap contributors (ODbL)"}}
                            : original;
                    values.insert("format", provider->routeId());
                    pages.append({provider->routeId(),
                                  provider->routeName(),
                                  RouteSources::fields(),
                                  values,
                                  RouteSources::validate,
                                  [&sources, id, &error](const QJsonObject &value) {
                                      error = sources.put(id, value);
                                  }});
                }
                if (pages.isEmpty()) {
                    return;
                }
                Modal<SettingsDialog> dialog(
                    pages,
                    [&error] {
                        return error;
                    },
                    widget,
                    true,
                    operation == 0);
                dialog->setWindowTitle(operation == 0 ? tr("Add route provider")
                                                      : tr("Edit route provider"));
                dialog->exec();
            });
        }
        layout->addLayout(row);
        auto *form = new QFormLayout;
        auto *speed = new QDoubleSpinBox(widget);
        speed->setRange(0.5, 25);
        speed->setValue(runningSpeed_);
        form->addRow(tr("Running speed (km/h)"), speed);
        connect(speed, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            runningSpeed_ = value;
            invalidate();
        });
        auto *width = new QDoubleSpinBox(widget);
        width->setRange(1, 8);
        width->setValue(tool_->width);
        form->addRow(tr("Route width (pixels)"), width);
        connect(width, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            tool_->width = value;
            emit tool_->changed();
        });
        layout->addLayout(form);
        for (auto *label : widget->findChildren<QLabel *>()) {
            if (label->buddy()) {
                label->setWordWrap(false);
                label->setMinimumWidth(label->sizeHint().width());
            }
        }
        return widget;
    }

private:
    void invalidate()
    {
        sources_.cancel();
        tool_->path = {};
        if (status_) {
            status_->clear();
        }
        emit tool_->changed();
    }
    struct Endpoint
    {
        QComboBox *choice = nullptr;
        QToolButton *pick = nullptr;
        // Longitude and latitude from the last map pick.
        std::optional<QPointF> mapPoint;
    };
    static bool validCoordinate(double longitude, double latitude)
    {
        return std::isfinite(longitude) && std::isfinite(latitude) && std::abs(longitude) <= 180 &&
               std::abs(latitude) <= 85.051129;
    }
    static QJsonObject endpointJson(const Endpoint &endpoint)
    {
        const auto key = endpoint.choice->currentData().toString();
        if (key.startsWith(QStringLiteral("marker:"))) {
            return {{"marker", key.mid(7)}};
        }
        if (key == QStringLiteral("map") && endpoint.mapPoint) {
            return {{"longitude", endpoint.mapPoint->x()}, {"latitude", endpoint.mapPoint->y()}};
        }
        return {};
    }
    // Markers are local objects with one point position.
    static bool isMarker(const UserObject &item)
    {
        return item.geometry.value(QStringLiteral("x")).isDouble() &&
               item.geometry.value(QStringLiteral("y")).isDouble();
    }
    std::optional<QPointF> coordinate(const Endpoint &endpoint) const
    {
        const auto key = endpoint.choice->currentData().toString();
        if (key == QStringLiteral("map")) {
            return endpoint.mapPoint;
        }
        if (!key.startsWith(QStringLiteral("marker:")) || !objects_) {
            return std::nullopt;
        }
        const auto marker = objects_->find(key.mid(7));
        if (!marker || !isMarker(*marker)) {
            return std::nullopt;
        }
        const auto location = unproject({marker->geometry.value(QStringLiteral("x")).toDouble(),
                                         marker->geometry.value(QStringLiteral("y")).toDouble()});
        return validCoordinate(location.x(), location.y()) ? std::optional<QPointF>(location)
                                                           : std::nullopt;
    }
    void refreshEndpoints()
    {
        QList<std::pair<QString, QString>> markers;
        if (objects_) {
            for (const auto &item : objects_->items()) {
                if (isMarker(item)) {
                    markers.append({item.title.isEmpty() ? item.type : item.title, item.id});
                }
            }
        }
        const QCollator collator;
        std::sort(markers.begin(), markers.end(), [&](const auto &a, const auto &b) {
            return collator.compare(a.first, b.first) < 0;
        });
        for (auto &endpoint : endpoints_) {
            const auto selected = endpoint.choice->currentData().toString();
            const QSignalBlocker block(endpoint.choice);
            endpoint.choice->clear();
            endpoint.choice->addItem(tr("Choose a marker or a map point"), QString{});
            if (endpoint.mapPoint) {
                endpoint.choice->addItem(uiIcon(UiIcon::Target),
                                         tr("Map point %1, %2")
                                             .arg(endpoint.mapPoint->y(), 0, 'f', 5)
                                             .arg(endpoint.mapPoint->x(), 0, 'f', 5),
                                         QStringLiteral("map"));
            }
            for (const auto &[title, id] : markers) {
                endpoint.choice->addItem(
                    uiIcon(UiIcon::Marker), title, QStringLiteral("marker:") + id);
            }
            const int index = endpoint.choice->findData(selected);
            endpoint.choice->setCurrentIndex(index < 0 ? 0 : index);
        }
    }
    void refreshAttribution()
    {
        const auto source = sources_.sources().value(sources_.activeId()).toObject();
        tool_->attribution = source.value("attribution").toString();
        attribution_->setText(tool_->attribution);
    }
    RouteSources &sources_;
    QPointer<UserObjects> objects_;
    RouteTool *tool_ = nullptr;
    QComboBox *mode_ = nullptr;
    Endpoint endpoints_[2];
    QLabel *status_ = nullptr;
    QLabel *attribution_ = nullptr;
    double runningSpeed_ = 10;
};
class RoutePlannerPlugin final : public QObject,
                                 public Plugin,
                                 public PanePlugin,
                                 public MapExtension
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "route_planner/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin swak::MapExtension)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.route-planner");
    }
    QString mapName() const override
    {
        return name();
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
    QString name() const override
    {
        return tr("Route planning");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return services.routes ? new RoutePane(services, parent) : new QWidget(parent);
    }
};
} // namespace swak
#include "route_planner.moc"
