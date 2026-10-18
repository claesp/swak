#pragma once
#include "core/map_grid.h"
#include "core/projection.h"
#include "core/settings.h"

#include <QObject>
class QPainter;
namespace swak {
class GridTypes final : public QObject, public SettingsParticipant, public SettingsWidgetProvider
{
    Q_OBJECT
public:
    explicit GridTypes(QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.grids");
    }
    QString settingsTitle() const override
    {
        return tr("Grids");
    }
    QList<SettingField> settingsFields() const override
    {
        return {};
    }
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    QWidget *createSettingsWidget(QWidget *parent) override;
    static QJsonObject defaults(const QString &system = QStringLiteral("geographic"));
    static QString validate(const QJsonObject &value);
    QString put(const QString &id, const QJsonObject &value);
    void remove(const QString &id);
    void select(const QString &id);
    void setVisible(bool visible);
    void migrate(const QJsonObject &mapValues);
    void paint(QPainter &painter, const Viewport &view) const;
    const QJsonObject &entries() const
    {
        return entries_;
    }
    const QString &selected() const
    {
        return selected_;
    }
signals:
    void changed();

private:
    QJsonObject entries_;
    QString selected_ = QStringLiteral("geographic");
    QString lastSelected_ = QStringLiteral("geographic");
    bool restored_ = false;
    std::optional<GridStyle> selectedStyle_;
    void refreshStyle();
};
} // namespace swak
