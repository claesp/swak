#pragma once
#include <QWidget>
class QPlainTextEdit;
class QTableWidget;
namespace swak {
class Servers;
class ProviderTable;
class SingleLineLabel;
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
    Servers &servers_;
    ProviderTable *table_;
    QPlainTextEdit *status_;
};
} // namespace swak
