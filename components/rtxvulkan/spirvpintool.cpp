#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "spirvpin.hpp"

// `openmw-rtx-spirv-pin <module>`: the module rewritten in place by `Rtx::pinFloatArithmetic`, which
// is the step the build runs on every shader between `glslc` and `spirv-val`. In place, because the
// file `glslc` wrote is the one its depfile names.

namespace
{
    std::vector<std::uint32_t> readWords(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw std::runtime_error("cannot be opened");

        const std::streamsize bytes = stream.tellg();
        if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0)
            throw std::runtime_error("is not a whole number of 32-bit words");

        std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(words.data()), bytes))
            throw std::runtime_error("could not be read");
        return words;
    }

    /// Written beside the module and moved over it, so a build that stops halfway leaves the
    /// module `glslc` wrote or the pinned one and never part of either.
    void writeWords(const std::filesystem::path& path, const std::vector<std::uint32_t>& words)
    {
        const std::filesystem::path written = path.string() + ".pinning";
        {
            std::ofstream stream(written, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(words.data()),
                static_cast<std::streamsize>(words.size() * sizeof(std::uint32_t)));
            if (!stream)
                throw std::runtime_error("could not be written");
        }

        std::error_code failed;
        std::filesystem::rename(written, path, failed);
        if (failed)
            throw std::runtime_error("could not be replaced: " + failed.message());
    }
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: openmw-rtx-spirv-pin <module.spv>\n";
        return 2;
    }

    const std::filesystem::path path(argv[1]);
    try
    {
        writeWords(path, Rtx::pinFloatArithmetic(readWords(path)));
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << path.string() << ": " << error.what() << '\n';
        return 1;
    }
}
