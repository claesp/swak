#include "core/maps.h"
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>
namespace swak {
namespace {
bool validSecret(const QByteArray &secret)
{
    return !secret.isEmpty() && secret.size() <= 4096 && std::all_of(secret.cbegin(), secret.cend(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}
}
bool authenticatedMap(const QJsonObject &source)
{ return source.value(QStringLiteral("authMode")).toString(QStringLiteral("none")) != QStringLiteral("none"); }
QString validateMapAuthentication(const QJsonObject &source)
{
    const auto mode = source.value(QStringLiteral("authMode")).toString(QStringLiteral("none"));
    if (source.contains(QStringLiteral("authMode")) && !source.value(QStringLiteral("authMode")).isString()) return QObject::tr("Invalid authentication method.");
    if (!QStringList{QStringLiteral("none"), QStringLiteral("basic"), QStringLiteral("bearer"), QStringLiteral("header"), QStringLiteral("query")}.contains(mode)) return QObject::tr("Invalid authentication method.");
    for (const auto *field : {"authUser", "authParameter", "authEnvironment"}) {
        const auto value = source.value(QLatin1String(field));
        const auto text = value.toString();
        if (!value.isUndefined() && (!value.isString() || text.size() > 256 ||
            std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() > 0x7e; })))
            return QObject::tr("Use bounded ASCII authentication fields.");
    }
    const auto environment = source.value(QStringLiteral("authEnvironment")).toString();
    static const QRegularExpression envPattern(QStringLiteral("\\ASWAK_MAP_[A-Z0-9_]{1,119}\\z"));
    if (!environment.isEmpty() && !envPattern.match(environment).hasMatch()) return QObject::tr("Use a secret variable named SWAK_MAP_ followed by uppercase letters, digits, or underscores.");
    if (mode == QStringLiteral("none")) return {};
    if (source.value(QStringLiteral("ignoreTls")).toBool()) return QObject::tr("Enable certificate validation before using credentials.");
    const auto user = source.value(QStringLiteral("authUser")).toString();
    if (mode == QStringLiteral("basic") && (user.isEmpty() || user.contains(':'))) return QObject::tr("Basic authentication needs a username without a colon.");
    const auto parameter = source.value(QStringLiteral("authParameter")).toString();
    static const QRegularExpression parameterPattern(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,63}\\z"));
    if ((mode == QStringLiteral("header") || mode == QStringLiteral("query")) && !parameterPattern.match(parameter).hasMatch()) return QObject::tr("Enter a valid API key parameter.");
    if (mode == QStringLiteral("header") && !parameter.startsWith(QLatin1String("X-"), Qt::CaseInsensitive)) return QObject::tr("Custom credential headers must start with X-.");
    return {};
}
QString authorizeMapRequest(QNetworkRequest &request, const QJsonObject &source, const QByteArray &secret)
{
    const auto error = validateMapAuthentication(source); if (!error.isEmpty()) return error;
    if (!authenticatedMap(source)) return {};
    if (!validSecret(secret)) return QObject::tr("Set a map credential through Credentials or its environment variable.");
    const QUrl origin(source.value(QStringLiteral("endpoint")).toString()); const auto url = request.url();
    if (origin.scheme() != QStringLiteral("https") || url.scheme() != QStringLiteral("https") ||
        origin.host().isEmpty() || origin.host() != url.host() || origin.port(443) != url.port(443) ||
        !url.userInfo().isEmpty() || url.hasFragment()) return QObject::tr("The map credential cannot be sent to a different server.");
    const auto mode = source.value(QStringLiteral("authMode")).toString();
    if (mode == QStringLiteral("basic")) request.setRawHeader("Authorization", "Basic " + (source.value(QStringLiteral("authUser")).toString().toUtf8() + ':' + secret).toBase64());
    else if (mode == QStringLiteral("bearer")) request.setRawHeader("Authorization", "Bearer " + secret);
    else if (mode == QStringLiteral("header")) request.setRawHeader(source.value(QStringLiteral("authParameter")).toString().toLatin1(), secret);
    else {
        auto target = url; QUrlQuery query(target); const auto parameter = source.value(QStringLiteral("authParameter")).toString();
        query.removeAllQueryItems(parameter);
        query.addQueryItem(parameter, QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(secret))));
        target.setQuery(query); request.setUrl(target);
    }
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    return {};
}
QString MapSources::setCredential(const QString &id, const QByteArray &secret)
{
    const auto source = sources().value(id).toObject();
    if (!authenticatedMap(source) || !sources().contains(id)) return tr("Select an authenticated map provider first.");
    const auto error = validateMapAuthentication(source); if (!error.isEmpty()) return error;
    if (!secret.isEmpty() && !validSecret(secret)) return tr("Use a secret with 1 through 4096 printable ASCII characters.");
    if (secret.isEmpty()) credentials_.remove(id); else credentials_.insert(id, secret);
    emit changed(); return {};
}
QByteArray MapSources::credential(const QString &id) const
{
    if (credentials_.contains(id)) return credentials_.value(id);
    const auto source = sources().value(id).toObject();
    if (!authenticatedMap(source) || !validateMapAuthentication(source).isEmpty()) return {};
    const auto environment = source.value(QStringLiteral("authEnvironment")).toString();
    if (environment.isEmpty()) return {};
    const auto value = qgetenv(environment.toLatin1().constData());
    return validSecret(value) ? value : QByteArray{};
}
}
