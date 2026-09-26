#include "crashnote.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)
#include <components/misc/windows.hpp>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#else
#include <functional>
#include <thread>
#endif

namespace Crash
{
    namespace
    {
        using Thread = std::atomic_ref<std::uint64_t>;
        using Sequence = std::atomic_ref<std::uint32_t>;

        /// One thread's note, written by that thread alone and read by any.
        ///
        /// **A sequence counter and not a lock**, because the reader is the monitor, and a crash
        /// stops the owner anywhere: odd while the owner writes, so a note stopped halfway is known
        /// as one. Plain words reached through `std::atomic_ref`, so the table is trivially
        /// copyable and the monitor can read it as the bytes it took out of the game.
        struct Slot
        {
            alignas(Thread::required_alignment) std::uint64_t mThread;
            alignas(Sequence::required_alignment) std::uint32_t mSequence;
            char mText[sNoteCapacity];
        };

        struct Table
        {
            alignas(Sequence::required_alignment) std::uint32_t mKind;
            char mReason[sNoteCapacity];
            Slot mSlots[sNoteThreads];
        };

        static_assert(std::is_trivially_copyable_v<Table>);

        // A signal handler may touch only what never locks.
        static_assert(Thread::is_always_lock_free);
        static_assert(Sequence::is_always_lock_free);

        Table sTable{};

        /// A thread's hold on its slot, given back as the thread ends, so that threads made and
        /// ended by the hundred do not fill the table with the dead.
        struct Claim
        {
            Slot* mSlot = nullptr;

            ~Claim()
            {
                if (mSlot != nullptr)
                    Thread(mSlot->mThread).store(0, std::memory_order_release);
            }
        };

        Slot* claimSlot(std::uint64_t thread)
        {
            for (Slot& slot : sTable.mSlots)
            {
                std::uint64_t free = 0;
                if (Thread(slot.mThread).compare_exchange_strong(free, thread, std::memory_order_acq_rel))
                    return &slot;
            }

            return nullptr;
        }

        std::size_t append(char (&into)[sNoteCapacity], std::size_t at, std::string_view text)
        {
            const std::size_t length = std::min(text.size(), sNoteCapacity - 1 - at);
            std::copy_n(text.data(), length, into + at);
            return at + length;
        }
    }

    std::uint64_t currentThread()
    {
#if defined(_WIN32)
        return GetCurrentThreadId();
#elif defined(__linux__)
        return static_cast<std::uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
        std::uint64_t thread = 0;
        pthread_threadid_np(nullptr, &thread);
        return thread;
#elif defined(__FreeBSD__)
        return static_cast<std::uint64_t>(pthread_getthreadid_np());
#else
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) | 1;
#endif
    }

    void note(std::string_view what, std::string_view subject)
    {
        // Claimed on the first note and not before, so a thread that never notes holds no slot, and
        // asked again while the table is full, so a thread that met it full notes once one ends.
        thread_local Claim claim;
        if (claim.mSlot == nullptr)
            claim.mSlot = claimSlot(currentThread());
        if (claim.mSlot == nullptr)
            return;

        Slot& slot = *claim.mSlot;
        Sequence(slot.mSequence).fetch_add(1, std::memory_order_acq_rel);
        std::size_t at = append(slot.mText, 0, what);
        if (!subject.empty())
        {
            at = append(slot.mText, at, " \"");
            at = append(slot.mText, at, subject);
            at = append(slot.mText, at, "\"");
        }
        slot.mText[at] = '\0';
        Sequence(slot.mSequence).fetch_add(1, std::memory_order_release);
    }

    void setReport(ReportKind kind, std::string_view reason)
    {
        sTable.mReason[append(sTable.mReason, 0, reason)] = '\0';
        Sequence(sTable.mKind).store(static_cast<std::uint32_t>(kind), std::memory_order_release);
    }

    std::span<const std::byte> noteTable()
    {
        return std::as_bytes(std::span(&sTable, 1));
    }

    void readNotes(std::span<const std::byte> table, std::uint64_t first, NotesRead& into)
    {
        // Bytes out of another process, which a crash may have left any shape: a table of another
        // size is no table, and a kind past the known ones is a crash's.
        into = NotesRead{};
        if (table.size() != sizeof(Table))
            return;

        Table copy;
        std::memcpy(&copy, table.data(), sizeof(Table));
        if (copy.mKind <= static_cast<std::uint32_t>(ReportKind::Report))
            into.mKind = static_cast<ReportKind>(copy.mKind);
        std::memcpy(into.mReason, copy.mReason, sNoteCapacity);
        into.mReason[sNoteCapacity - 1] = '\0';

        // The process stands still while its table is copied, so a note whose count is odd is one
        // its thread stopped in the middle of.
        const auto take = [&](const Slot& slot) {
            NoteCopy& note = into.mNotes[into.mCount++];
            note.mThread = slot.mThread;
            std::memcpy(note.mText, slot.mText, sNoteCapacity);
            note.mText[sNoteCapacity - 1] = '\0';
            note.mWhole = slot.mSequence % 2 == 0;
        };

        if (first != 0)
            for (const Slot& slot : copy.mSlots)
                if (slot.mThread == first)
                    take(slot);

        for (const Slot& slot : copy.mSlots)
            if (slot.mThread != 0 && slot.mThread != first)
                take(slot);
    }
}
