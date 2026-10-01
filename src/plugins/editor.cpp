#include "core/interfaces.h"
#include <algorithm>
#include "core/store.h"
#include "core/files.h"
#include "core/preferences.h"
#include <QDir>
#include <QFileInfo>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

namespace swak {
class Editor final : public QWidget, public SettingsParticipant {
public:
    Editor(Store &store, const Codec &codec, QWidget *parent) : QWidget(parent), store_(store), codec_(codec)
    {
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        uid_ = textField(QStringLiteral("identity"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        callsign_ = textField(QStringLiteral("callsign"), QStringLiteral("Local event"));
        type_ = textField(QStringLiteral("eventType"), QStringLiteral("a-u-G"));
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
        form->addRow(tr("Event &type"), type_);
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
        status_ = new QLabel(tr("Generate an event to add it to the map. No data is sent over the network."), this);
        status_->setObjectName(QStringLiteral("editorStatus"));
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        connect(generate, &QPushButton::clicked, this, [this] { generateEvent(); });
        connect(newIdentity, &QPushButton::clicked, this, [this] {
            uid_->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
            status_->setText(tr("A new identity is ready. Generate the event to add it to the map."));
        });
        connect(importButton, &QPushButton::clicked, this, [this] { importFile(); });
        connect(export_, &QPushButton::clicked, this, [this] { exportFile(); });
        const auto refreshPreview = [this] {
            const auto &events = store_.events();
            const auto event = std::find_if(events.begin(), events.end(), [this](const Event &item) {
                return item.uid == store_.selected();
            });
            if (event != events.end()) showEvent(*event);
        };
        connect(&store_, &Store::selectionChanged, this, refreshPreview);
        connect(&store_, &Store::changed, this, refreshPreview);
    }
    QString preferenceId() const override { return QStringLiteral("org.swak.editor"); }
    QString settingsTitle() const override { return tr("Event editor"); }
    QList<SettingField> settingsFields() const override
    {
        return {textSetting(QStringLiteral("identity"), tr("Identity"), 128),
                textSetting(QStringLiteral("callsign"), tr("Callsign"), 128),
                textSetting(QStringLiteral("type"), tr("Type"), 128),
                numberSetting(QStringLiteral("latitude"), tr("Latitude"), -90, 90, 6),
                numberSetting(QStringLiteral("longitude"), tr("Longitude"), -180, 180, 6),
                numberSetting(QStringLiteral("altitude"), tr("Altitude (meters)"), -100000, 100000000, 1),
                numberSetting(QStringLiteral("lifetime"), tr("Lifetime (seconds)"), 1, 86400),
                textSetting(QStringLiteral("importDirectory"), tr("Import directory"), 4096),
                textSetting(QStringLiteral("exportDirectory"), tr("Export directory"), 4096)};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("identity"), uid_->text()}, {QStringLiteral("callsign"), callsign_->text()},
                {QStringLiteral("type"), type_->text()}, {QStringLiteral("latitude"), latitude_->value()},
                {QStringLiteral("longitude"), longitude_->value()}, {QStringLiteral("altitude"), altitude_->value()},
                {QStringLiteral("lifetime"), lifetime_->value()}, {QStringLiteral("importDirectory"), importDirectory_},
                {QStringLiteral("exportDirectory"), exportDirectory_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        uid_->setText(preferenceText(values, QStringLiteral("identity"), uid_->text(), 128));
        callsign_->setText(preferenceText(values, QStringLiteral("callsign"), callsign_->text(), 128));
        type_->setText(preferenceText(values, QStringLiteral("type"), type_->text(), 128));
        latitude_->setValue(preferenceNumber(values, QStringLiteral("latitude"), 0, -90, 90));
        longitude_->setValue(preferenceNumber(values, QStringLiteral("longitude"), 0, -180, 180));
        altitude_->setValue(preferenceNumber(values, QStringLiteral("altitude"), 0, -100000, 100000000));
        lifetime_->setValue(preferenceInteger(values, QStringLiteral("lifetime"), 300, 1, 86400));
        importDirectory_ = preferenceText(values, QStringLiteral("importDirectory"), {}, 4096);
        exportDirectory_ = preferenceText(values, QStringLiteral("exportDirectory"), {}, 4096);
    }
private:
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
    void showEvent(const Event &event)
    {
        const auto next = codec_.encode(event);
        if (output_ == next) return;
        output_ = next;
        preview_->setPlainText(QString::fromUtf8(output_));
        export_->setEnabled(!output_.isEmpty());
    }
    void generateEvent()
    {
        Event event;
        event.uid = uid_->text();
        event.callsign = callsign_->text();
        event.type = type_->text();
        event.how = QStringLiteral("h-e");
        event.latitude = latitude_->value();
        event.longitude = longitude_->value();
        event.altitude = altitude_->value();
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(lifetime_->value());
        const auto error = store_.accept(event);
        if (!error.isEmpty()) { status_->setText(error); return; }
        showEvent(event);
        store_.select(event.uid);
        status_->setText(tr("The event is on the map. Export the XML to save it."));
    }
    void importFile()
    {
        const auto path = QFileDialog::getOpenFileName(this, tr("Import a CoT event"), !importDirectory_.isEmpty() && QDir(importDirectory_).exists() ? importDirectory_ : QDir::homePath(), tr("CoT XML (*.xml *.cot);;All files (*)"));
        if (path.isEmpty()) return;
        importDirectory_ = QFileInfo(path).absolutePath();
        const auto input = readEventFile(path);
        if (!input.error.isEmpty()) { status_->setText(input.error); return; }
        const auto decoded = codec_.decode(input.bytes);
        const auto error = decoded.event ? store_.accept(*decoded.event) : decoded.error;
        if (!error.isEmpty()) { status_->setText(error); return; }
        showEvent(*decoded.event);
        store_.select(decoded.event->uid);
        status_->setText(tr("The event was imported. Unknown detail fields are not included in exports."));
    }
    void exportFile()
    {
        if (output_.isEmpty()) return;
        const auto path = QFileDialog::getSaveFileName(this, tr("Export the preview event"), QDir(!exportDirectory_.isEmpty() && QDir(exportDirectory_).exists() ? exportDirectory_ : QDir::homePath()).filePath(QStringLiteral("event.xml")), tr("CoT XML (*.xml)"));
        if (path.isEmpty()) return;
        exportDirectory_ = QFileInfo(path).absolutePath();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(output_) != output_.size() || !file.commit()) {
            status_->setText(tr("The file cannot be saved: %1").arg(file.errorString()));
            return;
        }
        status_->setText(tr("The XML preview was saved."));
    }
    Store &store_;
    const Codec &codec_;
    QLineEdit *uid_;
    QLineEdit *callsign_;
    QLineEdit *type_;
    QDoubleSpinBox *latitude_;
    QDoubleSpinBox *longitude_;
    QDoubleSpinBox *altitude_;
    QSpinBox *lifetime_;
    QPlainTextEdit *preview_;
    QPushButton *export_;
    QLabel *status_;
    QByteArray output_;
    QString importDirectory_;
    QString exportDirectory_;
};
class EditorPlugin final : public QObject, public ToolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Tool/1.0")
    Q_INTERFACES(swak::ToolPlugin)
public:
    QString name() const override { return tr("Event editor"); }
    QWidget *create(Store &store, const Codec &codec, QWidget *parent) override { return new Editor(store, codec, parent); }
};
}
#include "editor.moc"
