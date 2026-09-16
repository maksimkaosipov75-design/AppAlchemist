// Lightweight unit tests for the conversion backend. No external test
// framework: a handful of CHECK macros, run via `ctest`.
//
// Covered:
//   * normalizeRpmRequires  - RPM dependency capability normalization
//   * RpmParser::readRpmHeader - binary RPM header parsing (synthetic package)
//   * ArchiveExtractor::extractSecure - zip-slip / path-traversal rejection

#include "utils.h"
#include "rpmparser.h"
#include "archive_extractor.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdio>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// normalizeRpmRequires
// ---------------------------------------------------------------------------
static void test_normalize_requires() {
    const QStringList in = {
        "libc.so.6()(64bit)",
        "libc.so.6(GLIBC_2.34)(64bit)",
        "libQt5Core.so.5()(64bit)",
        "libQt5Core.so.5(Qt_5)(64bit)",
        "pkgconfig(gtk+-3.0)",
        "perl(strict)",
        "rpmlib(PayloadIsZstd)",
        "rtld(GNU_HASH)",
        "/bin/sh",
        "bash >= 4.0",
        "config(foo)",
        ""
    };
    const QStringList out = normalizeRpmRequires(in);

    CHECK(out.contains("libc.so.6"), "libc soname kept");
    CHECK(out.count("libc.so.6") == 1, "libc soname deduplicated");
    CHECK(out.contains("libQt5Core.so.5"), "Qt soname kept");
    CHECK(out.count("libQt5Core.so.5") == 1, "Qt soname deduplicated");
    CHECK(out.contains("bash"), "version constraint stripped to name");
    CHECK(!out.contains("pkgconfig(gtk+-3.0)"), "pkgconfig capability dropped");
    CHECK(!out.contains("perl(strict)"), "perl capability dropped");
    CHECK(out.filter("rpmlib").isEmpty(), "rpmlib() dropped");
    CHECK(out.filter("rtld").isEmpty(), "rtld() dropped");
    CHECK(out.filter("config(").isEmpty(), "config() dropped");
    CHECK(out.filter("/").isEmpty(), "file-path requirement dropped");
    CHECK(normalizeRpmRequires({}).isEmpty(), "empty input -> empty output");
}

// ---------------------------------------------------------------------------
// RpmParser::readRpmHeader  (build a minimal synthetic RPM)
// ---------------------------------------------------------------------------
static void appendBE32(QByteArray& b, quint32 v) {
    b.append(char((v >> 24) & 0xff));
    b.append(char((v >> 16) & 0xff));
    b.append(char((v >> 8) & 0xff));
    b.append(char(v & 0xff));
}

namespace {
struct TagEntry { quint32 tag, type, offset, count; };
}

static QByteArray buildHeaderSection(const QList<TagEntry>& entries, const QByteArray& store) {
    QByteArray h;
    h.append(char(0x8e)); h.append(char(0xad)); h.append(char(0xe8)); h.append(char(0x01));
    h.append(4, '\0');                       // reserved
    appendBE32(h, entries.size());           // nindex
    appendBE32(h, store.size());             // hsize
    for (const TagEntry& e : entries) {
        appendBE32(h, e.tag);
        appendBE32(h, e.type);
        appendBE32(h, e.offset);
        appendBE32(h, e.count);
    }
    h.append(store);
    return h;
}

static QByteArray buildSyntheticRpm() {
    // Data store: contiguous null-terminated strings.
    QByteArray store;
    auto put = [&store](const QByteArray& s) -> quint32 {
        const quint32 off = quint32(store.size());
        store.append(s);
        store.append('\0');
        return off;
    };
    const quint32 offName = put("synthetic-app");
    const quint32 offVer  = put("2.5.0");
    const quint32 offRel  = put("3.fc42");
    const quint32 offSum  = put("A synthetic test package");
    const quint32 offReq0 = quint32(store.size());
    put("libc.so.6()(64bit)");
    put("rpmlib(PayloadIsZstd)");           // second array element
    const quint32 offComp = put("zstd");

    QList<TagEntry> idx = {
        {1000, 6, offName, 1},  // NAME (STRING)
        {1001, 6, offVer,  1},  // VERSION
        {1002, 6, offRel,  1},  // RELEASE
        {1004, 9, offSum,  1},  // SUMMARY (I18NSTRING)
        {1049, 8, offReq0, 2},  // REQUIRENAME (STRING_ARRAY, 2 entries)
        {1125, 6, offComp, 1},  // PAYLOADCOMPRESSOR
    };

    QByteArray rpm;
    rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
    rpm.append(92, '\0');                    // rest of 96-byte lead

    // Signature header with zero entries (16 bytes, already 8-aligned).
    rpm.append(buildHeaderSection({}, QByteArray()));

    // Main header.
    rpm.append(buildHeaderSection(idx, store));
    return rpm;
}

static void test_read_rpm_header() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "temp dir created");
    const QString path = dir.filePath("synthetic.rpm");
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly), "write synthetic rpm");
        const QByteArray bytes = buildSyntheticRpm();
        f.write(bytes);
    }

    const RpmHeaderInfo info = RpmParser::readRpmHeader(path);
    CHECK(info.valid, "header parsed as valid");
    CHECK(info.name == "synthetic-app", "name parsed");
    CHECK(info.version == "2.5.0", "version parsed");
    CHECK(info.release == "3.fc42", "release parsed");
    CHECK(info.summary == "A synthetic test package", "summary parsed");
    CHECK(info.payloadCompressor == "zstd", "payload compressor parsed");
    CHECK(info.requires_.size() == 2, "requires array parsed");
    CHECK(info.requires_.value(0) == "libc.so.6()(64bit)", "first require parsed");

    // Non-RPM input must be rejected, not crash.
    const QString bogus = dir.filePath("bogus.rpm");
    {
        QFile f(bogus);
        CHECK(f.open(QIODevice::WriteOnly), "write bogus file");
        f.write("not an rpm at all, just text padding padding padding");
    }
    CHECK(!RpmParser::readRpmHeader(bogus).valid, "non-rpm rejected");
}

// ---------------------------------------------------------------------------
// ArchiveExtractor::extractSecure  - zip-slip protection
// ---------------------------------------------------------------------------
#ifdef HAVE_LIBARCHIVE
static void writeTarEntry(struct archive* a, const char* path, const QByteArray& data) {
    struct archive_entry* e = archive_entry_new();
    archive_entry_set_pathname(e, path);
    archive_entry_set_size(e, data.size());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_write_header(a, e);
    archive_write_data(a, data.constData(), data.size());
    archive_entry_free(e);
}

static void test_zip_slip() {
    QTemporaryDir base;
    CHECK(base.isValid(), "temp base created");

    const QString tarPath = base.filePath("malicious.tar");
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, tarPath.toUtf8().constData());
    writeTarEntry(a, "../escaped.txt", "pwned");     // path traversal attempt
    writeTarEntry(a, "safe/inside.txt", "ok");        // legitimate file
    archive_write_close(a);
    archive_write_free(a);

    const QString dest = base.filePath("dest");
    QString err;
    const bool ok = ArchiveExtractor::extractSecure(tarPath, dest, &err);

    // Modern security requirements: archives with traversal entries must be REJECTED.
    CHECK(!ok, "archive with traversal entry is rejected");
    // Traversal entry must NOT have escaped into parent or beside dest.
    CHECK(!QFile::exists(base.filePath("escaped.txt")), "path traversal entry blocked");
    CHECK(!QFile::exists(dest + "/../escaped.txt"), "no escaped file beside dest");
}
#endif

int main(int, char**) {
    test_normalize_requires();
    test_read_rpm_header();
#ifdef HAVE_LIBARCHIVE
    test_zip_slip();
#endif

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
