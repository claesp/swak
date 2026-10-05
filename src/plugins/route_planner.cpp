#include "core/delete_confirmation.h"
#include "core/interfaces.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/measure.h"
#include "core/provider_table.h"
#include "core/routing.h"
#include "core/settings_dialog.h"

#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>

namespace swak {
class RouteTool final : public MapTool
{
public:
    explicit RouteTool(QObject *parent) : MapTool(parent) {}
    std::function<void(QPointF)> picked;
    bool picking = false;
    QPainterPath path;
    double width = 3;
    QString attribution;
    void activate(QWidget &) override
    {
        picking = true;
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
        : PaneContent(parent), sources_(*services.routes), tool_(new RouteTool(this))
    {
        setObjectName(QStringLiteral("routePlanner"));
        if (services.mapTools) {
            services.mapTools->add(tool_);
        }
        auto *layout = new QVBoxLayout(this);
        auto *notice = new QLabel(tr("Choose two points and a travel mode. Calculate sends those "
                                     "points to the selected route provider."),
                                  this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        providers_ = new QComboBox(this);
        providers_->setObjectName(QStringLiteral("routeProviderChoice"));
        layout->addWidget(providers_);
        auto *manage = new QPushButton(tr("Route providers ..."), this);
        layout->addWidget(manage);
        connect(manage, &QPushButton::clicked, this, [this] {
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Route Providers"));
            auto *box = new QVBoxLayout(&dialog);
            box->addWidget(createSettingsWidget(&dialog));
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            box->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            dialog.exec();
        });
        auto *form = new QFormLayout;
        for (int i = 0; i < 4; ++i) {
            points_[i] = new QDoubleSpinBox(this);
            points_[i]->setRange(i % 2 == 0 ? -85.051129 : -180, i % 2 == 0 ? 85.051129 : 180);
            points_[i]->setDecimals(6);
            points_[i]->setObjectName(QStringLiteral("routeCoordinate%1").arg(i));
            const QStringList names{tr("Start latitude"),
                                    tr("Start longitude"),
                                    tr("End latitude"),
                                    tr("End longitude")};
            form->addRow(names[i], points_[i]);
            connect(points_[i], &QDoubleSpinBox::valueChanged, this, [this] {
                invalidate();
            });
        }
        layout->addLayout(form);
        auto *pickRow = new QHBoxLayout;
        for (int i = 0; i < 2; ++i) {
            auto *pick = new QPushButton(i == 0 ? tr("Pick start") : tr("Pick end"), this);
            pick->setObjectName(i == 0 ? QStringLiteral("routePickStart")
                                       : QStringLiteral("routePickEnd"));
            pickRow->addWidget(pick);
            connect(pick, &QPushButton::clicked, this, [this, i] {
                tool_->picked = [this, i](QPointF point) {
                    points_[i * 2]->setValue(point.y());
                    points_[i * 2 + 1]->setValue(point.x());
                };
                emit tool_->activationRequested();
            });
        }
        layout->addLayout(pickRow);
        mode_ = new QComboBox(this);
        mode_->setObjectName(QStringLiteral("routeMode"));
        mode_->addItem(tr("Cycling"), QStringLiteral("cycling"));
        mode_->addItem(tr("Driving"), QStringLiteral("driving"));
        mode_->addItem(tr("Running"), QStringLiteral("running"));
        mode_->addItem(tr("Walking"), QStringLiteral("walking"));
        mode_->setCurrentIndex(1);
        layout->addWidget(mode_);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
            invalidate();
        });
        auto *row = new QHBoxLayout;
        auto *calculate = new QPushButton(tr("Calculate"), this);
        calculate->setObjectName(QStringLiteral("calculateRoute"));
        auto *clear = new QPushButton(tr("Cancel / clear"), this);
        row->addWidget(calculate);
        row->addWidget(clear);
        layout->addLayout(row);
        status_ = new QLabel(this);
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
            status_->setText(tr("Finding a distance-based route ..."));
            sources_.calculate({points_[1]->value(), points_[0]->value()},
                               {points_[3]->value(), points_[2]->value()},
                               mode_->currentData().toString(),
                               runningSpeed_);
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            tool_->cancel();
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
            refreshProviders();
            invalidate();
        });
        connect(providers_, &QComboBox::activated, this, [this] {
            sources_.select(providers_->currentData().toString());
        });
        refreshProviders();
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
        QJsonArray points;
        for (auto *point : points_) {
            points.append(point->value());
        }
        return {{"points", points},
                {"mode", mode_->currentData().toString()},
                {"runningSpeed", runningSpeed_},
                {"lineWidth", tool_->width}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto points = values.value("points").toArray();
        for (int i = 0; i < 4 && i < points.size(); ++i) {
            if (points[i].isDouble() && std::isfinite(points[i].toDouble())) {
                points_[i]->setValue(points[i].toDouble());
            }
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
        const auto refresh = [this, table] {
            const auto selected = table->selectedId();
            table->clearProviders();
            const auto entries = sources_.sources();
            auto ids = entries.keys();
            const QCollator collator;
            std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
                return collator.compare(entries[a].toObject().value("name").toString(),
                                        entries[b].toObject().value("name").toString()) < 0;
            });
            for (const auto &id : ids) {
                const auto source = entries[id].toObject();
                const auto *provider = sources_.provider(source);
                auto *button =
                    table->append(id,
                                  source.value("name").toString(),
                                  provider ? provider->routeName() : tr("Plugin unavailable"),
                                  source.value("endpoint").toString(),
                                  sources_.activeId() == id,
                                  provider != nullptr);
                connect(button, &QAbstractButton::clicked, table, [this, table, id] {
                    QTimer::singleShot(0, table, [this, id] {
                        sources_.select(id);
                    });
                });
            }
            table->selectId(selected);
        };
        connect(&sources_, &RouteSources::changed, table, refresh);
        refresh();
        auto *row = new QHBoxLayout;
        for (int operation = 0; operation < 3; ++operation) {
            auto *button = new QPushButton(
                QStringList{tr("Add"), tr("Edit"), tr("Delete ...")}[operation], widget);
            button->setObjectName(QStringList{QStringLiteral("addRouteProvider"),
                                              QStringLiteral("editRouteProvider"),
                                              QStringLiteral("removeRouteProvider")}[operation]);
            row->addWidget(button);
            connect(button, &QPushButton::clicked, widget, [this, table, operation, widget] {
                const auto id = operation == 0 ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                               : table->selectedId();
                if (id.isEmpty()) {
                    return;
                }
                if (operation == 2) {
                    if (confirmDeletion(widget, tr("this route provider"))) {
                        sources_.remove(id);
                    }
                    return;
                }
                QList<SettingsPage> pages;
                const auto original = sources_.sources().value(id).toObject();
                QString error;
                for (const auto *provider : sources_.providers()) {
                    if (operation == 1 && provider != sources_.provider(original)) {
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
                                  [this, id, &error](const QJsonObject &value) {
                                      error = sources_.put(id, value);
                                  }});
                }
                if (pages.isEmpty()) {
                    return;
                }
                SettingsDialog dialog(
                    pages,
                    [&error] {
                        return error;
                    },
                    widget,
                    true,
                    operation == 0);
                dialog.setWindowTitle(operation == 0 ? tr("Add route provider")
                                                     : tr("Edit route provider"));
                dialog.exec();
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
    void refreshProviders()
    {
        providers_->clear();
        const auto entries = sources_.sources();
        auto ids = entries.keys();
        const QCollator collator;
        std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
            return collator.compare(entries[a].toObject().value("name").toString(),
                                    entries[b].toObject().value("name").toString()) < 0;
        });
        for (const auto &id : ids) {
            providers_->addItem(entries[id].toObject().value("name").toString(), id);
        }
        providers_->setCurrentIndex(providers_->findData(sources_.activeId()));
        tool_->attribution =
            entries.value(sources_.activeId()).toObject().value("attribution").toString();
        attribution_->setText(
            entries.value(sources_.activeId()).toObject().value("attribution").toString());
    }
    RouteSources &sources_;
    RouteTool *tool_ = nullptr;
    QComboBox *providers_ = nullptr;
    QComboBox *mode_ = nullptr;
    QDoubleSpinBox *points_[4]{};
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
