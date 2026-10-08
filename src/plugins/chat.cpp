#include "core/chat.h"

#include "core/contacts.h"
#include "core/interfaces.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/single_line_label.h"

#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace swak {
namespace {
constexpr int MaxContactChoices = 50;

QString stateText(ChatMessage::State state)
{
    switch (state) {
    case ChatMessage::State::Received:
        return {};
    case ChatMessage::State::Queued:
        return QObject::tr("Waiting for a connection");
    case ChatMessage::State::Sent:
        return QObject::tr("Sent");
    case ChatMessage::State::Pending:
        return QObject::tr("Stored by the server");
    case ChatMessage::State::Delivered:
        return QObject::tr("Delivered");
    case ChatMessage::State::Read:
        return QObject::tr("Read");
    }
    return {};
}
class ChatPane final : public QWidget, public PreferenceParticipant
{
public:
    ChatPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), chat_(services.chat), contacts_(services.contacts)
    {
        setObjectName(QStringLiteral("chatPane"));
        auto *layout = new QVBoxLayout(this);
        splitter_ = new QSplitter(Qt::Horizontal, this);
        auto *splitter = splitter_;
        splitter->setObjectName(QStringLiteral("chatSplitter"));
        auto *roomsSide = new QWidget(splitter);
        auto *roomsLayout = new QVBoxLayout(roomsSide);
        roomsLayout->setContentsMargins(0, 0, 0, 0);
        rooms_ = new QListWidget(roomsSide);
        rooms_->setObjectName(QStringLiteral("chatRooms"));
        rooms_->setAccessibleName(tr("Chat rooms"));
        roomsLayout->addWidget(rooms_);
        auto *newChat = new QPushButton(tr("New Chat"), roomsSide);
        newChat->setObjectName(QStringLiteral("newChat"));
        newChat->setToolTip(tr("Start a direct chat with a TAK user from Contacts."));
        auto *people = new QMenu(newChat);
        people->setObjectName(QStringLiteral("newChatMenu"));
        newChat->setMenu(people);
        roomsLayout->addWidget(newChat);
        auto *messagesSide = new QWidget(splitter);
        auto *messagesLayout = new QVBoxLayout(messagesSide);
        messagesLayout->setContentsMargins(0, 0, 0, 0);
        messages_ = new QListWidget(messagesSide);
        messages_->setObjectName(QStringLiteral("chatMessages"));
        messages_->setAccessibleName(tr("Chat messages"));
        messages_->setWordWrap(true);
        messages_->setTextElideMode(Qt::ElideNone);
        messages_->setContextMenuPolicy(Qt::CustomContextMenu);
        messagesLayout->addWidget(messages_);
        auto *row = new QHBoxLayout;
        input_ = new QLineEdit(messagesSide);
        input_->setObjectName(QStringLiteral("chatInput"));
        input_->setAccessibleName(tr("Message"));
        input_->setPlaceholderText(tr("Message"));
        input_->setMaxLength(MaxChatText);
        row->addWidget(input_);
        auto *send = new QPushButton(tr("Send"), messagesSide);
        send->setObjectName(QStringLiteral("sendChat"));
        row->addWidget(send);
        messagesLayout->addLayout(row);
        splitter->setStretchFactor(1, 3);
        layout->addWidget(splitter);
        status_ = new SingleLineLabel({}, this);
        status_->setObjectName(QStringLiteral("chatStatus"));
        status_->hide();
        layout->addWidget(status_);
        if (!chat_) {
            setEnabled(false);
            return;
        }
        connect(chat_, &Chat::roomsChanged, this, &ChatPane::showRooms);
        connect(chat_, &Chat::messagesChanged, this, [this](const QString &key) {
            if (key == currentKey()) {
                showMessages();
            }
        });
        connect(chat_, &Chat::roomRequested, this, [this](const QString &key) {
            select(key);
            // Show the pane, so the user sees the requested room.
            for (auto *widget = parentWidget(); widget; widget = widget->parentWidget()) {
                if (auto *dock = qobject_cast<QDockWidget *>(widget)) {
                    dock->show();
                    dock->raise();
                    break;
                }
            }
            input_->setFocus();
        });
        connect(rooms_, &QListWidget::currentRowChanged, this, [this] {
            showMessages();
        });
        connect(send, &QPushButton::clicked, this, &ChatPane::sendMessage);
        connect(input_, &QLineEdit::returnPressed, this, &ChatPane::sendMessage);
        connect(people, &QMenu::aboutToShow, this, [this, people] {
            fillPeople(*people);
        });
        connect(messages_, &QWidget::customContextMenuRequested, this, [this](QPoint position) {
            auto *item = messages_->itemAt(position);
            if (!item) {
                item = messages_->currentItem();
            }
            if (!item) {
                return;
            }
            Modal<QMenu> menu(this);
            menu->setObjectName(QStringLiteral("chatMessageMenu"));
            auto *copy = menu->addAction(tr("Copy"));
            copy->setObjectName(QStringLiteral("copyChatMessage"));
            connect(copy, &QAction::triggered, this, [text = item->data(Qt::UserRole).toString()] {
                QApplication::clipboard()->setText(text);
            });
            menu->exec(messages_->viewport()->mapToGlobal(position));
        });
        showRooms();
    }

    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.chat");
    }
    QJsonObject savePreferences() const override
    {
        return {
            {QStringLiteral("splitter"), QString::fromLatin1(splitter_->saveState().toBase64())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto bytes = preferenceText(values, QStringLiteral("splitter"), {}, 1024).toLatin1();
        if (!bytes.isEmpty()) {
            splitter_->restoreState(
                QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
        }
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        markRead();
    }

private:
    QString currentKey() const
    {
        const auto *item = rooms_->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString{};
    }
    void select(const QString &key)
    {
        for (int row = 0; row < rooms_->count(); ++row) {
            if (rooms_->item(row)->data(Qt::UserRole).toString() == key) {
                rooms_->setCurrentRow(row);
                return;
            }
        }
    }
    void showRooms()
    {
        const auto current = currentKey();
        const QSignalBlocker blocker(rooms_);
        rooms_->clear();
        for (const auto &room : chat_->rooms()) {
            auto *item = new QListWidgetItem(
                room.unread > 0 ? tr("%1 (%2)").arg(room.title).arg(room.unread) : room.title,
                rooms_);
            item->setData(Qt::UserRole, room.key);
            item->setToolTip(room.kind == ChatRoom::Kind::Direct ? room.key : room.title);
            auto font = item->font();
            font.setBold(room.unread > 0);
            item->setFont(font);
        }
        select(current);
        if (rooms_->currentRow() < 0 && rooms_->count() > 0) {
            rooms_->setCurrentRow(0);
        }
        if (currentKey() != current) {
            showMessages();
        }
    }
    void showMessages()
    {
        messages_->clear();
        const auto *room = chat_->room(currentKey());
        if (!room) {
            return;
        }
        for (const auto &message : room->messages) {
            const auto local = message.time.toLocalTime();
            auto header = QStringLiteral("%1  %2").arg(local.toString(QStringLiteral("HH:mm")),
                                                       message.senderCallsign);
            if (message.own()) {
                header += QStringLiteral(" · ") + stateText(message.state);
            }
            auto *item = new QListWidgetItem(header + QLatin1Char('\n') + message.text, messages_);
            item->setData(Qt::UserRole, message.text);
            item->setToolTip(local.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            if (message.own()) {
                item->setTextAlignment(Qt::AlignRight);
            }
        }
        messages_->scrollToBottom();
        markRead();
    }
    // Reading happens only while the pane shows the room.
    void markRead()
    {
        if (chat_ && isVisible()) {
            chat_->markRead(currentKey());
        }
    }
    void sendMessage()
    {
        const auto error = chat_->send(currentKey(), input_->text());
        status_->setText(error);
        status_->setVisible(!error.isEmpty());
        if (error.isEmpty()) {
            input_->clear();
        }
    }
    void fillPeople(QMenu &menu)
    {
        menu.clear();
        int count = 0;
        if (contacts_) {
            for (const auto &contact : contacts_->contacts()) {
                if (count++ >= MaxContactChoices) {
                    break;
                }
                const auto title = contact.callsign.isEmpty() ? contact.uid : contact.callsign;
                auto *action = menu.addAction(title);
                action->setToolTip(contact.uid);
                connect(action,
                        &QAction::triggered,
                        this,
                        [this, uid = contact.uid, callsign = contact.callsign] {
                            if (chat_) {
                                chat_->openDirect(uid, callsign);
                            }
                        });
            }
        }
        if (count == 0) {
            auto *none = menu.addAction(tr("No known TAK users"));
            none->setObjectName(QStringLiteral("noChatContacts"));
            none->setEnabled(false);
        }
    }
    QPointer<Chat> chat_;
    QPointer<Contacts> contacts_;
    QSplitter *splitter_ = nullptr;
    QListWidget *rooms_ = nullptr;
    QListWidget *messages_ = nullptr;
    QLineEdit *input_ = nullptr;
    SingleLineLabel *status_ = nullptr;
};
} // namespace
class ChatPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "chat/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Chat");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new ChatPane(services, parent);
    }
};
} // namespace swak
#include "chat.moc"
