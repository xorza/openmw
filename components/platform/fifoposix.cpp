#include "fifo.hpp"

#include <cerrno>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <components/files/conversion.hpp>

namespace Platform::Fifo
{
    File::Handle openForWriting(const std::filesystem::path& path)
    {
        // `O_NONBLOCK` on a write-only fifo is what turns "no reader" from a hang into `ENXIO`.
        const int handle = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (handle == -1)
        {
            throw std::system_error(errno, std::generic_category(),
                std::string("Failed to open '") + Files::pathToUnicodeString(path) + "' for writing");
        }
        return static_cast<File::Handle>(handle);
    }

    void write(File::Handle handle, const void* data, std::size_t size)
    {
        const ssize_t written = ::write(static_cast<int>(handle), data, size);
        if (written != static_cast<ssize_t>(size))
        {
            throw std::system_error(
                errno, std::generic_category(), "An attempt to write " + std::to_string(size) + " bytes failed");
        }
    }
}
