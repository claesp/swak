#pragma once
#include "core/event.h"
#include "core/settings.h"

#include <QObject>
namespace swak {
class Identity final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    explicit Identity(QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.identity");
    }
    QString settingsTitle() const override
    {
        return tr("Identity");
    }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override
    {
        return values_;
    }
    void restorePreferences(const QJsonObject &values) override;
    QString validateSettings(const QJsonObject &values) const override;
    void apply(Event &event) const;
signals:
    void changed();

private:
    QJsonObject values_;
};
} // namespace swak
