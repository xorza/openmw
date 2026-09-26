#include "crashpage.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#include <components/misc/windows.hpp>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Crash
{
    namespace
    {
        // Short, because macOS takes 31 characters of a shared memory name.
        std::string nameOf(std::uint32_t process)
        {
#if defined(_WIN32)
            return "Local\\openmw-crash-" + std::to_string(process);
#else
            return "/openmw-crash-" + std::to_string(process);
#endif
        }
    }

    SharedPage SharedPage::create(std::uint32_t process)
    {
        SharedPage page;
        const std::string name = nameOf(process);
#if defined(_WIN32)
        const HANDLE mapping
            = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Heartbeat), name.c_str());
        if (mapping == nullptr)
            return page;

        void* const view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Heartbeat));
        if (view == nullptr)
        {
            CloseHandle(mapping);
            return page;
        }

        page.mHandle = mapping;
        page.mPage = static_cast<Heartbeat*>(view);
#else
        // A page left by an earlier process of the same id is made over, not reused.
        const int descriptor = shm_open(name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (descriptor < 0)
            return page;

        void* const view = ftruncate(descriptor, sizeof(Heartbeat)) == 0
            ? mmap(nullptr, sizeof(Heartbeat), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0)
            : MAP_FAILED;
        close(descriptor);
        if (view == MAP_FAILED)
        {
            shm_unlink(name.c_str());
            return page;
        }

        page.mPage = static_cast<Heartbeat*>(view);
        page.mMadeFor = process;
#endif
        *page.mPage = Heartbeat{};
        return page;
    }

    SharedPage SharedPage::open(std::uint32_t process)
    {
        SharedPage page;
        const std::string name = nameOf(process);
#if defined(_WIN32)
        const HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (mapping == nullptr)
            return page;

        void* const view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Heartbeat));
        if (view == nullptr)
        {
            CloseHandle(mapping);
            return page;
        }

        page.mHandle = mapping;
        page.mPage = static_cast<Heartbeat*>(view);
#else
        const int descriptor = shm_open(name.c_str(), O_RDWR, 0600);
        if (descriptor < 0)
            return page;

        void* const view = mmap(nullptr, sizeof(Heartbeat), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        close(descriptor);

        // The name goes as soon as both sides hold the page: nothing else opens it, and a game that
        // crashes leaves nothing behind in the system's list.
        shm_unlink(name.c_str());
        if (view == MAP_FAILED)
            return page;

        page.mPage = static_cast<Heartbeat*>(view);
#endif
        return page;
    }

    SharedPage::SharedPage(SharedPage&& other) noexcept
        : mPage(std::exchange(other.mPage, nullptr))
        , mHandle(std::exchange(other.mHandle, nullptr))
        , mMadeFor(std::exchange(other.mMadeFor, 0))
    {
    }

    SharedPage& SharedPage::operator=(SharedPage&& other) noexcept
    {
        std::swap(mPage, other.mPage);
        std::swap(mHandle, other.mHandle);
        std::swap(mMadeFor, other.mMadeFor);
        return *this;
    }

    SharedPage::~SharedPage()
    {
        if (mPage == nullptr)
            return;
#if defined(_WIN32)
        UnmapViewOfFile(mPage);
        CloseHandle(mHandle);
#else
        munmap(mPage, sizeof(Heartbeat));
        if (mMadeFor != 0)
            shm_unlink(nameOf(mMadeFor).c_str());
#endif
    }
}
