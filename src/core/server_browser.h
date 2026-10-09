#pragma once
#include "core/data_feeds.h"

#include <QList>
#include <QPointer>
#include <QWidget>
class QPlainTextEdit;
class QAction;
class QTableWidget;
namespace swak {
class MapSources;
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
    // The map catalog for Map Layers .... Without it, the command explains that it is off.
    void setMapSources(MapSources *maps);

private:
    void refresh();
    void status();
    void edit(bool adding);
    void enroll();
    void showEnrollStatus(const QString &text);
    void loadChannels();
    void chooseChannels(const QString &id, const QList<TakGroup> &groups, const QString &error);
    void loadMapLayers();
    void loadDataFeeds();
    void showDataFeeds(const QList<ServerFeed> &feeds,
                       const QSet<QString> &channels,
                       bool channelsKnown);
    void chooseMapLayers(const QString &id, const QByteArray &json);
    Servers &servers_;
    QAction *enroll_ = nullptr;
    SingleLineLabel *enrollStatus_ = nullptr;
    TakEnrollment *enrollment_ = nullptr;
    TakChannels *channels_ = nullptr;
    QPointer<MapSources> maps_;
    ProviderTable *table_;
    QPlainTextEdit *status_;
};
} // namespace swak
