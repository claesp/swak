#include "core/routing.h"

#include "core/logging.h"

#include <QJsonDocument>
#include <QNetworkRequest>

#include <algorithm>
#include <cmath>

namespace swak {
namespace {
constexpr qsizetype MaxReply = 4 * 1024 * 1024;
bool pointValid(QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y()) && std::abs(point.x()) <= 180 &&
           std::abs(point.y()) <= 85.051129;
}
} // namespace
RouteSources::RouteSources(const QList<QObject *> &plugins, QObject *parent) : QObject(parent)
{
    for (auto *plugin : plugins) {
        if (auto *format = qobject_cast<RouteProvider *>(plugin)) {
            providers_.append(format);
        }
    }
    deadline_.setSingleShot(true);
    cooldown_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(tr("The route request timed out."));
    });
    restorePreferences({});
}
QList<SettingField> RouteSources::fields()
{
    return {textSetting(QStringLiteral("name"), tr("Name"), 100),
            textSetting(QStringLiteral("endpoint"), tr("Route URL"), 2048),
            textSetting(QStringLiteral("attribution"), tr("Attribution"), 300)};
}
QString RouteSources::validate(const QJsonObject &source)
{
    const auto name = source.value(QStringLiteral("name")).toString();
    const auto endpoint = source.value(QStringLiteral("endpoint")).toString();
    const QUrl url(endpoint);
    const bool local =
        url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1");
    if (name.trimmed().isEmpty() || name.size() > 100 || endpoint.size() > 2048 || !url.isValid() ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (url.scheme() != QStringLiteral("https") &&
         !(local && url.scheme() == QStringLiteral("http")))) {
        return tr("Use a name and an HTTPS route URL without credentials, query, or fragment. "
                  "Local loopback HTTP is also supported.");
    }
    if (source.value(QStringLiteral("attribution")).toString().size() > 300) {
        return tr("Attribution is too long.");
    }
    return {};
}
RouteProvider *RouteSources::provider(const QJsonObject &source) const
{
    const auto found =
        std::find_if(providers_.cbegin(), providers_.cend(), [&source](const auto *entry) {
            return entry->routeId() == source.value(QStringLiteral("format")).toString();
        });
    return found == providers_.cend() ? nullptr : *found;
}
void RouteSources::restorePreferences(const QJsonObject &values)
{
    cancel();
    values_ = values;
    auto entries = sources();
    if (!values.contains(QStringLiteral("sources"))) {
        for (const auto *format : providers_) {
            const auto presets = format->presets();
            for (auto it = presets.begin(); it != presets.end(); ++it) {
                auto entry = it.value().toObject();
                entry.insert(QStringLiteral("format"), format->routeId());
                entries.insert(it.key(), entry);
            }
        }
    }
    QJsonObject retained;
    for (auto it = entries.begin(); it != entries.end() && retained.size() < 16; ++it) {
        if (it.key().size() <= 128 && it.value().isObject() &&
            validate(it.value().toObject()).isEmpty()) {
            retained.insert(it.key(), it.value());
        }
    }
    values_.insert(QStringLiteral("sources"), retained);
    values_.insert(QStringLiteral("active"),
                   preferenceText(values,
                                  QStringLiteral("active"),
                                  retained.isEmpty() ? QString{} : retained.begin().key(),
                                  128));
    emit changed();
}
QString RouteSources::put(const QString &id, const QJsonObject &source)
{
    const auto error = validate(source);
    if (!error.isEmpty()) {
        return error;
    }
    auto entries = sources();
    if (id.isEmpty() || id.size() > 128 || (!entries.contains(id) && entries.size() >= 16) ||
        !provider(source)) {
        return tr("An available route format and fewer than 16 providers are required.");
    }
    cancel();
    entries.insert(id, source);
    values_.insert(QStringLiteral("sources"), entries);
    emit changed();
    return {};
}
void RouteSources::select(const QString &id)
{
    if (!sources().contains(id)) {
        return;
    }
    cancel();
    values_.insert(QStringLiteral("active"), id);
    emit changed();
}
void RouteSources::remove(const QString &id)
{
    cancel();
    auto entries = sources();
    entries.remove(id);
    values_.insert(QStringLiteral("sources"), entries);
    if (activeId() == id) {
        values_.insert(QStringLiteral("active"), QString{});
    }
    emit changed();
}
void RouteSources::cancel()
{
    deadline_.stop();
    if (reply_) {
        auto *reply = reply_.data();
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    body_.clear();
}
void RouteSources::fail(const QString &message)
{
    cancel();
    result_ = {};
    result_.error = message;
    logMessage(Severity::Warning, Urgency::Routine, QStringLiteral("swak.routing"), message);
    emit completed();
}
void RouteSources::calculate(QPointF start, QPointF end, const QString &mode, double speed)
{
    if (cooldown_.isActive()) {
        fail(tr("Wait two seconds between route requests."));
        return;
    }
    cancel();
    const auto source = sources().value(activeId()).toObject();
    auto *format = provider(source);
    if (!format || !validate(source).isEmpty()) {
        fail(tr("Select an available route provider."));
        return;
    }
    if (!pointValid(start) || !pointValid(end) || start == end ||
        !QStringList{"driving", "walking", "running", "cycling"}.contains(mode) ||
        !std::isfinite(speed) || speed < 0.5 || speed > 25) {
        fail(tr("Use two different valid locations and a supported travel mode."));
        return;
    }
    result_ = {};
    QNetworkRequest request(QUrl(source.value(QStringLiteral("endpoint")).toString()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", "SWAK/0.1 routing");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    reply_ = network_.post(
        request,
        QJsonDocument(format->request(start, end, mode, speed)).toJson(QJsonDocument::Compact));
    reply_->setReadBufferSize(MaxReply + 1);
    cooldown_.start(2000);
    deadline_.start(20000);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        body_ += reply_->read(MaxReply + 1 - body_.size());
        if (body_.size() > MaxReply) {
            fail(tr("The route response exceeds 4 MiB."));
        }
    });
    connect(reply_, &QNetworkReply::finished, this, [this, format] {
        if (!reply_) {
            return;
        }
        auto *reply = reply_.data();
        body_ += reply->read(MaxReply + 1 - body_.size());
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (body_.size() > MaxReply) {
            fail(tr("The route response exceeds 4 MiB."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            fail(tr("Route request failed (HTTP %1): %2").arg(status).arg(reply->errorString()));
            return;
        }
        const auto body = body_;
        cancel();
        result_ = format->decode(body);
        if (!result_.error.isEmpty()) {
            logMessage(
                Severity::Warning, Urgency::Routine, QStringLiteral("swak.routing"), result_.error);
        }
        emit completed();
    });
}
} // namespace swak
