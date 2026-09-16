#include <catch2/catch_test_macros.hpp>
#include "debparser.h"
#include "rpmparser.h"
#include "tarballparser.h"
#include "test_helpers.h"
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>

TEST_CASE("DebParser: Extraction and metadata inspection", "[parser][deb]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString debPath = tempDir.filePath("sample_pkg_1.0.0_amd64.deb");
    REQUIRE(TestHelpers::buildSyntheticDeb(debPath, "sample-pkg", "1.0.0", "A sample synthetic deb package", "libc6 (>= 2.34), libssl3"));

    DebParser parser;
    REQUIRE(parser.validateDebFile(debPath));

    QString extractDir = tempDir.filePath("extracted_deb");
    REQUIRE(parser.extractDeb(debPath, extractDir));

    // Verify metadata parsing from extracted files
    DebMetadata meta = parser.parseMetadata(extractDir);
    REQUIRE(meta.package == "sample-pkg");
    REQUIRE(meta.version == "1.0.0");
    REQUIRE(meta.description.contains("sample synthetic deb package"));
    REQUIRE(meta.depends.contains("libc6"));

    // Verify executable discovery
    REQUIRE_FALSE(meta.executables.isEmpty());
    QStringList execs = parser.findExecutables(extractDir + "/data");
    REQUIRE_FALSE(execs.isEmpty());
    bool foundBin = false;
    for (const QString& exec : execs) {
        if (exec.contains("sample-pkg")) {
            foundBin = true;
            break;
        }
    }
    REQUIRE(foundBin);

    // Verify desktop file discovery
    QString desktopFile = parser.findDesktopFile(extractDir);
    REQUIRE_FALSE(desktopFile.isEmpty());
    REQUIRE(QFileInfo(desktopFile).exists());
    REQUIRE(desktopFile.endsWith(".desktop"));

    // Verify icon discovery
    QString iconFile = parser.findIcon(extractDir + "/data");
    REQUIRE_FALSE(iconFile.isEmpty());
    REQUIRE(QFileInfo(iconFile).exists());
    REQUIRE_FALSE(meta.iconPath.isEmpty());
    REQUIRE(QFileInfo(meta.iconPath).exists());
}

TEST_CASE("DebParser: Rejection of corrupted ar archives", "[parser][deb][corrupt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    DebParser parser;

    SECTION("Empty 0-byte file is rejected") {
        QString emptyDeb = tempDir.filePath("empty.deb");
        QFile f(emptyDeb);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.close();

        REQUIRE_FALSE(parser.validateDebFile(emptyDeb));
        REQUIRE_FALSE(parser.extractDeb(emptyDeb, tempDir.filePath("ext1")));
    }

    SECTION("File with invalid ar magic is rejected") {
        QString invalidMagicDeb = tempDir.filePath("invalid_magic.deb");
        QFile f(invalidMagicDeb);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("CORRUPTED_NOT_AN_AR_ARCHIVE");
        f.close();

        REQUIRE_FALSE(parser.validateDebFile(invalidMagicDeb));
        REQUIRE_FALSE(parser.extractDeb(invalidMagicDeb, tempDir.filePath("ext2")));
    }

    SECTION("Truncated ar header is rejected without crash") {
        QString truncatedDeb = tempDir.filePath("truncated.deb");
        QFile f(truncatedDeb);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("!<arch>\ndebian-binary   1700000000  0     0     100644  4"); // truncated mid-header
        f.close();

        // Must fail safely without crashing
        REQUIRE_FALSE(parser.extractDeb(truncatedDeb, tempDir.filePath("ext3")));
    }
}

TEST_CASE("RpmParser: Header parsing and compression classification", "[parser][rpm]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("Valid synthetic RPM parses tags and compressor correctly") {
        QString rpmPath = tempDir.filePath("synthetic.rpm");
        REQUIRE(TestHelpers::buildSyntheticRpm(
            rpmPath,
            "synthetic-app",
            "3.4.1",
            "2.fc42",
            "A synthetic test application",
            {"libc.so.6()(64bit)", "libm.so.6()(64bit)", "rpmlib(PayloadIsZstd)"},
            "zstd"
        ));

        RpmHeaderInfo info = RpmParser::readRpmHeader(rpmPath);
        REQUIRE(info.valid);
        REQUIRE(info.name == "synthetic-app");
        REQUIRE(info.version == "3.4.1");
        REQUIRE(info.release == "2.fc42");
        REQUIRE(info.summary == "A synthetic test application");
        REQUIRE(info.payloadCompressor == "zstd");
        REQUIRE(info.requires_.contains("libc.so.6()(64bit)"));
        REQUIRE(info.requires_.contains("libm.so.6()(64bit)"));
    }

    SECTION("Truncated or malformed RPM lead is safely rejected") {
        QString corruptRpm = tempDir.filePath("corrupt.rpm");
        QFile f(corruptRpm);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromHex("edabee")); // only 3 bytes of 4-byte magic
        f.close();

        RpmHeaderInfo info = RpmParser::readRpmHeader(corruptRpm);
        REQUIRE_FALSE(info.valid);
    }
}

TEST_CASE("TarballParser: Format detection and structure classification", "[parser][tarball]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    TarballParser parser;

    SECTION(".tar.gz standard structure detection and extraction") {
        QString tarGzPath = tempDir.filePath("test_app.tar.gz");
        REQUIRE(TestHelpers::buildSyntheticTarball(tarGzPath, "tar.gz", true, "my-standard-app"));

        REQUIRE(TarballParser::isSupportedTarball(tarGzPath));
        REQUIRE(TarballParser::getTarballType(tarGzPath) == TarballType::TAR_GZ);
        REQUIRE(parser.validateTarball(tarGzPath));

        QString extractDir = tempDir.filePath("ext_targz");
        REQUIRE(parser.extractTarball(tarGzPath, extractDir));

        PackageMetadata meta = parser.parseMetadata(extractDir);
        REQUIRE(meta.package == "my-standard-app");
        REQUIRE_FALSE(meta.executables.isEmpty());
    }

    SECTION(".tar.xz standard structure format detection") {
        QString tarXzPath = tempDir.filePath("test_app.tar.xz");
        REQUIRE(TestHelpers::buildSyntheticTarball(tarXzPath, "tar.xz", true, "my-xz-app"));

        REQUIRE(TarballParser::isSupportedTarball(tarXzPath));
        REQUIRE(TarballParser::getTarballType(tarXzPath) == TarballType::TAR_XZ);
        REQUIRE(parser.validateTarball(tarXzPath));
    }

    SECTION(".zip standard structure format detection") {
        QString zipPath = tempDir.filePath("test_app.zip");
        REQUIRE(TestHelpers::buildSyntheticTarball(zipPath, "zip", true, "my-zip-app"));

        REQUIRE(TarballParser::isSupportedTarball(zipPath));
        REQUIRE(TarballParser::getTarballType(zipPath) == TarballType::ZIP);
        REQUIRE(parser.validateTarball(zipPath));
    }

    SECTION("Flat structure classification and metadata extraction") {
        QString flatTarPath = tempDir.filePath("flat_app.tar.gz");
        REQUIRE(TestHelpers::buildSyntheticTarball(flatTarPath, "tar.gz", false, "flat-tool"));

        REQUIRE(TarballParser::isSupportedTarball(flatTarPath));
        QString extractDir = tempDir.filePath("ext_flat");
        REQUIRE(parser.extractTarball(flatTarPath, extractDir));

        PackageMetadata meta = parser.parseMetadata(extractDir);
        REQUIRE(meta.package == "flat-tool");
        REQUIRE_FALSE(meta.executables.isEmpty());
    }
}

TEST_CASE("TarballParser: Rejection of malformed archives", "[parser][tarball][corrupt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    TarballParser parser;

    SECTION("Zero-byte file is rejected") {
        QString emptyTar = tempDir.filePath("empty.tar.gz");
        QFile f(emptyTar);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.close();

        REQUIRE_FALSE(parser.validateTarball(emptyTar));
    }

    SECTION("Random binary bytes with .tar.gz extension fails magic check") {
        QString corruptTar = tempDir.filePath("corrupt.tar.gz");
        QFile f(corruptTar);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("NOT_GZIP_MAGIC_DATA_1234567890");
        f.close();

        REQUIRE_FALSE(parser.validateTarball(corruptTar));
    }
}

TEST_CASE("DebParser Empirical Challenge: Corrupted ar headers and invalid data.tar.gz", "[challenge][deb][corrupt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    DebParser parser;

    SECTION("Debian package with invalid gzip magic in data.tar.gz is safely rejected") {
        QString debPath = tempDir.filePath("invalid_gzip_data.deb");

        // Construct valid control.tar.gz
        QTemporaryDir subDir;
        REQUIRE(subDir.isValid());
        QString controlContent = "Package: test-invalid-gzip\nVersion: 1.0\nArchitecture: amd64\nDescription: test\n";
        QMap<QString, QByteArray> ctrlFiles;
        ctrlFiles["./control"] = controlContent.toUtf8();
        QString controlTarPath = subDir.filePath("control.tar.gz");
        REQUIRE(TestHelpers::writeArchiveWithLibArchive(controlTarPath, ctrlFiles, "tar.gz"));

        QFile cFile(controlTarPath);
        REQUIRE(cFile.open(QIODevice::ReadOnly));
        QByteArray controlBytes = cFile.readAll();

        // Corrupted data.tar.gz with invalid gzip magic
        QByteArray corruptedData = "THIS_IS_NOT_GZIP_MAGIC_DATA_STREAM_PAYLOAD_123456789";

        // Build ar container
        QByteArray debBytes;
        debBytes.append("!<arch>\n");

        auto appendArMember = [&debBytes](const QString& name, const QByteArray& content) {
            QString hdrStr = QString("%1%2%3%4%5%6`\n")
                .arg(name, -16)
                .arg(1700000000, -12)
                .arg(0, -6)
                .arg(0, -6)
                .arg(QString::number(0100644, 8), -8)
                .arg(content.size(), -10);
            debBytes.append(hdrStr.toLatin1());
            debBytes.append(content);
            if (content.size() % 2 == 1) {
                debBytes.append('\n');
            }
        };

        appendArMember("debian-binary", "2.0\n");
        appendArMember("control.tar.gz", controlBytes);
        appendArMember("data.tar.gz", corruptedData);

        QFile outFile(debPath);
        REQUIRE(outFile.open(QIODevice::WriteOnly));
        outFile.write(debBytes);
        outFile.close();

        // validateDebFile checks the outer ar container magic which is valid
        REQUIRE(parser.validateDebFile(debPath));

        // extractDeb MUST fail safely when data.tar.gz has invalid gzip magic
        QString extractDir = tempDir.filePath("ext_invalid_gzip");
        REQUIRE_FALSE(parser.extractDeb(debPath, extractDir));
    }

    SECTION("Debian package with empty 0-byte data.tar.gz is safely rejected") {
        QString debPath = tempDir.filePath("empty_data_tar.deb");
        QByteArray debBytes;
        debBytes.append("!<arch>\n");

        auto appendArMember = [&debBytes](const QString& name, const QByteArray& content) {
            QString hdrStr = QString("%1%2%3%4%5%6`\n")
                .arg(name, -16)
                .arg(1700000000, -12)
                .arg(0, -6)
                .arg(0, -6)
                .arg(QString::number(0100644, 8), -8)
                .arg(content.size(), -10);
            debBytes.append(hdrStr.toLatin1());
            debBytes.append(content);
            if (content.size() % 2 == 1) {
                debBytes.append('\n');
            }
        };

        appendArMember("debian-binary", "2.0\n");
        appendArMember("control.tar.gz", "dummy");
        appendArMember("data.tar.gz", "");

        QFile outFile(debPath);
        REQUIRE(outFile.open(QIODevice::WriteOnly));
        outFile.write(debBytes);
        outFile.close();

        QString extractDir = tempDir.filePath("ext_empty_data");
        REQUIRE_FALSE(parser.extractDeb(debPath, extractDir));
    }

    SECTION("Debian package missing data.tar member is safely rejected") {
        QString debPath = tempDir.filePath("missing_data_member.deb");
        QByteArray debBytes;
        debBytes.append("!<arch>\n");

        auto appendArMember = [&debBytes](const QString& name, const QByteArray& content) {
            QString hdrStr = QString("%1%2%3%4%5%6`\n")
                .arg(name, -16)
                .arg(1700000000, -12)
                .arg(0, -6)
                .arg(0, -6)
                .arg(QString::number(0100644, 8), -8)
                .arg(content.size(), -10);
            debBytes.append(hdrStr.toLatin1());
            debBytes.append(content);
            if (content.size() % 2 == 1) {
                debBytes.append('\n');
            }
        };

        appendArMember("debian-binary", "2.0\n");
        appendArMember("control.tar.gz", "dummy");

        QFile outFile(debPath);
        REQUIRE(outFile.open(QIODevice::WriteOnly));
        outFile.write(debBytes);
        outFile.close();

        QString extractDir = tempDir.filePath("ext_missing_data");
        REQUIRE_FALSE(parser.extractDeb(debPath, extractDir));
    }

    SECTION("Various truncated ar headers are safely handled") {
        // 1. Only 4 bytes
        QString p1 = tempDir.filePath("trunc1.deb");
        {
            QFile f(p1); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("!<ar");
        }
        REQUIRE_FALSE(parser.validateDebFile(p1));
        REQUIRE_FALSE(parser.extractDeb(p1, tempDir.filePath("ext_trunc1")));

        // 2. Only 8 bytes magic without any members
        QString p2 = tempDir.filePath("trunc2.deb");
        {
            QFile f(p2); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("!<arch>\n");
        }
        REQUIRE(parser.validateDebFile(p2));
        REQUIRE_FALSE(parser.extractDeb(p2, tempDir.filePath("ext_trunc2")));

        // 3. Member header claiming 10000000 bytes but file truncates after 30 bytes
        QString p3 = tempDir.filePath("trunc3.deb");
        {
            QFile f(p3); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("!<arch>\n");
            QString hdrStr = QString("%1%2%3%4%5%6`\n")
                .arg("data.tar.gz", -16)
                .arg(1700000000, -12)
                .arg(0, -6)
                .arg(0, -6)
                .arg(QString::number(0100644, 8), -8)
                .arg(10000000, -10);
            f.write(hdrStr.toLatin1());
            f.write("truncated_body");
        }
        REQUIRE(parser.validateDebFile(p3));
        REQUIRE_FALSE(parser.extractDeb(p3, tempDir.filePath("ext_trunc3")));
    }
}

TEST_CASE("RpmParser Empirical Challenge: Lead truncation, malformed tag counts, out-of-bounds offset tables", "[challenge][rpm][corrupt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    RpmParser parser;
    using TestHelpers::RpmTagEntry;

    auto appendBE32 = [](QByteArray& b, quint32 v) {
        b.append(char((v >> 24) & 0xff));
        b.append(char((v >> 16) & 0xff));
        b.append(char((v >> 8) & 0xff));
        b.append(char(v & 0xff));
    };

    auto buildHeaderSection = [&](const QList<RpmTagEntry>& entries, const QByteArray& store) -> QByteArray {
        QByteArray h;
        h.append(char(0x8e)); h.append(char(0xad)); h.append(char(0xe8)); h.append(char(0x01));
        h.append(4, '\0');                       // reserved
        appendBE32(h, entries.size());           // nindex
        appendBE32(h, store.size());             // hsize
        for (const RpmTagEntry& e : entries) {
            appendBE32(h, e.tag);
            appendBE32(h, e.type);
            appendBE32(h, e.offset);
            appendBE32(h, e.count);
        }
        h.append(store);
        return h;
    };

    SECTION("Lead truncation: 0 bytes, 3 bytes, 4 bytes, 50 bytes, 95 bytes") {
        const QList<int> leadSizes = {0, 3, 4, 50, 95};
        for (int sz : leadSizes) {
            QString path = tempDir.filePath(QString("trunc_lead_%1.rpm").arg(sz));
            QFile f(path);
            REQUIRE(f.open(QIODevice::WriteOnly));
            QByteArray data;
            if (sz >= 4) {
                data.append(char(0xed)); data.append(char(0xab));
                data.append(char(0xee)); data.append(char(0xdb));
                data.append(sz - 4, '\0');
            } else if (sz > 0) {
                data.append(char(0xed)); data.append(char(0xab)); data.append(char(0xee));
                data = data.left(sz);
            }
            f.write(data);
            f.close();

            RpmHeaderInfo info = RpmParser::readRpmHeader(path);
            REQUIRE_FALSE(info.valid);
            REQUIRE_FALSE(parser.extractRpm(path, tempDir.filePath(QString("ext_lead_%1").arg(sz))));
        }
    }

    SECTION("Malformed tag counts: nindex > 100000u or storeSize > 16MB") {
        // nindex = 100001
        QString p1 = tempDir.filePath("oversized_nindex.rpm");
        {
            QByteArray rpm;
            rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
            rpm.append(92, '\0'); // 96-byte lead
            // Signature header
            rpm.append(buildHeaderSection({}, QByteArray()));
            // Main header intro with nindex = 100001
            rpm.append(char(0x8e)); rpm.append(char(0xad)); rpm.append(char(0xe8)); rpm.append(char(0x01));
            rpm.append(4, '\0');
            appendBE32(rpm, 100001); // oversized nindex
            appendBE32(rpm, 64);     // storeSize
            rpm.append(64, '\0');

            QFile f(p1); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(rpm);
        }
        RpmHeaderInfo info1 = RpmParser::readRpmHeader(p1);
        REQUIRE_FALSE(info1.valid);
        REQUIRE_FALSE(parser.extractRpm(p1, tempDir.filePath("ext_nindex")));

        // storeSize = 17 MB
        QString p2 = tempDir.filePath("oversized_storesize.rpm");
        {
            QByteArray rpm;
            rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
            rpm.append(92, '\0');
            rpm.append(buildHeaderSection({}, QByteArray()));
            rpm.append(char(0x8e)); rpm.append(char(0xad)); rpm.append(char(0xe8)); rpm.append(char(0x01));
            rpm.append(4, '\0');
            appendBE32(rpm, 1);
            appendBE32(rpm, 17 * 1024 * 1024); // oversized storeSize

            QFile f(p2); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(rpm);
        }
        RpmHeaderInfo info2 = RpmParser::readRpmHeader(p2);
        REQUIRE_FALSE(info2.valid);
        REQUIRE_FALSE(parser.extractRpm(p2, tempDir.filePath("ext_storesize")));
    }

    SECTION("Out-of-bounds offset tables and extreme counts") {
        // Tag 1000 (NAME) with offset beyond storeSize
        QString p3 = tempDir.filePath("oob_offset.rpm");
        {
            QByteArray store = "valid_name\0";
            QList<RpmTagEntry> idx = {
                RpmTagEntry{1000, 6, 0xffffffff, 1}, // out-of-bounds offset
                RpmTagEntry{1001, 6, 0x10000, 1},    // out-of-bounds offset
                RpmTagEntry{1049, 8, 0x20000, 1000}, // out-of-bounds array
            };

            QByteArray rpm;
            rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
            rpm.append(92, '\0');
            rpm.append(buildHeaderSection({}, QByteArray()));
            rpm.append(buildHeaderSection(idx, store));

            QFile f(p3); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(rpm);
        }

        RpmHeaderInfo info3 = RpmParser::readRpmHeader(p3);
        // NAME is out-of-bounds, so info.name is empty -> valid is false
        REQUIRE_FALSE(info3.valid);
        REQUIRE(info3.name.isEmpty());
        REQUIRE(info3.requires_.isEmpty());
        REQUIRE_FALSE(parser.extractRpm(p3, tempDir.filePath("ext_oob")));
    }

    SECTION("Store without null terminator is bounded safely without buffer overflow") {
        QString p4 = tempDir.filePath("no_null_store.rpm");
        {
            QByteArray store = "unterminated_string_without_any_null_byte_at_all";
            QList<RpmTagEntry> idx = {
                RpmTagEntry{1000, 6, 0, 1}, // NAME starting at 0
            };

            QByteArray rpm;
            rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
            rpm.append(92, '\0');
            rpm.append(buildHeaderSection({}, QByteArray()));
            rpm.append(buildHeaderSection(idx, store));

            QFile f(p4); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(rpm);
        }

        RpmHeaderInfo info4 = RpmParser::readRpmHeader(p4);
        REQUIRE(info4.valid);
        // Must read up to store.size() without reading past buffer
        REQUIRE(info4.name == "unterminated_string_without_any_null_byte_at_all");
    }
}

TEST_CASE("TarballParser Empirical Challenge: Truncated archives and corrupted zip directory", "[challenge][tarball][corrupt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    TarballParser parser;

    SECTION("Truncated tarball variants across formats") {
        // 1. Truncated gzip (starts with \x1f\x8b but only 3 bytes)
        QString gzPath = tempDir.filePath("trunc.tar.gz");
        {
            QFile f(gzPath); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("\x1f\x8b\x08", 3);
        }
        REQUIRE(parser.validateTarball(gzPath)); // magic matched
        REQUIRE_FALSE(parser.extractTarball(gzPath, tempDir.filePath("ext_gz")));

        // 2. Truncated XZ (starts with \xFD7zXZ\x00 but truncated immediately)
        QString xzPath = tempDir.filePath("trunc.tar.xz");
        {
            QFile f(xzPath); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("\xFD\x37\x7A\x58\x5A\x00", 6);
        }
        REQUIRE(parser.validateTarball(xzPath));
        REQUIRE_FALSE(parser.extractTarball(xzPath, tempDir.filePath("ext_xz")));

        // 3. Truncated Bzip2 (starts with BZh but truncated)
        QString bz2Path = tempDir.filePath("trunc.tar.bz2");
        {
            QFile f(bz2Path); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("BZh9", 4);
        }
        REQUIRE(parser.validateTarball(bz2Path));
        REQUIRE_FALSE(parser.extractTarball(bz2Path, tempDir.filePath("ext_bz2")));

        // 4. Truncated Zstd (starts with \x28\xb5\x2f\xfd but truncated)
        QString zstPath = tempDir.filePath("trunc.tar.zst");
        {
            QFile f(zstPath); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("\x28\xb5\x2f\xfd", 4);
        }
        REQUIRE(parser.validateTarball(zstPath));
        REQUIRE_FALSE(parser.extractTarball(zstPath, tempDir.filePath("ext_zst")));

        // 5. Truncated uncompressed .tar
        QString tarPath = tempDir.filePath("trunc.tar");
        {
            QFile f(tarPath); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("USTAR_TRUNCATED_HEADER_50_BYTES_ONLY_12345678901234");
        }
        REQUIRE(parser.validateTarball(tarPath));
        REQUIRE_FALSE(parser.extractTarball(tarPath, tempDir.filePath("ext_tar")));
    }

    SECTION("Corrupted ZIP directory structures") {
        // 1. ZIP magic PK\x03\x04 followed by only 4 bytes of garbage
        QString zip1 = tempDir.filePath("trunc_header.zip");
        {
            QFile f(zip1); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write("PK\x03\x04\x00\x00\x00\x00");
        }
        REQUIRE(parser.validateTarball(zip1));
        REQUIRE_FALSE(parser.extractTarball(zip1, tempDir.filePath("ext_zip1")));

        // 2. Corrupted central directory end record
        QString zip2 = tempDir.filePath("corrupt_central_dir.zip");
        {
            QByteArray fakeZip;
            // Local file header (30 bytes)
            fakeZip.append("PK\x03\x04");
            fakeZip.append(26, '\0');
            // End of central directory record (22 bytes) claiming 100 entries at offset 0xFFFFFFFF
            fakeZip.append("PK\x05\x06");
            fakeZip.append(4, '\0'); // disk numbers
            fakeZip.append("\x64\x00", 2); // 100 entries on disk
            fakeZip.append("\x64\x00", 2); // 100 total entries
            fakeZip.append("\xff\xff\x00\x00", 4); // central dir size
            fakeZip.append("\xff\xff\xff\xff", 4); // out-of-bounds central dir offset
            fakeZip.append("\x00\x00", 2); // comment length

            QFile f(zip2); REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(fakeZip);
        }
        REQUIRE(parser.validateTarball(zip2));
        REQUIRE_FALSE(parser.extractTarball(zip2, tempDir.filePath("ext_zip2")));
    }

    SECTION("parseMetadata on non-existent or empty extraction directory") {
        TarballParser freshParser;
        PackageMetadata meta = freshParser.parseMetadata(tempDir.filePath("non_existent_dir"));
        REQUIRE(meta.executables.isEmpty());
        REQUIRE(meta.iconPath.isEmpty());
        REQUIRE(meta.mainExecutable.isEmpty());
    }
}

