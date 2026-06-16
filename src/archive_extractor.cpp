#include "archive_extractor.h"

#include <QDir>
#include <QDirIterator>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <QDebug>
#endif

namespace ArchiveExtractor {

bool isAvailable() {
#ifdef HAVE_LIBARCHIVE
    return true;
#else
    return false;
#endif
}

#ifdef HAVE_LIBARCHIVE

namespace {

// Returns true if the directory tree contains at least one regular file. Used
// to reject extractions that produced only empty directories or metadata.
bool directoryHasRegularFile(const QString& path) {
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    return it.hasNext();
}

QString relocateInto(const QString& destDir, QString p) {
    if (p.startsWith("./")) {
        p = p.mid(2);
    } else if (p.startsWith('/')) {
        p = p.mid(1);
    }
    return destDir + "/" + p;
}

} // namespace

bool extractSecure(const QString& archivePath, const QString& destDir, QString* error) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    struct archive* ext = archive_write_disk_new();
    // SECURE_NODOTDOT rejects "../" components; SECURE_SYMLINKS prevents writing
    // through a symlink that points outside the destination. Together they stop
    // path-traversal ("zip slip") attacks from untrusted packages.
    archive_write_disk_set_options(ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(ext);

    auto fail = [&](const QString& msg) {
        if (error) {
            *error = msg;
        }
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    };

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 65536) != ARCHIVE_OK) {
        return fail(QString::fromUtf8(archive_error_string(a) ? archive_error_string(a) : "open failed"));
    }

    QDir().mkpath(destDir);

    int fileCount = 0;
    bool readError = false;
    struct archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString original = QString::fromUtf8(archive_entry_pathname(entry));
        if (original.isEmpty() || original == "." || original == "./") {
            continue;
        }
        archive_entry_set_pathname(entry, relocateInto(destDir, original).toUtf8().constData());

        if (const char* hl = archive_entry_hardlink(entry)) {
            archive_entry_set_hardlink(entry,
                relocateInto(destDir, QString::fromUtf8(hl)).toUtf8().constData());
        }

        r = archive_write_header(ext, entry);
        if (r == ARCHIVE_OK && archive_entry_size(entry) > 0) {
            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    qWarning() << "libarchive write failed:" << archive_error_string(ext);
                    break;
                }
            }
            if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
                readError = true;
            }
        }
        archive_write_finish_entry(ext);
        if (archive_entry_filetype(entry) == AE_IFREG) {
            ++fileCount;
        }
    }

    if (r != ARCHIVE_EOF) {
        if (error) {
            *error = QString::fromUtf8(archive_error_string(a) ? archive_error_string(a) : "read error");
        }
        readError = true;
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    if (readError) {
        return false;
    }
    if (fileCount == 0) {
        if (error) {
            *error = QStringLiteral("archive produced no regular files");
        }
        return false;
    }
    return directoryHasRegularFile(destDir);
}

#else // !HAVE_LIBARCHIVE

bool extractSecure(const QString&, const QString&, QString* error) {
    if (error) {
        *error = QStringLiteral("built without libarchive support");
    }
    return false;
}

#endif // HAVE_LIBARCHIVE

} // namespace ArchiveExtractor
