#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <optional>

namespace snow_shot::update {
namespace {

constexpr qint64 kMaximumResponseBytes = 8 * 1024 * 1024;
constexpr int kRequestTimeoutMs = 30000;

struct Version {
    QStringList core;
    QStringList prerelease;
};

std::optional<Version> parseVersion(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
                       "(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
                       "(?:\\+([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    Version version{{match.captured(1), match.captured(2), match.captured(3)}, {}};
    if (!match.captured(4).isEmpty())
        version.prerelease = match.captured(4).split(u'.');
    return version;
}

bool numeric(const QString& text) {
    return !text.isEmpty() &&
           std::all_of(text.begin(), text.end(), [](QChar c) { return c >= u'0' && c <= u'9'; });
}

int compareIdentifier(const QString& left, const QString& right, bool number) {
    if (number && left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    return QString::compare(left, right, Qt::CaseSensitive);
}

int compareVersions(const Version& left, const Version& right) {
    for (qsizetype i = 0; i < 3; ++i) {
        const int order = compareIdentifier(left.core[i], right.core[i], true);
        if (order != 0)
            return order;
    }
    if (left.prerelease.isEmpty() != right.prerelease.isEmpty())
        return left.prerelease.isEmpty() ? 1 : -1;
    for (qsizetype i = 0; i < std::min(left.prerelease.size(), right.prerelease.size()); ++i) {
        const bool leftNumeric = numeric(left.prerelease[i]);
        const bool rightNumeric = numeric(right.prerelease[i]);
        if (leftNumeric != rightNumeric)
            return leftNumeric ? -1 : 1;
        const int order = compareIdentifier(left.prerelease[i], right.prerelease[i], leftNumeric);
        if (order != 0)
            return order;
    }
    return left.prerelease.size() == right.prerelease.size()
               ? 0
               : (left.prerelease.size() < right.prerelease.size() ? -1 : 1);
}

QString versionFromTag(const QString& tag) {
    QString version = tag.trimmed();
    if (version.startsWith(u'v') || version.startsWith(u'V'))
        version.remove(0, 1);
    const qsizetype suffix = version.indexOf(u'_');
    if (suffix >= 0)
        version.truncate(suffix);
    return version;
}

bool validHttpsUrl(const QUrl& url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty();
}

QString assetText(const QJsonObject& asset, const char* key) {
    return asset.value(QLatin1String(key)).toString();
}

class SystemProxyFactory final : public QNetworkProxyFactory {
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};

} // namespace

struct UpdateService::Impl {
    struct ReleaseAsset {
        QString version;
        QUrl appImageUrl;
        QUrl sha256Url;
        QString appImageName;
    };

    Impl(UpdateService& owner, Options value)
        : q(owner), options(std::move(value)), network(&owner), schedule(&owner) {
        if (options.installedVersion.isEmpty())
            options.installedVersion = QStringLiteral(SNOW_SHOT_VERSION);
        appImagePath = qEnvironmentVariable("APPIMAGE");
        isAppImage = !appImagePath.isEmpty() && QFileInfo(appImagePath).isFile();
        status.state = isAppImage ? UpdateState::Idle : UpdateState::Unavailable;
        schedule.setSingleShot(true);
        schedule.setTimerType(Qt::CoarseTimer);
        QObject::connect(&schedule, &QTimer::timeout, &q, [this] { check(false); });
    }

    UpdateService& q;
    Options options;
    UpdateStatus status;
    QNetworkAccessManager network;
    QTimer schedule;
    QPointer<QNetworkReply> reply;
    QTimer requestTimeout;
    QString mode = QStringLiteral("manual");
    QString appImagePath;
    bool isAppImage = false;
    bool started = false;
    bool downloading = false;
    bool cancelRequested = false;
    QString downloadedPath;
    QUrl pendingShaUrl;

    bool managedByStore() const {
        return !isAppImage;
    }

    bool busy() const {
        return status.state == UpdateState::Checking || status.state == UpdateState::Downloading ||
               status.state == UpdateState::Verifying || status.state == UpdateState::Applying;
    }

    void setState(UpdateState state) {
        if (status.state == state)
            return;
        status.state = state;
        emit q.statusChanged();
    }

    void fail(const QString& error) {
        status.error = error;
        setState(UpdateState::Failed);
        emit q.operationFinished(QStringLiteral("update"), QStringLiteral("failed"));
    }

    void armSchedule(std::chrono::milliseconds delay) {
        if (!started || mode == u"manual" || !isAppImage)
            return;
        schedule.start(delay);
    }

    void abortReply() {
        requestTimeout.stop();
        if (reply) {
            QNetworkReply* active = reply;
            reply = nullptr;
            active->abort();
            active->deleteLater();
        }
    }

    void beginRequest(const QUrl& url, const std::function<void(QNetworkReply*)>& handler) {
        abortReply();
        cancelRequested = false;
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("SnowShot/%1 Linux").arg(options.installedVersion));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        reply = network.get(request);
        requestTimeout.disconnect();
        requestTimeout.start(kRequestTimeoutMs);
        QObject::connect(&requestTimeout, &QTimer::timeout, &q, [this] {
            if (!reply)
                return;
            cancelRequested = true;
            abortReply();
            fail(QCoreApplication::translate("UpdateErrors", "The update request timed out."));
        });
        QObject::connect(reply, &QNetworkReply::finished, &q, [this, handler] {
            QNetworkReply* finished = reply;
            if (finished == nullptr)
                return;
            reply = nullptr;
            requestTimeout.stop();
            finished->deleteLater();
            handler(finished);
        });
    }

    void check(bool manual) {
        if (!isAppImage) {
            setState(UpdateState::Unavailable);
            emit q.operationFinished(QStringLiteral("check"), QStringLiteral("unsupported"));
            return;
        }
        if (busy()) {
            emit q.operationFinished(QStringLiteral("check"), QStringLiteral("busy"));
            return;
        }
        status.error.clear();
        setState(UpdateState::Checking);
        const QUrl url = options.githubApiUrl;
        if (!validHttpsUrl(url) &&
            !(options.allowLocalHttp && url.scheme() == QStringLiteral("http"))) {
            fail(QCoreApplication::translate("UpdateErrors", "The update endpoint is invalid."));
            return;
        }
        beginRequest(url, [this, manual](QNetworkReply* finished) {
            const QByteArray body = finished->readAll();
            if (finished->error() != QNetworkReply::NoError ||
                body.size() > kMaximumResponseBytes) {
                fail(QCoreApplication::translate("UpdateErrors", "The update check failed."));
                return;
            }
            const auto release = newestLinuxRelease(body);
            if (!release) {
                setState(UpdateState::Idle);
                emit q.operationFinished(QStringLiteral("check"), QStringLiteral("success"));
                armSchedule(options.automaticCheckInterval);
                return;
            }
            status.version = release->version;
            status.downloadUrl = release->appImageUrl;
            status.received = 0;
            status.total = 0;
            pendingShaUrl = release->sha256Url;
            pendingAppImageUrl = release->appImageUrl;
            pendingAppImageName = release->appImageName;
            setState(UpdateState::Available);
            if (!manual)
                emit q.automaticUpdateAvailable(release->version);
            emit q.operationFinished(QStringLiteral("check"), QStringLiteral("update-available"));
            armSchedule(options.automaticCheckInterval);
            if (manual)
                return;
            if (mode == u"download" || mode == u"next_launch")
                download();
        });
    }

    std::optional<ReleaseAsset> newestLinuxRelease(const QByteArray& body) const {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray())
            return std::nullopt;
        const auto installed = parseVersion(options.installedVersion);
        if (!installed)
            return std::nullopt;
        std::optional<ReleaseAsset> newest;
        std::optional<Version> newestVersion;
        for (const QJsonValue& value : document.array()) {
            const QJsonObject release = value.toObject();
            if (release.value(QStringLiteral("draft")).toBool() ||
                release.value(QStringLiteral("prerelease")).toBool()) {
                continue;
            }
            const QString version =
                versionFromTag(release.value(QStringLiteral("tag_name")).toString());
            const auto parsed = parseVersion(version);
            if (!parsed || compareVersions(*parsed, *installed) <= 0)
                continue;
            if (newestVersion && compareVersions(*parsed, *newestVersion) <= 0)
                continue;
            QUrl appImageUrl;
            QUrl sha256Url;
            QString appImageName;
            for (const QJsonValue& assetValue : release.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject asset = assetValue.toObject();
                const QString name = assetText(asset, "name");
                const QUrl downloadUrl(assetText(asset, "browser_download_url"));
                if (!downloadUrl.isValid())
                    continue;
                if (name.startsWith(QStringLiteral("snow-shot-")) &&
                    name.endsWith(QStringLiteral("-linux-x64.AppImage"))) {
                    appImageName = name;
                    appImageUrl = downloadUrl;
                } else if (name == appImageName + QStringLiteral(".sha256")) {
                    sha256Url = downloadUrl;
                }
            }
            if (appImageUrl.isEmpty() || sha256Url.isEmpty())
                continue;
            newestVersion = parsed;
            newest = ReleaseAsset{version, appImageUrl, sha256Url, appImageName};
        }
        return newest;
    }

    void download() {
        if (!isAppImage || status.state != UpdateState::Available ||
            !pendingAppImageUrl.isValid()) {
            emit q.operationFinished(QStringLiteral("download"), QStringLiteral("invalid-state"));
            return;
        }
        const QDir cache(options.cacheDirectory);
        if (!cache.mkpath(QStringLiteral("."))) {
            fail(QCoreApplication::translate("UpdateErrors", "The update cache is unavailable."));
            return;
        }
        const QString path = cache.filePath(pendingAppImageName);
        QFile::remove(path);
        auto* file = new QSaveFile(path, &q);
        if (!file->open(QIODevice::WriteOnly)) {
            delete file;
            fail(QCoreApplication::translate("UpdateErrors", "The update could not be saved."));
            return;
        }
        downloading = true;
        status.received = 0;
        status.total = 0;
        setState(UpdateState::Downloading);
        QNetworkRequest request(pendingAppImageUrl);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("SnowShot/%1 Linux").arg(options.installedVersion));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        reply = network.get(request);
        requestTimeout.disconnect();
        requestTimeout.start(kRequestTimeoutMs);
        QObject::connect(reply, &QNetworkReply::readyRead, &q, [this, file] {
            if (!reply)
                return;
            const QByteArray chunk = reply->readAll();
            if (!chunk.isEmpty())
                static_cast<void>(file->write(chunk));
            status.received += chunk.size();
            status.total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            emit q.statusChanged();
        });
        QObject::connect(&requestTimeout, &QTimer::timeout, &q, [this, file] {
            if (!reply)
                return;
            downloading = false;
            abortReply();
            file->cancelWriting();
            file->deleteLater();
            fail(QCoreApplication::translate("UpdateErrors", "The update download timed out."));
        });
        QObject::connect(reply, &QNetworkReply::finished, &q, [this, file] {
            QNetworkReply* finished = reply;
            if (finished == nullptr)
                return;
            reply = nullptr;
            requestTimeout.stop();
            finished->deleteLater();
            downloading = false;
            if (finished->error() != QNetworkReply::NoError || cancelRequested) {
                file->cancelWriting();
                file->deleteLater();
                setState(cancelRequested ? UpdateState::Available : UpdateState::Failed);
                if (cancelRequested)
                    emit q.operationFinished(QStringLiteral("download"),
                                             QStringLiteral("cancelled"));
                else
                    fail(
                        QCoreApplication::translate("UpdateErrors", "The update download failed."));
                return;
            }
            const QByteArray tail = finished->readAll();
            if (!tail.isEmpty())
                static_cast<void>(file->write(tail));
            if (!file->commit()) {
                file->deleteLater();
                fail(QCoreApplication::translate("UpdateErrors", "The update could not be saved."));
                return;
            }
            downloadedPath = file->fileName();
            file->deleteLater();
            verifyDownloadedImage();
        });
    }

    void verifyDownloadedImage() {
        setState(UpdateState::Verifying);
        beginRequest(pendingShaUrl, [this](QNetworkReply* finished) {
            const QByteArray body = finished->readAll().trimmed();
            if (finished->error() != QNetworkReply::NoError || body.size() > 4096) {
                QFile::remove(downloadedPath);
                fail(QCoreApplication::translate("UpdateErrors",
                                                 "The update signature could not be verified."));
                return;
            }
            const QByteArray expected = body.left(64).toLower();
            static const QRegularExpression hex(QStringLiteral("\\A[0-9a-f]{64}\\z"));
            if (!hex.match(QString::fromLatin1(expected)).hasMatch()) {
                QFile::remove(downloadedPath);
                fail(QCoreApplication::translate("UpdateErrors",
                                                 "The update signature could not be verified."));
                return;
            }
            QFile file(downloadedPath);
            if (!file.open(QIODevice::ReadOnly)) {
                fail(QCoreApplication::translate("UpdateErrors", "The update could not be read."));
                return;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&file)) {
                fail(QCoreApplication::translate("UpdateErrors", "The update could not be read."));
                return;
            }
            if (hash.result().toHex() != expected) {
                QFile::remove(downloadedPath);
                fail(QCoreApplication::translate("UpdateErrors",
                                                 "The update signature does not match."));
                return;
            }
            setState(UpdateState::Ready);
            emit q.updateReady();
            emit q.operationFinished(QStringLiteral("download"), QStringLiteral("success"));
        });
    }

    void apply() {
        if (status.state != UpdateState::Ready || downloadedPath.isEmpty() ||
            appImagePath.isEmpty()) {
            emit q.operationFinished(QStringLiteral("apply"), QStringLiteral("invalid-state"));
            return;
        }
        setState(UpdateState::Applying);
        const QString directory = QFileInfo(appImagePath).absolutePath();
        const QString temporary =
            QDir(directory).filePath(QStringLiteral(".snow-shot-update-%1")
                                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QFile source(downloadedPath);
        QFile target(temporary);
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
            fail(QCoreApplication::translate("UpdateErrors",
                                             "The update could not be installed. Check that the "
                                             "AppImage directory is writable."));
            return;
        }
        bool copied = true;
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(1024 * 1024);
            if (chunk.isEmpty())
                break;
            if (target.write(chunk) != chunk.size()) {
                copied = false;
                break;
            }
        }
        if (copied)
            copied = target.flush();
        target.close();
        source.close();
        if (!copied) {
            QFile::remove(temporary);
            fail(QCoreApplication::translate("UpdateErrors",
                                             "The update could not be installed. Check that the "
                                             "AppImage directory is writable."));
            return;
        }
        static_cast<void>(QFile::setPermissions(
            temporary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                           QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                           QFileDevice::ExeOther));
        // rename(2) replaces the running AppImage atomically; the live process
        // keeps its mapped image until the restart.
        const QByteArray temporaryName = QFile::encodeName(temporary);
        const QByteArray destinationName = QFile::encodeName(appImagePath);
        const bool replaced =
            std::rename(temporaryName.constData(), destinationName.constData()) == 0;
        if (!replaced) {
            QFile::remove(temporary);
            fail(QCoreApplication::translate("UpdateErrors",
                                             "The update could not replace the running AppImage."));
            return;
        }
        QFile::remove(downloadedPath);
        downloadedPath.clear();
        setState(UpdateState::Ready);
        emit q.handoffReady();
        emit q.restartRequested();
        emit q.operationFinished(QStringLiteral("apply"), QStringLiteral("success"));
    }

    QUrl pendingAppImageUrl;
    QString pendingAppImageName;
    QStringList relaunchArguments;
};

UpdateService::UpdateService(Options options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(options))) {}

UpdateService::~UpdateService() = default;

const UpdateStatus& UpdateService::status() const {
    return m_impl->status;
}

bool UpdateService::managedByStore() const {
    return m_impl->managedByStore();
}

bool UpdateService::busy() const {
    return m_impl->busy();
}

bool UpdateService::handoffPending() const {
    return false;
}

void UpdateService::start() {
    if (m_impl->started)
        return;
    m_impl->started = true;
    if (!m_impl->isAppImage) {
        m_impl->setState(UpdateState::Unavailable);
        return;
    }
    m_impl->armSchedule(m_impl->options.startupCheckDelay);
}

void UpdateService::setMode(const QString& mode) {
    m_impl->mode = mode;
    m_impl->armSchedule(m_impl->options.automaticCheckInterval);
}

void UpdateService::setSystemProxy(bool enabled) {
    if (enabled) {
        m_impl->network.setProxyFactory(new SystemProxyFactory);
    } else {
        m_impl->network.setProxyFactory(nullptr);
        m_impl->network.setProxy(QNetworkProxy::NoProxy);
    }
}

void UpdateService::setProgressAppearance(const QJsonObject&) {}

void UpdateService::check(bool manual) {
    m_impl->check(manual);
}

void UpdateService::download() {
    m_impl->download();
}

void UpdateService::cancel() {
    if (!m_impl->busy())
        return;
    m_impl->cancelRequested = true;
    m_impl->abortReply();
    if (m_impl->status.state == UpdateState::Downloading) {
        m_impl->setState(UpdateState::Available);
        emit operationFinished(QStringLiteral("download"), QStringLiteral("cancelled"));
    } else {
        m_impl->setState(UpdateState::Idle);
        emit operationFinished(QStringLiteral("check"), QStringLiteral("cancelled"));
    }
}

void UpdateService::requestRestart() {
    emit restartRequested();
}

void UpdateService::beginApply() {
    m_impl->apply();
}

void UpdateService::applyAtStartup() {
    // The AppImage replaces itself in place; no startup transaction is needed.
}

void UpdateService::setRelaunchArguments(const QStringList& arguments) {
    m_impl->relaunchArguments = arguments;
}

void UpdateService::clearRelaunchArguments() {
    m_impl->relaunchArguments.clear();
}

void UpdateService::reportBlocked(const QString&) {}

bool UpdateService::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        emit statusChanged();
    return QObject::event(event);
}

} // namespace snow_shot::update
