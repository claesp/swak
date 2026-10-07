#pragma once
#include "core/event.h"

#include <QList>

#include <functional>
#include <utility>
namespace swak {
// The stream keeps one bounded XML document between socket reads.
class CotStream final
{
public:
    // Returns the complete documents in bytes. When stop returns true for a document, the
    // stream returns at once and keeps the unread bytes for takeRemainder(). The TAK protocol
    // switch uses this, because binary data can follow the switch document in one read.
    QList<QByteArray> feed(const QByteArray &bytes,
                           const std::function<bool(const QByteArray &)> &stop = {});
    QByteArray takeRemainder()
    {
        return std::exchange(remainder_, {});
    }
    bool failed() const
    {
        return failed_;
    }
    bool pending() const
    {
        return !frame_.isEmpty();
    }
    void reset();

private:
    enum class State
    {
        Text,
        Tag,
        Comment,
        Cdata,
        Instruction
    };
    QByteArray frame_;
    QByteArray remainder_;
    qsizetype tag_ = 0;
    int depth_ = 0;
    char quote_ = 0;
    State state_ = State::Text;
    bool root_ = false;
    bool failed_ = false;
};
} // namespace swak
