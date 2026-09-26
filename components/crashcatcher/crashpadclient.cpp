#include "crash.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include <client/crashpad_client.h>
#include <client/crashpad_info.h>
#include <client/simple_string_dictionary.h>
#include <client/simulate_crash.h>

#include <components/platform/process.hpp>

#include "crashmonitorarguments.hpp"
#include "crashnote.hpp"
#include "crashpage.hpp"

#if defined(_WIN32)
#include <components/misc/windows.hpp>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Crash
{
    namespace
    {
        crashpad::CrashpadClient sClient;

        /// What `annotate` sets, registered with Crashpad at install. A static object and not the
        /// heap's, so a key set before install is there when the monitor starts.
        crashpad::SimpleStringDictionary sAnnotations;

        SharedPage sPage;
        std::atomic<bool> sInstalled{ false };

        /// This executable, which the monitor is started from: the running file, not `argv[0]`,
        /// which a shell may have given as a bare name or a relative path.
        std::filesystem::path executable()
        {
#if defined(_WIN32)
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length < path.size())
                {
                    path.resize(length);
                    return path;
                }
                path.resize(path.size() * 2);
            }
#elif defined(__APPLE__)
            std::uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string path(size, '\0');
            _NSGetExecutablePath(path.data(), &size);
            return std::filesystem::canonical(path.c_str());
#else
            return std::filesystem::read_symlink("/proc/self/exe");
#endif
        }

        /// A dump of every thread and a summary, after which the game goes on.
        void reportAndContinue(ReportKind kind, std::string_view reason)
        {
            setReport(kind, reason);
            CRASHPAD_SIMULATE_CRASH();
            setReport(ReportKind::Crash, {});
        }

        /// A hang report, asked for by the monitor, which knows how long the game stood still.
        void reportHang()
        {
            reportAndContinue(ReportKind::Hang, {});
        }

#if defined(_WIN32)
        /// A crash reported the way the system's own would not be: Crashpad dumps the process as it
        /// stands and returns, and the process ends here.
        [[noreturn]] void reportAndEnd(std::string_view reason)
        {
            setReport(ReportKind::Crash, reason);
            CRASHPAD_SIMULATE_CRASH();
            std::_Exit(3);
        }

        // Three ways MSVC's runtime ends a process without an exception the filter sees: `abort`
        // raises SIGABRT and then fails fast, and a pure virtual call and an invalid parameter go
        // to handlers of their own.
        void onAbort(int)
        {
            reportAndEnd("abort()");
        }

        void onPureCall()
        {
            reportAndEnd("a pure virtual function was called");
        }

        void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
        {
            reportAndEnd("the C runtime was given an invalid parameter");
        }

        DWORD WINAPI hangEntry(LPVOID)
        {
            reportHang();
            return 0;
        }
#else
        void onHangSignal(int)
        {
            reportHang();
        }
#endif

        /// **`std::terminate`, with the exception that called it**, which the fault it ends in
        /// names nothing of.
        void onTerminate()
        {
            std::string reason = "std::terminate";
            if (const std::exception_ptr current = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(current);
                }
                catch (const std::exception& error)
                {
                    reason += " on an uncaught exception: ";
                    reason += error.what();
                }
                catch (...)
                {
                    reason += " on an uncaught exception that is no std::exception";
                }
            }
#if defined(_WIN32)
            reportAndEnd(reason);
#else
            // Crashpad's own handler takes the abort, as it takes every fatal signal.
            setReport(ReportKind::Crash, reason);
            std::abort();
#endif
        }

#if defined(_MSC_VER)
        /// **The terminate hook on every thread**, because MSVC's runtime keeps one per thread and a
        /// new one starts with the default, which aborts. The loader calls this in each thread it
        /// starts, before the thread's own function; a thread started before `install` keeps the
        /// runtime's.
        void NTAPI onThreadStart(PVOID, DWORD reason, PVOID)
        {
            if (reason == DLL_THREAD_ATTACH && sInstalled.load(std::memory_order_acquire))
                std::set_terminate(onTerminate);
        }
#endif

        void hookEveryEnd()
        {
            std::set_terminate(onTerminate);
#if defined(_WIN32)
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
            std::signal(SIGABRT, onAbort);
            _set_purecall_handler(onPureCall);
            _set_invalid_parameter_handler(onInvalidParameter);
            std::atomic_ref(sPage.get()->mHangEntry)
                .store(reinterpret_cast<std::uint64_t>(&hangEntry), std::memory_order_release);
#else
            // On the thread's own stack and not the alternate one Crashpad sizes for its own fault
            // handler: a request arrives on a sound stack, and a whole dump taken inside a signal
            // frame outgrows the alternate one where the processor's saved state makes the frame
            // large.
            struct sigaction action = {};
            action.sa_handler = onHangSignal;
            action.sa_flags = SA_RESTART;
            sigemptyset(&action.sa_mask);
            sigaction(SIGUSR2, &action, nullptr);
#endif
        }
    }

    std::optional<std::string> install(const Settings& settings)
    {
        static std::atomic<bool> tried{ false };
        if (tried.exchange(true))
            return "install was called before, and a process installs once";

        const std::uint32_t process = Platform::Process::currentId();
        sPage = SharedPage::create(process);
        if (sPage.get() == nullptr)
            return "the page it shares with its monitor could not be made";

        // Everything the monitor needs to know of this process, on its command line: it reads the
        // notes from here at a crash, as it reads the stacks, and the page by this process's id.
        MonitorArguments monitor;
        monitor.mClient = process;
        monitor.mNotes = reinterpret_cast<std::uint64_t>(noteTable().data());
        monitor.mNotesSize = noteTable().size();
        monitor.mLog = settings.mLogFile;
        monitor.mApplication = settings.mApplication;
        monitor.mDialog = settings.mDialog;

        crashpad::CrashpadInfo* const info = crashpad::CrashpadInfo::GetCrashpadInfo();
        info->set_simple_annotations(&sAnnotations);

        // The object a crashed frame was working on is on the heap, and a dump of the stacks alone
        // names its address and nothing about it, which is what the RTX 2060 crash lacked. On
        // Windows Crashpad scans every stack for pointers and keeps what they point at, up to
        // 4 MiB; on Linux and macOS it keeps what the registers point at.
        info->set_gather_indirectly_referenced_memory(crashpad::TriState::kEnabled, 4 << 20);

        if (!sClient.StartHandler(base::FilePath(executable().native()),
                base::FilePath(settings.mReportFolder.native()), base::FilePath(), std::string(), std::string(),
                { { "product", settings.mApplication } }, monitor.write(), false, false))
        {
            sPage = SharedPage();
            return "its monitor did not start";
        }

#if defined(__linux__)
        // Every thread made after this gets its own through `pthread_create_linux.cc`; the one
        // installing is older than that.
        crashpad::CrashpadClient::InitializeSignalStackForThread();
#endif
        hookEveryEnd();
        sInstalled = true;
        return {};
    }

    void setHangLimit(std::chrono::seconds limit)
    {
        if (Heartbeat* const page = sPage.get())
            std::atomic_ref(page->mHangSeconds)
                .store(static_cast<std::uint32_t>(std::clamp<std::chrono::seconds::rep>(limit.count(), 0, 0xFFFFFFFF)),
                    std::memory_order_relaxed);
    }

    void heartbeat()
    {
        // One writer, the thread that draws, so a load and a store rather than a locked add.
        if (Heartbeat* const page = sPage.get())
        {
            std::atomic_ref frames(page->mFrames);
            frames.store(frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }

    void annotate(std::string_view key, std::string_view value)
    {
        sAnnotations.SetKeyValue(key, value);
    }

    void report(std::string_view reason)
    {
        if (!sInstalled)
            return;

        reportAndContinue(ReportKind::Report, reason);
    }
}

#if defined(_MSC_VER)
// The loader calls every pointer in `.CRT$XL*` at each thread's start. The two names keep the
// linker from dropping the table and the entry, which nothing else refers to.
#if defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_openmwCrashThreadStart")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:openmwCrashThreadStart")
#endif
#pragma section(".CRT$XLY", long, read)
extern "C" __declspec(allocate(".CRT$XLY")) const PIMAGE_TLS_CALLBACK openmwCrashThreadStart = Crash::onThreadStart;
#endif
