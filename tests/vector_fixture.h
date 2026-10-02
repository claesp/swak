#pragma once
#include <QByteArray>
#include <initializer_list>
#include <cstdint>

namespace vector_fixture {
inline QByteArray varint(uint64_t value)
{
    QByteArray bytes;
    do { auto part = static_cast<unsigned char>(value & 127U); value >>= 7U; if (value) part |= 128U; bytes.append(static_cast<char>(part)); } while (value);
    return bytes;
}
inline QByteArray number(uint32_t tag, uint64_t value) { return varint(static_cast<uint64_t>(tag) << 3U) + varint(value); }
inline QByteArray field(uint32_t tag, const QByteArray &bytes) { return varint((static_cast<uint64_t>(tag) << 3U) | 2U) + varint(static_cast<uint64_t>(bytes.size())) + bytes; }
inline QByteArray packed(uint32_t tag, std::initializer_list<uint32_t> values)
{
    QByteArray bytes; for (auto value : values) bytes += varint(value); return field(tag, bytes);
}
inline QByteArray feature(uint32_t kind, const QByteArray &commands)
{
    return number(1, 9007199254740993ULL) + packed(2, {0, 0, 1, 1}) + number(3, kind) + field(4, commands);
}
inline QByteArray geometry(std::initializer_list<uint32_t> values)
{
    QByteArray bytes; for (auto value : values) bytes += varint(value); return bytes;
}
inline QByteArray layer(const QByteArray &name, const QByteArray &featureBytes, uint32_t version = 2)
{
    return field(3, field(1, name) + field(2, featureBytes) + field(3, "name") + field(3, "class") +
        field(4, field(1, "Test place")) + field(4, field(1, "cafe")) + number(5, 4096) + number(15, version));
}
inline QByteArray sample()
{
    return layer("water", feature(3, geometry({9, 512, 512, 26, 7168, 0, 0, 7168, 7167, 0, 15}))) +
        layer("transportation", feature(2, geometry({9, 1024, 4096, 10, 6144, 0}))) +
        layer("poi", feature(1, geometry({9, 4096, 4096})));
}
}
