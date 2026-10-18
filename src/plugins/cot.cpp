#include "core/cot.h"

#include "core/interfaces.h"

#include <QObject>

namespace swak {
class CotPlugin final : public QObject, public Plugin, public Codec
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "cot/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::Codec)
public:
    QString name() const override
    {
        return tr("CoT XML 2.0");
    }
    DecodeResult decode(const QByteArray &data) const override
    {
        return decodeCot(data);
    }
    QByteArray encode(const Event &event) const override
    {
        return encodeCot(event);
    }
};
} // namespace swak
#include "cot.moc"
