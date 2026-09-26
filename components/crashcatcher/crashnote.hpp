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

    /// One thread's last note, as a report reads it.
    struct NoteCopy
    {
        /// Terminated.
        char mText[sNoteCapacity];

        /// The system's id of the thread that wrote it, as `currentThread` gives it.
        std::uint64_t mThread;

        /// Whether the text is one note, and not a note half written over by the next.
        bool mWhole;
    };

    /// What a report is: a crash, a hang the monitor found, or one the game asked for and lives on.
    enum class ReportKind : std::uint32_t
    {
        Crash,
        Hang,
        Report,
    };

    /// Everything the note table says, as a report reads it.
    struct NotesRead
    {
        ReportKind mKind = ReportKind::Crash;

        /// Why, where the code that asked for the report said: `std::terminate`'s exception, the
        /// reason given to `Crash::report`. Terminated, and empty for a fault.
        char mReason[sNoteCapacity] = {};

        std::size_t mCount = 0;

        /// The first thread's first, then the rest in the table's order.
        NoteCopy mNotes[sNoteThreads];
    };

    /// States what the calling thread is doing, for a report to name: `what`, then `subject` in
    /// quotes where there is one. Each note replaces the thread's last.
    ///
    /// **A fixed table, read from outside.** The monitor reads it out of the crashed process, as
    /// it reads the stacks, so it never depends on the crashed process to hand it over. Writing a
    /// note allocates nothing, so a loop may state each thing it works on. A thread has its own slot, because workers
    /// describe textures while the render thread uploads them.
    void note(std::string_view what, std::string_view subject = {});

    /// Says what the next report is and why, just before the code that wants it asks for it.
    /// Allocates nothing and locks nothing, so a signal handler may call it.
    void setReport(ReportKind kind, std::string_view reason);

    /// The system's id of the calling thread: what a crash dump and a debugger number threads by.
    /// Safe inside a signal handler.
    std::uint64_t currentThread();

    /// The table's bytes where they lie, for the monitor to read out of this process.
    std::span<const std::byte> noteTable();

    /// Reads a copy of `noteTable()`'s bytes, taken while no thread of the process that wrote them
    /// runs, with `first`'s note first. The copy must be the whole table: the monitor is this same
    /// executable and knows its layout.
    void readNotes(std::span<const std::byte> table, std::uint64_t first, NotesRead& into);
}
