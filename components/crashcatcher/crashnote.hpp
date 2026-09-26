#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <utility>

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

    /// **What the calling thread is doing, for as long as a scope lasts**, for a report to name, on
    /// the end of what the thread's note already says: `outer > text`. The scope's end puts the note
    /// back as it found it, so a note never outlives the work it names, and a crash inside nested
    /// work reads as the path to it. Where the thread holds no slot, it notes nothing.
    ///
    /// **A fixed table, read from outside.** The monitor reads it out of the crashed process, as
    /// it reads the stacks, so it never depends on the crashed process to hand it over. A scope
    /// allocates nothing — what it found is kept in the scope, a note's bytes at most — so a loop
    /// may state each thing it works on. A thread has its own slot, because workers read models
    /// while the render thread uploads textures.
    class NoteScope
    {
    public:
        explicit NoteScope(std::string_view text);

        /// The text `std::format` makes of `format` and `arguments`, formatted on the stack and cut
        /// to what a note holds. A subject goes in quotes: `"reading the model \"{}\""`.
        template <class... Arguments>
        requires(sizeof...(Arguments) > 0) explicit NoteScope(
            std::format_string<Arguments...> format, Arguments&&... arguments)
        {
            char text[sNoteCapacity];
            const auto written
                = std::format_to_n(text, sNoteCapacity - 1, format, std::forward<Arguments>(arguments)...);
            begin(std::string_view(text, static_cast<std::size_t>(written.out - text)));
        }

        ~NoteScope();

        NoteScope(const NoteScope&) = delete;
        NoteScope& operator=(const NoteScope&) = delete;

    private:
        void begin(std::string_view text);

        bool mNoted = false;
        std::size_t mFoundLength = 0;
        char mFound[sNoteCapacity];
    };

    /// Starts a report of `kind` for `reason`, where no report is in progress, and keeps what the
    /// table said before for `endReport` to put back; false, changing nothing, where one is.
    ///
    /// **One report at a time.** A hang request that landed between a report's saying what it is
    /// and its dump wrote over both, and a report the game then did not survive read as a hang, or
    /// as a crash with no reason. Allocates nothing and locks nothing, so a signal handler may call
    /// it.
    bool beginReport(ReportKind kind, std::string_view reason);

    /// Ends the report `beginReport` began, putting back the kind and the reason it found there.
    void endReport();

    /// Takes the table for a report the process does not outlive, `kind` for `reason`, once any
    /// report in progress has ended, and refuses every request from then on. A second call keeps
    /// the first's kind and reason. Not for a signal handler that may have interrupted a report.
    void finalReport(ReportKind kind, std::string_view reason);

    /// Whether a report is being written, or the process is ending on one.
    bool isReporting();

    /// The system's id of the calling thread: what a crash dump and a debugger number threads by.
    /// Safe inside a signal handler.
    std::uint64_t currentThread();

    /// The table's bytes where they lie, for the monitor to read out of this process.
    std::span<const std::byte> noteTable();

    /// Reads a copy of `noteTable()`'s bytes, taken while no thread of the process that wrote them
    /// runs, with `first`'s note first. A thread whose note is empty is doing nothing it noted, and
    /// is left out. The copy must be the whole table: the monitor is this same
    /// executable and knows its layout.
    void readNotes(std::span<const std::byte> table, std::uint64_t first, NotesRead& into);
}
