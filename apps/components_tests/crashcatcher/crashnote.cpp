#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <components/crashcatcher/crashnote.hpp>

namespace
{
    std::array<Crash::NoteCopy, Crash::sNoteThreads> notes;

    std::span<const Crash::NoteCopy> readAll()
    {
        return std::span(notes).first(Crash::readNotes(notes));
    }

    const Crash::NoteCopy* findThread(std::span<const Crash::NoteCopy> read, std::uint64_t thread)
    {
        const auto found
            = std::find_if(read.begin(), read.end(), [&](const Crash::NoteCopy& one) { return one.mThread == thread; });
        return found == read.end() ? nullptr : &*found;
    }

    /// A note reads back as it was written, subject in quotes, whole, and first among the notes
    /// the thread that wrote it reads — which is how a crash handler, running on the thread that
    /// crashed, finds that thread's. A note with no subject is its words alone, and a shorter note
    /// written over a longer one leaves nothing of the longer behind.
    TEST(CrashNoteTest, aNoteReadsBackAsWrittenAndItsThreadsComesFirst)
    {
        Crash::note("describing the texture", "textures/tx_a_rock.dds");
        std::span<const Crash::NoteCopy> read = readAll();
        ASSERT_FALSE(read.empty());
        EXPECT_EQ(std::string_view(read[0].mText), "describing the texture \"textures/tx_a_rock.dds\"");
        EXPECT_EQ(read[0].mThread, Crash::currentThread());
        EXPECT_TRUE(read[0].mWhole);

        Crash::note("loading");
        read = readAll();
        ASSERT_FALSE(read.empty());
        EXPECT_EQ(std::string_view(read[0].mText), "loading");
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
        const std::span<const Crash::NoteCopy> both = readAll();
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
        const std::span<const Crash::NoteCopy> full = readAll();
        EXPECT_EQ(full.size(), Crash::sNoteThreads);
        EXPECT_EQ(std::string_view(full[0].mText), "filling the table");

        read.count_down();
        for (std::thread& thread : threads)
            thread.join();

        const std::span<const Crash::NoteCopy> left = readAll();
        ASSERT_EQ(left.size(), 1u) << "an ended thread kept its slot";
        EXPECT_EQ(std::string_view(left[0].mText), "filling the table");
    }

    /// A note longer than its slot is cut to it: one word, the two characters opening the quote
    /// and 252 of the subject's 300 make 255, the slot's 256 less its terminator, and the closing
    /// quote is what goes.
    TEST(CrashNoteTest, aNoteLongerThanItsSlotIsCutToIt)
    {
        Crash::note("a", std::string(300, 'x'));
        const std::span<const Crash::NoteCopy> read = readAll();
        ASSERT_FALSE(read.empty());
        EXPECT_EQ(std::string_view(read[0].mText), "a \"" + std::string(252, 'x'));
        EXPECT_EQ(Crash::sNoteCapacity, 256u);
    }
}
