#pragma once
#include <QWidget>
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
namespace swak {
class Servers;
class ProviderTable;
class SingleLineLabel;
class TakEnrollment;
class LocalLanStatus final : public QWidget
{
    Q_OBJECT
public:
    explicit LocalLanStatus(Servers &servers, QWidget *parent = nullptr);

private:
    void refresh();
    Servers &servers_;
    QPlainTextEdit *status_;
    QTableWidget *peers_;
    QString selectedPeer_;
};
class ServerBrowser final : public QWidget
{
    Q_OBJECT
public:
    explicit ServerBrowser(Servers &servers, QWidget *parent = nullptr);

private:
    void refresh();
    void status();
    void edit(bool adding);
    void enroll();
    void showEnrollStatus(const QString &text);
    Servers &servers_;
    QPushButton *enroll_ = nullptr;
    SingleLineLabel *enrollStatus_ = nullptr;
    TakEnrollment *enrollment_ = nullptr;
    ProviderTable *table_;
    QPlainTextEdit *status_;
};
} // namespace swak
