#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashpackage.hpp>
#include <components/files/conversion.hpp>
#include <components/platform/process.hpp>

#include "zipreader.hpp"

namespace
{
    /// A folder of the test's own, empty at the start and gone at the end.
    class CrashPackageTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            mFolder = std::filesystem::temp_directory_path()
                / ("openmw-crashpackage-" + std::to_string(Platform::Process::currentId()));
            std::filesystem::remove_all(mFolder);
            std::filesystem::create_directories(mFolder);
        }

        void TearDown() override { std::filesystem::remove_all(mFolder); }

        std::filesystem::path write(std::string_view name, std::string_view content) const
        {
            const std::filesystem::path path = mFolder / Files::pathFromUnicodeString(name);
            std::ofstream(path, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
            return path;
        }

        /// Every name in `mFolder` and the folders under it, sorted.
        std::vector<std::filesystem::path> contents() const
        {
            std::vector<std::filesystem::path> found;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(mFolder))
                found.push_back(entry.path().lexically_relative(mFolder));
            std::sort(found.begin(), found.end());
            return found;
        }

        std::filesystem::path mFolder;
    };

    std::tm localOf(int year, int month, int day, int hour, int minute, int second)
    {
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        return local;
    }

    /// **Every file comes back as it went, in order, under its name**, read by a reader of its own
    /// that checks each header against the other: the CRC is the standard check value of
    /// "123456789", an empty file is an entry of nought bytes, one past three of the writer's reads
    /// comes back whole, and only a name outside ASCII is flagged UTF-8.
    TEST_F(CrashPackageTest, everyFileComesBackAsItWent)
    {
        std::string dump(200000, '\0');
        std::uint32_t state = 12345;
        for (std::size_t i = 0; i < dump.size(); i += 3)
        {
            state = state * 1664525u + 1013904223u;
            dump[i] = static_cast<char>(state >> 24);
        }

        const std::vector<Crash::PackageFile> files{
            { "openmw.log", write("check.txt", "123456789") },
            { "empty.dmp", write("empty.dmp", "") },
            { "big.dmp", write("big.dmp", dump) },
            { "журнал.log", write("journal.txt", "Crash: SIGSEGV at 0x10 in thread 7\n") },
        };
        const std::filesystem::path zip = mFolder / "package.zip";
        ASSERT_EQ(Crash::writePackage(zip, files, localOf(2026, 9, 27, 14, 30, 13)), std::nullopt);

        std::vector<CrashTests::ZipEntry> entries;
        ASSERT_EQ(CrashTests::readZip(zip, entries), std::nullopt);
        ASSERT_EQ(entries.size(), 4u);

        EXPECT_EQ(entries[0].mName, "openmw.log");
        EXPECT_EQ(entries[0].mContent, "123456789");
        EXPECT_EQ(entries[0].mCrc, 0xCBF43926u);
        EXPECT_EQ(entries[1].mName, "empty.dmp");
        EXPECT_EQ(entries[1].mContent, "");
        EXPECT_EQ(entries[1].mCrc, 0u);
        EXPECT_EQ(entries[2].mName, "big.dmp");
        EXPECT_EQ(entries[2].mContent, dump);
        EXPECT_EQ(entries[3].mName, "журнал.log");
        EXPECT_EQ(entries[3].mContent, "Crash: SIGSEGV at 0x10 in thread 7\n");

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            EXPECT_EQ(entries[i].mMethod, 8) << entries[i].mName;
            EXPECT_EQ(entries[i].mFlags, i == 3 ? 0x800 : 0) << entries[i].mName;
        }

        // Nothing but the sources and the package: the part it was written as is gone.
        EXPECT_EQ(contents().size(), 5u);
    }

    /// **The stamp, as MS-DOS keeps it**: the date in the high half, (year - 1980) << 9 | month << 5
    /// | day, the time in the low half, hour << 11 | minute << 5 | seconds / 2. 2026-09-27 is
    /// 46 << 9 | 9 << 5 | 27 = 23867 and 14:30:13 is 14 << 11 | 30 << 5 | 6 = 29638. A year before
    /// 1980 is 1980-01-01 00:00:00, 1 << 5 | 1 = 33, and one past 2107 is 2107-12-31 23:59:58,
    /// 127 << 9 | 12 << 5 | 31 = 65439 and 23 << 11 | 59 << 5 | 29 = 49021.
    TEST_F(CrashPackageTest, eachEntryIsStampedInMsDosLocalTime)
    {
        struct Case
        {
            std::tm mLocal;
            std::uint32_t mDosTime;
        };
        const std::vector<Case> cases{
            { localOf(2026, 9, 27, 14, 30, 13), 23867u << 16 | 29638u },
            { localOf(1979, 12, 31, 23, 59, 59), 33u << 16 },
            { localOf(2108, 1, 1, 0, 0, 0), 65439u << 16 | 49021u },
        };
        const std::vector<Crash::PackageFile> files{ { "a.log", write("a.log", "a") } };
        for (const Case& one : cases)
        {
            const std::filesystem::path zip = mFolder / "stamped.zip";
            ASSERT_EQ(Crash::writePackage(zip, files, one.mLocal), std::nullopt);
            std::vector<CrashTests::ZipEntry> entries;
            ASSERT_EQ(CrashTests::readZip(zip, entries), std::nullopt);
            ASSERT_EQ(entries.size(), 1u);
            EXPECT_EQ(entries[0].mDosTime, one.mDosTime) << one.mLocal.tm_year + 1900;
        }
    }

    /// **Every way a package fails says why, and leaves nothing behind**: a source that is gone, a
    /// source that is a folder, which has no size to read to, a destination whose
    /// folder is gone, and a destination a folder already holds, which fails at the rename, after
    /// the whole archive was written. Neither the zip nor the part it was written as is left.
    TEST_F(CrashPackageTest, everyFailureSaysWhyAndLeavesNothing)
    {
        const std::filesystem::path log = write("openmw.log", "log");
        const std::filesystem::path gone = mFolder / "gone.dmp";
        const std::filesystem::path folder = mFolder / "folder.dmp";
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(mFolder / "taken.zip");

        struct Case
        {
            std::string_view mName;
            std::vector<Crash::PackageFile> mFiles;
            std::filesystem::path mZip;
            std::string mSays;
        };
        const std::vector<Case> cases{
            { "a source that is gone", { { "openmw.log", log }, { "gone.dmp", gone } }, mFolder / "a.zip",
                Files::pathToUnicodeString(gone) + " could not be read" },
            { "a source that is a folder", { { "openmw.log", log }, { "folder.dmp", folder } }, mFolder / "b.zip",
                Files::pathToUnicodeString(folder) + " could not be read" },
            { "a destination whose folder is gone", { { "openmw.log", log } }, mFolder / "absent" / "c.zip",
                " could not be made" },
            { "a destination a folder holds", { { "openmw.log", log } }, mFolder / "taken.zip",
                Files::pathToUnicodeString(mFolder / "taken.zip") + " could not be put in place" },
        };

        const std::vector<std::filesystem::path> before = contents();
        for (const Case& one : cases)
        {
            const std::optional<std::string> why
                = Crash::writePackage(one.mZip, one.mFiles, localOf(2026, 9, 27, 14, 30, 13));
            ASSERT_TRUE(why.has_value()) << one.mName;
            EXPECT_NE(why->find(one.mSays), std::string::npos) << one.mName << ": " << *why;
            EXPECT_EQ(contents(), before) << one.mName;
        }
    }

    /// The application and the local time name a package, and a second package of the same second
    /// takes the next number rather than the first one's place.
    TEST_F(CrashPackageTest, aPackageNeverTakesAnotherOnesName)
    {
        const std::tm local = localOf(2026, 9, 7, 4, 3, 2);
        const std::filesystem::path first = Crash::freePackagePath(mFolder, "OpenMW", local);
        EXPECT_EQ(first, mFolder / "OpenMW-crash-2026-09-07-040302.zip");

        write("OpenMW-crash-2026-09-07-040302.zip", "");
        EXPECT_EQ(Crash::freePackagePath(mFolder, "OpenMW", local), mFolder / "OpenMW-crash-2026-09-07-040302-2.zip");

        write("OpenMW-crash-2026-09-07-040302-2.zip", "");
        EXPECT_EQ(Crash::freePackagePath(mFolder, "OpenMW", local), mFolder / "OpenMW-crash-2026-09-07-040302-3.zip");
    }

    /// A space, a character outside ASCII and a '#' are escaped, as UTF-8 bytes; the separators, a
    /// drive's colon and the unreserved characters are kept.
    TEST(CrashPackageUrlTest, aFolderIsAFileUrl)
    {
#if defined(_WIN32)
        EXPECT_EQ(Crash::folderUrl(Files::pathFromUnicodeString("C:\\Users\\x\\My Games\\ü#1_a-b.c~")),
            "file:///C:/Users/x/My%20Games/%C3%BC%231_a-b.c~");
#else
        EXPECT_EQ(Crash::folderUrl(Files::pathFromUnicodeString("/home/x/My Games/ü#1_a-b.c~")),
            "file:///home/x/My%20Games/%C3%BC%231_a-b.c~");
#endif
    }

    /// **A session is packaged from what is on disk**: the log first, then every dump that is there,
    /// in the order the session wrote them, under the application and the time in `folder`. A dump
    /// the disk no longer has is named, and the package goes without it.
    TEST_F(CrashPackageTest, aSessionIsPackagedFromWhatIsOnDisk)
    {
        const std::filesystem::path reports = mFolder / "crashes";
        std::filesystem::create_directories(reports);
        const std::filesystem::path log = write("openmw.log", "Crash: SIGSEGV at 0x10 in thread 7\n");
        const std::filesystem::path first = write("first.dmp", "MDMP first");
        const std::filesystem::path gone = mFolder / "gone.dmp";
        const std::filesystem::path last = write("last.dmp", "MDMP last");
        const std::vector<std::filesystem::path> dumps{ first, gone, last };

        const Crash::SessionPackage package
            = Crash::writeSessionPackage(reports, "OpenMW", log, dumps, localOf(2026, 9, 27, 14, 30, 13));
        EXPECT_EQ(package.mFailure, "");
        EXPECT_EQ(package.mMissing, std::vector<std::filesystem::path>{ gone });
        ASSERT_EQ(package.mZip, reports / "OpenMW-crash-2026-09-27-143013.zip");

        std::vector<CrashTests::ZipEntry> entries;
        ASSERT_EQ(CrashTests::readZip(package.mZip, entries), std::nullopt);
        ASSERT_EQ(entries.size(), 3u);
        EXPECT_EQ(entries[0].mName, "openmw.log");
        EXPECT_EQ(entries[0].mContent, "Crash: SIGSEGV at 0x10 in thread 7\n");
        EXPECT_EQ(entries[1].mName, "first.dmp");
        EXPECT_EQ(entries[1].mContent, "MDMP first");
        EXPECT_EQ(entries[2].mName, "last.dmp");
        EXPECT_EQ(entries[2].mContent, "MDMP last");
    }

    /// **The log is what the game had**: none at all, from a crash before the log was set up, is no
    /// missing file and packages the dumps alone, and one named but gone is a missing one. A
    /// session with no dump reported nothing and is not packaged, and one with nothing on disk
    /// says so. Neither leaves a file.
    TEST_F(CrashPackageTest, aSessionPackagesOnlyWhatItReported)
    {
        const std::filesystem::path dump = write("only.dmp", "MDMP");
        const std::filesystem::path log = write("openmw.log", "log");
        const std::filesystem::path goneLog = mFolder / "gone.log";
        const std::filesystem::path goneDump = mFolder / "gone.dmp";
        const std::tm local = localOf(2026, 9, 27, 14, 30, 13);

        const Crash::SessionPackage noLog = Crash::writeSessionPackage(mFolder, "OpenMW", {}, { &dump, 1 }, local);
        EXPECT_EQ(noLog.mFailure, "");
        EXPECT_TRUE(noLog.mMissing.empty());
        std::vector<CrashTests::ZipEntry> entries;
        ASSERT_EQ(CrashTests::readZip(noLog.mZip, entries), std::nullopt);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].mName, "only.dmp");
        std::filesystem::remove(noLog.mZip);

        const Crash::SessionPackage logGone
            = Crash::writeSessionPackage(mFolder, "OpenMW", goneLog, { &dump, 1 }, local);
        EXPECT_EQ(logGone.mMissing, std::vector<std::filesystem::path>{ goneLog });
        ASSERT_EQ(CrashTests::readZip(logGone.mZip, entries), std::nullopt);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].mName, "only.dmp");
        std::filesystem::remove(logGone.mZip);

        const std::vector<std::filesystem::path> before = contents();

        const Crash::SessionPackage nothingReported = Crash::writeSessionPackage(mFolder, "OpenMW", log, {}, local);
        EXPECT_EQ(nothingReported.mZip, std::filesystem::path());
        EXPECT_EQ(nothingReported.mFailure, "");
        EXPECT_TRUE(nothingReported.mMissing.empty());

        const Crash::SessionPackage nothingOnDisk
            = Crash::writeSessionPackage(mFolder, "OpenMW", goneLog, { &goneDump, 1 }, local);
        EXPECT_EQ(nothingOnDisk.mZip, std::filesystem::path());
        EXPECT_EQ(nothingOnDisk.mFailure, "neither the log nor a dump is on disk");
        EXPECT_EQ(nothingOnDisk.mMissing, (std::vector<std::filesystem::path>{ goneLog, goneDump }));

        const Crash::SessionPackage refused
            = Crash::writeSessionPackage(mFolder / "absent", "OpenMW", log, { &dump, 1 }, local);
        EXPECT_EQ(refused.mZip, std::filesystem::path());
        EXPECT_NE(refused.mFailure.find(" could not be made"), std::string::npos) << refused.mFailure;

        EXPECT_EQ(contents(), before);
    }

    /// A folder given relative to where the monitor runs, as `OPENMW_CRASH_REPORTS` may give it,
    /// comes back absolute and without a "." in it, since the dialog shows it to a player who is
    /// somewhere else.
    TEST_F(CrashPackageTest, aRelativeFolderGivesAnAbsolutePackage)
    {
        const std::filesystem::path dump = write("only.dmp", "MDMP");
        const std::filesystem::path was = std::filesystem::current_path();
        std::filesystem::current_path(mFolder);
        const Crash::SessionPackage package
            = Crash::writeSessionPackage(".", "OpenMW", {}, { &dump, 1 }, localOf(2026, 9, 27, 14, 30, 13));
        std::filesystem::current_path(was);

        EXPECT_EQ(package.mFailure, "");
        EXPECT_TRUE(package.mZip.is_absolute()) << package.mZip;
        EXPECT_EQ(package.mZip, package.mZip.lexically_normal());
        EXPECT_EQ(package.mZip.filename(), "OpenMW-crash-2026-09-27-143013.zip");
        EXPECT_TRUE(std::filesystem::equivalent(package.mZip.parent_path(), mFolder)) << package.mZip;
    }
}
