#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <latch>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashnote.hpp>

namespace
{
    /// The table as the monitor reads it, with this thread first: every test reads it while the
    /// threads that wrote it wait, as the monitor reads a process that stands still.
    Crash::NotesRead readAll()
    {
        Crash::NotesRead read;
        Crash::readNotes(Crash::noteTable(), Crash::currentThread(), read);
        return read;
    }

    const Crash::NoteCopy* findThread(const Crash::NotesRead& read, std::uint64_t thread)
    {
        const auto end = read.mNotes + read.mCount;
        const auto found
            = std::find_if(read.mNotes, end, [&](const Crash::NoteCopy& one) { return one.mThread == thread; });
        return found == end ? nullptr : found;
    }

    /// A note reads back as it was written, subject in quotes, whole, and first where its thread is
    /// the one named. A note with no subject is its words alone, and a shorter note written over a
    /// longer one leaves nothing of the longer behind.
    TEST(CrashNoteTest, aNoteReadsBackAsWrittenAndItsThreadsComesFirst)
    {
        Crash::note("describing the texture", "textures/tx_a_rock.dds");
        Crash::NotesRead read = readAll();
        ASSERT_GT(read.mCount, 0u);
        EXPECT_EQ(std::string_view(read.mNotes[0].mText), "describing the texture \"textures/tx_a_rock.dds\"");
        EXPECT_EQ(read.mNotes[0].mThread, Crash::currentThread());
        EXPECT_TRUE(read.mNotes[0].mWhole);

        Crash::note("loading");
        read = readAll();
        ASSERT_GT(read.mCount, 0u);
        EXPECT_EQ(std::string_view(read.mNotes[0].mText), "loading");
    }

    /// Two threads' notes stand side by side, each under the system's id of the thread that wrote
    /// it, and a thread's note goes with the thread.
    TEST(CrashNoteTest, eachThreadKeepsItsOwnNoteUntilItEnds)
    {
        Crash::note("uploading");

        std::latch noted(1);
        std::latch read(1);
        std::uint64_t other = 0;
        std::thread worker([&] {
            other = Crash::currentThread();
            Crash::note("staging the texture", "textures/tx_b.dds");
            noted.count_down();
            read.wait();
        });

        noted.wait();
        const Crash::NotesRead both = readAll();
        const Crash::NoteCopy* const mine = findThread(both, Crash::currentThread());
        const Crash::NoteCopy* const theirs = findThread(both, other);
        ASSERT_NE(mine, nullptr);
        ASSERT_NE(theirs, nullptr);
        EXPECT_NE(other, Crash::currentThread());
        EXPECT_EQ(std::string_view(mine->mText), "uploading");
        EXPECT_EQ(std::string_view(theirs->mText), "staging the texture \"textures/tx_b.dds\"");

        read.count_down();
        worker.join();
        EXPECT_EQ(findThread(readAll(), other), nullptr) << "an ended thread's note outlived it";
    }

    /// The table holds one note a thread for as many threads as it has slots, and a thread past
    /// them notes nothing rather than over another's. This thread holds one slot, so of as many
    /// threads again as there are slots, all but one hold the rest; as they end, the slots come
    /// back.
    TEST(CrashNoteTest, aThreadPastTheTableNotesNothingAndEndedThreadsGiveTheirSlotsBack)
    {
        Crash::note("filling the table");

        std::latch noted(Crash::sNoteThreads);
        std::latch read(1);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < Crash::sNoteThreads; ++i)
            threads.emplace_back([&, i] {
                Crash::note("thread", std::to_string(i));
                noted.count_down();
                read.wait();
            });

        noted.wait();
        const Crash::NotesRead full = readAll();
        EXPECT_EQ(full.mCount, Crash::sNoteThreads);
        EXPECT_EQ(std::string_view(full.mNotes[0].mText), "filling the table");

        read.count_down();
        for (std::thread& thread : threads)
            thread.join();

        const Crash::NotesRead left = readAll();
        ASSERT_EQ(left.mCount, 1u) << "an ended thread kept its slot";
        EXPECT_EQ(std::string_view(left.mNotes[0].mText), "filling the table");
    }

    /// A note longer than its slot is cut to it: one word, the two characters opening the quote
    /// and 252 of the subject's 300 make 255, the slot's 256 less its terminator, and the closing
    /// quote is what goes.
    TEST(CrashNoteTest, aNoteLongerThanItsSlotIsCutToIt)
    {
        Crash::note("a", std::string(300, 'x'));
        const Crash::NotesRead read = readAll();
        ASSERT_GT(read.mCount, 0u);
        EXPECT_EQ(std::string_view(read.mNotes[0].mText), "a \"" + std::string(252, 'x'));
        EXPECT_EQ(Crash::sNoteCapacity, 256u);
    }

    /// **One report at a time, and the one before it back afterwards.** A request that finds a
    /// report in progress changes nothing — the hang request that landed inside a report's window
    /// wrote over its kind and its reason — and a report that ends puts back what it found, so
    /// the next crash reads as a crash with the reason it had, and not as the report before it.
    TEST(CrashNoteTest, aReportInProgressRefusesAnotherAndPutsBackWhatItFound)
    {
        const auto kindAndReason = [] {
            Crash::NotesRead read;
            Crash::readNotes(Crash::noteTable(), 0, read);
            return std::pair{ read.mKind, std::string(read.mReason) };
        };

        const auto before = kindAndReason();
        EXPECT_FALSE(Crash::isReporting());

        ASSERT_TRUE(Crash::beginReport(Crash::ReportKind::Report, "asked"));
        EXPECT_TRUE(Crash::isReporting());
        EXPECT_EQ(kindAndReason(), std::pair(Crash::ReportKind::Report, std::string("asked")));

        EXPECT_FALSE(Crash::beginReport(Crash::ReportKind::Hang, {})) << "a hang request inside a report";
        EXPECT_EQ(kindAndReason(), std::pair(Crash::ReportKind::Report, std::string("asked")))
            << "the refused request wrote over the report in progress";

        Crash::endReport();
        EXPECT_FALSE(Crash::isReporting());
        EXPECT_EQ(kindAndReason(), before) << "what the report found was not put back";

        ASSERT_TRUE(Crash::beginReport(Crash::ReportKind::Hang, {})) << "the gate did not open again";
        Crash::endReport();
    }

    /// **The table as the monitor reads it**: a copy of its bytes, taken from outside, put in the
    /// order a report wants, with the thread it names first and what the next report is and why.
    /// A note whose thread stopped in the middle of writing it reads as half written, and a copy
    /// of another size is no table, and reads as nothing noted.
    TEST(CrashNoteTest, aCopyOfTheTableReadsAsTheLiveOneWithTheNamedThreadFirst)
    {
        Crash::note("drawing");
        std::uint64_t other = 0;
        std::latch noted(1);
        std::latch copied(1);
        std::thread worker([&] {
            other = Crash::currentThread();
            Crash::note("walking the cell", "Seyda Neen");
            noted.count_down();
            copied.wait();
        });
        noted.wait();

        ASSERT_TRUE(Crash::beginReport(Crash::ReportKind::Report, "a contract broken"));
        const std::span<const std::byte> live = Crash::noteTable();
        const std::vector<std::byte> copy(live.begin(), live.end());
        Crash::endReport();
        copied.count_down();
        worker.join();

        Crash::NotesRead read;
        Crash::readNotes(copy, other, read);
        EXPECT_EQ(read.mKind, Crash::ReportKind::Report);
        EXPECT_EQ(std::string_view(read.mReason), "a contract broken");
        ASSERT_EQ(read.mCount, 2u);
        EXPECT_EQ(read.mNotes[0].mThread, other);
        EXPECT_EQ(std::string_view(read.mNotes[0].mText), "walking the cell \"Seyda Neen\"");
        EXPECT_EQ(std::string_view(read.mNotes[1].mText), "drawing");
        EXPECT_TRUE(read.mNotes[0].mWhole && read.mNotes[1].mWhole);

        // A slot is its thread's id and then its sequence count, which a note in progress leaves
        // odd: one step on from the count the copy holds is the worker stopped halfway.
        std::vector<std::byte> halfway = copy;
        std::vector<std::size_t> slots;
        for (std::size_t at = 0; at + sizeof(other) <= halfway.size(); at += alignof(std::uint64_t))
            if (std::memcmp(halfway.data() + at, &other, sizeof(other)) == 0)
                slots.push_back(at);
        ASSERT_EQ(slots.size(), 1u) << "the worker's id is not in one place of the table";
        std::uint32_t sequence = 0;
        std::memcpy(&sequence, halfway.data() + slots[0] + sizeof(other), sizeof(sequence));
        ++sequence;
        std::memcpy(halfway.data() + slots[0] + sizeof(other), &sequence, sizeof(sequence));

        Crash::readNotes(halfway, other, read);
        ASSERT_EQ(read.mCount, 2u);
        EXPECT_FALSE(read.mNotes[0].mWhole) << "a note stopped halfway read as whole";
        EXPECT_EQ(std::string_view(read.mNotes[0].mText), "walking the cell \"Seyda Neen\"");
        EXPECT_TRUE(read.mNotes[1].mWhole);

        Crash::readNotes(std::span(copy).first(copy.size() - 1), other, read);
        EXPECT_EQ(read.mCount, 0u) << "a table of another size was read";
        EXPECT_EQ(read.mKind, Crash::ReportKind::Crash);

        EXPECT_EQ(readAll().mKind, Crash::ReportKind::Crash) << "the report kind outlived its report";
    }
}
