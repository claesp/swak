#pragma once
#include <QList>
#include <QWidget>
class QPlainTextEdit;
class QAction;
class QTableWidget;
namespace swak {
class Servers;
class ProviderTable;
class SingleLineLabel;
class TakEnrollment;
class TakChannels;
struct TakGroup;
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
    void loadChannels();
    void chooseChannels(const QString &id, const QList<TakGroup> &groups, const QString &error);
    Servers &servers_;
    QAction *enroll_ = nullptr;
    SingleLineLabel *enrollStatus_ = nullptr;
    TakEnrollment *enrollment_ = nullptr;
    TakChannels *channels_ = nullptr;
    ProviderTable *table_;
    QPlainTextEdit *status_;
};
} // namespace swak
