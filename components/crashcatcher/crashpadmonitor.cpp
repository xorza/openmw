#include "crash.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL_messagebox.h>
#include <SDL_misc.h>
#include <handler/handler_main.h>
#include <handler/user_stream_data_source.h>
#include <minidump/minidump_user_extension_stream_data_source.h>
#include <snapshot/cpu_context.h>
#include <snapshot/exception_snapshot.h>
#include <snapshot/memory_snapshot.h>
#include <snapshot/module_snapshot.h>
#include <snapshot/process_snapshot.h>
#include <snapshot/thread_snapshot.h>
#include <util/misc/uuid.h>
#include <util/process/process_memory.h>

#include <components/files/conversion.hpp>

#include "crashmonitorarguments.hpp"
#include "crashpackage.hpp"
#include "crashpage.hpp"
#include "crashsummary.hpp"

#if defined(_WIN32)
#include <components/misc/windows.hpp>

#include <shellapi.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Crash
{
    namespace
    {
        /// The game itself, held from the monitor's start, so a hang request and an End reach the
        /// process that started the monitor and never one that took its id after it ended: a
        /// pidfd on Linux and a handle on Windows. macOS acts on the id.
        class Client
        {
        public:
            explicit Client(std::uint32_t id)
                : mId(id)
            {
#if defined(_WIN32)
                mHandle = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                        | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE,
                    FALSE, static_cast<DWORD>(id));
#elif defined(__linux__) && defined(SYS_pidfd_open)
                mDescriptor = static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(id), 0));
#endif
            }

            Client(const Client&) = delete;
            Client& operator=(const Client&) = delete;

            ~Client()
            {
#if defined(_WIN32)
                if (mHandle != nullptr)
                    CloseHandle(mHandle);
#elif defined(__linux__)
                if (mDescriptor >= 0)
                    close(mDescriptor);
#endif
            }

            /// Has the game write a hang report: a signal on POSIX, and on Windows, which has no
            /// signal to take it on, a thread of the game's own started at the function it named, as
            /// a debugger starts one; the game's frames are untouched.
            void requestHangReport(Heartbeat& page) const
            {
#if defined(_WIN32)
                const auto entry = std::atomic_ref(page.mHangEntry).load();
                if (mHandle == nullptr || entry == 0)
                    return;

                if (const HANDLE thread = CreateRemoteThread(
                        mHandle, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), nullptr, 0, nullptr))
                    CloseHandle(thread);
#else
                (void)page;
                send(SIGUSR2);
#endif
            }

            /// Ends the game, and says whether it was there to be ended.
            bool end() const
            {
#if defined(_WIN32)
                if (mHandle == nullptr || WaitForSingleObject(mHandle, 0) == WAIT_OBJECT_0)
                    return false;
                return TerminateProcess(mHandle, 3) != FALSE;
#else
                return send(SIGKILL);
#endif
            }

        private:
#if !defined(_WIN32)
            bool send(int number) const
            {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
                // No descriptor is a kernel older than 5.3, where nothing is sent rather than
                // something sent to whatever process has the id now.
                return mDescriptor >= 0 && syscall(SYS_pidfd_send_signal, mDescriptor, number, nullptr, 0) == 0;
#else
                return kill(static_cast<pid_t>(mId), number) == 0;
#endif
            }
#endif

            [[maybe_unused]] std::uint32_t mId = 0;
#if defined(_WIN32)
            HANDLE mHandle = nullptr;
#elif defined(__linux__)
            int mDescriptor = -1;
#endif
        };

        /// What the game told the monitor on its command line, and what the monitor learnt since.
        struct Monitor : MonitorArguments
        {
            explicit Monitor(MonitorArguments arguments)
                : MonitorArguments(std::move(arguments))
                , mPage(SharedPage::open(mClient))
                , mGame(mClient)
            {
            }

            /// The game's log, which it hands over through the page once it knows it; empty before.
            std::filesystem::path getLog() const { return Files::pathFromUnicodeString(mPage.getLogPath()); }

            SharedPage mPage;
            Client mGame;

            /// How long the game stood still when the watch asked for a hang report.
            std::atomic<std::uint32_t> mStalledFor{ 0 };

            /// Ends the watch once Crashpad's handler returns. A flag and not `std::stop_token`,
            /// which Apple's libc++ keeps behind its experimental switch.
            std::mutex mWatchMutex;
            std::condition_variable mWatchWake;
            bool mWatchEnds = false;

            /// The watch and the summary both write to the log, and a line each is what it takes.
            std::mutex mLogMutex;

            /// Every dump of the session, which the package holds, and whether one was a crash: written on
            /// Crashpad's thread, read on the one that ran it once the game is gone.
            std::mutex mReportMutex;
            std::vector<std::filesystem::path> mDumps;
            bool mCrashed = false;

            /// Whether the player's End ended the game: written by the watch, read once it is joined.
            bool mEnded = false;
        };

        std::tm localTime(std::time_t seconds)
        {
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &seconds);
#else
            localtime_r(&seconds, &local);
#endif
            return local;
        }

        std::string stamp()
        {
            const auto now = std::chrono::system_clock::now();
            const std::tm local = localTime(std::chrono::system_clock::to_time_t(now));
            const auto milliseconds
                = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            char text[32];
            std::snprintf(text, sizeof(text), "[%02d:%02d:%02d.%03d E] ", local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(milliseconds));
            return text;
        }

        /// Appends `lines` to the game's log, stamped as the log stamps its own. The game holds the
        /// file open, and shares its writing.
        void appendToLog(Monitor& monitor, const std::vector<std::string>& lines)
        {
            // Before the game has set its log up there is none, and a summary is in its dump alone.
            const std::filesystem::path path = monitor.getLog();
            if (path.empty())
                return;

            const std::lock_guard lock(monitor.mLogMutex);
            std::ofstream log(path, std::ios::app | std::ios::binary);
            const std::string at = stamp();
            for (const std::string& line : lines)
                log << at << line << '\n';
        }

        std::string hex(std::uint64_t value)
        {
            char text[20];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
            return text;
        }

        using Names = std::span<const std::pair<std::uint32_t, std::string_view>>;

        /// `code`'s name in `names`, or `otherwise` where it has none.
        std::string nameOf(Names names, std::uint32_t code, std::string otherwise)
        {
            const auto named
                = std::find_if(names.begin(), names.end(), [&](const auto& one) { return one.first == code; });
            return named != names.end() ? std::string(named->second) : std::move(otherwise);
        }

        /// The exception as the system names it, or nothing where the dump was asked for rather
        /// than raised by a fault.
        std::string describe(const crashpad::ExceptionSnapshot& exception, std::uint32_t process)
        {
            const std::uint32_t code = exception.Exception();
#if defined(_WIN32)
            (void)process;
            if (code == 0x517a7ed)
                return {};

            static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 12> sNames{ {
                { EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION" },
                { EXCEPTION_IN_PAGE_ERROR, "EXCEPTION_IN_PAGE_ERROR" },
                { EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW" },
                { EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION" },
                { EXCEPTION_PRIV_INSTRUCTION, "EXCEPTION_PRIV_INSTRUCTION" },
                { EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO" },
                { EXCEPTION_INT_OVERFLOW, "EXCEPTION_INT_OVERFLOW" },
                { EXCEPTION_DATATYPE_MISALIGNMENT, "EXCEPTION_DATATYPE_MISALIGNMENT" },
                { EXCEPTION_BREAKPOINT, "EXCEPTION_BREAKPOINT" },
                { EXCEPTION_NONCONTINUABLE_EXCEPTION, "EXCEPTION_NONCONTINUABLE_EXCEPTION" },
                { 0xC0000374, "STATUS_HEAP_CORRUPTION" },
                { 0xC0000409, "STATUS_STACK_BUFFER_OVERRUN" },
            } };
            std::string text = nameOf(sNames, code, "exception " + hex(code));

            const std::vector<std::uint64_t>& codes = exception.Codes();
            if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && codes.size() >= 2)
                text += std::string(codes[0] == 0 ? " reading "
                                : codes[0] == 1   ? " writing "
                                                  : " executing ")
                    + hex(codes[1]);
            return text;
#elif defined(__APPLE__)
            (void)process;

            // `kMachExceptionSimulated`, 'CPsx'.
            if (code == 0x43507378u)
                return {};

            static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 6> sNames{ {
                { 1, "EXC_BAD_ACCESS" },
                { 2, "EXC_BAD_INSTRUCTION" },
                { 3, "EXC_ARITHMETIC" },
                { 6, "EXC_BREAKPOINT" },
                { 10, "EXC_CRASH" },
                { 12, "EXC_GUARD" },
            } };
            std::string text = nameOf(sNames, code, "Mach exception " + std::to_string(code));
            if (code == 1)
                text += " at " + hex(exception.ExceptionAddress());
            return text;
#else
            if (code == 0xFFFFFFFFu)
                return {};

            static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 7> sNames{ {
                { SIGSEGV, "SIGSEGV" },
                { SIGBUS, "SIGBUS" },
                { SIGILL, "SIGILL" },
                { SIGFPE, "SIGFPE" },
                { SIGABRT, "SIGABRT" },
                { SIGTRAP, "SIGTRAP" },
                { SIGSYS, "SIGSYS" },
            } };
            std::string text = nameOf(sNames, code, "signal " + std::to_string(code));

            // A code of nought or less is a signal sent rather than a fault, `kill` or `raise`, whose
            // address is no fault's. Crashpad keeps the sender's id first among the codes of the
            // signals that carry one: the game's own, as `abort` sends it, says nothing more.
            if (static_cast<std::int32_t>(exception.ExceptionInfo()) <= 0)
            {
                const std::vector<std::uint64_t>& codes = exception.Codes();
                if (!codes.empty() && codes[0] != process)
                    text += " sent by process " + std::to_string(codes[0]);
            }
            else if (code == SIGSEGV || code == SIGBUS)
                text += " at " + hex(exception.ExceptionAddress());
            return text;
#endif
        }

        /// The module an address lies in, and the offset in it: "openmw.exe+0x112a9a7".
        std::string locate(const std::vector<const crashpad::ModuleSnapshot*>& modules, std::uint64_t address)
        {
            for (const crashpad::ModuleSnapshot* module : modules)
                if (address >= module->Address() && address - module->Address() < module->Size())
                {
                    const std::string path = module->Name();
                    const std::size_t slash = path.find_last_of("/\\");
                    return (slash == std::string::npos ? path : path.substr(slash + 1)) + "+"
                        + hex(address - module->Address());
                }

            return {};
        }

        class Collect final : public crashpad::MemorySnapshot::Delegate
        {
        public:
            explicit Collect(std::vector<std::uint8_t>& into)
                : mInto(into)
            {
            }

            bool MemorySnapshotDelegateRead(void* data, size_t size) override
            {
                const auto* const bytes = static_cast<const std::uint8_t*>(data);
                mInto.assign(bytes, bytes + size);
                return true;
            }

        private:
            std::vector<std::uint8_t>& mInto;
        };

        /// Values on the faulting thread's stack, from its stack pointer up, that point into a
        /// module: the return addresses among them, and some that only look like one.
        void scanStack(const crashpad::ProcessSnapshot& snapshot, std::uint64_t thread,
            const crashpad::CPUContext& context, std::vector<std::string>& into)
        {
            constexpr std::size_t sCandidates = 16;

            for (const crashpad::ThreadSnapshot* one : snapshot.Threads())
            {
                const crashpad::MemorySnapshot* const stack = one->ThreadID() == thread ? one->Stack() : nullptr;
                if (stack == nullptr)
                    continue;

                std::vector<std::uint8_t> bytes;
                Collect collect(bytes);
                if (!stack->Read(&collect))
                    return;

                const std::size_t word = context.Is64Bit() ? 8 : 4;
                const std::uint64_t sp = context.StackPointer();
                std::size_t at = sp >= stack->Address() && sp - stack->Address() < bytes.size()
                    ? static_cast<std::size_t>(sp - stack->Address())
                    : 0;
                at -= at % word;

                const std::vector<const crashpad::ModuleSnapshot*> modules = snapshot.Modules();
                for (; at + word <= bytes.size() && into.size() < sCandidates; at += word)
                {
                    std::uint64_t value = 0;
                    std::copy_n(bytes.data() + at, word, reinterpret_cast<std::uint8_t*>(&value));
                    // A value spilled twice in a row is one address, and names one frame at most.
                    if (std::string where = locate(modules, value);
                        !where.empty() && (into.empty() || into.back() != where))
                        into.push_back(std::move(where));
                }
                return;
            }
        }

        class TextStream final : public crashpad::MinidumpUserExtensionStreamDataSource
        {
        public:
            explicit TextStream(std::string text)
                : MinidumpUserExtensionStreamDataSource(sSummaryStream)
                , mText(std::move(text))
            {
            }

            size_t StreamDataSize() override { return mText.size(); }

            bool ReadStreamData(Delegate* delegate) override
            {
                return delegate->ExtensionStreamDataSourceRead(mText.data(), mText.size());
            }

        private:
            std::string mText;
        };

        /// **The summary, written where the dump is**: Crashpad calls this with the snapshot the
        /// dump is written from, in this process and not the crashed one, on every system alike.
        class SummarySource final : public crashpad::UserStreamDataSource
        {
        public:
            explicit SummarySource(Monitor& monitor)
                : mMonitor(monitor)
            {
            }

            std::unique_ptr<crashpad::MinidumpUserExtensionStreamDataSource> ProduceStreamData(
                crashpad::ProcessSnapshot* snapshot) override
            {
                CrashFacts facts;
                const crashpad::ExceptionSnapshot* const exception = snapshot->Exception();
                facts.mThread = exception != nullptr ? exception->ThreadID() : 0;

                std::vector<std::byte> table(mMonitor.mNotesSize);
                const crashpad::ProcessMemory* const memory = snapshot->Memory();
                const bool read
                    = memory != nullptr && !table.empty() && memory->Read(mMonitor.mNotes, table.size(), table.data());
                readNotes(read ? std::span<const std::byte>(table) : std::span<const std::byte>(), facts.mThread,
                    facts.mNotes);

                facts.mStalledFor = mMonitor.mStalledFor.load();

                if (exception != nullptr)
                {
                    facts.mException = describe(*exception, mMonitor.mClient);
                    if (const crashpad::CPUContext* const context = exception->Context())
                    {
                        facts.mWhere = locate(snapshot->Modules(), context->InstructionPointer());
                        scanStack(*snapshot, facts.mThread, *context, facts.mStack);
                    }
                }

                for (const auto& [key, value] : snapshot->AnnotationsSimpleMap())
                    facts.mAnnotations.emplace_back(key, value);
                for (const crashpad::ModuleSnapshot* module : snapshot->Modules())
                    for (const auto& [key, value] : module->AnnotationsSimpleMap())
                        facts.mAnnotations.emplace_back(key, value);

                crashpad::UUID report;
                snapshot->ReportID(&report);
#if defined(_WIN32)
                const std::filesystem::path dump = mMonitor.mDatabase / "reports" / (report.ToString() + ".dmp");
#else
                const std::filesystem::path dump = mMonitor.mDatabase / "pending" / (report.ToString() + ".dmp");
#endif
                facts.mDump = Files::pathToUnicodeString(dump);

                std::vector<std::string> lines;
                summarise(facts, lines);
                appendToLog(mMonitor, lines);

                {
                    const std::lock_guard lock(mMonitor.mReportMutex);
                    mMonitor.mDumps.push_back(dump);
                    mMonitor.mCrashed = mMonitor.mCrashed || facts.mNotes.mKind == ReportKind::Crash;
                }

                std::string text;
                for (const std::string& line : lines)
                    text += line + '\n';
                return std::make_unique<TextStream>(std::move(text));
            }

        private:
            Monitor& mMonitor;
        };

        /// Whether the player chose to end a game that stands still.
        bool askToEnd(const Monitor& monitor, std::uint32_t seconds)
        {
            // **A harness's answer, where nobody is at the box**: End, after this many milliseconds,
            // which is what lets a test end a game that recovered, or ended, while it was asked.
            if (const char* const after = std::getenv("OPENMW_CRASH_END_AFTER_MS"))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::strtoul(after, nullptr, 10)));
                return true;
            }

            const std::string message = monitor.mApplication + " has not drawn a frame for " + std::to_string(seconds)
                + " seconds. A report of what it is doing is being written; the log names it:\n"
                + Files::pathToUnicodeString(monitor.getLog()) + "\n\nWait for it, or end it?";
            const std::array<SDL_MessageBoxButtonData, 2> buttons{ {
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Wait" },
                { 0, 1, "End" },
            } };
            const std::string title = monitor.mApplication + " is not responding";
            const SDL_MessageBoxData box{ SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), message.c_str(),
                static_cast<int>(buttons.size()), buttons.data(), nullptr };

            int chosen = 0;
            return SDL_ShowMessageBox(&box, &chosen) == 0 && chosen == 1;
        }

        /// Ends the game where it still stands where it stood when the player was asked: the box
        /// stands for as long as the player takes over it, and the game may have drawn again, or
        /// ended, in that time. Says in the log what it did.
        void endIfStillStalled(Monitor& monitor, std::uint64_t stalledAt)
        {
            bool ended = false;
            {
                const std::lock_guard lock(monitor.mWatchMutex);
                ended = monitor.mWatchEnds;
            }

            if (!ended && std::atomic_ref(monitor.mPage.get()->mFrames).load() != stalledAt)
            {
                appendToLog(monitor, { "Hang: the game drew again before End was answered, and goes on" });
                return;
            }

            if (ended || !monitor.mGame.end())
                appendToLog(monitor, { "Hang: the game ended before End was answered, and nothing was ended" });
            else
                monitor.mEnded = true;
        }

        /// **The hang watch**, once a second: a frame counter that stops for the limit is a hang,
        /// reported once until it moves again. It begins at the first frame, so a start that
        /// draws nothing for a while is not one.
        void watch(Monitor& monitor)
        {
            Heartbeat* const page = monitor.mPage.get();
            if (page == nullptr)
                return;

            std::uint64_t last = 0;
            bool started = false;
            bool reported = false;
            auto since = std::chrono::steady_clock::now();

            for (;;)
            {
                {
                    std::unique_lock lock(monitor.mWatchMutex);
                    if (monitor.mWatchWake.wait_for(lock, std::chrono::seconds(1), [&] { return monitor.mWatchEnds; }))
                        return;
                }

                const std::uint64_t frames = std::atomic_ref(page->mFrames).load();
                const std::uint32_t limit = std::atomic_ref(page->mHangSeconds).load();
                const auto now = std::chrono::steady_clock::now();
                const auto stalled = std::chrono::duration_cast<std::chrono::seconds>(now - since);

                if (frames != last || !started)
                {
                    if (reported)
                        appendToLog(
                            monitor, { "Hang: frames again after " + std::to_string(stalled.count()) + " seconds" });
                    started = started || frames != 0;
                    last = frames;
                    since = now;
                    reported = false;
                    continue;
                }

                if (limit == 0 || reported || stalled.count() < limit)
                    continue;

                reported = true;
                monitor.mStalledFor = static_cast<std::uint32_t>(stalled.count());
                monitor.mGame.requestHangReport(*page);
                if (monitor.mDialog && askToEnd(monitor, static_cast<std::uint32_t>(stalled.count())))
                    endIfStillStalled(monitor, last);
            }
        }

        /// **The package, said in the log** and on the monitor's errors, which a game started from a
        /// shell shares. Where it is, and empty where none was written.
        std::filesystem::path packageSession(Monitor& monitor, std::span<const std::filesystem::path> dumps)
        {
            const SessionPackage package = writeSessionPackage(
                monitor.mDatabase, monitor.mApplication, monitor.getLog(), dumps, localTime(std::time(nullptr)));

            std::vector<std::string> lines;
            for (const std::filesystem::path& missing : package.mMissing)
                lines.push_back("Crash package: " + Files::pathToUnicodeString(missing)
                    + " is not on disk, and the package goes without it");
            if (!package.mFailure.empty())
                lines.push_back("Crash package: none, " + package.mFailure);
            if (!package.mZip.empty())
            {
                const std::string named = Files::pathToUnicodeString(package.mZip);
                lines.push_back("Crash package: " + named);
                std::cerr << monitor.mApplication << ": the crash report is " << named << '\n';
            }
            appendToLog(monitor, lines);
            return package.mZip;
        }

        /// **What the player is told once the game crashed or was ended**: `message`, then a button to
        /// the folder that holds the report and one to where issues are reported. The box comes back
        /// after either, until the player closes it.
        void tellPlayer(const Monitor& monitor, const std::string& title, const std::string& message,
            const std::filesystem::path& folder)
        {
            enum Button : int
            {
                Close,
                ShowFolder,
                OpenIssues,
            };
            std::vector<SDL_MessageBoxButtonData> buttons{
                { 0, ShowFolder, "Open the folder" },
            };
            if (!monitor.mIssues.empty())
                buttons.push_back({ 0, OpenIssues, "Report an issue" });
            buttons.push_back(
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, Close, "Close" });

            const SDL_MessageBoxData box{ SDL_MESSAGEBOX_ERROR | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, nullptr,
                title.c_str(), message.c_str(), static_cast<int>(buttons.size()), buttons.data(), nullptr };
            for (;;)
            {
                int chosen = Close;
                if (SDL_ShowMessageBox(&box, &chosen) != 0 || chosen == Close)
                    return;
                SDL_OpenURL(chosen == ShowFolder ? folderUrl(folder).c_str() : monitor.mIssues.c_str());
            }
        }

        /// The dialog once the game is gone, where it crashed or the player ended it: the package, or
        /// where it could not be written, the dumps and the log it would have held.
        void tellPlayerOfReport(const Monitor& monitor, bool crashed, std::span<const std::filesystem::path> dumps,
            const std::filesystem::path& package)
        {
            const std::string title = monitor.mApplication + (crashed ? " has crashed" : " was ended");
            std::string message = crashed ? monitor.mApplication + " has crashed.\n\n"
                                          : monitor.mApplication + " stopped responding and was ended.\n\n";

            std::filesystem::path folder;
            if (!package.empty())
            {
                folder = package.parent_path();
                message += "A report of what happened is saved in one file:\n" + Files::pathToUnicodeString(package)
                    + "\n\n";
                message += monitor.mIssues.empty() ? "Sending it helps to fix it."
                                                   : "Please attach this file to a new issue at\n" + monitor.mIssues;
            }
            else
            {
                folder = dumps.back().parent_path();
                message += "A report is saved in\n";
                for (const std::filesystem::path& dump : dumps)
                    message += Files::pathToUnicodeString(dump) + "\n";
                message
                    += "\nand the log says what happened:\n" + Files::pathToUnicodeString(monitor.getLog()) + "\n\n";
                message += monitor.mIssues.empty() ? "Sending them helps to fix it."
                                                   : "Please attach these files to a new issue at\n" + monitor.mIssues;
            }

            tellPlayer(monitor, title, message, folder);
        }

        /// The command line as UTF-8, which is what Crashpad's own entry hands `HandlerMain`: on
        /// Windows from the wide one, because `argv` is in the system's code page there.
        std::vector<std::string> commandLine(int argc, char** argv)
        {
            std::vector<std::string> arguments;
#if defined(_WIN32)
            (void)argc;
            (void)argv;
            int count = 0;
            wchar_t** const wide = CommandLineToArgvW(GetCommandLineW(), &count);
            for (int i = 0; i < count; ++i)
            {
                const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
                std::string one(static_cast<std::size_t>(std::max(size, 1)) - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, one.data(), size, nullptr, nullptr);
                arguments.push_back(std::move(one));
            }
            LocalFree(wide);
#else
            arguments.assign(argv, argv + argc);
#endif
            return arguments;
        }
    }

    void runMonitorIfAsked(int argc, char** argv)
    {
        if (std::none_of(argv, argv + argc, [](const char* one) { return std::string_view(one) == sMonitorSwitch; }))
            return;

        std::vector<std::string> handler;
        Monitor monitor(MonitorArguments::read(commandLine(argc, argv), handler));

        crashpad::UserStreamDataSources sources;
        sources.push_back(std::make_unique<SummarySource>(monitor));

        std::vector<char*> handlerArgv;
        for (std::string& argument : handler)
            handlerArgv.push_back(argument.data());
        handlerArgv.push_back(nullptr);

        std::thread watchdog([&] { watch(monitor); });
        const int result
            = crashpad::HandlerMain(static_cast<int>(handlerArgv.size() - 1), handlerArgv.data(), &sources);
        {
            const std::lock_guard lock(monitor.mWatchMutex);
            monitor.mWatchEnds = true;
        }
        monitor.mWatchWake.notify_one();
        watchdog.join();

        std::vector<std::filesystem::path> dumps;
        bool crashed = false;
        {
            const std::lock_guard lock(monitor.mReportMutex);
            dumps = monitor.mDumps;
            crashed = monitor.mCrashed;
        }
        const std::filesystem::path package = packageSession(monitor, dumps);

        // Once the game is gone, so the box does not stand over a window that no longer draws.
        if (monitor.mDialog && (crashed || monitor.mEnded) && !dumps.empty())
            tellPlayerOfReport(monitor, crashed, dumps, package);

        std::exit(result);
    }
}
