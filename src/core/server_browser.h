#pragma once
#include <QWidget>
class QPlainTextEdit;
namespace swak {
class Servers;
class ProviderTable;
class SingleLineLabel;
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
