#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (predicate())
            return true;
        QThread::msleep(5);
    }
    return predicate();
}

class ReleaseServer final : public QTcpServer {
  public:
    ReleaseServer() {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] { serve(); });
    }

    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    QByteArray appImage = QByteArrayLiteral("replacement-image-bytes");
    bool corruptChecksum = false;

  private:
    void respond(QTcpSocket* socket) {
        const QByteArray request = socket->readAll();
        const QList<QByteArray> lines = request.split('\n');
        const QList<QByteArray> parts = lines.value(0).trimmed().split(' ');
        const QByteArray path = parts.size() >= 2 ? parts.at(1) : QByteArray();
        QByteArray body;
        QByteArray contentType = QByteArrayLiteral("application/octet-stream");
        if (path.endsWith("/releases")) {
            contentType = QByteArrayLiteral("application/json");
            const QString appImageUrl =
                baseUrl() + QStringLiteral("/snow-shot-9.9.9-linux-x64.AppImage");
            body = QStringLiteral("[{\"tag_name\":\"v9.9.9_snow-shot\",\"draft\":false,"
                                  "\"prerelease\":false,\"assets\":["
                                  "{\"name\":\"snow-shot-9.9.9-linux-x64.AppImage\","
                                  "\"browser_download_url\":\"%1\"},"
                                  "{\"name\":\"snow-shot-9.9.9-linux-x64.AppImage.sha256\","
                                  "\"browser_download_url\":\"%2\"}]}]")
                       .arg(appImageUrl, appImageUrl + QStringLiteral(".sha256"))
                       .toUtf8();
        } else if (path.endsWith(".AppImage")) {
            body = appImage;
        } else if (path.endsWith(".sha256")) {
            QByteArray digest =
                QCryptographicHash::hash(appImage, QCryptographicHash::Sha256).toHex();
            if (corruptChecksum)
                digest = QByteArray(64, '0');
            body = digest + "  snow-shot.AppImage\n";
        } else {
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + contentType + "\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void serve() {
        while (QTcpSocket* socket = nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { respond(socket); });
            if (socket->bytesAvailable() > 0)
                respond(socket);
        }
    }
};

std::unique_ptr<snow_shot::update::UpdateService>
makeService(const QString& baseUrl, const QString& root,
            const QString& version = QStringLiteral("1.4.8")) {
    snow_shot::update::UpdateService::Options options;
    options.applicationDirectory = root;
    options.root = root;
    options.cacheDirectory = root + QStringLiteral("/cache");
    options.allowLocalHttp = true;
    options.installedVersion = version;
    options.githubApiUrl = QUrl(baseUrl + QStringLiteral("/releases"));
    options.giteeApiUrl = QUrl(baseUrl + QStringLiteral("/releases"));
    options.startupCheckDelay = std::chrono::hours(1);
    options.automaticCheckInterval = std::chrono::hours(1);
    options.requestTimeout = std::chrono::seconds(10);
    return std::make_unique<snow_shot::update::UpdateService>(options);
}

void testUpdateFlow() {
    QTemporaryDir root;
    require(root.isValid(), "failed to create the update test root");
    const QString appImagePath = root.filePath(QStringLiteral("snow-shot.AppImage"));
    {
        QFile file(appImagePath);
        require(file.open(QIODevice::WriteOnly), "failed to seed the fake AppImage");
        file.write("original-image");
    }
    qputenv("APPIMAGE", appImagePath.toUtf8());
    ReleaseServer server;
    require(server.isListening(), "the local release server failed to listen");
    auto service = makeService(server.baseUrl(), root.path());
    require(!service->managedByStore(), "an AppImage must use the in-app updater");
    service->setSystemProxy(false);
    service->start();
    bool restartRequested = false;
    QObject::connect(service.get(), &snow_shot::update::UpdateService::restartRequested,
                     service.get(), [&restartRequested] { restartRequested = true; });
    service->check(true);
    const bool becameAvailable = waitFor(
        [&] { return service->status().state == snow_shot::update::UpdateState::Available; });
    require(becameAvailable, "the check must surface the newer release");
    require(service->status().version == QStringLiteral("9.9.9"),
            "the release tag must resolve to a semantic version");
    service->download();
    require(
        waitFor([&] { return service->status().state == snow_shot::update::UpdateState::Ready; }),
        "the download must verify and become ready");
    service->beginApply();
    require(waitFor([&] { return restartRequested; }),
            "applying the update must request a restart");
    QFile updated(appImagePath);
    require(updated.open(QIODevice::ReadOnly), "the replaced AppImage must be readable");
    require(updated.readAll() == server.appImage,
            "the AppImage must be replaced with the download");
    require(updated.permissions().testFlag(QFileDevice::ExeOwner),
            "the replaced AppImage must stay executable");
}

void testChecksumMismatch() {
    QTemporaryDir root;
    const QString appImagePath = root.filePath(QStringLiteral("snow-shot.AppImage"));
    {
        QFile file(appImagePath);
        require(file.open(QIODevice::WriteOnly), "failed to seed the fake AppImage");
        file.write("original-image");
    }
    qputenv("APPIMAGE", appImagePath.toUtf8());
    ReleaseServer server;
    server.corruptChecksum = true;
    auto service = makeService(server.baseUrl(), root.path());
    service->setSystemProxy(false);
    service->start();
    service->check(true);
    require(waitFor([&] {
                return service->status().state == snow_shot::update::UpdateState::Available;
            }),
            "the mismatching release must still be discoverable");
    service->download();
    require(
        waitFor([&] { return service->status().state == snow_shot::update::UpdateState::Failed; }),
        "a checksum mismatch must fail the download");
    QFile unchanged(appImagePath);
    require(unchanged.open(QIODevice::ReadOnly), "the original AppImage must remain readable");
    require(unchanged.readAll() == QByteArrayLiteral("original-image"),
            "a failed verification must not replace the AppImage");
}

void testNonAppImageIsManaged() {
    qunsetenv("APPIMAGE");
    QTemporaryDir root;
    auto service = makeService(QStringLiteral("https://example.invalid"), root.path());
    require(service->managedByStore(),
            "a non-AppImage install must delegate updates to the package manager");
    service->start();
    require(service->status().state == snow_shot::update::UpdateState::Unavailable,
            "a non-AppImage install must report updates as unavailable");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testUpdateFlow();
    testChecksumMismatch();
    testNonAppImageIsManaged();
    return 0;
}
