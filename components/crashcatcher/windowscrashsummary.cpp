#include "windowscrashsummary.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

#include "crashnote.hpp"

namespace Crash
{
    namespace
    {
        class Summary
        {
        public:
            Summary()
            {
                SYSTEMTIME time;
                GetLocalTime(&time);
                std::snprintf(mStamp, sizeof(mStamp), "[%02u:%02u:%02u.%03u E] ", time.wHour, time.wMinute,
                    time.wSecond, time.wMilliseconds);
            }

            /// One line, stamped as the log stamps its own.
            void line(const char* format, ...)
            {
                add(mStamp);

                std::va_list arguments;
                va_start(arguments, format);
                if (mLength < sizeof(mText) - 1)
                    advance(std::vsnprintf(mText + mLength, sizeof(mText) - mLength, format, arguments));
                va_end(arguments);

                add("\n");
            }

            /// `address` as the module holding it and its offset there, `openmw.exe+0x112a9a7`.
            void location(const char* prefix, DWORD64 address)
            {
                HMODULE module = nullptr;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(address), &module))
                {
                    line("%s0x%llx", prefix, static_cast<unsigned long long>(address));
                    return;
                }

                wchar_t path[MAX_PATH] = {};
                const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
                const wchar_t* name = path;
                for (DWORD i = 0; i < length; ++i)
                    if (path[i] == L'\\' || path[i] == L'/')
                        name = path + i + 1;

                char narrow[MAX_PATH * 3] = {};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                line("%s%s+0x%llx", prefix, narrow,
                    static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
            }

            void appendTo(const wchar_t* path) const
            {
                // Beside the log's own handle, which shares writing, and at the end whatever that
                // handle's position.
                const HANDLE file
                    = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                    return;

                DWORD written = 0;
                WriteFile(file, mText, static_cast<DWORD>(mLength), &written, nullptr);
                CloseHandle(file);
            }

        private:
            void add(const char* text)
            {
                if (mLength < sizeof(mText) - 1)
                    advance(std::snprintf(mText + mLength, sizeof(mText) - mLength, "%s", text));
            }

            /// Past what a print wrote, and never past the terminator, where it was cut.
            void advance(int wrote)
            {
                if (wrote > 0)
                    mLength = std::min(mLength + static_cast<std::size_t>(wrote), sizeof(mText) - 1);
            }

            char mStamp[32] = {};
            char mText[8192] = {};
            std::size_t mLength = 0;
        };

        /// Every frame the faulting thread unwinds to, the way a debugger unwinds it: by the unwind
        /// table every x64 module carries, and where a function has none it is a leaf, whose
        /// return address is where the stack points. Bounded by the thread's stack, because a fault
        /// may have left it any shape.
        void unwind(Summary& summary, const CONTEXT& faulted)
        {
#if defined(_M_X64)
            constexpr int sFrames = 48;

            const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const auto low = reinterpret_cast<DWORD64>(tib->StackLimit);
            const auto high = reinterpret_cast<DWORD64>(tib->StackBase);

            CONTEXT context = faulted;
            for (int frame = 0; frame < sFrames && context.Rip != 0; ++frame)
            {
                char prefix[16];
                std::snprintf(prefix, sizeof(prefix), "  #%-2d ", frame);
                summary.location(prefix, context.Rip);

                DWORD64 imageBase = 0;
                const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (function == nullptr)
                {
                    if (context.Rsp < low || context.Rsp + sizeof(DWORD64) > high)
                        return;
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += sizeof(DWORD64);
                    continue;
                }

                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(
                    UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                if (context.Rsp < low || context.Rsp >= high)
                    return;
            }
#else
            summary.line("Crash: no frames, which only an x64 build unwinds");
            (void)faulted;
#endif
        }
    }

    void appendCrashSummary(const wchar_t* logFile, const EXCEPTION_POINTERS& info)
    {
        Summary summary;

        const EXCEPTION_RECORD& record = *info.ExceptionRecord;
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
        {
            const ULONG_PTR operation = record.ExceptionInformation[0];
            summary.line("Crash: exception 0x%08lx %s 0x%llx in thread %lu", record.ExceptionCode,
                operation == 0       ? "reading"
                    : operation == 1 ? "writing"
                                     : "executing",
                static_cast<unsigned long long>(record.ExceptionInformation[1]), GetCurrentThreadId());
        }
        else
            summary.line("Crash: exception 0x%08lx in thread %lu", record.ExceptionCode, GetCurrentThreadId());

        unwind(summary, *info.ContextRecord);

        NoteCopy notes[sNoteThreads];
        const std::size_t count = readNotes(notes);
        for (std::size_t i = 0; i < count; ++i)
            summary.line("Crash: note of thread %llu%s: %s%s", static_cast<unsigned long long>(notes[i].mThread),
                notes[i].mThread == GetCurrentThreadId() ? ", which crashed" : "", notes[i].mText,
                notes[i].mWhole ? "" : " (half written)");
        if (count == 0)
            summary.line("Crash: no thread noted anything");

        summary.appendTo(logFile);
    }
}
