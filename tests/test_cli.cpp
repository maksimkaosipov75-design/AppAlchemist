#include <catch2/catch_test_macros.hpp>
#include "cli_converter.h"
#include "test_helpers.h"
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#ifndef APPALCHEMIST_CLI_PATH
#define APPALCHEMIST_CLI_PATH "./build/appalchemist-cli"
#endif

namespace {

struct ProcessExecutionResult {
    int exitCode = -1;
    QByteArray stdOut;
    QByteArray stdErr;
};

ProcessExecutionResult runCli(const QStringList& args) {
    QProcess proc;
    proc.start(APPALCHEMIST_CLI_PATH, args);
    proc.waitForFinished(15000);
    return {
        proc.exitCode(),
        proc.readAllStandardOutput(),
        proc.readAllStandardError()
    };
}

} // namespace

TEST_CASE("CLI: Argument parsing and options mapping", "[cli][options]") {
    SECTION("CliOptions struct default initialization and mapping") {
        CliOptions opts;
        REQUIRE_FALSE(opts.json);
        REQUIRE_FALSE(opts.quiet);
        REQUIRE_FALSE(opts.dryRun);
        REQUIRE_FALSE(opts.autoLaunch);
        REQUIRE_FALSE(opts.isBatch);
        REQUIRE(opts.packagePath.isEmpty());
        REQUIRE(opts.batchPaths.isEmpty());
        REQUIRE(opts.outputDir.isEmpty());

        opts.packagePath = "/tmp/sample.deb";
        opts.outputDir = "/tmp/out";
        opts.json = true;
        opts.quiet = true;
        opts.dryRun = true;
        opts.autoLaunch = true;
        opts.isBatch = true;
        opts.batchPaths << "/tmp/1.deb" << "/tmp/2.deb";

        REQUIRE(opts.packagePath == "/tmp/sample.deb");
        REQUIRE(opts.outputDir == "/tmp/out");
        REQUIRE(opts.json);
        REQUIRE(opts.quiet);
        REQUIRE(opts.dryRun);
        REQUIRE(opts.autoLaunch);
        REQUIRE(opts.isBatch);
        REQUIRE(opts.batchPaths.size() == 2);
    }

    SECTION("CLI subprocess --help outputs help description and exits 0") {
        ProcessExecutionResult res = runCli({"--help"});
        REQUIRE(res.exitCode == 0);
        REQUIRE_FALSE(res.stdOut.isEmpty());
        REQUIRE(res.stdOut.contains("AppAlchemist Headless CLI"));
    }

    SECTION("CLI subprocess --version outputs version and exits 0") {
        ProcessExecutionResult res = runCli({"--version"});
        REQUIRE(res.exitCode == 0);
        REQUIRE_FALSE(res.stdOut.isEmpty());
        REQUIRE(res.stdOut.contains("AppAlchemist CLI"));
    }
}

TEST_CASE("CLI: Exit codes on usage and syntax errors", "[cli][exitcodes]") {
    SECTION("Unknown option flag yields exit code 2") {
        ProcessExecutionResult res = runCli({"--nonexistent-flag-xyz"});
        REQUIRE(res.exitCode == 2);
        REQUIRE_FALSE(res.stdErr.isEmpty());
        REQUIRE(res.stdErr.contains("Usage error"));
    }

    SECTION("Zero arguments yields exit code 2") {
        ProcessExecutionResult res = runCli({});
        REQUIRE(res.exitCode == 2);
        REQUIRE_FALSE(res.stdErr.isEmpty());
        REQUIRE(res.stdErr.contains("No package file specified"));
    }

    SECTION("Batch flag with zero file arguments yields exit code 2") {
        ProcessExecutionResult res = runCli({"-b"});
        REQUIRE(res.exitCode == 2);
        REQUIRE_FALSE(res.stdErr.isEmpty());
        REQUIRE(res.stdErr.contains("No package files specified for batch conversion"));
    }
}

TEST_CASE("CLI: Conversion error on non-existent package", "[cli][error]") {
    SECTION("Non-existent package path returns exit code 1") {
        ProcessExecutionResult res = runCli({"/tmp/definitely_nonexistent_package_12345.deb"});
        REQUIRE(res.exitCode == 1);
        REQUIRE_FALSE(res.stdErr.isEmpty());
        REQUIRE(res.stdErr.contains("Package file not found"));
    }
}

TEST_CASE("CLI: NDJSON stream separation in --json mode", "[cli][json][stream]") {
    SECTION("Error on non-existent file formats NDJSON to stdout and logs to stderr") {
        ProcessExecutionResult res = runCli({"--json", "/tmp/definitely_nonexistent_67890.deb"});
        REQUIRE(res.exitCode == 1);

        // Stderr contains plain text error
        REQUIRE(res.stdErr.contains("Package file not found"));

        // Stdout contains valid JSON events separated by newlines
        QByteArrayList lines = res.stdOut.split('\n');
        bool foundComplete = false;
        for (const QByteArray& line : lines) {
            QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseErr);
            REQUIRE(parseErr.error == QJsonParseError::NoError);
            REQUIRE(doc.isObject());
            QJsonObject obj = doc.object();
            if (obj["event"].toString() == "complete") {
                foundComplete = true;
                REQUIRE_FALSE(obj["success"].toBool());
                REQUIRE(obj.contains("error"));
            }
        }
        REQUIRE(foundComplete);
    }

    SECTION("Dry-run on synthetic package emits valid NDJSON progress and complete events to stdout") {
        QTemporaryDir tempDir;
        REQUIRE(tempDir.isValid());
        QString debPath = tempDir.filePath("test_stream.deb");
        REQUIRE(TestHelpers::buildSyntheticDeb(debPath, "test-stream-app", "1.2.3"));

        ProcessExecutionResult res = runCli({"--json", "--dry-run", debPath});
        REQUIRE(res.exitCode == 0);

        QByteArrayList lines = res.stdOut.split('\n');
        bool foundProgress = false;
        bool foundComplete = false;
        for (const QByteArray& line : lines) {
            QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseErr);
            REQUIRE(parseErr.error == QJsonParseError::NoError);
            REQUIRE(doc.isObject());
            QJsonObject obj = doc.object();
            if (obj["event"].toString() == "progress") {
                foundProgress = true;
                REQUIRE(obj.contains("percent"));
            }
            if (obj["event"].toString() == "complete") {
                foundComplete = true;
                REQUIRE(obj["success"].toBool());
                REQUIRE(obj["dry_run"].toBool());
            }
        }
        REQUIRE(foundProgress);
        REQUIRE(foundComplete);
    }
}

TEST_CASE("CLI: Stream suppression in --quiet mode", "[cli][quiet][stream]") {
    SECTION("In quiet mode on error, stdout is completely empty (0 bytes)") {
        ProcessExecutionResult res = runCli({"--quiet", "/tmp/definitely_nonexistent_quiet_000.deb"});
        REQUIRE(res.exitCode == 1);
        REQUIRE(res.stdOut.isEmpty());
        REQUIRE(res.stdOut.size() == 0);
    }

    SECTION("In quiet mode on successful dry-run, stdout is completely empty (0 bytes)") {
        QTemporaryDir tempDir;
        REQUIRE(tempDir.isValid());
        QString debPath = tempDir.filePath("test_quiet.deb");
        REQUIRE(TestHelpers::buildSyntheticDeb(debPath, "test-quiet-app", "1.0.0"));

        ProcessExecutionResult res = runCli({"--quiet", "--dry-run", debPath});
        REQUIRE(res.exitCode == 0);
        REQUIRE(res.stdOut.isEmpty());
        REQUIRE(res.stdOut.size() == 0);
    }
}

TEST_CASE("CLI: Dry-run execution without AppImage output", "[cli][dryrun]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    QString outDir = tempDir.filePath("output");
    QDir().mkpath(outDir);

    QString debPath = tempDir.filePath("test_dryrun.deb");
    REQUIRE(TestHelpers::buildSyntheticDeb(debPath, "test-dryrun-pkg", "2.1.0"));

    ProcessExecutionResult res = runCli({"--dry-run", "-o", outDir, debPath});
    REQUIRE(res.exitCode == 0);

    // Stdout in non-quiet non-json mode emits the intended output path
    REQUIRE_FALSE(res.stdOut.isEmpty());
    REQUIRE(res.stdOut.contains("test_dryrun.AppImage"));

    // Crucial dry-run assertion: NO actual AppImage file was written to disk!
    QDir outDirObj(outDir);
    QStringList appImages = outDirObj.entryList({"*.AppImage"}, QDir::Files);
    REQUIRE(appImages.isEmpty());
}
