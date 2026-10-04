#pragma once
#include "core/preferences.h"

#include <QMap>
#include <QObject>
#include <QSet>
#include <QUndoStack>

#include <optional>

namespace swak {
struct UserObject
{
    QString id;
    QString type;
    QString title;
    QString description;
    QJsonObject properties;
    QJsonObject geometry;
    QString color = QStringLiteral("#ffc857");
    QString symbol = {};
    QString collection = {};
    bool restricted = false;
    QJsonObject json() const;
    static UserObject fromJson(const QJsonObject &value);
};
class DeleteObject;
class UserObjects final : public QObject, public PreferenceParticipant
{
    Q_OBJECT
public:
    explicit UserObjects(QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.userobjects");
    }
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    const QMap<QString, UserObject> &items() const
    {
        return items_;
    }
    std::optional<UserObject> find(const QString &id) const;
    QString put(const UserObject &item);
    void remove(const QString &id);
    QUndoStack &undoStack()
    {
        return undo_;
    }
    static QString validate(const UserObject &item);
signals:
    void changed();
    void restored(const QString &id);

private:
    friend class DeleteObject;
    QMap<QString, UserObject> items_;
    qsizetype reservedBytes_ = 0;
    int reservedCount_ = 0;
    QSet<QString> deletedIds_;
    QUndoStack undo_;
};
} // namespace swak
