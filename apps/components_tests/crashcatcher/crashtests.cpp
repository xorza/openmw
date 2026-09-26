// **Every way a game ends, and what the crash catcher makes of each**, run for real: a process per
// way, with the real catcher, its real monitor, and the files it leaves checked afterwards. Not a
// gtest binary, because every mode ends the process that runs it, as the bug it stands for would.
//
//   crash-tests <mode> <folder>     installs the catcher with its reports in <folder>, then <mode>
//   crash-tests --matrix <folder>   runs every mode this system has in a process of its own, and
//                                   checks the log and the dump each left; nought where all hold

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <components/crashcatcher/crash.hpp>
#include <components/crashcatcher/crashnote.hpp>
#include <components/crashcatcher/crashsummary.hpp>
#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/platform/process.hpp>

#include "zipreader.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if !defined(_WIN32)
#include <pthread.h>
#include <sys/wait.h>
#endif

namespace
{
    /// What one mode must leave: the summary's first line holds `mHeadline` and one of
    /// `mRaised` where that is not empty, and a dump carries the same summary. A mode the game
    /// lives through leaves `mFollows` after it, and a mode that reports nothing leaves no line.
    /// A crash ends the game with a status other than nought, and every other mode with nought.
    struct Mode
    {
        std::string_view mName;
        std::string_view mHeadline;
        std::vector<std::string_view> mRaised;
        std::string_view mFollows = {};
        bool mReports = true;

        /// How the note of the thread that raised the report is marked, where that thread noted
        /// what it was doing: the main thread does, before any mode.
        std::string_view mMarked = {};

        /// Whether the dump must carry `sHeapMarker`, which lies on the heap and which only the
        /// crashing stack points at. Crashpad scans stacks for pointers on Windows alone.
        bool mHeap = false;
    };

    /// **What the crashing frame was working on**, as the RTX 2060 crash's texture was: an object
    /// on the heap, which a dump of the stacks alone names the address of and nothing else.
    constexpr std::string_view sHeapMarker = "crash-tests: this text lies on the heap, pointed at from the stack";

    std::vector<Mode> modesOfThisSystem()
    {
#if defined(_WIN32)
        const std::vector<std::string_view> fault{ "EXCEPTION_ACCESS_VIOLATION reading 0x10" };
        const std::vector<std::string_view> overflow{ "EXCEPTION_STACK_OVERFLOW" };
        const std::vector<std::string_view> illegal{ "EXCEPTION_ILLEGAL_INSTRUCTION" };
        const std::vector<std::string_view> aborted{};
#elif defined(__APPLE__)
        const std::vector<std::string_view> fault{ "EXC_BAD_ACCESS at 0x10" };
        const std::vector<std::string_view> overflow{ "EXC_BAD_ACCESS" };
        const std::vector<std::string_view> illegal{ "EXC_BAD_INSTRUCTION", "EXC_BREAKPOINT" };
        const std::vector<std::string_view> aborted{ "EXC_CRASH", "SIGABRT" };
#else
        const std::vector<std::string_view> fault{ "SIGSEGV at 0x10" };
        const std::vector<std::string_view> overflow{ "SIGSEGV" };
        const std::vector<std::string_view> illegal{ "SIGILL", "SIGTRAP" };
        const std::vector<std::string_view> aborted{ "SIGABRT" };
#endif
        constexpr std::string_view crashed = ", which crashed";
        std::vector<Mode> modes
        {
#if defined(_WIN32)
            { "null-read", "Crash: ", fault, {}, true, crashed, true },
#else
            { "null-read", "Crash: ", fault, {}, true, crashed },
#endif
                { "stack-overflow", "Crash: ", overflow, {}, true, crashed },
                { "stack-overflow-worker", "Crash: ", overflow },
                { "illegal-instruction", "Crash: ", illegal, {}, true, crashed },
                { "terminate", "Crash: std::terminate on an uncaught exception: crash-tests threw", {} },
                { "fatal", "Crash: crash-tests gave up", {}, "crash-tests: what the reason leaves out", true, crashed },
                { "two-threads", "Crash: ", fault },
                { "report", "Report: crash-tests asked", {}, "crash-tests lived on", true, ", which asked" },
                { "hang", "Hang: no frame for", {}, "Hang: frames again after" },
                { "short-stall", "", {}, "crash-tests lived on", false },
                { "no-frames", "", {}, "crash-tests lived on", false },
                { "hang-off", "", {}, "crash-tests lived on", false },
                { "recovers-before-end", "Hang: no frame for", {},
                    "Hang: the game drew again before End was answered" },
                { "ends-before-end", "Hang: no frame for", {}, "Hang: the game ended before End was answered" },
        };
#if defined(_WIN32)
        modes.push_back({ "abort", "Crash: abort()", {}, {}, true, crashed });
        modes.push_back({ "pure-call", "Crash: a pure virtual function was called", {}, {}, true, crashed });
        modes.push_back(
            { "invalid-parameter", "Crash: the C runtime was given an invalid parameter", {}, {}, true, crashed });
#else
        modes.push_back({ "abort", "Crash: ", aborted, {}, true, crashed });
        modes.push_back({ "report-under-hang", "Report: crash-tests asked under a hang request", {},
            "crash-tests lived on", true, ", which asked" });
#endif
        return modes;
    }

#if defined(_MSC_VER)
#define CRASH_TESTS_NOINLINE __declspec(noinline)
#else
#define CRASH_TESTS_NOINLINE __attribute__((noinline))
#endif

    CRASH_TESTS_NOINLINE int readAt(const volatile int* at)
    {
        return *at;
    }

    CRASH_TESTS_NOINLINE int recurse(int depth)
    {
        // A way out no call takes, so the compiler does not call the recursion endless and fold it.
        if (depth < 0)
            return 0;
        volatile char frame[4096];
        frame[0] = static_cast<char>(depth);
        return recurse(depth + 1) + frame[0];
    }

    CRASH_TESTS_NOINLINE void illegalInstruction()
    {
#if defined(_MSC_VER)
        __ud2();
#else
        __builtin_trap();
#endif
    }

    struct Base
    {
        Base() { call(); }
        virtual ~Base() = default;
        CRASH_TESTS_NOINLINE void call() { pure(); }
        virtual void pure() = 0;
    };

    struct Derived : Base
    {
        void pure() override {}
    };

    /// Frames for a while, a stop of `stop`, and frames again, as a game that stalls and recovers.
    void stall(std::chrono::milliseconds stop)
    {
        for (int i = 0; i < 5; ++i)
        {
            Crash::heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(stop);
        for (int i = 0; i < 20; ++i)
        {
            Crash::heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    /// The modes whose monitor is asked whether to end the game, and answers End after this many
    /// milliseconds: long enough that the game has drawn again, or ended, by then.
    constexpr std::string_view sEndAfterMs = "2500";

    bool answersEnd(std::string_view mode)
    {
        return mode == "recovers-before-end" || mode == "ends-before-end";
    }

    /// **Started the way the game starts**: `wrapApplication` starts the catcher with its reports in
    /// `<folder>/crashes`, and `setupLogging` opens the log and hands it over, so what a mode writes
    /// afterwards goes through the stream the game writes its log with, the one the monitor's
    /// summaries have to survive.
    int run(std::string_view mode, const std::filesystem::path& folder)
    {
        std::filesystem::create_directories(folder);
        Debug::setupLogging(folder, "crash-tests");
        Crash::setHangLimit(std::chrono::seconds(2));
        Crash::annotate("mode", mode);
        const Crash::NoteScope noted("running the mode \"{}\"", mode);

        const auto livedOn = [] {
            Log(Debug::Info) << "crash-tests lived on";
            return 0;
        };

        if (mode == "null-read")
        {
            // Kept in this frame's memory and in no register the call keeps: what finds it is the
            // stack scan the catcher turns on, not the registers every dump reads around.
            const char* volatile onTheStack = (new std::string(sHeapMarker))->data();
            (void)onTheStack;
            return readAt(reinterpret_cast<const volatile int*>(static_cast<std::uintptr_t>(0x10)));
        }
        if (mode == "stack-overflow")
            return recurse(0);
        if (mode == "stack-overflow-worker")
        {
            std::thread([] { recurse(0); }).join();
            return 0;
        }
        if (mode == "illegal-instruction")
        {
            illegalInstruction();
            return 0;
        }
        if (mode == "terminate")
            std::thread([] { throw std::runtime_error("crash-tests threw"); }).join();
        if (mode == "abort")
            std::abort();
        if (mode == "fatal")
        {
            // Logged first, as a caller logs what is too long for a reason: it must be on disk
            // before the process ends.
            Log(Debug::Error) << "crash-tests: what the reason leaves out";
            Crash::fatal("crash-tests gave up");
        }
        if (mode == "two-threads")
        {
            std::thread other([] { readAt(reinterpret_cast<const volatile int*>(static_cast<std::uintptr_t>(0x10))); });
            readAt(reinterpret_cast<const volatile int*>(static_cast<std::uintptr_t>(0x10)));
            other.join();
            return 0;
        }
        if (mode == "report")
        {
            Crash::report("crash-tests asked");
            return livedOn();
        }
        if (mode == "hang")
        {
            stall(std::chrono::milliseconds(4500));
            return livedOn();
        }
        if (mode == "short-stall")
        {
            stall(std::chrono::milliseconds(1000));
            return livedOn();
        }
        if (mode == "no-frames")
        {
            // Longer than the limit, and before the first frame: a start, which is no hang.
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
            return livedOn();
        }
        if (mode == "hang-off")
        {
            Crash::setHangLimit(std::chrono::seconds(0));
            stall(std::chrono::milliseconds(3500));
            return livedOn();
        }
        if (mode == "recovers-before-end")
        {
            // Drawing again well before the monitor answers End, and still drawing when it does:
            // what the player was asked about is over, and the game goes on.
            stall(std::chrono::milliseconds(3500));
            for (int i = 0; i < 20; ++i)
            {
                Crash::heartbeat();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return livedOn();
        }
        if (mode == "ends-before-end")
        {
            // Gone without another frame before the monitor answers End: its id is nobody's to end.
            for (int i = 0; i < 5; ++i)
            {
                Crash::heartbeat();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
            return livedOn();
        }
#if !defined(_WIN32)
        if (mode == "report-under-hang")
        {
            // **One hang request, at the reporting thread, once its report is being written**: the
            // request lands between the report saying what it is and its dump, which is where one
            // wrote over both. The dump takes the monitor tens of milliseconds, so the request
            // lands inside it.
            const pthread_t reporter = pthread_self();
            std::thread asker([reporter] {
                while (!Crash::isReporting())
                    std::this_thread::yield();
                pthread_kill(reporter, SIGUSR2);
            });
            Crash::report("crash-tests asked under a hang request");
            asker.join();
            return livedOn();
        }
#endif
#if defined(_WIN32)
        if (mode == "pure-call")
        {
            Derived derived;
            return 0;
        }
        if (mode == "invalid-parameter")
        {
            char into[1];
            strcpy_s(into, sizeof(into), "longer than one");
            return 0;
        }
#endif
        std::cerr << "crash-tests: no mode " << mode << '\n';
        return 2;
    }

    std::string contentsOf(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    /// The summary stream of a minidump, or nothing where it has none.
    std::optional<std::string> summaryOf(const std::filesystem::path& dump)
    {
        const std::string bytes = contentsOf(dump);
        const auto word = [&](std::size_t at) {
            std::uint32_t value = 0;
            if (at + 4 <= bytes.size())
                std::memcpy(&value, bytes.data() + at, 4);
            return value;
        };

        if (bytes.size() < 32 || bytes.compare(0, 4, "MDMP") != 0)
            return std::nullopt;

        const std::uint32_t streams = word(8);
        const std::uint32_t directory = word(12);
        for (std::uint32_t i = 0; i < streams; ++i)
        {
            const std::size_t entry = directory + i * 12;
            if (word(entry) == Crash::sSummaryStream
                && word(entry + 8) + std::size_t{ word(entry + 4) } <= bytes.size())
                return bytes.substr(word(entry + 8), word(entry + 4));
        }
        return std::nullopt;
    }

    std::vector<std::filesystem::path> filesIn(
        const std::filesystem::path& folder, std::initializer_list<const char*> places, std::string_view extension)
    {
        std::vector<std::filesystem::path> found;
        for (const char* place : places)
            if (std::filesystem::is_directory(folder / place))
                for (const auto& entry : std::filesystem::directory_iterator(folder / place))
                    if (entry.path().extension() == extension)
                        found.push_back(entry.path());
        return found;
    }

    std::vector<std::filesystem::path> dumpsIn(const std::filesystem::path& folder)
    {
        return filesIn(folder, { "crashes/pending", "crashes/reports", "crashes/completed" }, ".dmp");
    }

    /// How a mode's process ended, from what `std::system` gives back: its exit code on Windows,
    /// and a wait status elsewhere, which a signal ends without one.
    std::string describeEnd(int status)
    {
#if defined(_WIN32)
        return "exit code " + std::to_string(static_cast<unsigned>(status));
#else
        if (WIFSIGNALED(status))
            return "signal " + std::to_string(WTERMSIG(status));
        return "exit code " + std::to_string(WEXITSTATUS(status));
#endif
    }

    /// Whether the log in `folder` says `text`, read again for a few seconds where it does not yet:
    /// the monitor goes on writing after the game has ended, and a mode may end first.
    bool follows(const std::filesystem::path& folder, std::string_view text)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        for (;;)
        {
            std::ifstream log(folder / "crash-tests.log");
            for (std::string line; std::getline(log, line);)
                if (line.find(text) != std::string::npos)
                    return true;

            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    /// Whether the package the monitor wrote once the game was gone holds the log, with the
    /// summary `headline` begins, and `dump` byte for byte, and nothing else.
    std::optional<std::string> checkPackage(
        const std::filesystem::path& folder, const std::filesystem::path& dump, const std::string& headline)
    {
        // Written once the game is gone, which the monitor outlives: the line that names the
        // package follows the package.
        if (!follows(folder, "Crash package: "))
            return "no package was written";

        const std::vector<std::filesystem::path> packages = filesIn(folder, { "crashes" }, ".zip");
        if (packages.size() != 1)
            return std::to_string(packages.size()) + " packages where one was due";

        std::vector<CrashTests::ZipEntry> entries;
        if (const std::optional<std::string> why = CrashTests::readZip(packages.front(), entries))
            return "the package does not read: " + *why;
        if (entries.size() != 2 || entries[0].mName != "crash-tests.log"
            || entries[1].mName != Files::pathToUnicodeString(dump.filename()))
            return "the package does not hold the log and the dump alone";
        if (entries[0].mContent.find(headline) == std::string::npos)
            return "the package's log does not carry the summary";

        if (entries[1].mContent != contentsOf(dump))
            return "the package's dump is not the dump";

        const std::string named = "Crash package: " + Files::pathToUnicodeString(packages.front());
        if (!follows(folder, named))
            return "the log does not name the package";
        return std::nullopt;
    }

    /// Whether `mode` left what it must in `folder`, and what it did not where it did not.
    std::optional<std::string> check(const Mode& mode, const std::filesystem::path& folder, int status)
    {
        if ((status != 0) != mode.mHeadline.starts_with("Crash: "))
            return "it ended with " + describeEnd(status);

        std::vector<std::string> lines;
        {
            std::ifstream log(folder / "crash-tests.log");
            for (std::string line; std::getline(log, line);)
                lines.push_back(line);
        }

        // A summary line is stamped as the log's are; the part after the stamp is what it says.
        std::vector<std::string> said;
        for (const std::string& line : lines)
            if (const std::size_t at = line.find("] "); line.starts_with("[") && at != std::string::npos)
                said.push_back(line.substr(at + 2));
            else
                said.push_back(line);

        const auto headed = [](const std::string& line) {
            return line.starts_with("Crash: ") || line.starts_with("Hang: ") || line.starts_with("Report: ");
        };
        const auto first = std::find_if(said.begin(), said.end(), headed);

        // `setupLogging` says so in the log and carries on, as the game does, where a mode that
        // reports nothing would then pass without a catcher to have kept quiet.
        if (const auto missing = std::find_if(
                said.begin(), said.end(), [](const std::string& line) { return line.starts_with("No crash catcher"); });
            missing != said.end())
            return *missing;

        if (!mode.mReports)
        {
            if (first != said.end())
                return "reported what it should not: " + *first;
            if (!filesIn(folder, { "crashes" }, ".zip").empty())
                return "packaged a session that reported nothing";
        }
        else
        {
            if (first == said.end())
                return "no summary in the log";
            if (first->find(mode.mHeadline) != 0)
                return "the summary begins \"" + *first + "\", not \"" + std::string(mode.mHeadline) + "\"";
            if (!mode.mRaised.empty()
                && std::none_of(mode.mRaised.begin(), mode.mRaised.end(),
                    [&](std::string_view raised) { return first->find(raised) != std::string::npos; }))
                return "the summary names none of the exceptions this mode raises: " + *first;
            if (std::count_if(said.begin(), said.end(),
                    [&](const std::string& line) {
                        return headed(line) && line.find("in thread") != std::string::npos;
                    })
                != 1)
                return "not one summary but several";
            const std::string note = "running the mode \"" + std::string(mode.mName) + "\"";
            const auto noted = std::find_if(said.begin(), said.end(),
                [&](const std::string& line) { return line.find(note) != std::string::npos; });
            if (noted == said.end())
                return "no note of the thread's in the summary";
            if (!mode.mMarked.empty() && noted->find(std::string(mode.mMarked) + ": ") == std::string::npos)
                return "the note of the thread that raised it is not marked: " + *noted;

            const std::string annotated = "mode: " + std::string(mode.mName);
            for (const std::string& annotation : { std::string("product: crash-tests"), annotated })
                if (std::none_of(said.begin(), said.end(),
                        [&](const std::string& line) { return line.find(annotation) != std::string::npos; }))
                    return "no annotation \"" + annotation + "\" in the summary";

            const std::vector<std::filesystem::path> dumps = dumpsIn(folder);
            if (dumps.size() != 1)
                return std::to_string(dumps.size()) + " dumps where one was due";
            const std::optional<std::string> summary = summaryOf(dumps.front());
            if (!summary || summary->find(first->substr(0, first->find(" in thread"))) == std::string::npos)
                return "the dump does not carry the summary";

            if (mode.mHeap)
            {
                if (contentsOf(dumps.front()).find(sHeapMarker) == std::string::npos)
                    return "the heap the crashing stack points at is not in the dump";
            }

            if (const std::optional<std::string> wrong = checkPackage(folder, dumps.front(), *first))
                return wrong;
        }

        if (!mode.mFollows.empty() && !follows(folder, mode.mFollows))
            return "nothing says \"" + std::string(mode.mFollows) + "\"";

        return std::nullopt;
    }

    std::string quoted(const std::string& text)
    {
        return "\"" + text + "\"";
    }

    int matrix(const std::filesystem::path& self, const std::filesystem::path& root)
    {
        int failed = 0;
        for (const Mode& mode : modesOfThisSystem())
        {
            const std::filesystem::path folder = root / std::string(mode.mName);
            std::filesystem::remove_all(folder);
            std::filesystem::create_directories(folder);

            const auto start = std::chrono::steady_clock::now();
            // Its errors and its monitor's, which share them, to show where the mode fails. Its
            // output apart, because the log is teed to it, and the matrix's own is the table.
            const std::filesystem::path errors = folder / "stderr.txt";
            const std::string command = quoted(Files::pathToUnicodeString(self)) + " " + std::string(mode.mName) + " "
                + quoted(Files::pathToUnicodeString(folder)) + " >"
                + quoted(Files::pathToUnicodeString(folder / "stdout.txt")) + " 2>"
                + quoted(Files::pathToUnicodeString(errors));
#if defined(_WIN32)
            // `cmd /c` takes the whole line in one more pair of quotes.
            const int status = std::system(quoted(command).c_str());
#else
            const int status = std::system(command.c_str());
#endif
            const auto took
                = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

            const std::optional<std::string> wrong = check(mode, folder, status);
            std::cout << (wrong ? "FAIL " : "ok   ") << mode.mName << " (" << took.count() << " ms)"
                      << (wrong ? ": " + *wrong : "") << '\n';
            failed += wrong ? 1 : 0;

            // Where a harness keeps nothing but this output, as CI does, it is all there is to read.
            if (wrong)
                for (const std::filesystem::path& file : { folder / "crash-tests.log", errors })
                {
                    std::ifstream text(file);
                    for (std::string line; std::getline(text, line);)
                        std::cout << "     " << Files::pathToUnicodeString(file.filename()) << ": " << line << '\n';
                }
        }

        std::cout << "crash-tests: " << failed << " of " << modesOfThisSystem().size() << " modes failed\n";
        return failed == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    Crash::runMonitorIfAsked(argc, argv);

    if (argc == 3 && std::string_view(argv[1]) == "--matrix")
        return matrix(std::filesystem::absolute(argv[0]), std::filesystem::absolute(argv[2]));
    if (argc == 3)
    {
        // Before `wrapApplication`, which starts the catcher from them: the reports beside the log,
        // no box, and for the modes that answer one, End after a while.
        const std::string_view mode = argv[1];
        const std::filesystem::path folder = std::filesystem::absolute(argv[2]);
        Platform::Process::setEnvironment(
            "OPENMW_CRASH_REPORTS", Files::pathToUnicodeString(folder / "crashes").c_str());
        Platform::Process::setEnvironment("OPENMW_CRASH_DIALOG", answersEnd(mode) ? "1" : "0");
        if (answersEnd(mode))
            Platform::Process::setEnvironment("OPENMW_CRASH_END_AFTER_MS", std::string(sEndAfterMs).c_str());

        return Debug::wrapApplication(
            [](int, char* arguments[]) { return run(arguments[1], std::filesystem::absolute(arguments[2])); }, argc,
            argv, "crash-tests");
    }

    std::cerr << "usage: crash-tests <mode> <folder> | --matrix <folder>\n";
    return 2;
}
