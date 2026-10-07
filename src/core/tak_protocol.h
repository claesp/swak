#pragma once
#include "core/event.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

#include <optional>

namespace swak {
// TAK Protocol version 1 messages and frames. The schema is src/core/proto/tak_protocol.proto.
//
// A stream frame is the byte 0xbf, a varint payload length, and one TakMessage.
// A mesh datagram is 0xbf, the varint protocol version 1, 0xbf, and one TakMessage.
inline constexpr char TakMagic = '\xbf';

struct TakDecodeResult
{
    std::optional<Event> event;
    QString error;
    // True for a message with only protocol control data and no event.
    bool control = false;
};
// Encodes one event. Typed detail elements use their protobuf fields when they hold only the
// typed attributes. All other detail elements travel as XML text. Returns empty bytes for an
// invalid event.
QByteArray encodeTakMessage(const Event &event);
// Decodes one TakMessage. The decoder rebuilds CoT XML and validates it with decodeCot, so
// both encodings pass the same checks and limits.
TakDecodeResult decodeTakMessage(QByteArrayView payload);
QByteArray takStreamFrame(const QByteArray &payload);
QByteArray takMeshDatagram(const QByteArray &payload);
// Returns the TakMessage of a version 1 mesh datagram, or nothing for another datagram.
std::optional<QByteArray> takMeshPayload(QByteArrayView datagram);

// Splits a TAK Protocol version 1 stream into payloads. Each payload holds at most
// MaxMessageBytes. A wrong magic byte, a bad length, or an oversized frame fails the stream.
class TakStream final
{
public:
    QList<QByteArray> feed(const QByteArray &bytes);
    bool failed() const
    {
        return failed_;
    }
    bool pending() const
    {
        return !buffer_.isEmpty();
    }
    void reset()
    {
        buffer_.clear();
        failed_ = false;
    }

private:
    QByteArray buffer_;
    bool failed_ = false;
};
} // namespace swak
