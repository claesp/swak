#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/measure.h"
#include "core/modal.h"
#include "core/polygon.h"
#include "core/provider_table.h"
#include "core/routing.h"
#include "core/settings_dialog.h"
#include "core/text.h"
#include "core/toolbar.h"
#include "core/ui_icons.h"
#include "core/user_objects.h"

#include <QCollator>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
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
    void setObjects(UserObjects *objects)
    {
        objects_ = objects;
    }
    // Runs with the map coordinate of the next picked point.
    void setPicked(std::function<void(QPointF)> picked)
    {
        picked_ = std::move(picked);
    }
    // The provider attribution stays visible while the last planned route exists.
    void setAttribution(const QString &attribution)
    {
        attribution_ = attribution;
    }
    const QString &attribution() const
    {
        return attribution_;
    }
    void setRouteId(const QString &routeId)
    {
        routeId_ = routeId;
    }
    void activate(QWidget &target) override
    {
        picking_ = true;
        // The cross hair shows that the next map click sets a route point, as in Marker mode.
        map_ = &target;
        target.setCursor(Qt::CrossCursor);
        target.setFocus(Qt::OtherFocusReason);
        emit overlayChanged();
    }
    bool guidesActive() const override
    {
        return picking_;
    }
    bool interactionActive() const override
    {
        return picking_;
    }
    bool cancel() override
    {
        if (!picking_) {
            return false;
        }
        picking_ = false;
        if (map_) {
            map_->unsetCursor();
        }
        emit overlayChanged();
        return true;
    }
    bool handle(QEvent &event, const Viewport &view, QWidget &) override
    {
        if (!picking_) {
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
                if (picked_) {
                    picked_(point);
                }
            }
            return true;
        }
        return false;
    }
    QStringList attributions() const override
    {
        if (attribution_.isEmpty() || routeId_.isEmpty() || !objects_ ||
            !objects_->find(routeId_)) {
            return {};
        }
        return {attribution_};
    }

private:
    std::function<void(QPointF)> picked_;
    bool picking_ = false;
    QPointer<QWidget> map_;
    QString attribution_;
    QString routeId_;
    QPointer<UserObjects> objects_;
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
        tool_->setObjects(objects_);
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
                tool_->setPicked([this, i](QPointF point) {
                    endpoints_[i].mapPoint = point;
                    refreshEndpoints();
                    endpoints_[i].choice->setCurrentIndex(
                        endpoints_[i].choice->findData(QStringLiteral("map")));
                    invalidate();
                });
                emit tool_->activationRequested();
            });
        }
        layout->addLayout(form);
        auto *row = new QHBoxLayout;
        auto *calculate = new QPushButton(tr("Calculate"), this);
        calculate->setObjectName(QStringLiteral("calculateRoute"));
        auto *clear = new QPushButton(uiIcon(UiIcon::Clear), tr("Clear ..."), this);
        clear->setObjectName(QStringLiteral("clearRoute"));
        clear->setToolTip(tr("Clear the start, the destination, and the status. Route markers "
                             "stay on the map."));
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
        // The window adds this action to the main toolbar and the Plugins menu.
        auto *show = new QAction(uiIcon(UiIcon::RoutePlanning), tr("Route planning"), this);
        show->setObjectName(QStringLiteral("routePlanningAction"));
        show->setToolTip(tr("Route planning"));
        show->setCheckable(true);
        setToolbarSection(*show, ToolbarSection::Measurement, 4);
        connect(show, &QAction::triggered, this, [this](bool visible) {
            auto *dock = this->dock();
            if (!dock) {
                return;
            }
            dock->setVisible(visible);
            if (visible) {
                dock->raise();
                mode_->setFocus(Qt::OtherFocusReason);
            }
        });
        // The dock exists after the window places this pane.
        QTimer::singleShot(0, this, [this, show] {
            if (auto *dock = this->dock()) {
                const auto sync = [show, dock] {
                    show->setChecked(!dock->isHidden());
                };
                connect(dock, &QDockWidget::visibilityChanged, show, sync);
                sync();
            }
        });
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
            if (!confirmAction(
                    this, tr("Clear"), tr("Clear the start and the destination?"), tr("Clear"))) {
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
            const auto &result = sources_.result();
            if (!result.error.isEmpty()) {
                status_->setText(result.error);
                return;
            }
            const auto summary = tr("%1. Estimated time: %2 minutes. Server limits can affect "
                                    "shortest-path accuracy.")
                                     .arg(formatDistance(result.meters))
                                     .arg(QLocale().toString(result.seconds / 60, 'f', 1));
            const auto error = addRouteMarker(result);
            status_->setText(error.isEmpty() ? summary : error);
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
                {"runningSpeed", runningSpeed_}};
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
            const auto ids = idsByName(entries);
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
    struct Endpoint
    {
        QComboBox *choice = nullptr;
        QToolButton *pick = nullptr;
        // Longitude and latitude from the last map pick.
        std::optional<QPointF> mapPoint;
    };
    QDockWidget *dock() const
    {
        for (auto *widget = parentWidget(); widget; widget = widget->parentWidget()) {
            if (auto *found = qobject_cast<QDockWidget *>(widget)) {
                return found;
            }
        }
        return nullptr;
    }
    void invalidate()
    {
        sources_.cancel();
        if (status_) {
            status_->clear();
        }
        emit tool_->changed();
    }
    QString endpointName(const Endpoint &endpoint) const
    {
        const auto key = endpoint.choice->currentData().toString();
        if (key.startsWith(QStringLiteral("marker:")) && objects_) {
            if (const auto marker = objects_->find(key.mid(7))) {
                return marker->title;
            }
        }
        return tr("map point");
    }
    // Stores a planned route as a Route marker. The marker keeps the provider values.
    QString addRouteMarker(const RouteResult &result)
    {
        if (!objects_) {
            return tr("Route markers are not available in this window.");
        }
        QPolygonF points;
        for (const auto &coordinate : result.coordinates) {
            if (!std::isfinite(coordinate.x()) || !std::isfinite(coordinate.y()) ||
                std::abs(coordinate.y()) > 85.051129) {
                continue;
            }
            const auto point = project(coordinate.y(), coordinate.x());
            if (points.isEmpty() || points.last() != point) {
                points.append(point);
            }
        }
        points = simplifiedRoute(points);
        // Simplification can join the last points. Keep neighbors distinct.
        QPolygonF distinct;
        for (const auto &point : points) {
            if (distinct.isEmpty() || distinct.last() != point) {
                distinct.append(point);
            }
        }
        UserObject item;
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.type = QStringLiteral("route");
        item.title = tr("Route from %1 to %2")
                         .arg(endpointName(endpoints_[0]), endpointName(endpoints_[1]))
                         .left(128);
        item.color = QStringLiteral("#8b5cf6");
        item.geometry = polygonGeometry(distinct);
        item.description = attribution_->text().left(2048);
        item.properties.insert(tr("Travel mode"), mode_->currentText());
        item.properties.insert(tr("Provider distance"), formatDistance(result.meters));
        item.properties.insert(
            tr("Estimated time"),
            tr("%1 minutes").arg(QLocale().toString(result.seconds / 60, 'f', 1)));
        const auto provider =
            sources_.sources().value(sources_.activeId()).toObject().value("name").toString();
        if (!provider.isEmpty()) {
            item.properties.insert(tr("Route provider"), provider.left(256));
        }
        const auto error = objects_->put(item);
        if (error.isEmpty()) {
            tool_->setRouteId(item.id);
            emit tool_->changed();
        }
        return error;
    }
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
        tool_->setAttribution(source.value("attribution").toString());
        attribution_->setText(tool_->attribution());
        emit tool_->changed();
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
    QAction *primaryAction(QWidget *pane) override
    {
        return pane->findChild<QAction *>(QStringLiteral("routePlanningAction"));
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return services.routes ? new RoutePane(services, parent) : new QWidget(parent);
    }
};
} // namespace swak
#include "route_planner.moc"
