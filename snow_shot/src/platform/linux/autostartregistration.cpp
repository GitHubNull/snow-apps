#include "snow_shot/platform/linux/autostartregistration.h"

#include "snow_shot/app/edition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace snow_shot::platform::linux {
namespace {

QString autostartPath() {
    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return configDirectory.isEmpty()
               ? QString()
               : QDir(configDirectory).filePath(QStringLiteral("autostart/snow-shot.desktop"));
}

} // namespace

AutostartResult setAutostartEnabled(bool enabled) {
    AutostartResult result;
    const QString path = autostartPath();
    const QString failure = QCoreApplication::translate("AutoStartRegistration",
                                                        "Could not update the autostart entry.");
    if (path.isEmpty()) {
        result.error = failure;
        return result;
    }
    if (!enabled) {
        result.success = !QFileInfo::exists(path) || QFile::remove(path);
        if (!result.success)
            result.error = failure;
        return result;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        result.error = failure;
        return result;
    }
    QString program = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (program.contains(u' '))
        program = QStringLiteral("\"%1\"").arg(program);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.error = failure;
        return result;
    }
    const QByteArray entry = QStringLiteral("[Desktop Entry]\n"
                                            "Type=Application\n"
                                            "Name=%1\n"
                                            "Comment=Start %1 in the background when you log in.\n"
                                            "Exec=%2 --autostart\n"
                                            "Terminal=false\n"
                                            "X-GNOME-Autostart-enabled=true\n")
                                 .arg(app::edition::productName(), program)
                                 .toUtf8();
    if (file.write(entry) != entry.size() || !file.commit()) {
        result.error = failure;
        return result;
    }
    result.success = true;
    return result;
}

bool autostartEnabled() {
    const QString path = autostartPath();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray contents = file.readAll();
    return !contents.contains("X-GNOME-Autostart-enabled=false");
}

} // namespace snow_shot::platform::linux
