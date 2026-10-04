#pragma once
#include <QWidget>
namespace swak {
class Servers;
class ProviderTable;
class SingleLineLabel;
class ServerBrowser final : public QWidget {
    Q_OBJECT
public:
    explicit ServerBrowser(Servers &servers, QWidget *parent = nullptr);
private:
    void refresh();
    void status();
    void edit(bool adding);
    Servers &servers_;
    ProviderTable *table_;
    SingleLineLabel *status_;
};
}
