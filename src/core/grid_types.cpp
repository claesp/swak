#include "core/grid_types.h"

#include "core/delete_confirmation.h"
#include "core/map_grid.h"
#include "core/modal.h"
#include "core/provider_table.h"
#include "core/settings_dialog.h"
#include "core/single_line_label.h"

#include <QCollator>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
namespace swak {
namespace {
QString systemName(const QString &id)
{
    return id == QLatin1String("utm")    ? QObject::tr("UTM")
           : id == QLatin1String("mgrs") ? QObject::tr("MGRS")
                                         : QObject::tr("Longitude/latitude");
}
QList<SettingField> fields()
{
    QList<SettingField> result{
        textSetting(QStringLiteral("name"), QObject::tr("Name"), 80),
        choiceSetting(QStringLiteral("system"),
                      QObject::tr("Coordinate system"),
                      {systemName("geographic"), systemName("utm"), systemName("mgrs")},
                      {"geographic", "utm", "mgrs"}),
        textSetting(QStringLiteral("color"), QObject::tr("Color (#RRGGBB)"), 7),
        numberSetting(QStringLiteral("opacity"), QObject::tr("Opacity"), 0.05, 1, 2),
        numberSetting(QStringLiteral("spacing"), QObject::tr("Target spacing (pixels)"), 80, 400),
        numberSetting(QStringLiteral("width"), QObject::tr("Line width (pixels)"), .5, 3, 1),
        choiceSetting(QStringLiteral("format"),
                      QObject::tr("Angle labels"),
                      {QObject::tr("Decimal degrees"),
                       QObject::tr("Degrees and minutes"),
                       QObject::tr("Degrees, minutes, and seconds")},
                      {0, 1, 2})};
    result.last().enabledBy = QStringLiteral("system");
    result.last().enabledValue = QStringLiteral("geographic");
    return result;
}
} // namespace
GridTypes::GridTypes(QObject *parent) : QObject(parent)
{
    for (const auto &id :
         {QStringLiteral("geographic"), QStringLiteral("utm"), QStringLiteral("mgrs")}) {
        entries_.insert(id, defaults(id));
    }
}
QJsonObject GridTypes::defaults(const QString &system)
{
    return {{"name", systemName(system)},
            {"system", system},
            {"color", "#000000"},
            {"opacity", .3},
            {"spacing", 140},
            {"width", 1},
            {"format", 0}};
}
QString GridTypes::validate(const QJsonObject &value)
{
    const auto name = value.value("name").toString();
    if (name.trimmed().isEmpty() || name.size() > 80 ||
        std::any_of(name.begin(), name.end(), [](QChar c) {
            return !c.isPrint() || c.category() == QChar::Other_Format ||
                   c.category() == QChar::Other_Surrogate;
        })) {
        return tr("Use a name with 1 to 80 visible characters.");
    }
    if (!QStringList{"geographic", "utm", "mgrs"}.contains(value.value("system").toString()) ||
        !QRegularExpression(QStringLiteral("^#[0-9A-Fa-f]{6}$"))
             .match(value.value("color").toString())
             .hasMatch()) {
        return tr("Choose a coordinate system and a color in #RRGGBB format.");
    }
    for (const auto &field : fields()) {
        if (field.kind == SettingField::Kind::Number) {
            const auto v = value.value(field.path);
            const double n = v.toDouble();
            if (!v.isDouble() || !std::isfinite(n) || n < field.minimum || n > field.maximum) {
                return tr("Use values within the shown limits.");
            }
        }
    }
    const auto format = value.value("format");
    if (!format.isDouble() || format.toDouble() != format.toInt(-1) || format.toInt() < 0 ||
        format.toInt() > 2) {
        return tr("Choose an angle label format.");
    }
    return {};
}
QJsonObject GridTypes::savePreferences() const
{
    return {{"types", entries_}, {"selected", selected_}, {"lastSelected", lastSelected_}};
}
void GridTypes::restorePreferences(const QJsonObject &values)
{
    restored_ = true;
    if (values.value("types").isObject()) {
        entries_ = {};
        const auto types = values.value("types").toObject();
        int count = 0;
        for (auto it = types.begin(); it != types.end() && count < 32; ++it, ++count) {
            if (!it.key().isEmpty() && it.key().size() <= 64 &&
                validate(it.value().toObject()).isEmpty()) {
                entries_.insert(it.key(), it.value());
            }
        }
    }
    selected_ = values.value("selected").toString(QStringLiteral("geographic"));
    if (!entries_.contains(selected_)) {
        selected_.clear();
    }
    lastSelected_ = values.value("lastSelected")
                        .toString(selected_.isEmpty() ? QStringLiteral("geographic") : selected_);
    if (!entries_.contains(lastSelected_)) {
        lastSelected_.clear();
    }
    emit changed();
}
QString GridTypes::put(const QString &id, const QJsonObject &value)
{
    if (id.isEmpty() || id.size() > 64 || (!entries_.contains(id) && entries_.size() >= 32)) {
        return tr("Use at most 32 grid types.");
    }
    const auto error = validate(value);
    if (!error.isEmpty()) {
        return error;
    }
    restored_ = true;
    entries_.insert(id, value);
    emit changed();
    return {};
}
void GridTypes::remove(const QString &id)
{
    restored_ = true;
    entries_.remove(id);
    if (selected_ == id) {
        selected_.clear();
    }
    emit changed();
}
void GridTypes::select(const QString &id)
{
    restored_ = true;
    selected_ = entries_.contains(id) ? id : QString{};
    if (!selected_.isEmpty()) {
        lastSelected_ = selected_;
    }
    emit changed();
}
void GridTypes::setVisible(bool visible)
{
    if (!visible || entries_.isEmpty()) {
        select({});
        return;
    }
    select(entries_.contains(lastSelected_)  ? lastSelected_
           : entries_.contains("geographic") ? QStringLiteral("geographic")
                                             : entries_.begin().key());
}
void GridTypes::migrate(const QJsonObject &mapValues)
{
    if (restored_ || !mapValues.contains("gridVisible")) {
        return;
    }
    restored_ = true;
    auto value = defaults();
    value.insert("opacity", preferenceNumber(mapValues, "gridOpacity", .3, .05, 1));
    value.insert("spacing", preferenceNumber(mapValues, "gridSpacing", 140, 80, 400));
    if (!mapValues.contains("gridStyleVersion") && value.value("opacity").toDouble() == .2) {
        value.insert("opacity", .3);
    }
    entries_.insert("geographic", value);
    if (!mapValues.value("gridVisible").toBool(true)) {
        selected_.clear();
    }
    emit changed();
}
void GridTypes::paint(QPainter &painter, const Viewport &view) const
{
    if (!selected_.isEmpty()) {
        paintGridType(painter, view, entries_.value(selected_).toObject());
    }
}
QWidget *GridTypes::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);
    auto *table = new ProviderTable(true, widget);
    table->setObjectName(QStringLiteral("gridTypesTable"));
    table->setHorizontalHeaderLabels({tr("Active"), tr("Name"), tr("Coordinate system")});
    layout->addWidget(table);
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    auto *status = new SingleLineLabel(widget);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    const auto refresh = [this, table, widget] {
        const auto selectedRow = table->selectedId();
        table->clearProviders();
        const QCollator collator;
        const auto &records = entries();
        auto ids = records.keys();
        std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
            return collator.compare(records.value(a).toObject().value("name").toString(),
                                    records.value(b).toObject().value("name").toString()) < 0;
        });
        for (const auto &id : ids) {
            const auto value = entries_.value(id).toObject();
            auto *button = table->append(id,
                                         value.value("name").toString(),
                                         systemName(value.value("system").toString()),
                                         {},
                                         id == selected_,
                                         true);
            connect(button, &QAbstractButton::clicked, widget, [this, id] {
                select(id);
            });
        }
        table->selectId(selectedRow);
    };
    connect(this, &GridTypes::changed, widget, refresh, Qt::QueuedConnection);
    const auto edit = [this, widget, table](bool add) {
        const auto id =
            add ? QUuid::createUuid().toString(QUuid::WithoutBraces) : table->selectedId();
        if (id.isEmpty()) {
            return;
        }
        auto value = add ? defaults() : entries_.value(id).toObject();
        if (add) {
            value.insert("name", "");
        }
        Modal<SettingsDialog> dialog(
            QList<SettingsPage>{{QStringLiteral("grid"),
                                 add ? tr("Add grid type") : tr("Edit grid type"),
                                 fields(),
                                 value,
                                 [this, id](const QJsonObject &v) {
                                     if (!entries_.contains(id) && entries_.size() >= 32) {
                                         return tr("Use at most 32 grid types.");
                                     }
                                     return validate(v);
                                 },
                                 [this, id](const QJsonObject &v) {
                                     put(id, v);
                                 }}},
            [] {
                return QString{};
            },
            widget,
            true,
            add);
        dialog->setObjectName(QStringLiteral("gridTypeEditor"));
        dialog->exec();
    };
    const auto button = [&](const QString &name, const QString &text, auto callback) {
        auto *b = new QPushButton(text, widget);
        b->setObjectName(name);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, widget, callback);
    };
    button("addGridType", tr("Add"), [edit] {
        edit(true);
    });
    button("editGridType", tr("Edit"), [edit] {
        edit(false);
    });
    button("deleteGridType", tr("Delete ..."), [this, table, widget] {
        const auto id = table->selectedId();
        if (!id.isEmpty() &&
            confirmDeletion(widget, entries_.value(id).toObject().value("name").toString())) {
            remove(id);
        }
    });
    button("hideGrid", tr("No grid"), [this] {
        select({});
    });
    refresh();
    status->setText(
        tr("Changes apply immediately. UTM and MGRS use WGS 84. Polar regions use UPS."));
    return widget;
}
} // namespace swak
