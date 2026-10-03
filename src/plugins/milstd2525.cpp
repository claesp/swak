#include "core/interfaces.h"
#include "core/marker_collections.h"
namespace swak {
class MilitaryMarkers final : public QObject, public Plugin, public MarkerCollectionProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "milstd2525/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MarkerCollectionProvider)
public:
    QJsonObject markerCollection() const override { return militaryMarkerCollection(); }
};
}
#include "milstd2525.moc"
