#include "cot_simulation.h"
#include "core/cot.h"
#include "core/files.h"
#include "core/servers.h"
#include "core/store.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QHostAddress>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>
#include <QTimer>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <algorithm>
using namespace swak;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
QJsonObject readObject(const QString &path, QString &error)
{
    const auto file = readRegularFile(path, 16384);
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(file.bytes, &parse);
    if (!file.error.isEmpty() || parse.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Cannot read a JSON object from the configuration file."); return {};
    }
    return document.object();
}
class Emulator final : public QObject {
public:
    Emulator(simulation::Scenario scenario, QCoreApplication &app) : scenario_(std::move(scenario)), app_(app)
    {
        server_.setMaxPendingConnections(8);
        timer_.setTimerType(Qt::PreciseTimer); timer_.setInterval(1);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                auto *socket = server_.nextPendingConnection();
                if (clients_.size() >= 8) { socket->abort(); socket->deleteLater(); continue; }
                socket->setReadBufferSize(16384); clients_.insert(socket);
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] { clients_.remove(socket); socket->deleteLater(); });
            }
        });
    }
    ~Emulator() override
    {
        timer_.stop(); server_.close();
        for (auto *client : clients_) { client->disconnect(this); client->abort(); }
    }
    QString start(const QHostAddress &address, quint16 port, bool udp, const QJsonObject &profile, int duration)
    {
        duration_ = duration; destination_ = address; port_ = port; udpMode_ = udp;
        if (!profile.isEmpty()) {
            auto source = serverDefaults(); for (auto it = profile.begin(); it != profile.end(); ++it) source.insert(it.key(), it.value());
            source.insert("name", QStringLiteral("CoT emulator")); source.insert("enabled", true);
            if (source.value("protocol") != "tls" && source.value("protocol") != "stcp") return QStringLiteral("A TAK destination must use TLS or TCP.");
            const auto error = validateServer(source); if (!error.isEmpty()) return error;
            store_ = std::make_unique<Store>(); tak_ = std::make_unique<Servers>(*store_);
            if (const auto failure = tak_->put(QStringLiteral("emulator"), source); !failure.isEmpty()) return failure;
        } else if (!udp && !server_.listen(address, port)) return QStringLiteral("Cannot listen on the requested TCP address and port.");
        if (tak_) connect(tak_.get(), &Servers::statusChanged, this, [this] {
            const auto state = tak_->statistics().value(QStringLiteral("emulator")).toObject().value(QStringLiteral("state")).toString();
            if (state != lastState_) { lastState_ = state; std::fprintf(stderr, "TAK connection: %s\n", qPrintable(state)); }
        });
        else std::fprintf(stderr, "%s %s:%u\n", udp ? "UDP destination" : "TCP listener", qPrintable(address.toString()), port);
        clock_.start(); timer_.start(); return {};
    }
    void report() const
    {
        const QJsonObject result{{"generated", static_cast<double>(generated_)}, {"queued", static_cast<double>(queued_)},
            {"dropped", static_cast<double>(dropped_)}, {"scheduleSkipped", static_cast<double>(skipped_)},
            {"slowClients", static_cast<double>(slow_)}, {"units", scenario_.units.size()}, {"seconds", static_cast<double>(clock_.elapsed()) / 1000.0}};
        const auto bytes = QJsonDocument(result).toJson(); std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    }
private:
    void tick()
    {
        const double elapsed = static_cast<double>(clock_.elapsed()) / 1000.0;
        if (interrupted || (duration_ && elapsed >= duration_)) { timer_.stop(); app_.quit(); return; }
        for (auto *client : clients_.values()) client->read(16384);
        const double rate = static_cast<double>(scenario_.units.size()) * scenario_.frequency;
        const auto due = static_cast<qint64>(std::floor(elapsed * rate));
        if (due - sequence_ >= 16) { skipped_ += due - sequence_ - 15; sequence_ = due - 15; }
        const auto now = QDateTime::currentDateTimeUtc();
        for (; sequence_ <= due; ++sequence_) {
            const auto &unit = scenario_.units.at(sequence_ % scenario_.units.size());
            const auto event = simulation::sample(scenario_, unit, elapsed, now); ++generated_;
            if (tak_) { if (tak_->send(event).isEmpty()) ++queued_; else ++dropped_; continue; }
            const auto bytes = encodeCot(event);
            if (bytes.isEmpty()) { ++dropped_; continue; }
            if (udpMode_) {
                if (udp_.writeDatagram(bytes, destination_, port_) == bytes.size()) ++queued_; else ++dropped_;
            } else {
                const auto clients = clients_.values();
                for (auto *client : clients) {
                    if (client->state() != QAbstractSocket::ConnectedState) continue;
                    if (client->bytesToWrite() + bytes.size() > 256 * 1024) { ++slow_; client->abort(); continue; }
                    if (client->write(bytes) == bytes.size()) ++queued_; else ++dropped_;
                }
            }
        }
        const auto delay = std::clamp(std::ceil(static_cast<double>(sequence_) / rate * 1000 - static_cast<double>(clock_.elapsed())), 1.0, 100.0);
        timer_.setInterval(static_cast<int>(delay));
    }
    QString lastState_;
    simulation::Scenario scenario_;
    QCoreApplication &app_;
    QTimer timer_;
    QElapsedTimer clock_;
    QTcpServer server_;
    QUdpSocket udp_;
    QSet<QTcpSocket *> clients_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Servers> tak_;
    QHostAddress destination_;
    quint16 port_ = 0;
    bool udpMode_ = false;
    int duration_ = 0;
    qint64 sequence_ = 0, generated_ = 0, queued_ = 0, dropped_ = 0, skipped_ = 0, slow_ = 0;
};
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); app.setApplicationName(QStringLiteral("swak_cot_emulator"));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("Simulate two teams of ground and air units. Default: TCP listener at 127.0.0.1:18087.")); parser.addHelpOption();
    parser.addOptions({{QStringLiteral("scenario"), "Read a scenario JSON file.", "file"}, {QStringLiteral("print-defaults"), "Print the default scenario JSON and exit."},
        {QStringLiteral("address"), "Numeric TCP bind address or UDP destination.", "address", "127.0.0.1"},
        {QStringLiteral("port"), "TCP listen port or UDP destination port.", "port", "18087"}, {QStringLiteral("udp"), "Send UDP instead of listening for TCP clients."},
        {QStringLiteral("tak-profile"), "Connect to a TAK server using its server configuration JSON.", "file"},
        {QStringLiteral("duration"), "Stop after this many seconds. Zero means until interrupted.", "seconds", "0"},
        {QStringLiteral("frequency"), "Override messages per second per unit (0.1 to 20).", "hz"},
        {QStringLiteral("latitude"), "Override scenario center latitude.", "degrees"}, {QStringLiteral("longitude"), "Override scenario center longitude.", "degrees"},
        {QStringLiteral("session"), "Use this UID prefix for a repeatable run. Default: random UUID.", "prefix"}});
    parser.process(app);
    if (parser.isSet("print-defaults")) { const auto bytes = QJsonDocument(simulation::defaults()).toJson(); std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout); return 0; }
    QString error; QJsonObject values;
    if (parser.isSet("scenario")) values = readObject(parser.value("scenario"), error);
    for (const auto *name : {"frequency", "latitude", "longitude"}) if (parser.isSet(name)) {
        bool ok = false; const auto value = parser.value(name).toDouble(&ok);
        if (!ok || !std::isfinite(value)) error = QStringLiteral("A numeric argument is invalid."); else values.insert(QLatin1String(name), value);
    }
    const auto session = parser.isSet("session") ? parser.value("session") : QStringLiteral("SIM-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto scenario = simulation::load(values, session);
    bool portOk = false, durationOk = false; const int port = parser.value("port").toInt(&portOk), duration = parser.value("duration").toInt(&durationOk);
    QHostAddress address; const bool addressOk = address.setAddress(parser.value("address")) && !address.isMulticast() && address != QHostAddress::Broadcast;
    if (!portOk || port < 1 || port > 65535 || !durationOk || duration < 0 || duration > 86400 || !addressOk ||
        (parser.isSet("udp") && (address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6))) error = QStringLiteral("A port, duration, or numeric address is invalid.");
    if (parser.isSet("tak-profile") && (parser.isSet("udp") || parser.isSet("address") || parser.isSet("port"))) error = QStringLiteral("Use either a TAK profile or a direct TCP/UDP configuration.");
    QJsonObject profile;
    if (parser.isSet("tak-profile") && error.isEmpty()) { profile = readObject(parser.value("tak-profile"), error); if (profile.isEmpty()) error = QStringLiteral("The TAK profile is empty."); }
    if (error.isEmpty()) error = scenario.error;
    if (!error.isEmpty()) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 2; }
    Emulator emulator(scenario, app);
    if (const auto failure = emulator.start(address, static_cast<quint16>(port), parser.isSet("udp"), profile, duration); !failure.isEmpty()) { std::fprintf(stderr, "%s\n", qPrintable(failure)); return 1; }
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
    std::fprintf(stderr, "Emulator started: %lld units, %.2f messages per second per unit. Press Ctrl+C to stop.\n", static_cast<long long>(scenario.units.size()), scenario.frequency);
    const auto result = app.exec(); emulator.report(); return result;
}
