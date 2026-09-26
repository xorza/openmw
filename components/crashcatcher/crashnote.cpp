#include "crashnote.hpp"

#include <algorithm>
#include <atomic>

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
        /// One thread's note, written by that thread alone and read by any.
        ///
        /// **A sequence counter and not a lock**, because the reader is a crash handler that
        /// cannot wait: odd while the owner writes, so a copy taken across a write is known to be
        /// one. The text is `volatile`, because nothing in the process reads it but a crash, and
        /// a compiler may otherwise drop every write.
        struct Slot
        {
            std::atomic<std::uint64_t> mThread{ 0 };
            std::atomic<std::uint32_t> mSequence{ 0 };
            volatile char mText[sNoteCapacity] = {};
        };

        // A signal handler may touch only what never locks.
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

        Slot sSlots[sNoteThreads];

        /// A thread's hold on its slot, given back as the thread ends, so that threads made and
        /// ended by the hundred do not fill the table with the dead.
        struct Claim
        {
            Slot* mSlot = nullptr;

            ~Claim()
            {
                if (mSlot != nullptr)
                    mSlot->mThread.store(0, std::memory_order_release);
            }
        };

        Slot* claimSlot(std::uint64_t thread)
        {
            for (Slot& slot : sSlots)
            {
                std::uint64_t free = 0;
                if (slot.mThread.compare_exchange_strong(free, thread, std::memory_order_acq_rel))
                    return &slot;
            }

            return nullptr;
        }

        std::size_t append(Slot& slot, std::size_t at, std::string_view text)
        {
            const std::size_t length = std::min(text.size(), sNoteCapacity - 1 - at);
            for (std::size_t i = 0; i < length; ++i)
                slot.mText[at + i] = text[i];

            return at + length;
        }

        void copy(const Slot& slot, std::uint64_t thread, NoteCopy& into)
        {
            const std::uint32_t before = slot.mSequence.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < sNoteCapacity; ++i)
                into.mText[i] = slot.mText[i];
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint32_t after = slot.mSequence.load(std::memory_order_relaxed);

            into.mText[sNoteCapacity - 1] = '\0';
            into.mThread = thread;
            into.mWhole = before == after && before % 2 == 0;
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
        slot.mSequence.fetch_add(1, std::memory_order_acq_rel);
        std::size_t at = append(slot, 0, what);
        if (!subject.empty())
        {
            at = append(slot, at, " \"");
            at = append(slot, at, subject);
            at = append(slot, at, "\"");
        }
        slot.mText[at] = '\0';
        slot.mSequence.fetch_add(1, std::memory_order_release);
    }

    std::size_t readNotes(std::span<NoteCopy, sNoteThreads> into)
    {
        const std::uint64_t self = currentThread();
        std::size_t count = 0;

        for (const Slot& slot : sSlots)
            if (slot.mThread.load(std::memory_order_acquire) == self)
                copy(slot, self, into[count++]);

        for (const Slot& slot : sSlots)
        {
            const std::uint64_t thread = slot.mThread.load(std::memory_order_acquire);
            if (thread != 0 && thread != self)
                copy(slot, thread, into[count++]);
        }

        return count;
    }
}
