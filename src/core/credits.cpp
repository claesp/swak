#include "core/credits.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QSslSocket>
#include <QTableWidget>
#include <QVBoxLayout>

#include <GeographicLib/Config.h>
#include <archive.h>

namespace swak {
void showCredits(QWidget *parent)
{
    if (auto *existing = parent->findChild<QDialog *>(QStringLiteral("creditsDialog"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("creditsDialog"));
    dialog->setWindowTitle(QObject::tr("Credits"));
    dialog->resize(960, 460);
    auto *layout = new QVBoxLayout(dialog);
    auto *notice =
        new QLabel(QObject::tr("SWAK uses these libraries and icon assets. System libraries and "
                               "graphics drivers depend on your Linux installation."),
                   dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *table = new QTableWidget(0, 5, dialog);
    table->setObjectName(QStringLiteral("creditsTable"));
    table->setHorizontalHeaderLabels({QObject::tr("Name"),
                                      QObject::tr("Version"),
                                      QObject::tr("License"),
                                      QObject::tr("Attribution"),
                                      QObject::tr("Project URL")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(true);
    table->verticalHeader()->hide();
    const auto add = [table](const QString &name,
                             const QString &version,
                             const QString &license,
                             const QString &credit,
                             const QString &url) {
        const int row = table->rowCount();
        table->insertRow(row);
        const QStringList cells{name, version, license, credit, url};
        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells[col]);
            item->setToolTip(cells[col]);
            table->setItem(row, col, item);
        }
    };
    add(QStringLiteral("Qt"),
        QString::fromLatin1(qVersion()),
        QStringLiteral("LGPL-3.0 / GPL-3.0 / commercial"),
        QStringLiteral("The Qt Company and contributors"),
        QStringLiteral("https://www.qt.io/"));
    add(QStringLiteral("libarchive"),
        QString::fromLatin1(archive_version_string()),
        QStringLiteral("BSD-2-Clause; bundled notices apply"),
        QStringLiteral("Tim Kientzle and contributors"),
        QStringLiteral("https://www.libarchive.org/"));
    add(QStringLiteral("GeographicLib"),
        QStringLiteral(GEOGRAPHICLIB_VERSION_STRING),
        QStringLiteral("MIT"),
        QStringLiteral("Charles Karney"),
        QStringLiteral("https://geographiclib.sourceforge.io/"));
    add(QStringLiteral("protozero"),
        QStringLiteral("1.8.2"),
        QStringLiteral("BSD-2-Clause; Apache-2.0 for Folly portions"),
        QStringLiteral("Mapbox, Jochen Topf, and contributors"),
        QStringLiteral("https://github.com/mapbox/protozero"));
    add(QStringLiteral("Lucide icons"),
        QStringLiteral("0.468.0"),
        QStringLiteral("ISC; MIT for Feather portions"),
        QStringLiteral("Lucide Contributors; Cole Bemis (Feather)"),
        QStringLiteral("https://lucide.dev/"));
    const auto compression = [&](const QString &name,
                                 const char *version,
                                 const QString &license,
                                 const QString &credit,
                                 const QString &url) {
        if (version) {
            add(name, QString::fromLatin1(version), license, credit, url);
        }
    };
    compression(QStringLiteral("zlib"),
                archive_zlib_version(),
                QStringLiteral("Zlib"),
                QStringLiteral("Jean-loup Gailly and Mark Adler"),
                QStringLiteral("https://zlib.net/"));
    compression(QStringLiteral("liblzma"),
                archive_liblzma_version(),
                QStringLiteral("0BSD / public domain; bundled notices apply"),
                QStringLiteral("XZ Utils contributors"),
                QStringLiteral("https://tukaani.org/xz/"));
    compression(QStringLiteral("bzip2"),
                archive_bzlib_version(),
                QStringLiteral("bzip2-1.0.6"),
                QStringLiteral("Julian Seward and contributors"),
                QStringLiteral("https://sourceware.org/bzip2/"));
    compression(QStringLiteral("LZ4"),
                archive_liblz4_version(),
                QStringLiteral("BSD-2-Clause"),
                QStringLiteral("Yann Collet and contributors"),
                QStringLiteral("https://lz4.org/"));
    compression(QStringLiteral("Zstandard"),
                archive_libzstd_version(),
                QStringLiteral("BSD-3-Clause / GPL-2.0"),
                QStringLiteral("Meta and contributors"),
                QStringLiteral("https://facebook.github.io/zstd/"));
    const auto tlsVersion = QSslSocket::sslLibraryVersionString();
    if (!tlsVersion.isEmpty()) {
        const bool openssl3 = tlsVersion.startsWith(QStringLiteral("OpenSSL 3."));
        add(QObject::tr("TLS backend (%1)").arg(QSslSocket::activeBackend()),
            tlsVersion,
            openssl3 ? QStringLiteral("Apache-2.0") : QObject::tr("See installed backend license"),
            QObject::tr("Installed Qt TLS backend"),
            openssl3 ? QStringLiteral("https://openssl.org/")
                     : QStringLiteral("https://doc.qt.io/qt-6/qsslsocket.html"));
    }
    table->setSortingEnabled(true);
    table->sortItems(0);
    table->setColumnWidth(0, 155);
    table->setColumnWidth(1, 135);
    table->setColumnWidth(2, 210);
    table->setColumnWidth(3, 230);
    table->horizontalHeader()->setStretchLastSection(true);
    table->resizeRowsToContents();
    layout->addWidget(table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}
} // namespace swak
