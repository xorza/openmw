#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashnote.hpp>
#include <components/crashcatcher/windowscrashsummary.hpp>

namespace
{
    __declspec(noinline) int readAt(const volatile int* at)
    {
        return *at;
    }

    /// Faults with `code`, a read of address 0x10 for an access violation, and summarises the
    /// fault into `log` from the filter, which is where a crash handler stands. No object with a
    /// destructor may share a frame with `__try`, so this takes and makes none.
    bool summariseFault(const wchar_t* log, DWORD code)
    {
        __try
        {
            if (code == EXCEPTION_ACCESS_VIOLATION)
                readAt(reinterpret_cast<const volatile int*>(static_cast<std::uintptr_t>(0x10)));
            else
                RaiseException(code, 0, 0, nullptr);
        }
        __except (Crash::appendCrashSummary(log, *GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }

        return false;
    }

    std::vector<std::string> linesOf(const std::filesystem::path& file)
    {
        std::ifstream stream(file);
        std::vector<std::string> lines;
        for (std::string line; std::getline(stream, line);)
            lines.push_back(line);
        return lines;
    }

    std::string lowered(std::string text)
    {
        for (char& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    /// Where the frames end: at the thread's start in the system, which a walk that stopped early
    /// never reaches.
    bool reachesThreadStart(const std::vector<std::string>& lines)
    {
        for (const std::string& line : lines)
            if (lowered(line).find("   #") != std::string::npos
                && lowered(line).find("kernel32.dll+0x") != std::string::npos)
                return true;
        return false;
    }

    class CrashSummaryTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            mLog = std::filesystem::temp_directory_path()
                / ("openmw-crash-summary-" + std::to_string(GetCurrentProcessId()) + ".log");
            std::ofstream(mLog) << "what the log held\n";
        }

        void TearDown() override { std::filesystem::remove(mLog); }

        std::filesystem::path mLog;
        const std::string mThread = std::to_string(GetCurrentThreadId());
        const std::string mStamp = R"(\[\d\d:\d\d:\d\d\.\d{3} E\] )";
    };

    /// A real access violation, summarised from where a crash handler stands: appended after what
    /// the log held, the operation and the address the fault states, the thread, the frames from
    /// the faulting function in this binary down to the thread's start in the system, and this
    /// thread's note, marked as the one that crashed.
    TEST_F(CrashSummaryTest, anAccessViolationIsSummarisedWithItsFramesAndItsThreadsNote)
    {
        Crash::note("testing the summary", "textures/tx_crash.dds");
        ASSERT_TRUE(summariseFault(mLog.c_str(), EXCEPTION_ACCESS_VIOLATION));

        const std::vector<std::string> lines = linesOf(mLog);
        ASSERT_GE(lines.size(), 4u);
        EXPECT_EQ(lines[0], "what the log held");
        EXPECT_TRUE(std::regex_match(
            lines[1], std::regex(mStamp + "Crash: exception 0xc0000005 reading 0x10 in thread " + mThread)))
            << lines[1];
        EXPECT_TRUE(std::regex_match(lines[2], std::regex(mStamp + R"(  #0  components-tests\.exe\+0x[0-9a-f]+)")))
            << lines[2] << ": the faulting frame is not this binary's";
        EXPECT_TRUE(reachesThreadStart(lines)) << "the frames stopped before the thread's start";

        const std::string noted
            = "Crash: note of thread " + mThread + ", which crashed: testing the summary \"textures/tx_crash.dds\"";
        EXPECT_NE(
            std::find_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.ends_with(noted); }),
            lines.end())
            << "no note of the crashed thread";
    }

    /// An exception that is not an access violation states its code alone, and its first frame
    /// is where the system raised it.
    TEST_F(CrashSummaryTest, anyOtherExceptionIsSummarisedByItsCode)
    {
        ASSERT_TRUE(summariseFault(mLog.c_str(), 0xE0000001));

        const std::vector<std::string> lines = linesOf(mLog);
        ASSERT_GE(lines.size(), 3u);
        EXPECT_TRUE(std::regex_match(lines[1], std::regex(mStamp + "Crash: exception 0xe0000001 in thread " + mThread)))
            << lines[1];
        EXPECT_NE(lowered(lines[2]).find("  #0  kernelbase.dll+0x"), std::string::npos) << lines[2];
        EXPECT_TRUE(reachesThreadStart(lines)) << "the frames stopped before the thread's start";
    }
}
