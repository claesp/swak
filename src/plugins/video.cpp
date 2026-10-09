#include "core/interfaces.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/single_line_label.h"
#include "core/video_sources.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

namespace swak {
namespace {
// A stream that does not start in this time shows as unavailable.
constexpr int StartTimeoutMilliseconds = 15000;

class VideoPane final : public QWidget, public PreferenceParticipant
{
public:
    VideoPane(PluginServices &services, QWidget *parent) : QWidget(parent), sources_(services.video)
    {
        setObjectName(QStringLiteral("videoPane"));
        auto *layout = new QVBoxLayout(this);
        splitter_ = new QSplitter(Qt::Horizontal, this);
        splitter_->setObjectName(QStringLiteral("videoSplitter"));
        auto *side = new QWidget(splitter_);
        auto *sideLayout = new QVBoxLayout(side);
        sideLayout->setContentsMargins(0, 0, 0, 0);
        list_ = new QListWidget(side);
        list_->setObjectName(QStringLiteral("videoSources"));
        list_->setAccessibleName(tr("Video sources"));
        sideLayout->addWidget(list_);
        auto *row = new QHBoxLayout;
        auto *play = new QPushButton(tr("Play"), side);
        play->setObjectName(QStringLiteral("playVideo"));
        auto *stop = new QPushButton(tr("Stop"), side);
        stop->setObjectName(QStringLiteral("stopVideo"));
        row->addWidget(play);
        row->addWidget(stop);
        sideLayout->addLayout(row);
        auto *second = new QHBoxLayout;
        auto *refresh = new QPushButton(tr("Refresh"), side);
        refresh->setObjectName(QStringLiteral("refreshVideo"));
        refresh->setToolTip(tr("Read the video lists of the TAK Server entries."));
        auto *add = new QPushButton(tr("Add URL ..."), side);
        add->setObjectName(QStringLiteral("addVideo"));
        second->addWidget(refresh);
        second->addWidget(add);
        sideLayout->addLayout(second);
        // The video widget and player load the media backend. Create them at the first Play,
        // so a window that never plays video does not load FFmpeg.
        placeholder_ = new QLabel(tr("No video"), splitter_);
        placeholder_->setObjectName(QStringLiteral("videoPlaceholder"));
        placeholder_->setAlignment(Qt::AlignCenter);
        placeholder_->setMinimumSize(160, 90);
        splitter_->setStretchFactor(1, 3);
        layout->addWidget(splitter_);
        status_ = new SingleLineLabel(tr("Select a source and Play."), this);
        status_->setObjectName(QStringLiteral("videoStatus"));
        layout->addWidget(status_);
        start_.setSingleShot(true);
        start_.setInterval(StartTimeoutMilliseconds);
        connect(&start_, &QTimer::timeout, this, [this] {
            waitingForFrame_ = false;
            if (player_) {
                player_->stop();
            }
            showStatus(tr("Unavailable: the stream did not start in %1 seconds.")
                           .arg(StartTimeoutMilliseconds / 1000));
        });
        if (!sources_) {
            setEnabled(false);
            return;
        }
        connect(sources_, &VideoSources::changed, this, &VideoPane::showSources);
        connect(sources_, &VideoSources::refreshed, this, [this](const QString &error) {
            showStatus(
                error.isEmpty()
                    ? tr("%n video sources.", nullptr, static_cast<int>(sources_->sources().size()))
                    : error);
        });
        connect(play, &QPushButton::clicked, this, &VideoPane::playSelected);
        connect(list_, &QListWidget::itemDoubleClicked, this, &VideoPane::playSelected);
        connect(stop, &QPushButton::clicked, this, [this] {
            waitingForFrame_ = false;
            start_.stop();
            if (player_) {
                player_->stop();
                player_->setSource({});
            }
            showStatus(tr("Stopped."));
        });
        connect(refresh, &QPushButton::clicked, this, [this] {
            showStatus(tr("Reading the video lists…"));
            sources_->refresh();
        });
        connect(add, &QPushButton::clicked, this, &VideoPane::addUrl);
        showSources();
    }
    ~VideoPane() override
    {
        // Stop the stream before the video widget goes away.
        if (player_) {
            player_->stop();
        }
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.video");
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
    void hideEvent(QHideEvent *event) override
    {
        // A hidden pane does not keep a stream open.
        // A stream that connects is still in the stopped state, so check both.
        if (player_ &&
            (waitingForFrame_ || player_->playbackState() != QMediaPlayer::StoppedState)) {
            waitingForFrame_ = false;
            start_.stop();
            player_->stop();
            showStatus(tr("Stopped, because the pane is hidden."));
        }
        QWidget::hideEvent(event);
    }

private:
    // Creates the video widget and the player at the first Play.
    void ensurePlayer()
    {
        if (player_) {
            return;
        }
        view_ = new QVideoWidget;
        view_->setObjectName(QStringLiteral("videoView"));
        view_->setMinimumSize(160, 90);
        splitter_->replaceWidget(1, view_);
        placeholder_->deleteLater();
        placeholder_ = nullptr;
        // The player has no audio output, so a stream cannot play sound unexpectedly.
        player_ = new QMediaPlayer(this);
        player_->setObjectName(QStringLiteral("videoPlayer"));
        player_->setVideoOutput(view_);
        // A live stream can stay in the buffering state while it shows frames. The first frame
        // shows that the stream plays.
        connect(view_->videoSink(),
                &QVideoSink::videoFrameChanged,
                this,
                [this](const QVideoFrame &frame) {
                    if (frame.isValid() && waitingForFrame_) {
                        waitingForFrame_ = false;
                        start_.stop();
                        showStatus(tr("Playing %1").arg(playing_));
                    }
                });
        connect(player_,
                &QMediaPlayer::errorOccurred,
                this,
                [this](QMediaPlayer::Error, const QString &text) {
                    waitingForFrame_ = false;
                    start_.stop();
                    showStatus(
                        tr("Unavailable: %1")
                            .arg(text.isEmpty() ? tr("the stream cannot play.") : text.left(200)));
                });
        connect(player_,
                &QMediaPlayer::mediaStatusChanged,
                this,
                [this](QMediaPlayer::MediaStatus state) {
                    switch (state) {
                    case QMediaPlayer::LoadingMedia:
                    case QMediaPlayer::BufferingMedia:
                    case QMediaPlayer::StalledMedia:
                    case QMediaPlayer::LoadedMedia:
                    case QMediaPlayer::BufferedMedia:
                        if (waitingForFrame_) {
                            showStatus(state == QMediaPlayer::LoadingMedia ? tr("Connecting…")
                                                                           : tr("Buffering…"));
                        }
                        break;
                    case QMediaPlayer::EndOfMedia:
                        showStatus(tr("The stream ended."));
                        break;
                    case QMediaPlayer::InvalidMedia:
                        waitingForFrame_ = false;
                        start_.stop();
                        showStatus(tr("Unavailable: the stream format cannot play."));
                        break;
                    case QMediaPlayer::NoMedia:
                        break;
                    }
                });
    }
    void showStatus(const QString &text)
    {
        status_->setText(text);
    }
    void showSources()
    {
        const auto *current = list_->currentItem();
        const auto previous = current ? current->data(Qt::UserRole).toString() : QString{};
        list_->clear();
        for (const auto &source : sources_->sources()) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1 · %2").arg(source.name, videoProtocolName(source.protocol)),
                list_);
            item->setData(Qt::UserRole, source.key);
            item->setToolTip(source.key);
            if (source.key == previous) {
                list_->setCurrentItem(item);
            }
        }
    }
    void playSelected()
    {
        const auto *item = list_->currentItem();
        if (!item) {
            showStatus(tr("Select a source."));
            return;
        }
        const auto key = item->data(Qt::UserRole).toString();
        const auto &all = sources_->sources();
        const auto found = std::find_if(all.cbegin(), all.cend(), [&key](const auto &source) {
            return source.key == key;
        });
        if (found == all.cend()) {
            return;
        }
        playing_ = found->name;
        waitingForFrame_ = true;
        ensurePlayer();
        player_->stop();
        player_->setSource(found->url);
        player_->play();
        start_.start();
        showStatus(tr("Connecting to %1…").arg(found->name));
    }
    void addUrl()
    {
        Modal<QInputDialog> dialog(this);
        dialog->setObjectName(QStringLiteral("videoUrlDialog"));
        dialog->setWindowTitle(tr("Add Video"));
        dialog->setLabelText(tr("Stream URL, such as rtsp://camera.example/live"));
        if (dialog->exec() != QDialog::Accepted || !dialog || !sources_) {
            return;
        }
        const QUrl url(dialog->textValue().trimmed(), QUrl::StrictMode);
        const auto error = sources_->add(url.host(), url);
        showStatus(error.isEmpty() ? tr("The source is added.") : error);
    }
    QPointer<VideoSources> sources_;
    QSplitter *splitter_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *placeholder_ = nullptr;
    QVideoWidget *view_ = nullptr;
    SingleLineLabel *status_ = nullptr;
    QMediaPlayer *player_ = nullptr;
    QTimer start_;
    QString playing_;
    bool waitingForFrame_ = false;
};
} // namespace
class VideoPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "video/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Video");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new VideoPane(services, parent);
    }
};
} // namespace swak
#include "video.moc"
