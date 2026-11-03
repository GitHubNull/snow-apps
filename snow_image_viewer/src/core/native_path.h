#pragma once

#include <QByteArray>
#include <QString>

#include <filesystem>

namespace snow::image_viewer {

inline std::filesystem::path nativePath(const QString& path) {
#if defined(Q_OS_WIN)
    return std::filesystem::path(path.toStdWString());
#else
    const QByteArray encoded = path.toUtf8();
    return std::filesystem::path(encoded.constData());
#endif
}

} // namespace snow::image_viewer
