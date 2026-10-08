#include "core/delete_confirmation.h"
#include "core/emergency.h"
#include "core/files.h"
#include "core/identity.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/preferences.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/ui_icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace swak {
namespace {
class Editor final : public QWidget, public SettingsParticipant
{
public:
    Editor(Store &store, const Codec &codec, Identity *identity, Servers *servers, QWidget *parent)
        : QWidget(parent), store_(store), codec_(codec), identity_(identity), servers_(servers)
    {
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        uid_ = textField(QStringLiteral("identity"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces));
        callsign_ = textField(QStringLiteral("callsign"), QStringLiteral("Local event"));
        type_ = textField(QStringLiteral("eventType"), QStringLiteral("a-u-G"));
        how_ = textField(QStringLiteral("locationMethod"), QStringLiteral("h-e"));
        how_->setMaxLength(3);
        team_ = textField(QStringLiteral("team"), {});
        team_->setMaxLength(64);
        role_ = textField(QStringLiteral("role"), {});
        role_->setMaxLength(64);
        latitude_ = numberField(QStringLiteral("latitude"), -90, 90, 6);
        longitude_ = numberField(QStringLiteral("longitude"), -180, 180, 6);
        altitude_ = numberField(QStringLiteral("altitude"), -100000, 100000000, 1);
        altitude_->setToolTip(tr("Height above the WGS 84 reference surface, in meters."));
        lifetime_ = new QSpinBox(this);
        lifetime_->setObjectName(QStringLiteral("lifetime"));
        lifetime_->setRange(1, 86400);
        lifetime_->setValue(300);
        lifetime_->setSuffix(tr(" s"));
        form->addRow(tr("&Identity"), uid_);
        form->addRow(tr("&Callsign"), callsign_);
        type_->setAccessibleName(tr("Type"));
        how_->setAccessibleName(tr("Location method"));
        team_->setAccessibleName(tr("Team"));
        role_->setAccessibleName(tr("Role"));
        form->addRow(tr("Event &type"), cotFieldEditor(type_, CotField::Type));
        form->addRow(tr("Location method"), cotFieldEditor(how_, CotField::How));
        form->addRow(tr("Team"), cotFieldEditor(team_, CotField::Team));
        form->addRow(tr("Role"), cotFieldEditor(role_, CotField::Role));
        form->addRow(tr("&Latitude (°)"), latitude_);
        form->addRow(tr("L&ongitude (°)"), longitude_);
        form->addRow(tr("&Altitude (m)"), altitude_);
        form->addRow(tr("&Lifetime"), lifetime_);
        layout->addLayout(form);
        auto *actions = new QHBoxLayout;
        auto *generate = new QPushButton(tr("Generate event"), this);
        generate->setObjectName(QStringLiteral("generateEvent"));
        auto *newIdentity = new QPushButton(tr("New identity"), this);
        actions->addWidget(generate);
        actions->addWidget(newIdentity);
        layout->addLayout(actions);
        send_ = new QCheckBox(tr("Send generated CoT to connected servers and LAN peers"), this);
        send_->setObjectName(QStringLiteral("sendGenerated"));
        send_->setEnabled(servers_ != nullptr);
        layout->addWidget(send_);
        // An emergency comes from the sender identity at the position above.
        auto *alerts = new QHBoxLayout;
        emergencyKind_ = new QComboBox(this);
        emergencyKind_->setObjectName(QStringLiteral("emergencyKind"));
        emergencyKind_->setAccessibleName(tr("Emergency type"));
        for (const auto &kind : emergencyKinds()) {
            emergencyKind_->addItem(kind.label);
        }
        auto *sendAlert =
            new QPushButton(uiIcon(UiIcon::Emergency), tr("Send emergency ..."), this);
        sendAlert->setObjectName(QStringLiteral("sendEmergency"));
        sendAlert->setEnabled(identity_ != nullptr);
        cancelAlert_ = new QPushButton(tr("Cancel emergency"), this);
        cancelAlert_->setObjectName(QStringLiteral("cancelEmergency"));
        cancelAlert_->setEnabled(false);
        alerts->addWidget(emergencyKind_);
        alerts->addWidget(sendAlert);
        alerts->addWidget(cancelAlert_);
        layout->addLayout(alerts);
        connect(sendAlert, &QPushButton::clicked, this, [this] {
            sendEmergency();
        });
        connect(cancelAlert_, &QPushButton::clicked, this, [this] {
            cancelEmergency();
        });
        auto *useIdentity = new QPushButton(tr("Use sender identity"), this);
        useIdentity->setObjectName(QStringLiteral("useSenderIdentity"));
        useIdentity->setEnabled(identity_ != nullptr);
        layout->addWidget(useIdentity);
        connect(useIdentity, &QPushButton::clicked, this, [this] {
            useSenderIdentity();
        });
        if (identity_) {
            useSenderIdentity();
            connect(identity_, &Identity::changed, this, [this] {
                useSenderIdentity();
            });
        }

        auto *previewLabel = new QLabel(tr("CoT &XML preview"), this);
        preview_ = new QPlainTextEdit(this);
        preview_->setObjectName(QStringLiteral("xmlPreview"));
        preview_->setReadOnly(true);
        preview_->setMaximumBlockCount(300);
        preview_->setMinimumHeight(90);
        previewLabel->setBuddy(preview_);
        layout->addWidget(previewLabel);
        layout->addWidget(preview_);
        auto *files = new QHBoxLayout;
        auto *importButton = new QPushButton(tr("Import XML…"), this);
        export_ = new QPushButton(tr("Export XML…"), this);
        export_->setEnabled(false);
        files->addWidget(importButton);
        files->addWidget(export_);
        layout->addLayout(files);
        status_ = new QLabel(
            tr("Generate an event to add it to the map. No data is sent over the network."), this);
        status_->setObjectName(QStringLiteral("editorStatus"));
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        connect(generate, &QPushButton::clicked, this, [this] {
            generateEvent();
        });
        connect(newIdentity, &QPushButton::clicked, this, [this] {
            uid_->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
            showStatus(tr("A new identity is ready. Generate the event to add it to the map."));
        });
        connect(importButton, &QPushButton::clicked, this, [this] {
            importFile();
        });
        connect(export_, &QPushButton::clicked, this, [this] {
            exportFile();
        });
        const auto refreshPreview = [this] {
            const auto &events = store_.events();
            const auto event =
                std::find_if(events.begin(), events.end(), [this](const Event &item) {
                    return item.uid == store_.selected();
                });
            if (event != events.end()) {
                showPreview(*event);
            }
        };
        connect(&store_, &Store::selectionChanged, this, refreshPreview);
        connect(&store_, &Store::changed, this, refreshPreview);
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.editor");
    }
    QString settingsTitle() const override
    {
        return tr("CoT Event Generator");
    }
    QList<SettingField> settingsFields() const override
    {
        return {booleanSetting(QStringLiteral("send"),
                               tr("Send generated CoT to connected servers and LAN peers")),
                textSetting(QStringLiteral("identity"), tr("Identity"), 128),
                textSetting(QStringLiteral("callsign"), tr("Callsign"), 128),
                cotSetting(QStringLiteral("type"), tr("Type"), CotField::Type),
                cotSetting(QStringLiteral("how"), tr("Location method"), CotField::How),
                cotSetting(QStringLiteral("team"), tr("Team"), CotField::Team),
                cotSetting(QStringLiteral("role"), tr("Role"), CotField::Role),
                numberSetting(QStringLiteral("latitude"), tr("Latitude"), -90, 90, 6),
                numberSetting(QStringLiteral("longitude"), tr("Longitude"), -180, 180, 6),
                numberSetting(
                    QStringLiteral("altitude"), tr("Altitude (meters)"), -100000, 100000000, 1),
                numberSetting(QStringLiteral("lifetime"), tr("Lifetime (seconds)"), 1, 86400),
                textSetting(QStringLiteral("importDirectory"), tr("Import directory"), 4096),
                textSetting(QStringLiteral("exportDirectory"), tr("Export directory"), 4096)};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("send"), send_->isChecked()},
                {QStringLiteral("identity"), uid_->text()},
                {QStringLiteral("callsign"), callsign_->text()},
                {QStringLiteral("type"), type_->text()},
                {QStringLiteral("how"), how_->text()},
                {QStringLiteral("team"), team_->text()},
                {QStringLiteral("role"), role_->text()},
                {QStringLiteral("latitude"), latitude_->value()},
                {QStringLiteral("longitude"), longitude_->value()},
                {QStringLiteral("altitude"), altitude_->value()},
                {QStringLiteral("lifetime"), lifetime_->value()},
                {QStringLiteral("importDirectory"), importDirectory_},
                {QStringLiteral("exportDirectory"), exportDirectory_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        send_->setChecked(values.value(QStringLiteral("send")).toBool(false));
        uid_->setText(preferenceText(values, QStringLiteral("identity"), uid_->text(), 128));
        callsign_->setText(
            preferenceText(values, QStringLiteral("callsign"), callsign_->text(), 128));
        type_->setText(preferenceText(values, QStringLiteral("type"), type_->text(), 128));
        how_->setText(preferenceText(values, QStringLiteral("how"), how_->text(), 3));
        team_->setText(preferenceText(values, QStringLiteral("team"), team_->text(), 64));
        role_->setText(preferenceText(values, QStringLiteral("role"), role_->text(), 64));
        latitude_->setValue(preferenceNumber(values, QStringLiteral("latitude"), 0, -90, 90));
        longitude_->setValue(preferenceNumber(values, QStringLiteral("longitude"), 0, -180, 180));
        altitude_->setValue(
            preferenceNumber(values, QStringLiteral("altitude"), 0, -100000, 100000000));
        lifetime_->setValue(preferenceInteger(values, QStringLiteral("lifetime"), 300, 1, 86400));
        importDirectory_ = preferenceText(values, QStringLiteral("importDirectory"), {}, 4096);
        exportDirectory_ = preferenceText(values, QStringLiteral("exportDirectory"), {}, 4096);
    }

private:
    void useSenderIdentity()
    {
        if (!identity_) {
            return;
        }
        Event event;
        identity_->apply(event);
        uid_->setText(event.uid);
        callsign_->setText(event.callsign);
        type_->setText(event.type);
        how_->setText(event.how);
        team_->setText(event.team);
        role_->setText(event.role);
    }
    void showStatus(const QString &message, Severity severity = Severity::Info)
    {
        status_->setText(message);
        logMessage(severity,
                   severity >= Severity::Warning ? Urgency::Attention : Urgency::Routine,
                   QStringLiteral("swak.editor"),
                   message);
    }
    QLineEdit *textField(const QString &name, const QString &value)
    {
        auto *field = new QLineEdit(value, this);
        field->setMaxLength(128);
        field->setObjectName(name);
        return field;
    }
    QDoubleSpinBox *numberField(const QString &name, double low, double high, int decimals)
    {
        auto *field = new QDoubleSpinBox(this);
        field->setObjectName(name);
        field->setDecimals(decimals);
        field->setRange(low, high);
        return field;
    }
    void showPreview(const Event &event)
    {
        const auto next = codec_.encode(event);
        if (output_ == next) {
            return;
        }
        output_ = next;
        preview_->setPlainText(QString::fromUtf8(output_));
        export_->setEnabled(!output_.isEmpty());
    }
    void generateEvent()
    {
        Event event;
        if (identity_) {
            identity_->apply(event);
        }
        event.uid = uid_->text();
        event.callsign = callsign_->text();
        event.type = type_->text();
        event.how = how_->text();
        event.team = team_->text();
        event.role = role_->text();
        event.latitude = latitude_->value();
        event.longitude = longitude_->value();
        event.altitude = altitude_->value();
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(lifetime_->value());
        const auto error = store_.accept(event);
        if (!error.isEmpty()) {
            showStatus(error, Severity::Error);
            return;
        }
        showPreview(event);
        store_.select(event.uid);
        if (send_->isChecked() && servers_) {
            const auto sendError = servers_->send(event);
            showStatus(
                sendError.isEmpty()
                    ? tr("The event is on the map and queued for connected peers and servers.")
                    : sendError,
                sendError.isEmpty() ? Severity::Info : Severity::Warning);
        } else {
            showStatus(tr("The event is on the map. Export the XML to save it."));
        }
    }
    Event sender() const
    {
        Event event;
        identity_->apply(event);
        event.latitude = latitude_->value();
        event.longitude = longitude_->value();
        event.altitude = altitude_->value();
        return event;
    }
    void sendEmergency()
    {
        if (!identity_) {
            return;
        }
        const auto label = emergencyKind_->currentText();
        const QPointer<Editor> self(this);
        const bool confirmed =
            confirmAction(this,
                          tr("Send emergency"),
                          tr("Send a %1 at %2, %3 to all connected servers and peers?")
                              .arg(label,
                                   QLocale().toString(latitude_->value(), 'f', 5),
                                   QLocale().toString(longitude_->value(), 'f', 5)),
                          tr("Send"));
        // The confirmation runs a nested loop that can delete this pane.
        // cppcheck-suppress knownConditionTrueFalse
        if (!self || !confirmed) {
            return;
        }
        const auto event = emergencyEvent(sender(), label);
        const auto error = store_.accept(event);
        if (!error.isEmpty()) {
            showStatus(error, Severity::Error);
            return;
        }
        cancelAlert_->setEnabled(true);
        const auto sendError = servers_ ? servers_->send(event) : tr("No server service exists.");
        showStatus(sendError.isEmpty()
                       ? tr("The %1 is on the map and sent.").arg(label)
                       : tr("The %1 is on the map but was not sent: %2").arg(label, sendError),
                   sendError.isEmpty() ? Severity::Warning : Severity::Error);
    }
    void cancelEmergency()
    {
        if (!identity_) {
            return;
        }
        const auto event = emergencyCancelEvent(sender());
        store_.accept(event);
        cancelAlert_->setEnabled(false);
        const auto sendError = servers_ ? servers_->send(event) : tr("No server service exists.");
        showStatus(sendError.isEmpty()
                       ? tr("The emergency is cancelled.")
                       : tr("The emergency is cancelled here but the cancel was not sent: %1")
                             .arg(sendError),
                   sendError.isEmpty() ? Severity::Info : Severity::Error);
    }
    void importFile()
    {
        // The file dialog runs a nested loop. It can delete this editor.
        const QPointer<Editor> self(this);
        const auto path = QFileDialog::getOpenFileName(
            this,
            tr("Import a CoT event"),
            !importDirectory_.isEmpty() && QDir(importDirectory_).exists() ? importDirectory_
                                                                           : QDir::homePath(),
            tr("CoT XML (*.xml *.cot);;All files (*)"));
        // The nested dialog loop can delete this object. Cppcheck cannot see that.
        // cppcheck-suppress knownConditionTrueFalse
        if (!self || path.isEmpty()) {
            return;
        }
        importDirectory_ = QFileInfo(path).absolutePath();
        const auto input = readEventFile(path);
        if (!input.error.isEmpty()) {
            showStatus(input.error, Severity::Error);
            return;
        }
        const auto decoded = codec_.decode(input.bytes);
        if (!decoded.event) {
            showStatus(decoded.error.isEmpty() ? tr("The file does not contain a CoT event.")
                                               : decoded.error,
                       Severity::Error);
            return;
        }
        const auto error = store_.accept(*decoded.event);
        if (!error.isEmpty()) {
            showStatus(error, Severity::Error);
            return;
        }
        showPreview(*decoded.event);
        store_.select(decoded.event->uid);
        showStatus(tr("The event was imported. Exports keep its detail fields."));
    }
    void exportFile()
    {
        if (output_.isEmpty()) {
            return;
        }
        const QPointer<Editor> self(this);
        const auto path = QFileDialog::getSaveFileName(
            this,
            tr("Export the preview event"),
            QDir(!exportDirectory_.isEmpty() && QDir(exportDirectory_).exists() ? exportDirectory_
                                                                                : QDir::homePath())
                .filePath(QStringLiteral("event.xml")),
            tr("CoT XML (*.xml)"));
        // The nested dialog loop can delete this object. Cppcheck cannot see that.
        // cppcheck-suppress knownConditionTrueFalse
        if (!self || path.isEmpty()) {
            return;
        }
        exportDirectory_ = QFileInfo(path).absolutePath();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(output_) != output_.size() ||
            !file.commit()) {
            showStatus(tr("The file cannot be saved: %1").arg(file.errorString()), Severity::Error);
            return;
        }
        showStatus(tr("The XML preview was saved."));
    }
    Store &store_;
    const Codec &codec_;
    QPointer<Identity> identity_;
    QPointer<Servers> servers_;
    QCheckBox *send_ = nullptr;
    QComboBox *emergencyKind_ = nullptr;
    QPushButton *cancelAlert_ = nullptr;
    QLineEdit *uid_ = nullptr;
    QLineEdit *callsign_ = nullptr;
    QLineEdit *type_ = nullptr;
    QLineEdit *how_ = nullptr;
    QLineEdit *team_ = nullptr;
    QLineEdit *role_ = nullptr;
    QDoubleSpinBox *latitude_ = nullptr;
    QDoubleSpinBox *longitude_ = nullptr;
    QDoubleSpinBox *altitude_ = nullptr;
    QSpinBox *lifetime_ = nullptr;
    QPlainTextEdit *preview_ = nullptr;
    QPushButton *export_ = nullptr;
    QLabel *status_ = nullptr;
    QByteArray output_;
    QString importDirectory_;
    QString exportDirectory_;
};
} // namespace
class EditorPlugin final : public QObject, public Plugin, public ToolPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "editor/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::ToolPlugin)
public:
    QString name() const override
    {
        return tr("CoT Event Generator");
    }
    QWidget *create(PluginServices &services, const Codec &codec, QWidget *parent) override
    {
        return new Editor(services.store, codec, services.identity, services.servers, parent);
    }
};
} // namespace swak
#include "editor.moc"
