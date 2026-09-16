#ifndef ARCHIVE_EXTRACTOR_H
#define ARCHIVE_EXTRACTOR_H

#include <QString>

// In-process archive extraction backed by libarchive. A single secure code
// path is shared by every package format (RPM, tar.*, zip) so that path
// traversal ("zip slip") and unsafe symlinks are rejected uniformly, and the
// gzip/xz/lzma/bzip2/zstd payload compressors are handled without external
// tools.
namespace ArchiveExtractor {

struct DecompressionQuotas {
    int maxFileCount = 100000;
    quint64 maxUncompressedBytes = 8589934592ULL; // 8 GiB
};

// True when the project was built with libarchive support (HAVE_LIBARCHIVE).
bool isAvailable();

// Securely extract any libarchive-supported archive into destDir.
//
// Security: entries that would escape destDir via "../" components or absolute
// paths are refused, hardlinks and symlinks are verified to remain confined within
// destDir (with standard /usr/ and /opt/ distribution prefixes allowed for subsequent
// relativization), special files (FIFOs, device nodes, sockets) are skipped, SUID/SGID
// bits and world-writable permissions are sanitized, write failures abort immediately,
// and decompression quotas (100,000 files, 8 GiB cumulative bytes) are strictly enforced.
// Returns false (and sets *error, when provided) on failure, quota breach, or when no regular
// file was produced. When libarchive is unavailable this always returns false.
bool extractSecure(const QString& archivePath, const QString& destDir, QString* error = nullptr,
                   const DecompressionQuotas& quotas = DecompressionQuotas());

} // namespace ArchiveExtractor

#endif // ARCHIVE_EXTRACTOR_H
