#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashsummary.hpp>

namespace
{
    Crash::NoteCopy noteOf(std::uint64_t thread, const char* text, bool whole = true)
    {
        Crash::NoteCopy note{};
        std::strncpy(note.mText, text, sizeof(note.mText) - 1);
        note.mThread = thread;
        note.mWhole = whole;
        return note;
    }

    /// A fault, word for word: the exception and the thread that raised it, where it stopped,
    /// what its stack points into, every thread's note with the crashed one marked and a half
    /// written one said to be, what the game said of itself, and the dump.
    TEST(CrashSummaryTest, aFaultIsSummarisedLineByLine)
    {
        Crash::CrashFacts facts;
        facts.mException = "EXCEPTION_ACCESS_VIOLATION reading 0x12204cfe000";
        facts.mThread = 12632;
        facts.mWhere = "VCRUNTIME140.dll+0x1dd06";
        facts.mStack = { "openmw.exe+0x112a9a7", "openmw.exe+0x112b1f7" };
        facts.mNotes.mCount = 2;
        facts.mNotes.mNotes[0] = noteOf(12632, "staging the texture \"textures/tx_a.dds\"");
        facts.mNotes.mNotes[1] = noteOf(8812, "describing the texture \"textures/tx_b", false);
        facts.mAnnotations = { { "product", "OpenMW" }, { "renderer", "ray tracing" } };
        facts.mDump = "C:/Users/x/crashes/reports/1.dmp";

        std::vector<std::string> lines;
        Crash::summarise(facts, lines);
        const std::vector<std::string> expected{
            "Crash: EXCEPTION_ACCESS_VIOLATION reading 0x12204cfe000 in thread 12632",
            "Crash: at VCRUNTIME140.dll+0x1dd06",
            "Crash: return addresses on its stack: openmw.exe+0x112a9a7 openmw.exe+0x112b1f7",
            "Crash: note of thread 12632, which crashed: staging the texture \"textures/tx_a.dds\"",
            "Crash: note of thread 8812: describing the texture \"textures/tx_b (half written)",
            "Crash: product: OpenMW",
            "Crash: renderer: ray tracing",
            "Crash: dump C:/Users/x/crashes/reports/1.dmp",
        };
        EXPECT_EQ(lines, expected);
    }

    /// A reason given by the code that asked leads, and the exception it was raised as follows
    /// it: `std::terminate`'s abort names nothing of its cause. A report the game asked for marks
    /// the thread that asked. A hang marks none, since the thread that took the request is any.
    /// With nothing known, each says so rather than leaving a line out.
    TEST(CrashSummaryTest, eachKindHeadsItsLinesAndMarksItsThread)
    {
        Crash::CrashFacts terminate;
        std::strcpy(terminate.mNotes.mReason, "std::terminate on an uncaught exception: bad");
        terminate.mException = "SIGABRT";
        terminate.mThread = 7;
        std::vector<std::string> lines;
        Crash::summarise(terminate, lines);
        EXPECT_EQ(lines[0], "Crash: std::terminate on an uncaught exception: bad in thread 7");
        EXPECT_EQ(lines[1], "Crash: raised as SIGABRT");
        EXPECT_EQ(lines[2], "Crash: no thread noted anything");
        EXPECT_EQ(lines[3], "Crash: no dump was written");
        EXPECT_EQ(lines.size(), 4u);

        Crash::CrashFacts report;
        report.mNotes.mKind = Crash::ReportKind::Report;
        std::strcpy(report.mNotes.mReason, "a contract broken");
        report.mThread = 7;
        report.mNotes.mCount = 1;
        report.mNotes.mNotes[0] = noteOf(7, "placing");
        lines.clear();
        Crash::summarise(report, lines);
        EXPECT_EQ(lines[0], "Report: a contract broken in thread 7");
        EXPECT_EQ(lines[1], "Report: note of thread 7, which asked: placing");

        Crash::CrashFacts hang;
        hang.mNotes.mKind = Crash::ReportKind::Hang;
        std::strcpy(hang.mNotes.mReason, "no frame for 20 seconds");
        hang.mThread = 7;
        hang.mNotes.mCount = 1;
        hang.mNotes.mNotes[0] = noteOf(7, "compiling");
        lines.clear();
        Crash::summarise(hang, lines);
        EXPECT_EQ(lines[0], "Hang: no frame for 20 seconds in thread 7");
        EXPECT_EQ(lines[1], "Hang: note of thread 7: compiling");

        Crash::CrashFacts nothing;
        lines.clear();
        Crash::summarise(nothing, lines);
        EXPECT_EQ(lines[0], "Crash: no exception was recorded");
    }
}
