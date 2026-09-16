#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QMap>
#include <archive.h>
#include <archive_entry.h>

namespace TestHelpers {

inline bool createSampleElf(const QString& path) {
    // Minimal valid 64-bit ELF binary (x86-64)
    // 64-byte Ehdr + 56-byte Phdr = 120 bytes
    static const unsigned char elfBytes[] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, // e_ident (ELF64, LSB, v1, SYSV)
        2, 0,                         // e_type = ET_EXEC (2)
        62, 0,                        // e_machine = EM_X86_64 (0x3e)
        1, 0, 0, 0,                   // e_version = 1
        0x78, 0, 0x40, 0, 0, 0, 0, 0, // e_entry = 0x400078
        64, 0, 0, 0, 0, 0, 0, 0,      // e_phoff = 64
        0, 0, 0, 0, 0, 0, 0, 0,       // e_shoff = 0
        0, 0, 0, 0,                   // e_flags = 0
        64, 0,                        // e_ehsize = 64
        56, 0,                        // e_phentsize = 56
        1, 0,                         // e_phnum = 1
        64, 0,                        // e_shentsize = 64
        0, 0,                         // e_shnum = 0
        0, 0,                         // e_shstrndx = 0
        // Program Header (56 bytes)
        1, 0, 0, 0,                   // p_type = PT_LOAD (1)
        5, 0, 0, 0,                   // p_flags = PF_R | PF_X (5)
        0, 0, 0, 0, 0, 0, 0, 0,       // p_offset = 0
        0, 0, 0x40, 0, 0, 0, 0, 0,    // p_vaddr = 0x400000
        0, 0, 0x40, 0, 0, 0, 0, 0,    // p_paddr = 0x400000
        120, 0, 0, 0, 0, 0, 0, 0,     // p_filesz = 120
        120, 0, 0, 0, 0, 0, 0, 0,     // p_memsz = 120
        0, 0x10, 0, 0, 0, 0, 0, 0     // p_align = 0x1000
    };

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(elfBytes), sizeof(elfBytes));
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                     QFile::ReadGroup | QFile::ExeGroup |
                     QFile::ReadOther | QFile::ExeOther);
    return true;
}

inline bool writeArchiveWithLibArchive(const QString& outPath, const QMap<QString, QByteArray>& files, const QString& format) {
    struct archive* a = archive_write_new();
    if (!a) return false;

    if (format == "tar.gz" || format == "tgz") {
        archive_write_add_filter_gzip(a);
        archive_write_set_format_pax_restricted(a);
    } else if (format == "tar.xz" || format == "txz") {
        archive_write_add_filter_xz(a);
        archive_write_set_format_pax_restricted(a);
    } else if (format == "zip") {
        archive_write_set_format_zip(a);
    } else {
        archive_write_set_format_pax_restricted(a);
    }

    if (archive_write_open_filename(a, outPath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }

    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        const QString& entryPath = it.key();
        const QByteArray& data = it.value();

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, entryPath.toUtf8().constData());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        bool isExec = (data.size() >= 4 && data[0] == '\x7f' && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') ||
                      data.startsWith("#!") ||
                      entryPath.contains("bin") || entryPath.endsWith(".sh") || entryPath == "AppRun";
        if (isExec) {
            archive_entry_set_perm(entry, 0755);
        } else {
            archive_entry_set_perm(entry, 0644);
        }
        archive_entry_set_mtime(entry, 1700000000, 0);

        archive_write_header(a, entry);
        if (!data.isEmpty()) {
            archive_write_data(a, data.constData(), data.size());
        }
        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);
    return true;
}

inline bool buildSyntheticDeb(const QString& debPath,
                              const QString& pkgName = "synthetic-app",
                              const QString& version = "1.0.0",
                              const QString& description = "Synthetic test package",
                              const QString& depends = "libc6 (>= 2.34)") {
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) return false;

    // 1. Create control file
    QString controlContent = QString(
        "Package: %1\n"
        "Version: %2\n"
        "Section: utils\n"
        "Priority: optional\n"
        "Architecture: amd64\n"
        "Depends: %3\n"
        "Maintainer: Test <test@example.com>\n"
        "Description: %4\n"
    ).arg(pkgName, version, depends, description);

    QMap<QString, QByteArray> controlFiles;
    controlFiles["./control"] = controlContent.toUtf8();
    QString controlTarPath = tempDir.filePath("control.tar.gz");
    if (!writeArchiveWithLibArchive(controlTarPath, controlFiles, "tar.gz")) return false;

    // 2. Create data files
    QByteArray elfBytes;
    {
        QString elfTemp = tempDir.filePath("sample_elf");
        if (createSampleElf(elfTemp)) {
            QFile f(elfTemp);
            if (f.open(QIODevice::ReadOnly)) {
                elfBytes = f.readAll();
            }
        }
    }
    if (elfBytes.isEmpty()) return false;

    QString desktopContent = QString(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=%1\n"
        "Exec=%1\n"
        "Icon=%1\n"
        "Categories=Utility;\n"
    ).arg(pkgName);

    // Minimal valid 64x64 PNG header
    static const unsigned char dummyPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d,
        'I', 'H', 'D', 'R',
        0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40,
        0x08, 0x06, 0x00, 0x00, 0x00,
        0x5d, 0x88, 0x81, 0xec
    };
    QByteArray iconBytes(reinterpret_cast<const char*>(dummyPng), sizeof(dummyPng));

    QMap<QString, QByteArray> dataFiles;
    dataFiles[QString("./usr/bin/%1").arg(pkgName)] = elfBytes;
    dataFiles[QString("./usr/share/applications/%1.desktop").arg(pkgName)] = desktopContent.toUtf8();
    dataFiles[QString("./usr/share/icons/hicolor/64x64/apps/%1.png").arg(pkgName)] = iconBytes;

    QString dataTarPath = tempDir.filePath("data.tar.gz");
    if (!writeArchiveWithLibArchive(dataTarPath, dataFiles, "tar.gz")) return false;

    // 3. Assemble ar archive
    QFile cFile(controlTarPath);
    if (!cFile.open(QIODevice::ReadOnly)) return false;
    QByteArray controlBytes = cFile.readAll();

    QFile dFile(dataTarPath);
    if (!dFile.open(QIODevice::ReadOnly)) return false;
    QByteArray dataBytes = dFile.readAll();

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
    appendArMember("data.tar.gz", dataBytes);

    QFile outFile(debPath);
    if (!outFile.open(QIODevice::WriteOnly)) return false;
    outFile.write(debBytes);
    outFile.close();
    return true;
}

struct RpmTagEntry {
    quint32 tag;
    quint32 type;
    quint32 offset;
    quint32 count;
};

inline bool buildSyntheticRpm(const QString& rpmPath,
                              const QString& pkgName = "synthetic-app",
                              const QString& version = "2.5.0",
                              const QString& release = "1.fc42",
                              const QString& summary = "Synthetic test package",
                              const QStringList& reqList = QStringList{"libc.so.6()(64bit)", "rpmlib(PayloadIsZstd)"},
                              const QString& compressor = "zstd") {
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

    QByteArray store;
    auto put = [&store](const QByteArray& s) -> quint32 {
        const quint32 off = quint32(store.size());
        store.append(s);
        store.append('\0');
        return off;
    };

    const quint32 offName = put(pkgName.toUtf8());
    const quint32 offVer  = put(version.toUtf8());
    const quint32 offRel  = put(release.toUtf8());
    const quint32 offSum  = put(summary.toUtf8());

    quint32 offReq = 0;
    if (!reqList.isEmpty()) {
        offReq = quint32(store.size());
        for (const QString& req : reqList) {
            put(req.toUtf8());
        }
    }
    const quint32 offComp = put(compressor.toUtf8());

    QList<RpmTagEntry> idx = {
        RpmTagEntry{1000, 6, offName, 1},  // NAME (STRING)
        RpmTagEntry{1001, 6, offVer,  1},  // VERSION
        RpmTagEntry{1002, 6, offRel,  1},  // RELEASE
        RpmTagEntry{1004, 9, offSum,  1},  // SUMMARY (I18NSTRING)
    };
    if (!reqList.isEmpty()) {
        idx.append(RpmTagEntry{1049, 8, offReq, quint32(reqList.size())}); // REQUIRENAME (STRING_ARRAY)
    }
    if (!compressor.isEmpty()) {
        idx.append(RpmTagEntry{1125, 6, offComp, 1}); // PAYLOADCOMPRESSOR
    }

    QByteArray rpm;
    rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
    rpm.append(92, '\0'); // rest of 96-byte lead

    // Signature header
    rpm.append(buildHeaderSection({}, QByteArray()));

    // Main header
    rpm.append(buildHeaderSection(idx, store));

    QFile f(rpmPath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(rpm);
    f.close();
    return true;
}

inline bool buildSyntheticTarball(const QString& tarPath,
                                  const QString& format = "tar.gz",
                                  bool standardStructure = true,
                                  const QString& appName = "synthetic-app") {
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) return false;

    QByteArray elfBytes;
    {
        QString elfTemp = tempDir.filePath("sample_elf");
        if (createSampleElf(elfTemp)) {
            QFile f(elfTemp);
            if (f.open(QIODevice::ReadOnly)) {
                elfBytes = f.readAll();
            }
        }
    }
    if (elfBytes.isEmpty()) return false;

    QString desktopContent = QString(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=%1\n"
        "Exec=%1\n"
        "Icon=%1\n"
        "Categories=Utility;\n"
    ).arg(appName);

    QMap<QString, QByteArray> files;
    if (standardStructure) {
        files[QString("usr/bin/%1").arg(appName)] = elfBytes;
        files[QString("usr/share/applications/%1.desktop").arg(appName)] = desktopContent.toUtf8();
    } else {
        // Flat structure
        files[appName] = elfBytes;
        files[QString("%1.desktop").arg(appName)] = desktopContent.toUtf8();
    }

    return writeArchiveWithLibArchive(tarPath, files, format);
}

} // namespace TestHelpers

#endif // TEST_HELPERS_H
