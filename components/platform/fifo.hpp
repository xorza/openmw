#pragma once

#include <cstddef>
#include <filesystem>

#include "file.hpp"

/// The write end of a fifo something else is reading — perf's control fifo is the one the ray
/// tracer opens. A fifo is a file to the handle, so what is opened here is a `File::Handle`,
/// held by a `File::ScopedHandle` and closed by `File::close`; only the opening and the writing
/// are its own. One header over `fifoposix.cpp` and `fifowin32.cpp`, the way `file.hpp` sits over
/// its two. Windows has neither fifos in its file system nor perf, so opening one there is refused
/// by name rather than answered with a handle to nothing.
namespace Platform::Fifo
{
    /// Opens the fifo at `path` for writing without blocking. A fifo nobody is reading is a
    /// `std::system_error` naming it, never a wait: a profiling run that hung would look like the
    /// benchmark being slow.
    File::Handle openForWriting(const std::filesystem::path& path);

    /// Writes the whole of `size` bytes at `data`, or throws a `std::system_error` naming what could
    /// not be sent.
    void write(File::Handle handle, const void* data, std::size_t size);
}
