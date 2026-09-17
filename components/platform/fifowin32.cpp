#include "fifo.hpp"

#include <string>
#include <system_error>

#include <components/files/conversion.hpp>

namespace Platform::Fifo
{
    File::Handle openForWriting(const std::filesystem::path& path)
    {
        throw std::system_error(std::make_error_code(std::errc::not_supported),
            std::string("Failed to open '") + Files::pathToUnicodeString(path)
                + "' for writing: there is no fifo, and no perf to read one, on Windows");
    }

    void write(File::Handle, const void*, std::size_t)
    {
        // Never reached: no handle to a fifo is ever handed out here.
        throw std::system_error(std::make_error_code(std::errc::not_supported), "A write to a fifo on Windows");
    }
}
