#pragma once
#include <QString>
class QLineEdit;
class QWidget;
namespace swak {
enum class CotField
{
    None,
    Type,
    How,
    Team,
    Role
};
QString cotFieldDescription(CotField kind, const QString &code);
QString cotFieldValue(const QLineEdit *field);
void setCotFieldValue(QLineEdit *field, const QString &code);
QWidget *cotFieldEditor(QLineEdit *field, CotField kind, bool descriptive = false);
} // namespace swak
