#pragma once
#include "core/event.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <deque>
#include <optional>

namespace swak {
class Identity;
class Servers;
class Store;

// The GeoChat room that every TAK user can read.
inline const QString AllChatRooms = QStringLiteral("All Chat Rooms");
inline constexpr int MaxChatText = 4096;
inline constexpr int MaxChatRooms = 128;
inline constexpr int MaxRoomMessages = 500;

struct ChatMessage
{
    // The delivery state of an own message. Received messages use Received.
    enum class State
    {
        Received,
        Queued,
        Sent,
        Pending,
        Delivered,
        Read
    };
    QString messageId;
    QString senderUid;
    QString senderCallsign;
    QString text;
    QDateTime time;
    State state = State::Received;
    bool own() const
    {
        return state != State::Received;
    }
};
struct ChatRoom
{
    enum class Kind
    {
        All,
        Team,
        Direct
    };
    // All Chat Rooms, the team name, or the UID of the other user.
    QString key;
    QString title;
    Kind kind = Kind::All;
    QList<ChatMessage> messages;
    int unread = 0;
};
// A received GeoChat message with the room that it belongs to.
struct IncomingChat
{
    ChatRoom::Kind kind = ChatRoom::Kind::All;
    // The room identifier in the message: All Chat Rooms, a team, or the addressed UID.
    QString roomId;
    QString roomTitle;
    ChatMessage message;
};
struct ChatReceipt
{
    QString messageId;
    ChatMessage::State state = ChatMessage::State::Delivered;
    QString senderUid;
};

// Reads a GeoChat message (b-t-f). Returns no value for other types and invalid content.
std::optional<IncomingChat> readChatMessage(const Event &event);
// Reads a chat receipt: b-t-f-p (pending), b-t-f-d (delivered), or b-t-f-r (read).
std::optional<ChatReceipt> readChatReceipt(const Event &event);
// Builds a GeoChat message in the ATAK format. The sender is the identity of this client.
Event chatMessageEvent(const Event &sender,
                       const ChatRoom &room,
                       const QString &peerCallsign,
                       const ChatMessage &message,
                       const QDateTime &now);
// Builds a delivered or read receipt for a received direct message.
Event chatReceiptEvent(const Event &sender,
                       const ChatMessage &message,
                       ChatMessage::State state,
                       const QDateTime &now);

// The GeoChat rooms and messages of one window. It keeps history in memory only.
//
// It receives chat events from the Store, removes duplicates by message ID, and sends
// delivered and read receipts for direct messages. Own messages wait in a bounded queue
// when no output is connected, and SWAK tries again every 5 seconds.
class Chat final : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxQueued = 64;
    static constexpr int MaxKnownMessages = 4096;
    static constexpr int RetryMilliseconds = 5000;
    Chat(Store &store, Servers *servers, Identity *identity, QObject *parent = nullptr);
    // The rooms in display order: All Chat Rooms, the team room, then direct chats.
    const QList<ChatRoom> &rooms() const
    {
        return rooms_;
    }
    const ChatRoom *room(const QString &key) const;
    // Opens a direct chat with a user and returns its key. Returns an empty key when the
    // room limit is full or the UID is invalid.
    QString openDirect(const QString &uid, const QString &callsign);
    // Sends a message to a room. Returns an error for empty or long text and unknown rooms.
    QString send(const QString &key, const QString &text);
    // Marks a room as read and sends read receipts for its direct messages.
    void markRead(const QString &key);
    // Sends queued messages now. Tests and the retry timer use it.
    void flush();
    // Receives one chat event. The Store calls it for b-t-f events.
    void receive(const Event &event);
signals:
    void roomsChanged();
    void messagesChanged(const QString &key);
    // A user asked to show a room, for example through Chat with in Contacts.
    void roomRequested(const QString &key);

private:
    struct Outgoing
    {
        QString messageId;
        Event event;
    };
    Event sender() const;
    ChatRoom *findRoom(const QString &key);
    ChatRoom *ensureRoom(const QString &key, const QString &title, ChatRoom::Kind kind);
    void updateTeam();
    bool remember(const QString &messageId);
    void addMessage(ChatRoom &room, const ChatMessage &message);
    void setState(const QString &messageId, ChatMessage::State state);
    void queueReceipt(const Event &event);
    QPointer<Servers> servers_;
    QPointer<Identity> identity_;
    QList<ChatRoom> rooms_;
    QSet<QString> known_;
    std::deque<QString> knownOrder_;
    QList<Outgoing> queue_;
    QList<Event> receipts_;
    QTimer retry_;
};
} // namespace swak
