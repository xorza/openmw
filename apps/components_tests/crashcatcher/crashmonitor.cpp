#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashmonitorarguments.hpp>
#include <components/crashcatcher/crashpage.hpp>
#include <components/platform/process.hpp>

namespace
{
    /// **What the game writes is what its monitor reads**, whatever Crashpad puts around it: every
    /// field back as it went, a path with a space and letters outside ASCII included, and nothing
    /// of the game's left among Crashpad's arguments, which keep their order behind `argv[0]`.
    TEST(CrashMonitorArgumentsTest, whatTheGameWritesIsWhatTheMonitorReads)
    {
        Crash::MonitorArguments written;
        written.mClient = 4242;
        written.mNotes = 0x7ffd12345678;
        written.mNotesSize = 9352;
        written.mLog = std::filesystem::path(u8"C:/Users/Игрок/My Games/OpenMW/openmw.log");
        written.mApplication = "OpenMW";
        written.mDialog = false;

        std::vector<std::string> line{ "openmw", "--database=/home/x/crashes" };
        for (const std::string& argument : written.write())
            line.push_back(argument);
        line.push_back("--initial-client-fd=3");

        std::vector<std::string> handler;
        const Crash::MonitorArguments read = Crash::MonitorArguments::read(line, handler);
        EXPECT_EQ(read.mClient, 4242u);
        EXPECT_EQ(read.mNotes, 0x7ffd12345678u);
        EXPECT_EQ(read.mNotesSize, 9352u);
        EXPECT_EQ(read.mLog, written.mLog);
        EXPECT_EQ(read.mApplication, "OpenMW");
        EXPECT_FALSE(read.mDialog);
        EXPECT_EQ(read.mDatabase, std::filesystem::path("/home/x/crashes"));

        const std::vector<std::string> crashpads{ "openmw", "--database=/home/x/crashes", "--initial-client-fd=3" };
        EXPECT_EQ(handler, crashpads);
        EXPECT_EQ(written.write().front(), Crash::sMonitorSwitch);
    }

    /// A note table stated with a length that does not read as one is no table, and the monitor
    /// reads nothing out of the game rather than whatever lies at the address.
    TEST(CrashMonitorArgumentsTest, aNoteTableWithoutALengthIsNoTable)
    {
        std::vector<std::string> handler;
        const std::vector<std::string> line{ "openmw", "--openmw-notes=0x1000", "--openmw-dialog=1" };
        const Crash::MonitorArguments read = Crash::MonitorArguments::read(line, handler);
        EXPECT_EQ(read.mNotes, 0u);
        EXPECT_EQ(read.mNotesSize, 0u);
        EXPECT_TRUE(read.mDialog);
        EXPECT_EQ(handler, std::vector<std::string>{ "openmw" });
    }

    /// **The page both sides map**, found by the game's id: what one side writes the other reads,
    /// both ways, and a page made fresh starts at nought. An id that made none has none to open.
    TEST(CrashPageTest, theGameAndItsMonitorShareOnePageByTheGamesId)
    {
        // An id no running process has, so the test never meets a real game's page.
        const std::uint64_t id = (std::uint64_t{ 1 } << 40) + Platform::Process::currentId();

        const Crash::SharedPage game = Crash::SharedPage::create(id);
        ASSERT_NE(game.get(), nullptr);
        EXPECT_EQ(game.get()->mFrames, 0u);
        EXPECT_EQ(game.get()->mHangSeconds, 0u);

        std::atomic_ref(game.get()->mFrames).store(5);
        std::atomic_ref(game.get()->mHangSeconds).store(20);

        const Crash::SharedPage monitor = Crash::SharedPage::open(id);
        ASSERT_NE(monitor.get(), nullptr);
        EXPECT_NE(monitor.get(), game.get()) << "one mapping, not two";
        EXPECT_EQ(std::atomic_ref(monitor.get()->mFrames).load(), 5u);
        EXPECT_EQ(std::atomic_ref(monitor.get()->mHangSeconds).load(), 20u);

        std::atomic_ref(monitor.get()->mHangEntry).store(0x1234);
        EXPECT_EQ(std::atomic_ref(game.get()->mHangEntry).load(), 0x1234u);

        EXPECT_EQ(Crash::SharedPage::open(id + 1).get(), nullptr);
    }
}
