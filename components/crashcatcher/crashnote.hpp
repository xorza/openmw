#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Crash
{
    /// How many bytes a note holds, its terminating nought included. A longer one is cut short.
    inline constexpr std::size_t sNoteCapacity = 256;

    /// How many threads can hold a note at once. A thread past them notes nothing until one of
    /// them ends, which gives its slot back.
    inline constexpr std::size_t sNoteThreads = 32;

    /// One thread's last note, as a crash handler reads it.
    struct NoteCopy
    {
        /// Terminated.
        char mText[sNoteCapacity];

        /// The system's id of the thread that wrote it, as `currentThread` gives it.
        std::uint64_t mThread;

        /// Whether the text is one note, and not a note half written over by the next.
        bool mWhole;
    };

    /// States what the calling thread is doing, for a crash to report: `what`, then `subject` in
    /// quotes where there is one. Each note replaces the thread's last.
    ///
    /// **Where a crash can read it.** The notes are a fixed table in the data segment, which a
    /// player's crash dump carries when it carries no heap, and a crash handler copies them
    /// without allocating or locking. Writing one allocates nothing either, so a loop may state
    /// each thing it works on. A thread has its own slot, because workers describe textures while
    /// the render thread uploads them.
    void note(std::string_view what, std::string_view subject = {});

    /// The system's id of the calling thread: what a crash dump and a debugger number threads by.
    /// Safe inside a signal handler.
    std::uint64_t currentThread();

    /// Every live thread's last note into `into`, the calling thread's first where it has one,
    /// and how many there are. As a signal handler may: no allocation and no lock.
    std::size_t readNotes(std::span<NoteCopy, sNoteThreads> into);
}
