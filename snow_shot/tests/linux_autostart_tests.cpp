#include "snow_shot/platform/linux/autostartregistration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QTemporaryDir configRoot;
    require(configRoot.isValid(), "failed to create an isolated config directory");
    qputenv("XDG_CONFIG_HOME", configRoot.path().toUtf8());
    QCoreApplication application(argc, argv);

    using snow_shot::platform::linux::autostartEnabled;
    using snow_shot::platform::linux::setAutostartEnabled;
    const QString path =
        QDir(configRoot.path()).filePath(QStringLiteral("autostart/snow-shot.desktop"));
    require(!autostartEnabled(), "autostart must start disabled in a fresh profile");

    const auto enabled = setAutostartEnabled(true);
    require(enabled.success, "enabling autostart must succeed");
    require(QFile::exists(path), "enabling autostart must write the desktop entry");
    require(autostartEnabled(), "the written desktop entry must read back as enabled");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "the desktop entry must be readable");
    const QByteArray entry = file.readAll();
    require(entry.contains("[Desktop Entry]") && entry.contains("Type=Application") &&
                entry.contains("--autostart") && entry.contains("X-GNOME-Autostart-enabled=true"),
            "the desktop entry must launch the app in the background at login");

    const auto disabled = setAutostartEnabled(false);
    require(disabled.success, "disabling autostart must succeed");
    require(!QFile::exists(path), "disabling autostart must remove the desktop entry");
    require(!autostartEnabled(), "autostart must read back as disabled");

    const auto missing = setAutostartEnabled(false);
    require(missing.success, "disabling an absent autostart entry must be idempotent");
    return 0;
}
