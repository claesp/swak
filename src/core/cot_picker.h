#pragma once
#include <QString>
class QLineEdit;
class QWidget;
namespace swak {
enum class CotField { None, Type, How, Team, Role };
QWidget *cotFieldEditor(QLineEdit *field, CotField kind);
}
