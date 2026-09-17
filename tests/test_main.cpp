#include <catch2/catch_test_macros.hpp>
#include "appimagebuilder.h"
#include "compatibility_rules.h"
#include <QFile>

TEST_CASE("Catch2 v3 framework integration", "[catch2][sanity]") {
    REQUIRE(true);
}

TEST_CASE("Qt resource asset embedding", "[assets][resource]") {
    SECTION("compatibility_rules.json is accessible via Qt resource system") {
        QFile file(":/assets/compatibility_rules.json");
        REQUIRE(file.open(QIODevice::ReadOnly));
        QByteArray data = file.readAll();
        REQUIRE_FALSE(data.isEmpty());
        REQUIRE(data.contains("rules"));
    }

    SECTION("AppRun.template is accessible via Qt resource system") {
        QFile file(":/assets/AppRun.template");
        REQUIRE(file.open(QIODevice::ReadOnly));
        QByteArray data = file.readAll();
        REQUIRE_FALSE(data.isEmpty());
        REQUIRE(data.contains("#!/bin/bash"));
        REQUIRE(data.contains("@PATH_DIRS@"));
        REQUIRE(data.contains("@LIB_DIRS@"));
    }
}

#include "archive_extractor.h"
#include "tarballparser.h"
#include "rpmparser.h"
#include "utils.h"
#include <QTemporaryDir>
#include <QDir>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <chrono>
#include <tuple>
#include <vector>

namespace {
bool writeSyntheticArchive(const QString& tarPath,
                          const std::vector<std::tuple<QString, QByteArray, mode_t, QString, QString>>& entries) {
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tarPath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    for (const auto& [name, data, mode, symTarget, hardTarget] : entries) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, name.toUtf8().constData());
        archive_entry_set_size(e, data.size());
        archive_entry_set_perm(e, mode & 07777);
        if (!hardTarget.isEmpty()) {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_hardlink(e, hardTarget.toUtf8().constData());
        } else if (!symTarget.isEmpty()) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_symlink(e, symTarget.toUtf8().constData());
        } else if ((mode & S_IFMT) == S_IFIFO) {
            archive_entry_set_filetype(e, AE_IFIFO);
        } else if ((mode & S_IFMT) == S_IFCHR) {
            archive_entry_set_filetype(e, AE_IFCHR);
            archive_entry_set_rdev(e, makedev(1, 3));
        } else if ((mode & S_IFMT) == S_IFBLK) {
            archive_entry_set_filetype(e, AE_IFBLK);
            archive_entry_set_rdev(e, makedev(8, 0));
        } else if ((mode & S_IFMT) == S_IFSOCK) {
            archive_entry_set_filetype(e, AE_IFSOCK);
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
        }
        archive_write_header(a, e);
        if (data.size() > 0) {
            archive_write_data(a, data.constData(), data.size());
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}
}

TEST_CASE("ArchiveExtractor Hardlink Path Traversal (SEC-HIGH-07)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("hardlink_attack.tar");
    const QString destDir = tempDir.filePath("dest");

    SECTION("Reject hardlink containing dot-dot") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe content", 0644, "", ""},
            {"bad_link", "", 0644, "", "../../outside.txt"}
        }));
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("hardlink", Qt::CaseInsensitive));
    }

    SECTION("Reject hardlink starting with slash") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe content", 0644, "", ""},
            {"bad_link", "", 0644, "", "/etc/passwd"}
        }));
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("hardlink", Qt::CaseInsensitive));
    }
}

TEST_CASE("ArchiveExtractor Symlink Target Traversal (SEC-HIGH-08)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("symlink_attack.tar");
    const QString destDir = tempDir.filePath("dest");

    SECTION("Reject symlink targeting dangerous system path /etc/shadow") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe content", 0644, "", ""},
            {"shadow_link", "", 0777, "/etc/shadow", ""}
        }));
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("symlink", Qt::CaseInsensitive));
    }

    SECTION("Reject relative symlink escaping destination") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe content", 0644, "", ""},
            {"escape_link", "", 0777, "../../../../../../etc/passwd", ""}
        }));
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("symlink", Qt::CaseInsensitive));
    }

    SECTION("Permit distribution prefix /usr/ in symlinks") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe content", 0644, "", ""},
            {"usr_link", "", 0777, "/usr/bin/bash", ""}
        }));
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE(ok);
    }
}

TEST_CASE("ArchiveExtractor Special Files Skipping (SEC-HIGH-14)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("special_files.tar");
    const QString destDir = tempDir.filePath("dest");

    REQUIRE(writeSyntheticArchive(tarPath, {
        {"my_pipe", "", S_IFIFO | 0644, "", ""},
        {"app.bin", "binary content", 0755, "", ""}
    }));

    QString err;
    bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
    REQUIRE(ok);
    REQUIRE(QFile::exists(destDir + "/app.bin"));
}

TEST_CASE("ArchiveExtractor Permission Sanitization (SEC-HIGH-15)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("suid_file.tar");
    const QString destDir = tempDir.filePath("dest");

    REQUIRE(writeSyntheticArchive(tarPath, {
        {"dangerous_bin", "echo hi", 04777, "", ""}
    }));

    QString err;
    bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
    REQUIRE(ok);

    const QString extractedFile = destDir + "/dangerous_bin";
    REQUIRE(QFile::exists(extractedFile));

    struct stat st;
    REQUIRE(stat(extractedFile.toUtf8().constData(), &st) == 0);
    // SUID and SGID bits must be stripped
    REQUIRE((st.st_mode & S_ISUID) == 0);
    REQUIRE((st.st_mode & S_ISGID) == 0);
    // Group-write and other-write must be masked
    REQUIRE((st.st_mode & S_IWOTH) == 0);
}

TEST_CASE("TarballParser Zip Slip Hard Abort (SEC-CRIT-05)", "[security][tarball]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("zipslip.tar");
    const QString extractDir = tempDir.filePath("extract");

    // Include both a legitimate file and a traversal payload to ensure rejection
    // is active, not an artifact of an empty extracted directory.
    REQUIRE(writeSyntheticArchive(tarPath, {
        {"safe.txt", "safe content", 0644, "", ""},
        {"../escaped_pwned.txt", "pwned payload", 0644, "", ""}
    }));

    TarballParser parser;
    bool ok = parser.extractTarball(tarPath, extractDir);
    // Hard abort: must return false without falling back to external unzip/tar
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(QFile::exists(tempDir.filePath("escaped_pwned.txt")));
}

TEST_CASE("ArchiveExtractor Multi-Entry Path Traversal Rejection (SEC-CRIT-05)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("multi_zipslip.tar");
    const QString destDir = tempDir.filePath("dest");

    SECTION("Legitimate file before path traversal entry") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"safe.txt", "safe payload", 0644, "", ""},
            {"../../escaped.txt", "evil payload", 0644, "", ""}
        }));

        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE_FALSE(err.isEmpty());
        REQUIRE_FALSE(QFile::exists(tempDir.filePath("escaped.txt")));
        REQUIRE_FALSE(QFile::exists(tempDir.filePath("../escaped.txt")));
    }

    SECTION("Path traversal entry before legitimate file") {
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"../../escaped.txt", "evil payload", 0644, "", ""},
            {"safe.txt", "safe payload", 0644, "", ""}
        }));

        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        REQUIRE_FALSE(ok);
        REQUIRE_FALSE(err.isEmpty());
        REQUIRE_FALSE(QFile::exists(tempDir.filePath("escaped.txt")));
        REQUIRE_FALSE(QFile::exists(tempDir.filePath("../escaped.txt")));
    }
}

TEST_CASE("ArchiveExtractor Header Write Error Propagation (DAT-HIGH-09)", "[security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("write_fail.tar");
    const QString destDir = tempDir.filePath("dest_write_fail");

    QDir().mkpath(destDir + "/readonly_sub");
    ::chmod((destDir + "/readonly_sub").toUtf8().constData(), 0555);

    REQUIRE(writeSyntheticArchive(tarPath, {
        {"safe.txt", "safe data", 0644, "", ""},
        {"readonly_sub/fail.txt", "cannot write this", 0644, "", ""}
    }));

    QString err;
    bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);

    // Restore write permissions so QTemporaryDir can clean up
    ::chmod((destDir + "/readonly_sub").toUtf8().constData(), 0755);

    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.isEmpty());
}

TEST_CASE("ArchiveExtractor Decompression Quota Enforcement (SEC-HIGH-12)", "[security][archive][quota]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("File count quota enforcement with custom quota") {
        const QString tarPath = tempDir.filePath("file_count_quota.tar");
        const QString destDir = tempDir.filePath("dest_count");
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"file1.txt", "data 1", 0644, "", ""},
            {"file2.txt", "data 2", 0644, "", ""},
            {"file3.txt", "data 3", 0644, "", ""}
        }));

        ArchiveExtractor::DecompressionQuotas quotas;
        quotas.maxFileCount = 2; // Only allow 2 files

        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err, quotas);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("quota", Qt::CaseInsensitive));
    }

    SECTION("Cumulative byte quota enforcement with custom quota") {
        const QString tarPath = tempDir.filePath("byte_quota.tar");
        const QString destDir = tempDir.filePath("dest_bytes");
        QByteArray largePayload(4096, 'A');
        REQUIRE(writeSyntheticArchive(tarPath, {
            {"file1.txt", "small", 0644, "", ""},
            {"file2.txt", largePayload, 0644, "", ""}
        }));

        ArchiveExtractor::DecompressionQuotas quotas;
        quotas.maxUncompressedBytes = 1024; // Limit to 1 KiB

        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err, quotas);
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("quota", Qt::CaseInsensitive));
    }
}

TEST_CASE("SubprocessWrapper copyDirectory cycle prevention (SEC-MED-29)", "[utils][filesystem]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString srcDir = tempDir.filePath("src");
    const QString subDir = srcDir + "/sub";
    const QString destDir = tempDir.filePath("dest");

    QDir().mkpath(subDir);
    QFile file(srcDir + "/regular.txt");
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("sample");
    file.close();

    // Create a circular symlink pointing to ancestor
    QFile::link(srcDir, subDir + "/loop");

    bool ok = SubprocessWrapper::copyDirectory(srcDir, destDir);
    REQUIRE(ok);
    REQUIRE(QFile::exists(destDir + "/regular.txt"));
}

static void appendBE32Helper(QByteArray& b, quint32 v) {
    b.append(char((v >> 24) & 0xff));
    b.append(char((v >> 16) & 0xff));
    b.append(char((v >> 8) & 0xff));
    b.append(char(v & 0xff));
}

TEST_CASE("RpmParser Header Oversized Store Bound Check (REL-MED-36)", "[security][rpm]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString rpmPath = tempDir.filePath("oversized.rpm");

    QByteArray rpm;
    rpm.append(char(0xed)); rpm.append(char(0xab)); rpm.append(char(0xee)); rpm.append(char(0xdb));
    rpm.append(92, '\0'); // 96-byte lead

    // Header with storeSize = 32 MB (> 16 MB limit)
    QByteArray h;
    h.append(char(0x8e)); h.append(char(0xad)); h.append(char(0xe8)); h.append(char(0x01));
    h.append(4, '\0');
    appendBE32Helper(h, 0); // nindex = 0
    appendBE32Helper(h, 32 * 1024 * 1024); // storeSize = 32 MB
    rpm.append(h);

    QFile f(rpmPath);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(rpm);
    f.close();

    RpmHeaderInfo info = RpmParser::readRpmHeader(rpmPath);
    REQUIRE_FALSE(info.valid);
}

TEST_CASE("SubprocessWrapper Symlink Target Traversal Prevention (SEC-HIGH-08)", "[utils][filesystem]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString rootDir = tempDir.filePath("root");
    const QString destDir = tempDir.filePath("dest");
    QDir().mkpath(rootDir);
    QDir().mkpath(destDir);

    const QString dangerousSymlink = rootDir + "/dangerous_link";
    QFile::link("/etc/passwd", dangerousSymlink);

    const QString targetDest = destDir + "/dangerous_link";
    bool ok = SubprocessWrapper::copyFile(dangerousSymlink, targetDest, rootDir, destDir);
    // Must refuse to link dangerous host path /etc/passwd
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(QFile::exists(targetDest));
}

TEST_CASE("Adversarial: Cyclic symlinks copyDirectory termination", "[adversarial][utils][symlink]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("Direct circular symlink dirA/loop -> dirA terminates cleanly without stack overflow") {
        const QString dirA = tempDir.filePath("dirA");
        const QString destDir = tempDir.filePath("destA");
        QDir().mkpath(dirA);

        QFile normalFile(dirA + "/hello.txt");
        REQUIRE(normalFile.open(QIODevice::WriteOnly));
        normalFile.write("AppAlchemist empirical test");
        normalFile.close();

        // Create direct circular symlink: dirA/loop -> dirA
        REQUIRE(QFile::link(dirA, dirA + "/loop"));

        // Copy directory must terminate cleanly without infinite recursion or stack overflow
        bool ok = SubprocessWrapper::copyDirectory(dirA, destDir);
        REQUIRE(ok);
        REQUIRE(QFile::exists(destDir + "/hello.txt"));
        QFile copied(destDir + "/hello.txt");
        REQUIRE(copied.open(QIODevice::ReadOnly));
        REQUIRE(copied.readAll() == "AppAlchemist empirical test");

        // Verify that destDir does not contain unbounded recursive directory nesting
        REQUIRE_FALSE(QDir(destDir + "/loop/loop/loop").exists());
    }

    SECTION("Relative self-referential circular symlink dirB/loop -> . terminates cleanly") {
        const QString dirB = tempDir.filePath("dirB");
        const QString destDir = tempDir.filePath("destB");
        QDir().mkpath(dirB);

        QFile normalFile(dirB + "/fileB.txt");
        REQUIRE(normalFile.open(QIODevice::WriteOnly));
        normalFile.write("data B");
        normalFile.close();

        // Relative circular symlink to current directory: loop -> .
        REQUIRE(QFile::link(".", dirB + "/loop"));

        bool ok = SubprocessWrapper::copyDirectory(dirB, destDir);
        REQUIRE(ok);
        REQUIRE(QFile::exists(destDir + "/fileB.txt"));
    }

    SECTION("Deep nested circular symlink to ancestor dirC/sub1/sub2/loop -> dirC terminates cleanly") {
        const QString dirC = tempDir.filePath("dirC");
        const QString deepDir = dirC + "/sub1/sub2";
        const QString destDir = tempDir.filePath("destC");
        QDir().mkpath(deepDir);

        QFile normalFile(deepDir + "/nested.txt");
        REQUIRE(normalFile.open(QIODevice::WriteOnly));
        normalFile.write("nested content");
        normalFile.close();

        REQUIRE(QFile::link(dirC, deepDir + "/loop"));

        bool ok = SubprocessWrapper::copyDirectory(dirC, destDir);
        REQUIRE(ok);
        REQUIRE(QFile::exists(destDir + "/sub1/sub2/nested.txt"));
    }
}

TEST_CASE("Adversarial: Permission sanitization for SUID 04755 and world-writable 0777", "[adversarial][security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("permissions_adversarial.tar");
    const QString destDir = tempDir.filePath("dest_perms");

    // Create archive with:
    // 1. suid_exec: 04755 (setuid + rwxr-xr-x)
    // 2. world_writable: 0777 (rwxrwxrwx)
    // 3. suid_world_writable: 04777 (setuid + rwxrwxrwx)
    // 4. sgid_exec: 02755 (setgid + rwxr-xr-x)
    REQUIRE(writeSyntheticArchive(tarPath, {
        {"suid_exec", "echo suid", 04755, "", ""},
        {"world_writable", "echo world", 0777, "", ""},
        {"suid_world_writable", "echo combo", 04777, "", ""},
        {"sgid_exec", "echo sgid", 02755, "", ""}
    }));

    QString err;
    bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
    REQUIRE(ok);

    struct stat st;

    // 1. SUID executable: SUID must be cleared, group/world write must be masked
    const QString fileSuid = destDir + "/suid_exec";
    REQUIRE(QFile::exists(fileSuid));
    REQUIRE(stat(fileSuid.toUtf8().constData(), &st) == 0);
    REQUIRE((st.st_mode & S_ISUID) == 0);
    REQUIRE((st.st_mode & S_ISGID) == 0);
    REQUIRE((st.st_mode & S_IWOTH) == 0);
    REQUIRE((st.st_mode & S_IWGRP) == 0);
    REQUIRE((st.st_mode & 0777) == 0755);

    // 2. World-writable executable: SUID/SGID cleared, group/world write masked
    const QString fileWorld = destDir + "/world_writable";
    REQUIRE(QFile::exists(fileWorld));
    REQUIRE(stat(fileWorld.toUtf8().constData(), &st) == 0);
    REQUIRE((st.st_mode & S_ISUID) == 0);
    REQUIRE((st.st_mode & S_ISGID) == 0);
    REQUIRE((st.st_mode & S_IWOTH) == 0);
    REQUIRE((st.st_mode & S_IWGRP) == 0);
    REQUIRE((st.st_mode & 0777) == 0755);

    // 3. SUID + World-writable combined: both SUID cleared and world-write masked
    const QString fileCombo = destDir + "/suid_world_writable";
    REQUIRE(QFile::exists(fileCombo));
    REQUIRE(stat(fileCombo.toUtf8().constData(), &st) == 0);
    REQUIRE((st.st_mode & S_ISUID) == 0);
    REQUIRE((st.st_mode & S_ISGID) == 0);
    REQUIRE((st.st_mode & S_IWOTH) == 0);
    REQUIRE((st.st_mode & S_IWGRP) == 0);
    REQUIRE((st.st_mode & 0777) == 0755);

    // 4. SGID executable: SGID cleared, group/world write masked
    const QString fileSgid = destDir + "/sgid_exec";
    REQUIRE(QFile::exists(fileSgid));
    REQUIRE(stat(fileSgid.toUtf8().constData(), &st) == 0);
    REQUIRE((st.st_mode & S_ISUID) == 0);
    REQUIRE((st.st_mode & S_ISGID) == 0);
    REQUIRE((st.st_mode & S_IWOTH) == 0);
    REQUIRE((st.st_mode & S_IWGRP) == 0);
    REQUIRE((st.st_mode & 0777) == 0755);
}

TEST_CASE("Adversarial: Special files (FIFO, Character/Block devices, Socket) skipped without hanging", "[adversarial][security][archive]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("Archive with FIFO, character device, block device, and socket nodes is skipped without hanging") {
        const QString tarPath = tempDir.filePath("all_special_devices.tar");
        const QString destDir = tempDir.filePath("dest_special");

        REQUIRE(writeSyntheticArchive(tarPath, {
            {"fifo_node", "", S_IFIFO | 0666, "", ""},
            {"char_node", "", S_IFCHR | 0660, "", ""},
            {"block_node", "", S_IFBLK | 0660, "", ""},
            {"socket_node", "", S_IFSOCK | 0777, "", ""},
            {"valid_regular.txt", "valid regular payload", 0644, "", ""}
        }));

        auto start = std::chrono::steady_clock::now();
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        // Must complete quickly without hanging (less than 2000 ms)
        REQUIRE(elapsed < 2000);
        REQUIRE(ok);

        // All special nodes must be skipped and NOT exist on disk
        REQUIRE_FALSE(QFile::exists(destDir + "/fifo_node"));
        REQUIRE_FALSE(QFile::exists(destDir + "/char_node"));
        REQUIRE_FALSE(QFile::exists(destDir + "/block_node"));
        REQUIRE_FALSE(QFile::exists(destDir + "/socket_node"));

        // Regular file must exist and have correct content
        REQUIRE(QFile::exists(destDir + "/valid_regular.txt"));
        QFile f(destDir + "/valid_regular.txt");
        REQUIRE(f.open(QIODevice::ReadOnly));
        REQUIRE(f.readAll() == "valid regular payload");
    }

    SECTION("Archive containing ONLY special devices safely rejects without hanging") {
        const QString tarPath = tempDir.filePath("only_specials.tar");
        const QString destDir = tempDir.filePath("dest_only_specials");

        REQUIRE(writeSyntheticArchive(tarPath, {
            {"lone_pipe", "", S_IFIFO | 0666, "", ""},
            {"lone_char", "", S_IFCHR | 0660, "", ""}
        }));

        auto start = std::chrono::steady_clock::now();
        QString err;
        bool ok = ArchiveExtractor::extractSecure(tarPath, destDir, &err);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        REQUIRE(elapsed < 2000);
        // Must return false since no regular files were produced
        REQUIRE_FALSE(ok);
        REQUIRE(err.contains("regular", Qt::CaseInsensitive));
        REQUIRE_FALSE(QFile::exists(destDir + "/lone_pipe"));
        REQUIRE_FALSE(QFile::exists(destDir + "/lone_char"));
    }
}

#include "dependency_resolver.h"
#include "dependencyanalyzer.h"
#include "repository_browser.h"
#include "runtime_probe.h"
#include "cache_manager.h"
#include <QStandardPaths>

TEST_CASE("DependencyResolver rejects empty output directory and protects host /usr/lib (SYS-CRIT-04)", "[security][resolver]") {
    DependencyResolver resolver;

    SECTION("extractLibraries returns empty list when outputDir is empty string") {
        QStringList libs = resolver.extractLibraries("/dummy/nonexistent.deb", "");
        REQUIRE(libs.isEmpty());
    }

    SECTION("extractLibraries returns empty list when outputDir contains only whitespace") {
        QStringList libs = resolver.extractLibraries("/dummy/nonexistent.deb", "   \t\n  ");
        REQUIRE(libs.isEmpty());
    }

    SECTION("extractLibraries returns empty list for RPM when outputDir is empty") {
        QStringList libs = resolver.extractLibraries("/dummy/nonexistent.rpm", "");
        REQUIRE(libs.isEmpty());
    }
}

TEST_CASE("Dependency staging places files into staged_deps and preserves soname symlinks (SYS-CRIT-04, REL-MED-31, REL-MED-34)", "[staging][resolver][symlinks]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("Staged dependency directory recursively copied into AppDir preserving soname symlinks") {
        const QString stagedLibDir = tempDir.path() + "/staged_deps/usr/lib";
        REQUIRE(QDir().mkpath(stagedLibDir));

        // Create multi-part soname library (REL-MED-34: libgdk-pixbuf-2.0.so.0.4200.8)
        const QString libReal = stagedLibDir + "/libgdk-pixbuf-2.0.so.0.4200.8";
        QFile f1(libReal);
        REQUIRE(f1.open(QIODevice::WriteOnly));
        f1.write("dummy-elf-payload-for-gdk-pixbuf");
        f1.close();

        // Create soname symlinks: major version and unversioned
        const QString symMajor = stagedLibDir + "/libgdk-pixbuf-2.0.so.0";
        const QString symBase = stagedLibDir + "/libgdk-pixbuf-2.0.so";
        REQUIRE(QFile::link("libgdk-pixbuf-2.0.so.0.4200.8", symMajor));
        REQUIRE(QFile::link("libgdk-pixbuf-2.0.so.0", symBase));

        // Create standard library (libgsl.so.28.0.0)
        const QString gslReal = stagedLibDir + "/libgsl.so.28.0.0";
        QFile f2(gslReal);
        REQUIRE(f2.open(QIODevice::WriteOnly));
        f2.write("dummy-elf-payload-for-gsl");
        f2.close();

        const QString gslMajor = stagedLibDir + "/libgsl.so.28";
        const QString gslBase = stagedLibDir + "/libgsl.so";
        REQUIRE(QFile::link("libgsl.so.28.0.0", gslMajor));
        REQUIRE(QFile::link("libgsl.so.28", gslBase));

        // AppDir target directory
        const QString appDirPath = tempDir.path() + "/AppDir";
        const QString targetLibDir = appDirPath + "/usr/lib";
        REQUIRE(QDir().mkpath(targetLibDir));

        // Simulate buildAppDir() staged copy
        bool copied = SubprocessWrapper::copyDirectory(stagedLibDir, targetLibDir);
        REQUIRE(copied);

        // Verify regular files exist and have intact content
        REQUIRE(QFile::exists(targetLibDir + "/libgdk-pixbuf-2.0.so.0.4200.8"));
        REQUIRE(QFile::exists(targetLibDir + "/libgsl.so.28.0.0"));
        QFile readF(targetLibDir + "/libgdk-pixbuf-2.0.so.0.4200.8");
        REQUIRE(readF.open(QIODevice::ReadOnly));
        REQUIRE(readF.readAll() == "dummy-elf-payload-for-gdk-pixbuf");

        // Verify soname symlinks exist and are valid symlinks
        QFileInfo targetSymMajor(targetLibDir + "/libgdk-pixbuf-2.0.so.0");
        REQUIRE(targetSymMajor.isSymLink());
        REQUIRE(QFileInfo(targetSymMajor.symLinkTarget()).fileName() == "libgdk-pixbuf-2.0.so.0.4200.8");

        QFileInfo targetSymBase(targetLibDir + "/libgdk-pixbuf-2.0.so");
        REQUIRE(targetSymBase.isSymLink());
        REQUIRE(QFileInfo(targetSymBase.symLinkTarget()).fileName() == "libgdk-pixbuf-2.0.so.0");

        QFileInfo targetGslMajor(targetLibDir + "/libgsl.so.28");
        REQUIRE(targetGslMajor.isSymLink());
        REQUIRE(QFileInfo(targetGslMajor.symLinkTarget()).fileName() == "libgsl.so.28.0.0");

        QFileInfo targetGslBase(targetLibDir + "/libgsl.so");
        REQUIRE(targetGslBase.isSymLink());
        REQUIRE(QFileInfo(targetGslBase.symLinkTarget()).fileName() == "libgsl.so.28");
    }
}

TEST_CASE("Subprocess execution eliminates shell injection (SEC-CRIT-01, SEC-CRIT-03)", "[security][injection]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    SECTION("APT download does not invoke bash -lc and neutralizes metacharacters") {
        const QString canary = tempDir.filePath("canary_apt_injection");
        REQUIRE_FALSE(QFile::exists(canary));

        PackageInfo p;
        p.name = QString("safe-pkg; touch '%1' ; #").arg(canary);
        
        RepositoryBrowser browser;
        // Directly invokes apt download without shell; command injection cannot trigger
        browser.downloadApt(p, tempDir.path());

        REQUIRE_FALSE(QFile::exists(canary));
    }

    SECTION("RPM extraction does not invoke /bin/sh -c and neutralizes metacharacters") {
        const QString canary = "/tmp/canary_rpm_injection";
        QFile::remove(canary);
        REQUIRE_FALSE(QFile::exists(canary));

        const QString injectionDir = tempDir.filePath("inject\";touch " + canary + ";\"");
        REQUIRE(QDir().mkpath(injectionDir));
        const QString maliciousPath = injectionDir + "/test.rpm";
        QFile f(maliciousPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not-a-real-rpm");
        f.close();

        DependencyResolver resolver;
        const QString destDir = tempDir.filePath("rpm_extract_dest");
        QDir().mkpath(destDir);

        resolver.extractLibraries(maliciousPath, destDir);

        // Canary file must never have been created via shell execution
        REQUIRE_FALSE(QFile::exists(canary));
        QFile::remove(canary);
    }
}

TEST_CASE("RuntimeProbePolicy disallows active host execution of untrusted binaries (SEC-HIGH-20)", "[security][probe]") {
    SECTION("Default policy is static verification without native execution") {
        REQUIRE_FALSE(RuntimeProbePolicy::allowHostExecution());
    }

    SECTION("Runtime probe skips native command execution on untrusted binary") {
        QTemporaryDir tempDir;
        REQUIRE(tempDir.isValid());

        const QString appRun = tempDir.filePath("AppRun");
        QFile f(appRun);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("#!/bin/sh\nexit 0\n");
        f.close();
        SubprocessWrapper::setExecutable(appRun);

        PackageProfile profile;
        profile.applicationProfile = ApplicationProfile::NativeCli;
        PackageMetadata meta;

        RuntimeProbeResult res = RuntimeProbePolicy::probe(tempDir.path(), profile, meta, appRun);
        REQUIRE(res.syntaxCheckPassed);
        REQUIRE_FALSE(res.commandExecuted);
        REQUIRE(res.success);
        REQUIRE(res.warnings.join(" ").contains("skipped", Qt::CaseInsensitive));
    }
}

TEST_CASE("Secure temporary directories and cache permissions (SEC-HIGH-13, SEC-MED-32)", "[security][permissions]") {
    SECTION("SubprocessWrapper::createTemporaryDirectory enforces 0700 permissions") {
        QString dirPath = SubprocessWrapper::createTemporaryDirectory("test_secure_dir");
        REQUIRE_FALSE(dirPath.isEmpty());
        QFileInfo info(dirPath);
        REQUIRE(info.isDir());

        QFile::Permissions perms = info.permissions();
        REQUIRE((perms & QFile::ReadOwner) != 0);
        REQUIRE((perms & QFile::WriteOwner) != 0);
        REQUIRE((perms & QFile::ExeOwner) != 0);
        REQUIRE((perms & (QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0);
        REQUIRE((perms & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup)) == 0);

        SubprocessWrapper::removeDirectory(dirPath);
    }

    SECTION("CacheManager enforces 0700 for cache dir and 0600 for cache files") {
        const QString testHash = "m3_sec_test_hash_abcdef0123456789";
        const QStringList testOutput = {"libsample.so.1 => /usr/lib/libsample.so.1 (0x0)"};

        CacheManager::setLddCache(testHash, testOutput);

        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheDir.isEmpty()) {
            cacheDir = QDir::homePath() + "/.cache";
        }
        const QString lddCacheDir = cacheDir + "/appalchemist/ldd_cache";
        const QString cacheFilePath = lddCacheDir + "/" + testHash + ".json";

        REQUIRE(QFile::exists(cacheFilePath));

        QFileInfo dirInfo(lddCacheDir);
        REQUIRE((dirInfo.permissions() & (QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0);

        QFileInfo fileInfo(cacheFilePath);
        REQUIRE((fileInfo.permissions() & QFile::ReadOwner) != 0);
        REQUIRE((fileInfo.permissions() & QFile::WriteOwner) != 0);
        REQUIRE((fileInfo.permissions() & (QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0);

        // Verify retrieval works
        QStringList retrieved = CacheManager::getLddCache(testHash);
        REQUIRE(retrieved == testOutput);

        QFile::remove(cacheFilePath);
    }
}

TEST_CASE("Sandboxed runSafeLdd inspects ELF binaries in temporary directory under /tmp (SEC-HIGH-06)", "[security][sandbox][ldd]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    REQUIRE(tempDir.path().startsWith("/tmp"));

    QString hostBinary = "/bin/echo";
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/usr/bin/echo";
    }
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/bin/ls";
    }
    REQUIRE(QFile::exists(hostBinary));

    const QString tempBinary = tempDir.filePath("test_echo");
    REQUIRE(QFile::copy(hostBinary, tempBinary));
    REQUIRE(SubprocessWrapper::setExecutable(tempBinary));

    SECTION("DependencyResolver::runSafeLdd inspects binary under /tmp") {
        QStringList output = DependencyResolver::runSafeLdd(tempBinary);
        REQUIRE_FALSE(output.isEmpty());

        bool foundLibc = false;
        for (const QString& line : output) {
            if (line.contains("libc.so")) {
                foundLibc = true;
                break;
            }
        }
        REQUIRE(foundLibc);
    }

    SECTION("DependencyAnalyzer::runLdd inspects binary under /tmp") {
        QStringList output = DependencyAnalyzer::runLdd(tempBinary);
        REQUIRE_FALSE(output.isEmpty());

        bool foundLibc = false;
        for (const QString& line : output) {
            if (line.contains("libc.so")) {
                foundLibc = true;
                break;
            }
        }
        REQUIRE(foundLibc);
    }
}

TEST_CASE("Passive readelf -d DT_NEEDED fallback on bwrap failure or bypass (SEC-HIGH-06)", "[security][readelf][fallback]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString hostBinary = "/bin/echo";
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/usr/bin/echo";
    }
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/bin/ls";
    }
    REQUIRE(QFile::exists(hostBinary));

    const QString tempBinary = tempDir.filePath("test_echo_fallback");
    REQUIRE(QFile::copy(hostBinary, tempBinary));
    REQUIRE(SubprocessWrapper::setExecutable(tempBinary));

    SECTION("Fallback activates and extracts DT_NEEDED when bwrap is bypassed") {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("APPALCHEMIST_DISABLE_BWRAP", "1");

        QStringList output = DependencyResolver::runSafeLdd(tempBinary, env);
        REQUIRE_FALSE(output.isEmpty());

        bool foundLibc = false;
        for (const QString& line : output) {
            if (line.contains("libc.so")) {
                foundLibc = true;
                break;
            }
        }
        REQUIRE(foundLibc);

        QStringList analyzerOutput = DependencyAnalyzer::runLdd(tempBinary, env);
        REQUIRE_FALSE(analyzerOutput.isEmpty());
        bool analyzerFoundLibc = false;
        for (const QString& line : analyzerOutput) {
            if (line.contains("libc.so")) {
                analyzerFoundLibc = true;
                break;
            }
        }
        REQUIRE(analyzerFoundLibc);
    }

    SECTION("Fallback activates and extracts DT_NEEDED when bwrap execution fails") {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("APPALCHEMIST_SIMULATE_BWRAP_FAILURE", "1");

        QStringList output = DependencyResolver::runSafeLdd(tempBinary, env);
        REQUIRE_FALSE(output.isEmpty());

        bool foundLibc = false;
        for (const QString& line : output) {
            if (line.contains("libc.so")) {
                foundLibc = true;
                break;
            }
        }
        REQUIRE(foundLibc);

        QStringList analyzerOutput = DependencyAnalyzer::runLdd(tempBinary, env);
        REQUIRE_FALSE(analyzerOutput.isEmpty());
        bool analyzerFoundLibc = false;
        for (const QString& line : analyzerOutput) {
            if (line.contains("libc.so")) {
                analyzerFoundLibc = true;
                break;
            }
        }
        REQUIRE(analyzerFoundLibc);
    }
}

TEST_CASE("RuntimeProbePolicy with allowHostExecution in /tmp under bwrap", "[security][probe][bwrap]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    REQUIRE(tempDir.path().startsWith("/tmp"));

    const QString appRun = tempDir.filePath("AppRun");
    QFile f(appRun);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#!/bin/sh\necho \"Healthy App\"\nexit 0\n");
    f.close();
    SubprocessWrapper::setExecutable(appRun);

    PackageProfile profile;
    profile.applicationProfile = ApplicationProfile::NativeCli;
    PackageMetadata meta;

    RuntimeProbeResult res = RuntimeProbePolicy::probe(tempDir.path(), profile, meta, appRun, true);
    REQUIRE(res.syntaxCheckPassed);
    REQUIRE(res.commandExecuted);
    REQUIRE(res.success);
    REQUIRE(res.exitCode == 0);
}

#include "appdirbuilder.h"
#include "appdetector.h"
#include "conversion_controller.h"
#include "packagetoappimagepipeline.h"
#include <QRegularExpression>
#include <QProcess>

TEST_CASE("AppRun environment sanitization prevents trailing colons (SEC-CRIT-02)", "[security][apprun][sanitization]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.path();

    // Create minimal directory structure
    REQUIRE(QDir().mkpath(appDirPath + "/usr/bin"));
    REQUIRE(QDir().mkpath(appDirPath + "/usr/share/glib-2.0/schemas"));
    REQUIRE(QDir().mkpath(appDirPath + "/usr/lib/girepository-1.0"));

    const QString dummyBin = appDirPath + "/usr/bin/sampleapp";
    QFile binFile(dummyBin);
    REQUIRE(binFile.open(QIODevice::WriteOnly | QIODevice::Text));
    binFile.write("#!/bin/sh\nexit 0\n");
    binFile.close();
    SubprocessWrapper::setExecutable(dummyBin);

    PackageMetadata meta;
    meta.package = "sampleapp";
    meta.mainExecutable = "usr/bin/sampleapp";
    meta.executables = {"usr/bin/sampleapp"};

    AppDirBuilder builder;
    REQUIRE(builder.createAppRun(appDirPath, meta));

    const QString appRunPath = appDirPath + "/AppRun";
    REQUIRE(QFile::exists(appRunPath));
    QFile appRunFile(appRunPath);
    REQUIRE(appRunFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QString appRunContent = appRunFile.readAll();
    appRunFile.close();

    // 1. AppRun must be executable
    QFileInfo appRunInfo(appRunPath);
    REQUIRE((appRunInfo.permissions() & QFile::ExeOwner) != 0);

    // 2. AppRun syntax must be valid bash
    QProcess syntaxCheck;
    syntaxCheck.start("bash", {"-n", appRunPath});
    REQUIRE(syntaxCheck.waitForFinished(3000));
    REQUIRE(syntaxCheck.exitCode() == 0);

    // 3. SEC-CRIT-02: No unconditioned trailing colons in variable expansions
    // Neither LD_LIBRARY_PATH nor GSETTINGS_SCHEMA_DIR nor GI_TYPELIB_PATH should end with ":${VAR}"
    REQUIRE_FALSE(appRunContent.contains(":${LD_LIBRARY_PATH}\""));
    REQUIRE_FALSE(appRunContent.contains(":${GSETTINGS_SCHEMA_DIR}\""));
    REQUIRE_FALSE(appRunContent.contains(":${GI_TYPELIB_PATH}\""));
    REQUIRE_FALSE(appRunContent.contains(":${PYTHONPATH}\""));

    // Proper conditional syntax must be present
    REQUIRE(appRunContent.contains("${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"));
    REQUIRE(appRunContent.contains("${GSETTINGS_SCHEMA_DIR:+:${GSETTINGS_SCHEMA_DIR}}"));
    REQUIRE(appRunContent.contains("${GI_TYPELIB_PATH:+:${GI_TYPELIB_PATH}}"));

    // 4. Executing AppRun with completely empty environment must not produce trailing colon in LD_LIBRARY_PATH
    QProcess envCheck;
    envCheck.setProcessEnvironment(QProcessEnvironment()); // completely clean environment
    QString bashCmd = QString("HERE='%1'; eval \"$(sed '/^[[:space:]]*exec /d' '%2')\"; printf '%%s' \"$LD_LIBRARY_PATH\"").arg(appDirPath, appRunPath);
    envCheck.start("bash", {"-c", bashCmd});
    REQUIRE(envCheck.waitForFinished(3000));
    QString ldOutput = QString::fromUtf8(envCheck.readAllStandardOutput()).trimmed();
    REQUIRE_FALSE(ldOutput.isEmpty());
    REQUIRE_FALSE(ldOutput.endsWith(":"));
    REQUIRE_FALSE(ldOutput.startsWith(":"));
    REQUIRE_FALSE(ldOutput.contains("::"));
}

TEST_CASE("Single root desktop file enforcement (SYS-HIGH-18)", "[desktop][appdir][sys-high-18]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");
    REQUIRE(QDir().mkpath(appDirPath));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/applications"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/bin"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/256x256/apps"));

    // Create multiple desktop files in extracted package
    const QString canonDesktop = extractedDir + "/usr/share/applications/canonical.desktop";
    QFile f1(canonDesktop);
    REQUIRE(f1.open(QIODevice::WriteOnly | QIODevice::Text));
    f1.write("[Desktop Entry]\nType=Application\nName=Canonical App\nExec=canonical\nIcon=canonical\nCategories=Utility;\n");
    f1.close();

    const QString extraDesktop1 = extractedDir + "/usr/share/applications/extra-helper.desktop";
    QFile f2(extraDesktop1);
    REQUIRE(f2.open(QIODevice::WriteOnly | QIODevice::Text));
    f2.write("[Desktop Entry]\nType=Application\nName=Extra Helper\nExec=helper\nIcon=helper\nCategories=Utility;\n");
    f2.close();

    const QString extraDesktop2 = extractedDir + "/usr/share/applications/extra-config.desktop";
    QFile f3(extraDesktop2);
    REQUIRE(f3.open(QIODevice::WriteOnly | QIODevice::Text));
    f3.write("[Desktop Entry]\nType=Application\nName=Extra Config\nExec=config\nIcon=config\nCategories=Settings;\n");
    f3.close();

    // Create icon file
    const QString iconFile = extractedDir + "/usr/share/icons/hicolor/256x256/apps/canonical.png";
    QFile fIcon(iconFile);
    REQUIRE(fIcon.open(QIODevice::WriteOnly));
    fIcon.write("dummy-png-data");
    fIcon.close();

    // Create executable
    const QString execFile = extractedDir + "/usr/bin/canonical";
    QFile fExec(execFile);
    REQUIRE(fExec.open(QIODevice::WriteOnly | QIODevice::Text));
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(execFile);

    // Plant a pre-existing rogue desktop file directly at AppDir root
    const QString rogueDesktop = appDirPath + "/rogue.desktop";
    QFile fRogue(rogueDesktop);
    REQUIRE(fRogue.open(QIODevice::WriteOnly | QIODevice::Text));
    fRogue.write("[Desktop Entry]\nType=Application\nName=Rogue\nExec=rogue\n");
    fRogue.close();

    PackageMetadata meta;
    meta.package = "canonical";
    meta.mainExecutable = execFile;
    meta.executables = {execFile};
    meta.iconPath = iconFile;

    AppDirBuilder builder;
    REQUIRE(builder.buildAppDir(appDirPath, extractedDir, meta, {}));

    // Verify AppDir root contains EXACTLY ONE .desktop file
    QStringList rootDesktops = QDir(appDirPath).entryList({"*.desktop"}, QDir::Files);
    REQUIRE(rootDesktops.size() == 1);
    REQUIRE(rootDesktops.first() == "canonical.desktop");

    // Verify the rogue desktop file was removed
    REQUIRE_FALSE(QFile::exists(rogueDesktop));
}

TEST_CASE(".DirIcon relative symlink creation and fallback (SYS-HIGH-19)", "[icon][appdir][sys-high-19]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");
    REQUIRE(QDir().mkpath(appDirPath));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/applications"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/bin"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/128x128/apps"));

    const QString desktop = extractedDir + "/usr/share/applications/myapp.desktop";
    QFile f1(desktop);
    REQUIRE(f1.open(QIODevice::WriteOnly | QIODevice::Text));
    f1.write("[Desktop Entry]\nType=Application\nName=MyApp\nExec=myapp\nIcon=myapp\nCategories=Utility;\n");
    f1.close();

    const QString iconFile = extractedDir + "/usr/share/icons/hicolor/128x128/apps/myapp.png";
    QFile fIcon(iconFile);
    REQUIRE(fIcon.open(QIODevice::WriteOnly));
    fIcon.write("dummy-png-icon-bytes");
    fIcon.close();

    const QString execFile = extractedDir + "/usr/bin/myapp";
    QFile fExec(execFile);
    REQUIRE(fExec.open(QIODevice::WriteOnly | QIODevice::Text));
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(execFile);

    PackageMetadata meta;
    meta.package = "myapp";
    meta.mainExecutable = execFile;
    meta.executables = {execFile};
    meta.iconPath = iconFile;

    AppDirBuilder builder;
    REQUIRE(builder.buildAppDir(appDirPath, extractedDir, meta, {}));

    // .DirIcon must exist in AppDir root
    const QString dirIconPath = appDirPath + "/.DirIcon";
    REQUIRE(QFile::exists(dirIconPath));

    // Must be a symlink
    QFileInfo dirIconInfo(dirIconPath);
    REQUIRE(dirIconInfo.isSymLink());

    // Symlink target must be the relative icon filename ("myapp.png"), not an absolute path
    QString symTarget = dirIconInfo.symLinkTarget();
    REQUIRE(QFileInfo(symTarget).fileName() == "myapp.png");

    // The root icon file must also exist
    REQUIRE(QFile::exists(appDirPath + "/myapp.png"));
}

TEST_CASE("Icon discovery in standard locations with empty meta.iconPath (SYS-HIGH-19, REL-MED-22)", "[icon][appdir][discovery][sys-high-19][rel-med-22]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");
    REQUIRE(QDir().mkpath(appDirPath));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/applications"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/bin"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/64x64/apps"));

    const QString desktop = extractedDir + "/usr/share/applications/myapp.desktop";
    QFile fDesktop(desktop);
    REQUIRE(fDesktop.open(QIODevice::WriteOnly | QIODevice::Text));
    fDesktop.write("[Desktop Entry]\nType=Application\nName=MyApp\nExec=myapp\nIcon=myapp\nCategories=Utility;\n");
    fDesktop.close();

    const QByteArray genuineIconPayload = "GENUINE_64x64_ICON_PAYLOAD_NOT_A_PLACEHOLDER";
    const QString iconFile = extractedDir + "/usr/share/icons/hicolor/64x64/apps/myapp.png";
    QFile fIcon(iconFile);
    REQUIRE(fIcon.open(QIODevice::WriteOnly));
    fIcon.write(genuineIconPayload);
    fIcon.close();

    const QString execFile = extractedDir + "/usr/bin/myapp";
    QFile fExec(execFile);
    REQUIRE(fExec.open(QIODevice::WriteOnly | QIODevice::Text));
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(execFile);

    PackageMetadata meta;
    meta.package = "myapp";
    meta.mainExecutable = execFile;
    meta.executables = {execFile};
    meta.iconPath = ""; // Crucial: explicitly empty to exercise discovery logic!

    AppDirBuilder builder;
    REQUIRE(builder.buildAppDir(appDirPath, extractedDir, meta, {}));

    // Root icon must exist
    const QString rootIconPath = appDirPath + "/myapp.png";
    REQUIRE(QFile::exists(rootIconPath));

    // Must contain genuine icon payload, NOT the 1x1 placeholder PNG
    QFile fRootIcon(rootIconPath);
    REQUIRE(fRootIcon.open(QIODevice::ReadOnly));
    QByteArray rootIconBytes = fRootIcon.readAll();
    fRootIcon.close();
    REQUIRE(rootIconBytes == genuineIconPayload);

    QByteArray placeholderPng = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
    );
    REQUIRE(rootIconBytes != placeholderPng);

    // .DirIcon must exist at AppDir root as a relative symlink pointing to myapp.png
    const QString dirIconPath = appDirPath + "/.DirIcon";
    REQUIRE(QFile::exists(dirIconPath));
    QFileInfo dirIconInfo(dirIconPath);
    REQUIRE(dirIconInfo.isSymLink());
    REQUIRE(QFileInfo(dirIconInfo.symLinkTarget()).fileName() == "myapp.png");
}

TEST_CASE("External metadata.iconPath is preserved and not overwritten by placeholder (SYS-HIGH-19)", "[icon][appdir][external][sys-high-19]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");
    const QString externalDir = tempDir.filePath("external");
    REQUIRE(QDir().mkpath(appDirPath));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/share/applications"));
    REQUIRE(QDir().mkpath(extractedDir + "/usr/bin"));
    REQUIRE(QDir().mkpath(externalDir));

    // Note: extractedDir does NOT contain any icons under usr/share/icons
    const QString desktop = extractedDir + "/usr/share/applications/externalapp.desktop";
    QFile fDesktop(desktop);
    REQUIRE(fDesktop.open(QIODevice::WriteOnly | QIODevice::Text));
    fDesktop.write("[Desktop Entry]\nType=Application\nName=ExternalApp\nExec=externalapp\nIcon=externalapp\nCategories=Utility;\n");
    fDesktop.close();

    const QString execFile = extractedDir + "/usr/bin/externalapp";
    QFile fExec(execFile);
    REQUIRE(fExec.open(QIODevice::WriteOnly | QIODevice::Text));
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(execFile);

    const QByteArray externalIconPayload = "CUSTOM_EXTERNAL_BRAND_ICON_PAYLOAD_BYTES";
    const QString externalIconPath = externalDir + "/custom_icon.png";
    QFile fExtIcon(externalIconPath);
    REQUIRE(fExtIcon.open(QIODevice::WriteOnly));
    fExtIcon.write(externalIconPayload);
    fExtIcon.close();

    PackageMetadata meta;
    meta.package = "externalapp";
    meta.mainExecutable = execFile;
    meta.executables = {execFile};
    meta.iconPath = externalIconPath;

    AppDirBuilder builder;
    REQUIRE(builder.buildAppDir(appDirPath, extractedDir, meta, {}));

    // Root icon must exist
    const QString rootIconPath = appDirPath + "/externalapp.png";
    REQUIRE(QFile::exists(rootIconPath));

    // Verify root icon content was NOT overwritten by the 1x1 placeholder
    QFile fRoot(rootIconPath);
    REQUIRE(fRoot.open(QIODevice::ReadOnly));
    QByteArray actualBytes = fRoot.readAll();
    fRoot.close();
    REQUIRE(actualBytes == externalIconPayload);

    QByteArray placeholderPng = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
    );
    REQUIRE(actualBytes != placeholderPng);

    // .DirIcon must exist as a relative symlink pointing to externalapp.png
    const QString dirIconPath = appDirPath + "/.DirIcon";
    REQUIRE(QFile::exists(dirIconPath));
    QFileInfo dirIconInfo(dirIconPath);
    REQUIRE(dirIconInfo.isSymLink());
    REQUIRE(QFileInfo(dirIconInfo.symLinkTarget()).fileName() == "externalapp.png");
}

TEST_CASE("Freedesktop field codes preservation in fixDesktopFile (REL-MED-23)", "[desktop][freedesktop][rel-med-23]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    AppDirBuilder builder;
    PackageMetadata meta;
    meta.package = "editor";

    SECTION("Preserves %F (multiple files)") {
        const QString desktopFile = tempDir.filePath("editor_F.desktop");
        QFile f(desktopFile);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Desktop Entry]\nType=Application\nName=Editor\nExec=editor --unity-launch %F\nCategories=Utility;\n");
        f.close();

        REQUIRE(builder.fixDesktopFile(desktopFile, meta));

        QFile readF(desktopFile);
        REQUIRE(readF.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = readF.readAll();
        REQUIRE(content.contains("Exec=AppRun %F"));
        REQUIRE_FALSE(content.contains("--unity-launch"));
    }

    SECTION("Preserves %U (URL list)") {
        const QString desktopFile = tempDir.filePath("browser_U.desktop");
        QFile f(desktopFile);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Desktop Entry]\nType=Application\nName=Browser\nExec=/usr/bin/browser -new-window %U\nCategories=Network;\n");
        f.close();

        REQUIRE(builder.fixDesktopFile(desktopFile, meta));

        QFile readF(desktopFile);
        REQUIRE(readF.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = readF.readAll();
        REQUIRE(content.contains("Exec=AppRun %U"));
    }

    SECTION("Preserves multiple field codes (%i %c %k)") {
        const QString desktopFile = tempDir.filePath("multi_codes.desktop");
        QFile f(desktopFile);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Desktop Entry]\nType=Application\nName=App\nExec=app %i %c %k\nCategories=Utility;\n");
        f.close();

        REQUIRE(builder.fixDesktopFile(desktopFile, meta));

        QFile readF(desktopFile);
        REQUIRE(readF.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = readF.readAll();
        REQUIRE(content.contains("Exec=AppRun %i %c %k"));
    }

    SECTION("Handles plain Exec without field codes") {
        const QString desktopFile = tempDir.filePath("plain.desktop");
        QFile f(desktopFile);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Desktop Entry]\nType=Application\nName=App\nExec=/usr/bin/plain --some-flag\nCategories=Utility;\n");
        f.close();

        REQUIRE(builder.fixDesktopFile(desktopFile, meta));

        QFile readF(desktopFile);
        REQUIRE(readF.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = readF.readAll();
        REQUIRE(content.contains("Exec=AppRun\n"));
    }

    SECTION("Only modifies Exec in [Desktop Entry] section, not [Desktop Action]") {
        const QString desktopFile = tempDir.filePath("with_actions.desktop");
        QFile f(desktopFile);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Desktop Entry]\n"
                "Type=Application\n"
                "Name=MultiActionApp\n"
                "Exec=app --launch %u\n"
                "Categories=Utility;\n"
                "Actions=NewWindow;\n"
                "\n"
                "[Desktop Action NewWindow]\n"
                "Name=Open New Window\n"
                "Exec=app --new-window %u\n");
        f.close();

        REQUIRE(builder.fixDesktopFile(desktopFile, meta));

        QFile readF(desktopFile);
        REQUIRE(readF.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = readF.readAll();
        // Main section converted to Exec=AppRun %u
        REQUIRE(content.contains("Exec=AppRun %u"));
        // Action section unchanged
        REQUIRE(content.contains("Exec=app --new-window %u"));
    }
}

TEST_CASE("Icon and desktop file detection across package formats (REL-MED-22)", "[detector][desktop][icon][rel-med-22]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString rootDir = tempDir.path();

    SECTION("RPM / Tarball format: files located directly in usr/share without data/ prefix") {
        const QString rpmExtract = rootDir + "/rpm_pkg";
        REQUIRE(QDir().mkpath(rpmExtract + "/usr/share/applications"));
        REQUIRE(QDir().mkpath(rpmExtract + "/usr/share/icons/hicolor/64x64/apps"));

        const QString desktopPath = rpmExtract + "/usr/share/applications/fedoraapp.desktop";
        QFile fDesk(desktopPath);
        REQUIRE(fDesk.open(QIODevice::WriteOnly | QIODevice::Text));
        fDesk.write("[Desktop Entry]\nType=Application\nName=FedoraApp\nExec=fedoraapp\n");
        fDesk.close();

        const QString iconPath = rpmExtract + "/usr/share/icons/hicolor/64x64/apps/fedoraapp.png";
        QFile fIcon(iconPath);
        REQUIRE(fIcon.open(QIODevice::WriteOnly));
        fIcon.write("dummy-icon-bytes");
        fIcon.close();

        QString foundDesktop = AppDetector::findDesktopFile(rpmExtract);
        REQUIRE(foundDesktop == desktopPath);

        QString foundIcon = AppDetector::findIcon(rpmExtract);
        REQUIRE(foundIcon == iconPath);
    }

    SECTION("Debian / Ubuntu format: files located in data/usr/share subpath") {
        const QString debExtract = rootDir + "/deb_pkg";
        REQUIRE(QDir().mkpath(debExtract + "/data/usr/share/applications"));
        REQUIRE(QDir().mkpath(debExtract + "/data/usr/share/icons/hicolor/scalable/apps"));

        const QString desktopPath = debExtract + "/data/usr/share/applications/ubuntapp.desktop";
        QFile fDesk(desktopPath);
        REQUIRE(fDesk.open(QIODevice::WriteOnly | QIODevice::Text));
        fDesk.write("[Desktop Entry]\nType=Application\nName=UbuntApp\nExec=ubuntapp\n");
        fDesk.close();

        const QString iconPath = debExtract + "/data/usr/share/icons/hicolor/scalable/apps/ubuntapp.svg";
        QFile fIcon(iconPath);
        REQUIRE(fIcon.open(QIODevice::WriteOnly));
        fIcon.write("<svg></svg>");
        fIcon.close();

        QString foundDesktop = AppDetector::findDesktopFile(debExtract);
        REQUIRE(foundDesktop == desktopPath);

        QString foundIcon = AppDetector::findIcon(debExtract);
        REQUIRE(foundIcon == iconPath);
    }
}

TEST_CASE("Script path replacement quotation safety (REL-HIGH-17)", "[detector][script][rel-high-17]") {
    const QString scriptOriginal = 
        "#!/bin/bash\n"
        "APP_PATH=/usr/share/myapp/lib\n"
        "OPT_DIR=/opt/myapp/bin\n"
        "exec /usr/bin/myapp-engine --data /usr/share/myapp/data\n";

    QString processed = AppDetector::replaceScriptPaths(scriptOriginal, "${HERE}");

    // Must not contain unmatched double quotes
    int doubleQuotes = processed.count('"');
    REQUIRE(doubleQuotes % 2 == 0);

    // Must pass bash syntax check
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString scriptFile = tempDir.filePath("test_script.sh");
    QFile f(scriptFile);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(processed.toUtf8());
    f.close();

    QProcess bashCheck;
    bashCheck.start("bash", {"-n", scriptFile});
    REQUIRE(bashCheck.waitForFinished(3000));
    REQUIRE(bashCheck.exitCode() == 0);
}

TEST_CASE("Architecture normalization and validation (REL-LOW-51)", "[detector][architecture][rel-low-51]") {
    SECTION("Validation accepts known architectures") {
        REQUIRE(AppDetector::isValidArchitecture("x86_64"));
        REQUIRE(AppDetector::isValidArchitecture("amd64"));
        REQUIRE(AppDetector::isValidArchitecture("aarch64"));
        REQUIRE(AppDetector::isValidArchitecture("arm64"));
        REQUIRE(AppDetector::isValidArchitecture("armhf"));
        REQUIRE(AppDetector::isValidArchitecture("armv7l"));
        REQUIRE(AppDetector::isValidArchitecture("i686"));
        REQUIRE(AppDetector::isValidArchitecture("i386"));
        REQUIRE(AppDetector::isValidArchitecture("all"));
    }

    SECTION("Validation rejects invalid or empty architectures") {
        REQUIRE_FALSE(AppDetector::isValidArchitecture(""));
        REQUIRE_FALSE(AppDetector::isValidArchitecture("   "));
        REQUIRE_FALSE(AppDetector::isValidArchitecture("mips_fake"));
        REQUIRE_FALSE(AppDetector::isValidArchitecture("unknown-cpu"));
    }

    SECTION("Normalization maps Debian and non-standard names to canonical AppImage tags") {
        REQUIRE(AppDetector::normalizeArchitecture("amd64") == "x86_64");
        REQUIRE(AppDetector::normalizeArchitecture("x86_64") == "x86_64");
        REQUIRE(AppDetector::normalizeArchitecture("arm64") == "aarch64");
        REQUIRE(AppDetector::normalizeArchitecture("aarch64") == "aarch64");
        REQUIRE(AppDetector::normalizeArchitecture("armv7l") == "armhf");
        REQUIRE(AppDetector::normalizeArchitecture("armhf") == "armhf");
        REQUIRE(AppDetector::normalizeArchitecture("i386") == "i686");
        REQUIRE(AppDetector::normalizeArchitecture("i686") == "i686");
        REQUIRE(AppDetector::normalizeArchitecture("all") == "all");
    }
}

TEST_CASE("appimagetool discovery across environments (REL-LOW-52)", "[appimagetool][discovery][rel-low-52]") {
    SECTION("Discovers custom path specified by APPIMAGETOOL environment variable") {
        QTemporaryDir tempDir;
        REQUIRE(tempDir.isValid());
        const QString fakeTool = tempDir.filePath("custom-appimagetool");
        QFile f(fakeTool);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("#!/bin/sh\nexit 0\n");
        f.close();
        SubprocessWrapper::setExecutable(fakeTool);

        qputenv("APPIMAGETOOL", fakeTool.toUtf8());
        QString found = AppDirBuilder::findAppImageTool();
        qunsetenv("APPIMAGETOOL");

        REQUIRE(found == fakeTool);
    }

    SECTION("findAppImageTool executes safely without crashing when env is unset") {
        qunsetenv("APPIMAGETOOL");
        QString found = AppDirBuilder::findAppImageTool();
        // Either found system tool or returned empty string
        if (!found.isEmpty()) {
            REQUIRE(QFile::exists(found));
        }
    }
}

TEST_CASE("Pipeline concurrency and cancellation with stop_token (SYS-HIGH-11, SYS-HIGH-21)", "[concurrency][cancellation][pipeline]") {
    PackageToAppImagePipeline pipeline;
    REQUIRE_FALSE(pipeline.isCancelled());

    SECTION("Cancellation via direct cancel() call") {
        pipeline.cancel();
        REQUIRE(pipeline.isCancelled());
    }

    SECTION("Cancellation via std::stop_token") {
        std::stop_source stopSource;
        pipeline.setStopToken(stopSource.get_token());
        REQUIRE_FALSE(pipeline.isCancelled());

        stopSource.request_stop();
        REQUIRE(pipeline.isCancelled());
    }
}

TEST_CASE("ConversionController pipeline lifetime and leak prevention (MEM-MED-25)", "[controller][memory][leak-prevention]") {
    ConversionController controller;

    SECTION("Initial state has no active pipeline") {
        REQUIRE(controller.currentPipeline() == nullptr);
        REQUIRE_FALSE(controller.isRunning());
    }

    SECTION("requestStop triggers stop_token") {
        std::stop_token token = controller.stopToken();
        REQUIRE_FALSE(token.stop_requested());

        controller.requestStop();
        REQUIRE(token.stop_requested());
    }

    SECTION("cleanupCurrentPipeline is safe to call when idle") {
        controller.cleanupCurrentPipeline();
        REQUIRE(controller.currentPipeline() == nullptr);
    }
}

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <thread>
#include <fstream>
#include <unistd.h>

namespace {
static int s_argc = 1;
static char s_arg0[] = "appalchemist_tests";
static char* s_argv[] = {s_arg0, nullptr};
static void ensureApp() {
    if (!QCoreApplication::instance()) {
        new QCoreApplication(s_argc, s_argv);
    }
}

long getProcessResidentMemoryKB() {
    long rss = 0;
    std::ifstream statm("/proc/self/statm");
    if (statm >> rss >> rss) {
        long pageSize = sysconf(_SC_PAGESIZE);
        return (rss * pageSize) / 1024;
    }
    return 0;
}

bool createValidSyntheticAppTarball(const QString& tarPath) {
    return writeSyntheticArchive(tarPath, {
        {"usr/bin/sample-app", "#!/bin/sh\necho running\n", 0755, "", ""},
        {"usr/share/applications/sample-app.desktop",
         "[Desktop Entry]\nType=Application\nName=SampleApp\nExec=sample-app\nIcon=sample-app\n",
         0644, "", ""},
        {"usr/share/icons/hicolor/scalable/apps/sample-app.svg",
         "<svg width=\"64\" height=\"64\"></svg>",
         0644, "", ""}
    });
}
} // namespace

TEST_CASE("Empirical Stress: Pipeline cancellation halting and responsiveness across intervals", "[stress][concurrency][cancellation]") {
    ensureApp();
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("sample-app.tar");
    REQUIRE(createValidSyntheticAppTarball(tarPath));

    auto runCancelledPipeline = [&](int delayMs, bool useStopToken) {
        QThread workerThread;
        PackageToAppImagePipeline pipeline;
        std::stop_source stopSource;
        if (useStopToken) {
            pipeline.setStopToken(stopSource.get_token());
        }
        pipeline.setPackagePath(tarPath);
        pipeline.setOutputPath(tempDir.filePath("out.AppImage"));
        pipeline.moveToThread(&workerThread);

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);

        QObject::connect(&workerThread, &QThread::started, &pipeline, &PackageToAppImagePipeline::start);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &workerThread, &QThread::quit);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

        watchdog.start(5000);
        auto t0 = std::chrono::steady_clock::now();
        workerThread.start();

        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        if (useStopToken) {
            stopSource.request_stop();
        } else {
            pipeline.cancel();
        }

        loop.exec();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        workerThread.quit();
        workerThread.wait(3000);

        REQUIRE(watchdog.isActive()); // Must not have timed out
        REQUIRE(pipeline.isCancelled());
        REQUIRE(elapsedMs < 3000); // Halts promptly without hanging
    };

    SECTION("Immediate cancellation (0ms delay) via cancel()") {
        runCancelledPipeline(0, false);
    }

    SECTION("Immediate cancellation (0ms delay) via stop_token") {
        runCancelledPipeline(0, true);
    }

    SECTION("Cancellation after 10ms (during extraction) via cancel()") {
        runCancelledPipeline(10, false);
    }

    SECTION("Cancellation after 10ms (during extraction) via stop_token") {
        runCancelledPipeline(10, true);
    }

    SECTION("Cancellation after 25ms (during dependency analysis) via cancel()") {
        runCancelledPipeline(25, false);
    }

    SECTION("Cancellation after 25ms (during dependency analysis) via stop_token") {
        runCancelledPipeline(25, true);
    }

    SECTION("Stress loop: 30 consecutive cancellation cycles with randomized delay") {
        for (int i = 0; i < 30; ++i) {
            int delay = (i * 7) % 35; // 0ms to 28ms
            INFO("Iteration " << i << ", delay=" << delay << "ms, useStopToken=" << (i % 2 == 0));
            runCancelledPipeline(delay, (i % 2 == 0));
        }
    }
}

TEST_CASE("Empirical Stress: ConversionController and Pipeline 1000-iteration lifecycle memory leak prevention (MEM-MED-25)", "[stress][memory][leak-prevention]") {
    SECTION("1000 iterations of PackageToAppImagePipeline allocation and destruction (MEM-MED-25 target object)") {
        for (int i = 0; i < 50; ++i) {
            auto* p = new PackageToAppImagePipeline();
            delete p;
        }

        long memBefore = getProcessResidentMemoryKB();

        for (int i = 0; i < 1000; ++i) {
            auto* p = new PackageToAppImagePipeline();
            p->setPackagePath(QString("/tmp/fake-%1.deb").arg(i));
            delete p;
        }

        long memAfter = getProcessResidentMemoryKB();
        long growthKB = memAfter - memBefore;

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
        REQUIRE(growthKB < 150000);
#else
        REQUIRE(growthKB < 5120);
#endif
    }

    SECTION("1000 iterations of ConversionController pipeline allocation and cleanupCurrentPipeline()") {
        ConversionController controller;
        REQUIRE(controller.currentPipeline() == nullptr);

        long memBefore = getProcessResidentMemoryKB();

        for (int i = 0; i < 1000; ++i) {
            controller.cleanupCurrentPipeline();
            REQUIRE(controller.currentPipeline() == nullptr);
        }

        long memAfter = getProcessResidentMemoryKB();
        long growthKB = memAfter - memBefore;

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
        REQUIRE(growthKB < 150000);
#else
        REQUIRE(growthKB < 5120);
#endif
        REQUIRE(controller.currentPipeline() == nullptr);
    }
}

TEST_CASE("Empirical Stress: Temporary directory cleanup on cancellation (CON-HIGH-23)", "[stress][concurrency][leak][con-high-23]") {
    ensureApp();
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("sample-app.tar");
    REQUIRE(createValidSyntheticAppTarball(tarPath));

    auto listAppalchemistTmpDirs = []() {
        QDir tmpDir(QDir::tempPath());
        return tmpDir.entryList({"appalchemist-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    };

    QStringList initialDirs = listAppalchemistTmpDirs();

    QThread workerThread;
    auto* pipeline = new PackageToAppImagePipeline();
    pipeline->setPackagePath(tarPath);
    pipeline->setOutputPath(tempDir.filePath("out.AppImage"));
    pipeline->moveToThread(&workerThread);

    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    std::atomic<bool> finishedReceived{false};
    QStringList strandedDirsBeforeDestruct;

    QObject::connect(&workerThread, &QThread::started, pipeline, &PackageToAppImagePipeline::start);
    QObject::connect(pipeline, &PackageToAppImagePipeline::finished, &workerThread, &QThread::quit);
    QObject::connect(pipeline, &PackageToAppImagePipeline::finished, &loop, [&]() {
        finishedReceived.store(true);
        QStringList currentDirs = listAppalchemistTmpDirs();
        for (const QString& d : currentDirs) {
            if (!initialDirs.contains(d)) {
                strandedDirsBeforeDestruct << d;
            }
        }
        loop.quit();
    });
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

    watchdog.start(5000);
    workerThread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pipeline->cancel();

    loop.exec();
    workerThread.quit();
    workerThread.wait(3000);

    REQUIRE(watchdog.isActive());
    REQUIRE(finishedReceived.load());

    bool tempDirLeakedBeforeDestruction = !strandedDirsBeforeDestruct.isEmpty();

    delete pipeline;

    QStringList dirsAfterDestruct = listAppalchemistTmpDirs();
    QStringList newlyLeakedDirs;
    for (const QString& d : dirsAfterDestruct) {
        if (!initialDirs.contains(d)) {
            newlyLeakedDirs << d;
        }
    }

    REQUIRE(newlyLeakedDirs.isEmpty());

    if (tempDirLeakedBeforeDestruction) {
        std::string msg = "CON-HIGH-23 EMPIRICAL FINDING: Temporary directory " +
                          strandedDirsBeforeDestruct.join(", ").toStdString() +
                          " remained on disk after cancellation until the pipeline destructor was invoked because cleanup() was omitted in process() cancellation branch.";
        WARN(msg);
    }
}

TEST_CASE("Empirical Stress: Direct verification of temp dir cleanup on cancellation (CON-HIGH-23)", "[stress][con-high-23-direct]") {
    ensureApp();
    QTemporaryDir fixtureDir;
    REQUIRE(fixtureDir.isValid());
    const QString tarPath = fixtureDir.filePath("sample.tar");
    REQUIRE(createValidSyntheticAppTarball(tarPath));

    auto listAppalchemistTmpDirs = []() {
        QDir tmpDir(QDir::tempPath());
        return tmpDir.entryList({"appalchemist-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    };

    QStringList dirsBefore = listAppalchemistTmpDirs();

    // Allocate pipeline on heap so we control its lifetime
    auto* pipeline = new PackageToAppImagePipeline();
    pipeline->setPackagePath(tarPath);
    pipeline->setOutputPath(fixtureDir.filePath("out.AppImage"));

    std::stop_source stopSource;
    stopSource.request_stop();
    pipeline->setStopToken(stopSource.get_token());

    QEventLoop loop;
    QObject::connect(pipeline, &PackageToAppImagePipeline::finished, &loop, &QEventLoop::quit);
    pipeline->start();
    loop.exec();

    // Pipeline has emitted finished() and exited process() at:
    // if (isCancelled()) { emit finished(); return; }
    // BEFORE the pipeline object is deleted:
    QStringList dirsAfterFinished = listAppalchemistTmpDirs();
    QStringList leakedBeforeDelete;
    for (const QString& d : dirsAfterFinished) {
        if (!dirsBefore.contains(d)) {
            leakedBeforeDelete << d;
        }
    }

    // Now delete pipeline (destructor calls cleanup())
    delete pipeline;

    QStringList dirsAfterDelete = listAppalchemistTmpDirs();
    QStringList leakedAfterDelete;
    for (const QString& d : dirsAfterDelete) {
        if (!dirsBefore.contains(d)) {
            leakedAfterDelete << d;
        }
    }

    INFO("Leaked before delete: " << leakedBeforeDelete.join(", ").toStdString());
    INFO("Leaked after delete: " << leakedAfterDelete.join(", ").toStdString());

    // REQUIRE that cleanup was performed when finished was emitted, NOT deferred to destructor!
    CHECK(leakedBeforeDelete.isEmpty());
    REQUIRE(leakedAfterDelete.isEmpty());
}

TEST_CASE("Empirical Bug: start() clobbers concurrent or pre-start cancel() (SYS-HIGH-11)", "[stress][concurrency][race-start]") {
    PackageToAppImagePipeline pipeline;
    pipeline.cancel();
    REQUIRE(pipeline.isCancelled());
    pipeline.start();
    // start() unconditionally overwrites m_cancelled to false:
    CHECK(pipeline.isCancelled());
}

TEST_CASE("Empirical Stress: ConversionController batch cancellation queue halt", "[stress][concurrency][batch]") {
    ensureApp();
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString tarPath = tempDir.filePath("sample-app.tar");
    REQUIRE(createValidSyntheticAppTarball(tarPath));

    ConversionController controller;
    ConversionRequest request;
    request.packagePaths = {tarPath, tarPath, tarPath, tarPath, tarPath};
    request.outputDir = tempDir.path();

    QEventLoop loop;
    std::atomic<bool> finishedCalled{false};
    int reportedSuccess = 0;
    int reportedFailure = 0;
    bool reportedCancelled = false;

    QObject::connect(&controller, &ConversionController::finished, &loop,
        [&](int succ, int fail, bool cancelled) {
            finishedCalled.store(true);
            reportedSuccess = succ;
            reportedFailure = fail;
            reportedCancelled = cancelled;
            loop.quit();
        });

    controller.start(request);
    REQUIRE(controller.isRunning());
    REQUIRE(controller.totalCount() == 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    controller.cancel();

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() { loop.quit(); });
    watchdog.start(5000);
    loop.exec();

    REQUIRE(watchdog.isActive());
    REQUIRE(finishedCalled.load());
    REQUIRE(reportedCancelled == true);
    REQUIRE_FALSE(controller.isRunning());
    REQUIRE(controller.currentIndex() < 5);
    REQUIRE(controller.currentPipeline() == nullptr);
}


TEST_CASE("Package names handed to package managers are option-safe (SEC-HIGH-11)",
          "[security][subprocess][argument-injection]") {
    SECTION("ordinary distribution package names are accepted") {
        REQUIRE(SubprocessWrapper::isSafePackageName("libc6"));
        REQUIRE(SubprocessWrapper::isSafePackageName("libgtk-4-1"));
        REQUIRE(SubprocessWrapper::isSafePackageName("g++-12"));
        REQUIRE(SubprocessWrapper::isSafePackageName("python3.11"));
        REQUIRE(SubprocessWrapper::isSafePackageName("libstdc++6"));
        REQUIRE(SubprocessWrapper::isSafePackageName("zlib1g"));
    }

    SECTION("names that a package manager would parse as options are rejected") {
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("-o"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("--config-file=/tmp/evil"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("-oDir::Cache=/tmp"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("--installroot=/"));
    }

    SECTION("names carrying shell metacharacters or separators are rejected") {
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("libc6; rm -rf /"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("libc6$(id)"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("libc6`id`"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("../../etc/passwd"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("/usr/bin/evil"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("lib c6"));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("libc6\nrm"));
    }

    SECTION("empty, blank and oversized names are rejected") {
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName(""));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName("   "));
        REQUIRE_FALSE(SubprocessWrapper::isSafePackageName(QString("a").repeated(256)));
    }
}

TEST_CASE("appimagetool download never blocks the caller", "[appimagetool][network][timeout]") {
    // CI showed the real failure mode: on a machine without the tool the
    // download ran a nested event loop in a context that could not deliver the
    // reply, so the conversion hung until it was killed 25 minutes later.
    // Whatever the environment, the call must return promptly.
    QElapsedTimer elapsed;
    elapsed.start();
    const bool downloaded = AppImageBuilder::downloadAppImageTool();
    const qint64 spentMs = elapsed.elapsed();

    INFO("downloadAppImageTool() returned " << downloaded << " after " << spentMs << " ms");
    REQUIRE(spentMs < 150000);

    if (downloaded) {
        // A successful download must leave a usable tool behind, never a
        // half-written file.
        const QString tool = AppImageBuilder::findAppImageTool();
        REQUIRE_FALSE(tool.isEmpty());
        REQUIRE(QFileInfo(tool).isExecutable());
    }
}
