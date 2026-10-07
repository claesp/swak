#include "core/cot_stream.h"
namespace swak {
void CotStream::reset()
{
    frame_.clear();
    remainder_.clear();
    tag_ = 0;
    depth_ = 0;
    quote_ = 0;
    state_ = State::Text;
    root_ = false;
    failed_ = false;
}
QList<QByteArray> CotStream::feed(const QByteArray &bytes,
                                  const std::function<bool(const QByteArray &)> &stop)
{
    QList<QByteArray> frames;
    if (failed_ || bytes.size() > MaxMessageBytes) {
        failed_ = true;
        return frames;
    }
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        const char ch = bytes.at(index);
        if (frame_.isEmpty() && (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t')) {
            continue;
        }
        if (ch == '\0' || frame_.size() >= MaxMessageBytes) {
            failed_ = true;
            break;
        }
        frame_.append(ch);
        if (state_ == State::Text) {
            if (ch == '<') {
                tag_ = frame_.size() - 1;
                state_ = State::Tag;
            } else if (!root_ && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                failed_ = true;
                break;
            }
        } else if (state_ == State::Comment) {
            if (frame_.endsWith("-->")) {
                state_ = State::Text;
            }
        } else if (state_ == State::Cdata) {
            if (frame_.endsWith("]]>")) {
                state_ = State::Text;
            }
        } else if (state_ == State::Instruction) {
            if (frame_.endsWith("?>")) {
                state_ = State::Text;
            }
        } else {
            const auto length = frame_.size() - tag_;
            const auto token = QByteArrayView(frame_).sliced(tag_);
            if (length == 2 && ch == '?') {
                state_ = State::Instruction;
                continue;
            }
            if (length >= 2 && token[1] == '!') {
                if (token == "<!--") {
                    state_ = State::Comment;
                } else if (token == "<![CDATA[") {
                    state_ = State::Cdata;
                } else if (!QByteArrayView("<!--").startsWith(token) &&
                           !QByteArrayView("<![CDATA[").startsWith(token)) {
                    failed_ = true;
                    break;
                }
                continue;
            }
            if (quote_) {
                if (ch == quote_) {
                    quote_ = 0;
                }
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote_ = ch;
                continue;
            }
            if (ch != '>') {
                continue;
            }
            const bool closing = token.size() > 2 && token[1] == '/';
            const bool empty = token.size() > 2 && token[token.size() - 2] == '/';
            if (!root_) {
                if (!token.startsWith("<event") || token.size() < 7 ||
                    (token[6] != ' ' && token[6] != '\t' && token[6] != '\r' && token[6] != '\n' &&
                     token[6] != '/' && token[6] != '>')) {
                    failed_ = true;
                    break;
                }
                root_ = true;
            }
            depth_ += closing ? -1 : empty ? 0 : 1;
            if (depth_ < 0 || depth_ > 12) {
                failed_ = true;
                break;
            }
            state_ = State::Text;
            if (depth_ == 0) {
                frames.append(frame_);
                reset();
                if (stop && stop(frames.last())) {
                    remainder_ = bytes.mid(index + 1);
                    return frames;
                }
            }
        }
    }
    if (failed_) {
        frame_.clear();
    }
    return frames;
}
} // namespace swak
