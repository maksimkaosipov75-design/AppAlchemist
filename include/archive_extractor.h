#ifndef ARCHIVE_EXTRACTOR_H
#define ARCHIVE_EXTRACTOR_H

#include <QString>

// In-process archive extraction backed by libarchive. A single secure code
// path is shared by every package format (RPM, tar.*, zip) so that path
// traversal ("zip slip") and unsafe symlinks are rejected uniformly, and the
// gzip/xz/lzma/bzip2/zstd payload compressors are handled without external
// tools.
namespace ArchiveExtractor {

// True when the project was built with libarchive support (HAVE_LIBARCHIVE).
bool isAvailable();

// Securely extract any libarchive-supported archive into destDir.
//
// Security: entries that would escape destDir via "../" components or absolute
// paths are refused, and symlinks/hardlinks are kept inside the destination.
// Returns false (and sets *error, when provided) on failure or when no regular
// file was produced. When libarchive is unavailable this always returns false
// so callers fall back to external tools.
bool extractSecure(const QString& archivePath, const QString& destDir, QString* error = nullptr);

} // namespace ArchiveExtractor

#endif // ARCHIVE_EXTRACTOR_H
