#pragma once
#include "core/event.h"

#include <QList>
namespace swak {
// The stream keeps one bounded XML document between socket reads.
class CotStream final
{
public:
    QList<QByteArray> feed(const QByteArray &bytes);
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
    qsizetype tag_ = 0;
    int depth_ = 0;
    char quote_ = 0;
    State state_ = State::Text;
    bool root_ = false;
    bool failed_ = false;
};
} // namespace swak
