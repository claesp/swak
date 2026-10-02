#include "core/logging.h"
#include "core/interfaces.h"
#include "core/store.h"
#include "core/preferences.h"
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace swak {
class Receiver final : public QWidget, public SettingsParticipant {
public:
    Receiver(Store &store, const Codec &codec, QWidget *parent)
        : QWidget(parent), store_(store), codec_(codec)
    {
        auto *layout = new QVBoxLayout(this);
        auto *note = new QLabel(tr("UDP messages are not authenticated. Use a trusted local source or a protected network."), this);
        note->setWordWrap(true);
        note->setTextFormat(Qt::PlainText);
        layout->addWidget(note);
        auto *form = new QFormLayout;
        address_ = new QLineEdit(QStringLiteral("127.0.0.1"), this);
        address_->setMaxLength(64);
        address_->setObjectName(QStringLiteral("bindAddress"));
        port_ = new QSpinBox(this);
        port_->setRange(1, 65535);
        port_->setValue(6969);
        port_->setObjectName(QStringLiteral("bindPort"));
        form->addRow(tr("Bind &address"), address_);
        form->addRow(tr("&Port"), port_);
        layout->addLayout(form);
        button_ = new QPushButton(tr("Start receiver"), this);
        button_->setObjectName(QStringLiteral("receiverToggle"));
        layout->addWidget(button_);
        status_ = new QLabel(tr("Receiver stopped. No network socket is open."), this);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        status_->setObjectName(QStringLiteral("receiverStatus"));
        layout->addWidget(status_);
        layout->addStretch();
        socket_ = new QUdpSocket(this);
        timer_ = new QTimer(this);
        timer_->setInterval(25);
        summary_ = new QTimer(this);
        summary_->setInterval(1000);
        connect(summary_, &QTimer::timeout, this, [this] {
            if (!reportPending_) return;
            const auto severity = rejectedSinceReport_ ? Severity::Warning : Severity::Info;
            logMessage(severity, rejectedSinceReport_ ? Urgency::Attention : Urgency::Routine,
                QStringLiteral("swak.udp"),
                tr("Receiver summary. Accepted: %1. Rejected: %2.").arg(accepted_).arg(rejected_));
            reportPending_ = false;
            rejectedSinceReport_ = false;
        });
        connect(button_, &QPushButton::clicked, this, [this] { toggle(); });
        connect(timer_, &QTimer::timeout, this, [this] { drain(); });
    }
    QString preferenceId() const override { return QStringLiteral("org.swak.udp"); }
    QString settingsTitle() const override { return tr("UDP receiver"); }
    QList<SettingField> settingsFields() const override
    {
        return {textSetting(QStringLiteral("address"), tr("Local address"), 64),
                numberSetting(QStringLiteral("port"), tr("Port"), 1, 65535),
                booleanSetting(QStringLiteral("running"), tr("Run receiver now and after restart"))};
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        QHostAddress address;
        if (!address.setAddress(values.value(QStringLiteral("address")).toString()) || address.isMulticast() || address == QHostAddress::Broadcast)
            return tr("Enter a numeric local address. Multicast and broadcast addresses are not supported.");
        return {};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("address"), address_->text()}, {QStringLiteral("port"), port_->value()},
                {QStringLiteral("running"), runningRequested_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        if (runningRequested_) toggle();
        address_->setText(preferenceText(values, QStringLiteral("address"), QStringLiteral("127.0.0.1"), 64));
        port_->setValue(preferenceInteger(values, QStringLiteral("port"), 6969, 1, 65535));
        if (values.value(QStringLiteral("running")).toBool(false)) {
            toggle();
            if (!timer_->isActive()) {
                runningRequested_ = true;
                button_->setText(tr("Stop receiver"));
                showStatus(tr("The saved receiver cannot start. Stop it to cancel the next startup attempt.\n%1")
                                 .arg(status_->text()), Severity::Error);
            }
        }
    }
private:
    void showStatus(const QString &message, Severity severity = Severity::Info)
    {
        status_->setText(message);
        logMessage(severity, severity >= Severity::Warning ? Urgency::Attention : Urgency::Routine,
            QStringLiteral("swak.udp"), message);
    }
    void toggle()
    {
        if (runningRequested_) {
            runningRequested_ = false;
            timer_->stop();
            summary_->stop();
            socket_->close();
            address_->setEnabled(true);
            port_->setEnabled(true);
            button_->setText(tr("Start receiver"));
            showStatus(tr("Receiver stopped. Accepted: %1. Rejected: %2.").arg(accepted_).arg(rejected_));
            return;
        }
        QHostAddress address;
        if (!address.setAddress(address_->text()) || address.isMulticast() || address == QHostAddress::Broadcast) {
            showStatus(tr("Enter a numeric local address. Multicast and broadcast addresses are not supported."), Severity::Error);
            return;
        }
        if (!socket_->bind(address, static_cast<quint16>(port_->value()), QUdpSocket::DontShareAddress)) {
            showStatus(tr("The receiver cannot bind to this address and port: %1").arg(socket_->errorString()), Severity::Error);
            return;
        }
        socket_->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 256 * 1024);
        runningRequested_ = true;
        accepted_ = 0;
        rejected_ = 0;
        lastError_.clear();
        address_->setEnabled(false);
        port_->setEnabled(false);
        button_->setText(tr("Stop receiver"));
        showStatus(tr("Receiver active on %1:%2. No messages yet.").arg(address_->text()).arg(port_->value()));
        reportPending_ = false;
        rejectedSinceReport_ = false;
        timer_->start();
        summary_->start();
    }
    void drain()
    {
        QElapsedTimer budget;
        budget.start();
        bool received = false;
        const auto rejectedBefore = rejected_;
        lastError_.clear();
        for (int count = 0; count < 32 && socket_->hasPendingDatagrams() && budget.elapsed() < 4; ++count) {
            received = true;
            const qint64 bytes = socket_->pendingDatagramSize();
            if (bytes <= 0 || bytes > MaxMessageBytes) {
                socket_->readDatagram(nullptr, 0);
                ++rejected_;
                lastError_ = tr("A datagram exceeded the size limit or was empty.");
                continue;
            }
            QByteArray payload(static_cast<qsizetype>(bytes), Qt::Uninitialized);
            if (socket_->readDatagram(payload.data(), payload.size()) != bytes) {
                ++rejected_;
                lastError_ = tr("The receiver cannot read a complete datagram.");
                continue;
            }
            const auto decoded = codec_.decode(payload);
            const QString error = decoded.event ? store_.accept(*decoded.event) : decoded.error;
            if (error.isEmpty()) ++accepted_;
            else { ++rejected_; lastError_ = error; }
        }
        if (received) {
            status_->setText(tr("Receiver active. Accepted: %1. Rejected: %2.\n%3")
                .arg(accepted_).arg(rejected_).arg(lastError_));
            reportPending_ = true;
            rejectedSinceReport_ = rejectedSinceReport_ || rejected_ != rejectedBefore;
        }
    }
    Store &store_;
    const Codec &codec_;
    QLineEdit *address_;
    QSpinBox *port_;
    QPushButton *button_;
    QLabel *status_;
    QUdpSocket *socket_;
    QTimer *timer_;
    QTimer *summary_;
    bool reportPending_ = false;
    bool rejectedSinceReport_ = false;
    bool runningRequested_ = false;
    quint64 accepted_ = 0;
    quint64 rejected_ = 0;
    QString lastError_;
};
class UdpPlugin final : public QObject, public Plugin, public SourcePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "udp/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::SourcePlugin)
public:
    QString name() const override { return tr("UDP receiver"); }
    QWidget *create(PluginServices &services, const Codec &codec, QWidget *parent) override
    { return new Receiver(services.store, codec, parent); }
};
}
#include "udp.moc"
