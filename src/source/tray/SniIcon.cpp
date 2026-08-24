// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/tray/SniIcon.h"

#include "Util/Endian.h"

#include <QLatin1String>
#include <QStringLiteral>

#include <cstdint>

// Outside the namespace, as Q_INIT_RESOURCE requires. dish_core is a static
// library, so nothing else forces the linker to keep the resource object.
static void initTrayResource() {
    static const bool sInitialised = [] {
        Q_INIT_RESOURCE(tray);
        return true;
    }();
    static_cast<void>(sInitialised);
}

namespace dish::source {

QString sniIconName(bool themeHasIcon) {
    return themeHasIcon ? QLatin1String(kSniIconName) : QLatin1String("");
}

SniIconPixmapList sniTrayPixmaps() {
    initTrayResource();
    SniIconPixmapList pixmaps;
    for (const int size : {22, 48}) {
        const QImage image(QStringLiteral(":/tray/dish-%1.png").arg(size));
        if (image.isNull()) { continue; }
        pixmaps.append(toSniPixmap(image));
    }
    return pixmaps;
}

SniIconPixmap toSniPixmap(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    SniIconPixmap pixmap;
    pixmap.width = image.width();
    pixmap.height = image.height();
    const qsizetype byteCount =
        static_cast<qsizetype>(pixmap.width) * static_cast<qsizetype>(pixmap.height) * 4;
    pixmap.data = QByteArray(byteCount, '\0');
    auto* out = reinterpret_cast<std::uint8_t*>(pixmap.data.data());
    qsizetype offset = 0;
    for (int y = 0; y < pixmap.height; ++y) {
        for (int x = 0; x < pixmap.width; ++x) {
            util::putU32Be(out + offset, static_cast<std::uint32_t>(image.pixel(x, y)));
            offset += 4;
        }
    }
    return pixmap;
}

} // namespace dish::source
