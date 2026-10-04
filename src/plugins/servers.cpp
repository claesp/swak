#include "core/servers.h"

#include "core/interfaces.h"
#include "core/server_browser.h"
namespace swak {
class ServersPlugin final : public QObject, public Plugin, public SourcePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "servers/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::SourcePlugin)
public:
    QString paneId() const override
    {
        return QStringLiteral("org.swak.udp.pane");
    }
    QString name() const override
    {
        return tr("Servers");
    }
    QWidget *create(PluginServices &services, const Codec &, QWidget *parent) override
    {
        auto *servers = services.servers;
        if (!servers) {
            servers = new Servers(services.store);
        }
        auto *browser = new ServerBrowser(*servers, parent);
        if (!services.servers) {
            servers->setParent(browser);
        }
        return browser;
    }
};
} // namespace swak
#include "servers.moc"
