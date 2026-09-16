#include "archive_extractor.h"

#include <QDir>
#include <QDirIterator>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
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

bool extractSecure(const QString& archivePath, const QString& destDir, QString* error,
                   const DecompressionQuotas& quotas) {
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
        archive_read_close(a);
        archive_read_free(a);
        archive_write_close(ext);
        archive_write_free(ext);
        return false;
    };

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 65536) != ARCHIVE_OK) {
        return fail(QString::fromUtf8(archive_error_string(a) ? archive_error_string(a) : "open failed"));
    }

    QDir().mkpath(destDir);
    const QString cleanDestDir = QDir::cleanPath(destDir);
    const QString destPrefix = cleanDestDir.endsWith('/') ? cleanDestDir : cleanDestDir + "/";

    int fileCount = 0;
    int totalEntries = 0;
    quint64 totalBytes = 0;
    bool readError = false;
    struct archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        ++totalEntries;
        if (totalEntries > quotas.maxFileCount) {
            const QString countStr = (quotas.maxFileCount == 100000)
                ? QStringLiteral("100,000")
                : QString::number(quotas.maxFileCount);
            return fail(QStringLiteral("Archive entry count exceeds quota (") + countStr + QStringLiteral(" files)"));
        }

        const QString original = QString::fromUtf8(archive_entry_pathname(entry));
        if (original.isEmpty() || original == "." || original == "./") {
            continue;
        }

        // SEC-CRIT-05: Explicit pathname traversal validation on all entries
        if (original.contains("..") || original.startsWith('/') || original.startsWith('\\')) {
            return fail(QStringLiteral("Unsafe path traversal entry in archive: ") + original);
        }
        const QString cleanedOriginalPath = QDir::cleanPath(destDir + "/" + original);
        if (!cleanedOriginalPath.startsWith(destPrefix) && cleanedOriginalPath != cleanDestDir) {
            return fail(QStringLiteral("Unsafe path traversal entry in archive: ") + original);
        }

        // SEC-HIGH-14: Skip special files (FIFOs, character/block devices, sockets)
        const mode_t fileType = archive_entry_filetype(entry);
        if (fileType == AE_IFIFO || fileType == AE_IFCHR || fileType == AE_IFBLK || fileType == AE_IFSOCK) {
            qWarning() << "Skipping special device/FIFO/socket archive entry:" << original;
            continue;
        }

        const QString relocatedPath = relocateInto(destDir, original);
        archive_entry_set_pathname(entry, relocatedPath.toUtf8().constData());

        // SEC-HIGH-07: Hardlink path traversal
        if (const char* hl = archive_entry_hardlink(entry)) {
            const QString target = QString::fromUtf8(hl);
            if (target.contains("..") || target.startsWith('/') || target.startsWith('\\')) {
                return fail(QStringLiteral("Unsafe hardlink target in archive: ") + target);
            }
            const QString relocated = QDir::cleanPath(cleanDestDir + "/" + target);
            if (!relocated.startsWith(destPrefix) && relocated != cleanDestDir) {
                return fail(QStringLiteral("Hardlink escapes destination directory: ") + target);
            }
            archive_entry_set_hardlink(entry, relocated.toUtf8().constData());
        }

        // SEC-HIGH-08: Symlink target traversal
        if (const char* symlinkTarget = archive_entry_symlink(entry)) {
            const QString target = QString::fromUtf8(symlinkTarget);
            if (target.startsWith('/') || target.startsWith('\\')) {
                const QString cleanTarget = QDir::cleanPath(target);
                // Allow standard distribution prefixes ("/usr/", "/opt/"), but reject dangerous absolute targets ("/etc", "/root", "/home", "/var", "/tmp", "/dev", "/proc")
                if (!cleanTarget.startsWith("/usr/") && !cleanTarget.startsWith("/opt/") &&
                    cleanTarget != "/usr" && cleanTarget != "/opt") {
                    return fail(QStringLiteral("Disallowed absolute symlink target in archive: ") + target);
                }
            } else {
                const QString entryParent = QFileInfo(relocatedPath).absolutePath();
                const QString resolved = QDir::cleanPath(entryParent + "/" + target);
                if (!resolved.startsWith(destPrefix) && resolved != cleanDestDir) {
                    return fail(QStringLiteral("Symlink escapes destination directory: ") + target);
                }
            }
        }

        // SEC-HIGH-15: Clear SUID/SGID bits and sanitize world-writable permissions
        mode_t mode = archive_entry_mode(entry);
        mode &= ~(S_ISUID | S_ISGID);
        mode &= ~0022; // Clear group-writable and world-writable permissions
        archive_entry_set_mode(entry, mode);

        int writeHeaderRes = archive_write_header(ext, entry);
        if (writeHeaderRes < ARCHIVE_OK) {
            if (error) {
                const char* extErr = archive_error_string(ext);
                *error = QString::fromUtf8(extErr ? extErr : "write header failed");
            }
            readError = true;
            break;
        }

        if (writeHeaderRes == ARCHIVE_OK && archive_entry_size(entry) > 0) {
            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                totalBytes += size;
                // SEC-HIGH-12: Enforce cumulative uncompressed bytes quota
                if (totalBytes > quotas.maxUncompressedBytes) {
                    qWarning() << "Decompression byte quota exceeded";
                    readError = true;
                    if (error) {
                        *error = (quotas.maxUncompressedBytes == 8589934592ULL)
                            ? QStringLiteral("Decompression byte quota exceeded (8 GiB)")
                            : QStringLiteral("Decompression byte quota exceeded (%1 bytes)").arg(quotas.maxUncompressedBytes);
                    }
                    break;
                }
                // DAT-HIGH-09: Silent write failure fix
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    qWarning() << "libarchive write failed:" << archive_error_string(ext);
                    readError = true;
                    if (error) {
                        const char* extErr = archive_error_string(ext);
                        *error = QString::fromUtf8(extErr ? extErr : "write data block failed");
                    }
                    break;
                }
            }
            if (readError || (r != ARCHIVE_EOF && r != ARCHIVE_OK)) {
                readError = true;
                break; // Break outer entry loop
            }
        }
        archive_write_finish_entry(ext);
        if (archive_entry_filetype(entry) == AE_IFREG) {
            ++fileCount;
        }
    }

    if (r != ARCHIVE_EOF && !readError) {
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

bool extractSecure(const QString&, const QString&, QString* error,
                   const DecompressionQuotas&) {
    if (error) {
        *error = QStringLiteral("built without libarchive support");
    }
    return false;
}

#endif // HAVE_LIBARCHIVE

} // namespace ArchiveExtractor
