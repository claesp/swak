#include "core/chat.h"

#include "core/cot_detail.h"
#include "core/identity.h"
#include "core/servers.h"
#include "core/store.h"

#include <QUuid>

#include <algorithm>

namespace swak {
namespace {
const QString RootContactGroup = QStringLiteral("RootContactGroup");
const QString TeamGroups = QStringLiteral("TeamGroups");
constexpr int ChatLifetimeSeconds = 24 * 60 * 60;
constexpr int MaxReceipts = 256;
constexpr double UnknownHeight = 9999999;

// Keeps text readable as plain text. It removes control and format characters, such as
// direction overrides, but keeps line breaks and tabs.
QString plainText(const QString &text, qsizetype limit)
{
    QString result;
    for (qsizetype i = 0; i < text.size() && result.size() < limit; ++i) {
        const auto ch = text.at(i);
        if (ch.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            // A surrogate pair is valid text, such as an emoji. Keep both halves together.
            if (result.size() + 2 <= limit) {
                result += ch;
                result += text.at(i + 1);
            }
            ++i;
        } else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\t') ||
                   (ch.category() != QChar::Other_Control && ch.category() != QChar::Other_Format &&
                    !ch.isSurrogate())) {
            result += ch;
        }
    }
    return result;
}
bool identifier(const QString &text)
{
    return !text.isEmpty() && text.size() <= 128 && plainText(text, 128) == text &&
           !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\t'));
}
CotElement element(const QString &name, QList<std::pair<QString, QString>> attributes = {})
{
    return {name, std::move(attributes), {}, {}};
}
// The UID of the sender: the parent link, then the first chat group member.
QString senderOf(const Event &event, const std::optional<CotElement> &chatElement)
{
    const auto all = links(event);
    const auto parent = std::find_if(all.cbegin(), all.cend(), [](const CotLink &link) {
        return link.relation == QStringLiteral("p-p") && identifier(link.uid);
    });
    if (parent != all.cend()) {
        return parent->uid;
    }
    if (chatElement) {
        if (const auto *group = chatElement->child(u"chatgrp")) {
            const auto uid = group->attribute(u"uid0");
            if (identifier(uid)) {
                return uid;
            }
        }
    }
    return {};
}
Event baseEvent(const QString &uid, const QString &type, const QDateTime &now)
{
    Event event;
    event.uid = uid;
    event.type = type;
    event.how = QStringLiteral("h-e");
    event.time = event.start = now;
    event.stale = now.addSecs(ChatLifetimeSeconds);
    event.altitude = UnknownHeight;
    return event;
}
} // namespace

std::optional<IncomingChat> readChatMessage(const Event &event)
{
    if (event.type != QStringLiteral("b-t-f")) {
        return {};
    }
    const auto chatElement = detailElement(event, u"__chat");
    const auto details = chat(event);
    if (!chatElement || !details || !identifier(details->messageId)) {
        return {};
    }
    IncomingChat result;
    result.roomId = details->id.isEmpty() ? details->chatroom : details->id;
    result.roomTitle = details->chatroom.isEmpty() ? result.roomId : details->chatroom;
    if (!identifier(result.roomId) || !identifier(result.roomTitle)) {
        return {};
    }
    if (result.roomId == AllChatRooms) {
        result.kind = ChatRoom::Kind::All;
    } else if (details->parent == TeamGroups) {
        result.kind = ChatRoom::Kind::Team;
    } else {
        result.kind = ChatRoom::Kind::Direct;
    }
    auto &message = result.message;
    message.messageId = details->messageId;
    message.senderUid = senderOf(event, chatElement);
    message.senderCallsign = plainText(details->senderCallsign, 128).trimmed();
    if (message.senderCallsign.isEmpty()) {
        message.senderCallsign = message.senderUid;
    }
    const auto text = remarks(event);
    message.text = plainText(text, MaxChatText);
    if (text.size() > MaxChatText) {
        message.text += QStringLiteral(" …");
    }
    message.time = event.time;
    if (message.senderUid.isEmpty() || message.text.trimmed().isEmpty()) {
        return {};
    }
    return result;
}
std::optional<ChatReceipt> readChatReceipt(const Event &event)
{
    ChatReceipt receipt;
    if (event.type == QStringLiteral("b-t-f-d")) {
        receipt.state = ChatMessage::State::Delivered;
    } else if (event.type == QStringLiteral("b-t-f-r")) {
        receipt.state = ChatMessage::State::Read;
    } else if (event.type == QStringLiteral("b-t-f-p")) {
        receipt.state = ChatMessage::State::Pending;
    } else {
        return {};
    }
    auto element = detailElement(event, u"__chatreceipt");
    if (!element) {
        element = detailElement(event, u"__chat");
    }
    receipt.messageId = element ? element->attribute(u"messageId") : QString{};
    if (receipt.messageId.isEmpty()) {
        receipt.messageId = event.uid;
    }
    receipt.senderUid = senderOf(event, element);
    if (!identifier(receipt.messageId)) {
        return {};
    }
    return receipt;
}
Event chatMessageEvent(const Event &sender,
                       const ChatRoom &room,
                       const QString &peerCallsign,
                       const ChatMessage &message,
                       const QDateTime &now)
{
    auto uid = QStringLiteral("GeoChat.%1.%2.%3").arg(sender.uid, room.key, message.messageId);
    if (uid.size() > 128) {
        uid = QStringLiteral("GeoChat.") + message.messageId;
    }
    auto event = baseEvent(uid, QStringLiteral("b-t-f"), now);
    const bool direct = room.kind == ChatRoom::Kind::Direct;
    const auto chatroom = direct && !peerCallsign.isEmpty() ? peerCallsign : room.title;
    auto chatElement = element(QStringLiteral("__chat"),
                               {{QStringLiteral("parent"),
                                 room.kind == ChatRoom::Kind::Team ? TeamGroups : RootContactGroup},
                                {QStringLiteral("groupOwner"), QStringLiteral("false")},
                                {QStringLiteral("messageId"), message.messageId},
                                {QStringLiteral("chatroom"), chatroom},
                                {QStringLiteral("id"), room.key},
                                {QStringLiteral("senderCallsign"), sender.callsign}});
    chatElement.children.push_back(element(QStringLiteral("chatgrp"),
                                           {{QStringLiteral("uid0"), sender.uid},
                                            {QStringLiteral("uid1"), room.key},
                                            {QStringLiteral("id"), room.key}}));
    auto text = element(QStringLiteral("remarks"),
                        {{QStringLiteral("source"), QStringLiteral("BAO.F.ATAK.") + sender.uid},
                         {QStringLiteral("to"), room.key},
                         {QStringLiteral("time"), now.toString(Qt::ISODateWithMs)}});
    text.text = message.text;
    std::vector<CotElement> detail{std::move(chatElement),
                                   element(QStringLiteral("link"),
                                           {{QStringLiteral("uid"), sender.uid},
                                            {QStringLiteral("type"), sender.type},
                                            {QStringLiteral("relation"), QStringLiteral("p-p")}}),
                                   std::move(text)};
    if (direct) {
        auto marti = element(QStringLiteral("marti"));
        marti.children.push_back(
            element(QStringLiteral("dest"), {{QStringLiteral("callsign"), chatroom}}));
        detail.push_back(std::move(marti));
    }
    event.detail = detailText(detail);
    return event;
}
Event chatReceiptEvent(const Event &sender,
                       const ChatMessage &message,
                       ChatMessage::State state,
                       const QDateTime &now)
{
    auto event = baseEvent(message.messageId,
                           state == ChatMessage::State::Read ? QStringLiteral("b-t-f-r")
                                                             : QStringLiteral("b-t-f-d"),
                           now);
    event.stale = now.addSecs(60);
    auto receipt = element(QStringLiteral("__chatreceipt"),
                           {{QStringLiteral("parent"), RootContactGroup},
                            {QStringLiteral("groupOwner"), QStringLiteral("false")},
                            {QStringLiteral("messageId"), message.messageId},
                            {QStringLiteral("chatroom"), message.senderCallsign},
                            {QStringLiteral("id"), message.senderUid},
                            {QStringLiteral("senderCallsign"), sender.callsign}});
    receipt.children.push_back(element(QStringLiteral("chatgrp"),
                                       {{QStringLiteral("uid0"), sender.uid},
                                        {QStringLiteral("uid1"), message.senderUid},
                                        {QStringLiteral("id"), message.senderUid}}));
    auto marti = element(QStringLiteral("marti"));
    marti.children.push_back(
        element(QStringLiteral("dest"), {{QStringLiteral("callsign"), message.senderCallsign}}));
    event.detail = detailText({std::move(receipt),
                               element(QStringLiteral("link"),
                                       {{QStringLiteral("uid"), sender.uid},
                                        {QStringLiteral("type"), sender.type},
                                        {QStringLiteral("relation"), QStringLiteral("p-p")}}),
                               std::move(marti)});
    return event;
}

Chat::Chat(Store &store, Servers *servers, Identity *identity, QObject *parent)
    : QObject(parent), servers_(servers), identity_(identity)
{
    setObjectName(QStringLiteral("chat"));
    retry_.setSingleShot(true);
    retry_.setInterval(RetryMilliseconds);
    connect(&retry_, &QTimer::timeout, this, &Chat::flush);
    connect(&store, &Store::messageReceived, this, &Chat::receive);
    rooms_.append({AllChatRooms, AllChatRooms, ChatRoom::Kind::All, {}, 0});
    if (identity_) {
        connect(identity_, &Identity::changed, this, &Chat::updateTeam);
    }
    updateTeam();
}
const ChatRoom *Chat::room(const QString &key) const
{
    const auto found = std::find_if(rooms_.cbegin(), rooms_.cend(), [&key](const auto &target) {
        return target.key == key;
    });
    return found == rooms_.cend() ? nullptr : &*found;
}
ChatRoom *Chat::findRoom(const QString &key)
{
    const auto found = std::find_if(rooms_.begin(), rooms_.end(), [&key](const auto &target) {
        return target.key == key;
    });
    return found == rooms_.end() ? nullptr : &*found;
}
ChatRoom *Chat::ensureRoom(const QString &key, const QString &title, ChatRoom::Kind kind)
{
    if (auto *existing = findRoom(key)) {
        return existing;
    }
    if (rooms_.size() >= MaxChatRooms) {
        return nullptr;
    }
    // Team rooms come after All Chat Rooms and before direct chats.
    auto position = rooms_.size();
    if (kind == ChatRoom::Kind::Team) {
        position = std::find_if(rooms_.cbegin(),
                                rooms_.cend(),
                                [](const auto &target) {
                                    return target.kind == ChatRoom::Kind::Direct;
                                }) -
                   rooms_.cbegin();
    }
    rooms_.insert(position, {key, title, kind, {}, 0});
    emit roomsChanged();
    return &rooms_[position];
}
void Chat::updateTeam()
{
    const auto team = sender().team;
    if (identifier(team)) {
        ensureRoom(team, team, ChatRoom::Kind::Team);
    }
}
QString Chat::openDirect(const QString &uid, const QString &callsign)
{
    if (!identifier(uid) || uid == sender().uid) {
        return {};
    }
    const auto title = identifier(callsign) ? callsign : uid;
    auto *target = ensureRoom(uid, title, ChatRoom::Kind::Direct);
    if (!target) {
        return {};
    }
    if (target->kind != ChatRoom::Kind::Direct) {
        return {};
    }
    if (target->title != title) {
        target->title = title;
        emit roomsChanged();
    }
    emit roomRequested(uid);
    return uid;
}
bool Chat::remember(const QString &messageId)
{
    if (known_.contains(messageId)) {
        return false;
    }
    known_.insert(messageId);
    knownOrder_.push_back(messageId);
    if (knownOrder_.size() > MaxKnownMessages) {
        known_.remove(knownOrder_.front());
        knownOrder_.pop_front();
    }
    return true;
}
void Chat::addMessage(ChatRoom &target, const ChatMessage &message)
{
    // Stored messages from TAK Server can arrive late. Keep the room in time order.
    auto position = target.messages.size();
    while (position > 0 && target.messages.at(position - 1).time > message.time) {
        --position;
    }
    target.messages.insert(position, message);
    if (target.messages.size() > MaxRoomMessages) {
        target.messages.removeFirst();
    }
    if (!message.own()) {
        target.unread = std::min(target.unread + 1, MaxRoomMessages);
    }
    emit messagesChanged(target.key);
    emit roomsChanged();
}
void Chat::receive(const Event &event)
{
    if (const auto receipt = readChatReceipt(event)) {
        setState(receipt->messageId, receipt->state);
        return;
    }
    const auto incoming = readChatMessage(event);
    const auto self = sender();
    if (!incoming || incoming->message.senderUid == self.uid ||
        !remember(incoming->message.messageId)) {
        return;
    }
    ChatRoom *target = nullptr;
    switch (incoming->kind) {
    case ChatRoom::Kind::All:
        target = findRoom(AllChatRooms);
        break;
    case ChatRoom::Kind::Team:
        // Multicast peers also see the chats of other teams. Show only the own team.
        if (incoming->roomId == self.team) {
            target = ensureRoom(incoming->roomId, incoming->roomTitle, ChatRoom::Kind::Team);
        }
        break;
    case ChatRoom::Kind::Direct:
        // A direct message to this user names this UID. Others are for other users.
        if (incoming->roomId == self.uid) {
            target = ensureRoom(incoming->message.senderUid,
                                incoming->message.senderCallsign,
                                ChatRoom::Kind::Direct);
        }
        break;
    }
    if (!target ||
        (incoming->kind == ChatRoom::Kind::Direct && target->kind != ChatRoom::Kind::Direct)) {
        return;
    }
    // The signals of addMessage can change the room list. Do not use the room after it.
    const bool direct = target->kind == ChatRoom::Kind::Direct;
    addMessage(*target, incoming->message);
    if (direct) {
        queueReceipt(chatReceiptEvent(self,
                                      incoming->message,
                                      ChatMessage::State::Delivered,
                                      QDateTime::currentDateTimeUtc()));
        flush();
    }
}
QString Chat::send(const QString &key, const QString &text)
{
    auto *target = findRoom(key);
    const auto self = sender();
    const auto trimmed = text.trimmed();
    if (!target) {
        return tr("The chat room does not exist.");
    }
    if (trimmed.isEmpty()) {
        return tr("Type a message.");
    }
    if (trimmed.size() > MaxChatText || plainText(trimmed, MaxChatText) != trimmed) {
        return tr("Use at most %1 characters of plain text.").arg(MaxChatText);
    }
    if (!identifier(self.uid)) {
        return tr("Set a UID in File > Identity.");
    }
    if (queue_.size() >= MaxQueued) {
        return tr("Too many messages wait for a connection.");
    }
    ChatMessage message;
    message.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    message.senderUid = self.uid;
    message.senderCallsign = self.callsign;
    message.text = trimmed;
    message.time = QDateTime::currentDateTimeUtc();
    message.state = ChatMessage::State::Queued;
    const auto event = chatMessageEvent(self, *target, target->title, message, message.time);
    if (!eventError(event).isEmpty() || event.detail.isEmpty()) {
        return tr("The message cannot be sent as CoT.");
    }
    remember(message.messageId);
    queue_.append({message.messageId, event});
    addMessage(*target, message);
    flush();
    return {};
}
void Chat::markRead(const QString &key)
{
    auto *target = findRoom(key);
    if (!target || target->unread == 0) {
        return;
    }
    if (target->kind == ChatRoom::Kind::Direct) {
        // The unread messages are the newest received messages of the room.
        int remaining = target->unread;
        const auto self = sender();
        const auto now = QDateTime::currentDateTimeUtc();
        for (auto it = target->messages.crbegin(); it != target->messages.crend() && remaining > 0;
             ++it) {
            if (!it->own()) {
                queueReceipt(chatReceiptEvent(self, *it, ChatMessage::State::Read, now));
                --remaining;
            }
        }
    }
    target->unread = 0;
    emit roomsChanged();
    flush();
}
void Chat::setState(const QString &messageId, ChatMessage::State state)
{
    for (auto &target : rooms_) {
        // A receipt never moves a message back, for example from Read to Delivered.
        const auto found = std::find_if(
            target.messages.begin(), target.messages.end(), [&](const ChatMessage &message) {
                return message.messageId == messageId && message.own() && message.state < state;
            });
        if (found != target.messages.end()) {
            found->state = state;
            emit messagesChanged(target.key);
            return;
        }
    }
}
void Chat::queueReceipt(const Event &event)
{
    if (receipts_.size() >= MaxReceipts) {
        receipts_.removeFirst();
    }
    receipts_.append(event);
}
Event Chat::sender() const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    return event;
}
void Chat::flush()
{
    if (!servers_) {
        return;
    }
    const auto retry = [this] {
        if (!retry_.isActive()) {
            retry_.start();
        }
    };
    while (!receipts_.isEmpty()) {
        if (!servers_->send(receipts_.first()).isEmpty()) {
            retry();
            return;
        }
        receipts_.removeFirst();
    }
    while (!queue_.isEmpty()) {
        const auto outgoing = queue_.first();
        if (!servers_->send(outgoing.event).isEmpty()) {
            retry();
            return;
        }
        queue_.removeFirst();
        setState(outgoing.messageId, ChatMessage::State::Sent);
    }
}
} // namespace swak
