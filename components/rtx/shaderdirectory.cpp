#include "shaderdirectory.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

#include <smhasher/MurmurHash3.h>

#include <components/debug/debuglog.hpp>
#include <components/files/hash.hpp>

namespace Rtx
{
    std::filesystem::path shaderDirectory(const std::filesystem::path& resources, const bool withSource)
    {
        return resources / "rtx" / (withSource ? "shaders-source" : "shaders");
    }

    std::array<std::uint64_t, 2> digestShaders(const std::filesystem::path& directory)
    {
        std::error_code failed;
        std::vector<std::filesystem::path> files;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, failed))
            if (entry.is_regular_file(failed))
                files.push_back(entry.path());

        std::sort(files.begin(), files.end());

        std::array<std::uint64_t, 2> digest{ 0, 0 };
        for (const std::filesystem::path& file : files)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                continue;

            // The name as well as the contents, so that renaming a shader is a change and two files
            // trading contents is not the same set.
            const std::string name = file.filename().string();
            std::array<std::uint64_t, 2> step{ 0, 0 };
            MurmurHash3_x64_128(name.data(), static_cast<int>(name.size()), digest.data(), step.data());
            digest = step;

            // `Files::getHash` throws where a read fails, and this may not: a digest is a key, and a
            // module that will not read is one the renderer reading it reports.
            try
            {
                const std::array<std::uint64_t, 2> content = Files::getHash(name, stream);
                MurmurHash3_x64_128(content.data(), static_cast<int>(sizeof(content)), digest.data(), step.data());
                digest = step;
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "Rtx: " << name << " would not read for the shaders' digest: " << error.what();
            }
        }

        return digest;
    }
}
