#include "pipelinecache.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <span>
#include <system_error>

#include <components/debug/debuglog.hpp>

namespace Rtx
{
    namespace
    {
        /// Bytes of `VkPipelineCacheHeaderVersionOne`, which every blob starts with.
        constexpr std::size_t sHeaderBytes = 32;

        /// Where the fields identifying the writer sit inside that header.
        constexpr std::size_t sVersionAt = 4;
        constexpr std::size_t sVendorAt = 8;
        constexpr std::size_t sDeviceAt = 12;
        constexpr std::size_t sUuidAt = 16;

        std::uint32_t readWord(std::span<const std::uint8_t> data, std::size_t at)
        {
            std::uint32_t word = 0;
            std::memcpy(&word, data.data() + at, sizeof(word));
            return word;
        }

        /// The cache's name, which is the driver's own identifier for what it can read back.
        ///
        /// **Named for the driver rather than only checked against it.** Vulkan will refuse a blob
        /// from a different one anyway, but a single filename would mean every run after an update
        /// reading a file it cannot use and then overwriting it — where a name that changes with the
        /// driver simply starts afresh, and leaves the old one for a rollback.
        std::filesystem::path cachePath(const VkPhysicalDeviceProperties& properties)
        {
            // `temp_directory_path` is what makes this portable without a branch: TMPDIR or /tmp on
            // one platform, whatever `GetTempPath` answers on the other. A machine with no temporary
            // directory at all is one this does without a cache on.
            std::error_code failed;
            const std::filesystem::path directory = std::filesystem::temp_directory_path(failed);
            if (failed)
                return {};

            std::string name = "openmw-rtx-";
            for (const std::uint8_t byte : properties.pipelineCacheUUID)
            {
                constexpr std::array<char, 16> digits{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c',
                    'd', 'e', 'f' };
                name.push_back(digits[byte >> 4]);
                name.push_back(digits[byte & 0xF]);
            }
            name += ".pipelinecache";

            return directory / name;
        }

        /// The file's contents, if there is a file and this driver wrote it.
        std::vector<std::uint8_t> readCache(
            const std::filesystem::path& path, const VkPhysicalDeviceProperties& properties)
        {
            if (path.empty())
                return {};

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};

            const std::streamoff bytes = file.tellg();
            if (bytes < static_cast<std::streamoff>(sHeaderBytes))
                return {};

            std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(data.data()), bytes))
                return {};

            if (!PipelineCache::accepts(data, properties))
                return {};

            return data;
        }
    }

    bool PipelineCache::accepts(std::span<const std::uint8_t> blob, const VkPhysicalDeviceProperties& properties)
    {
        if (blob.size() < sHeaderBytes)
            return false;

        return readWord(blob, 0) == sHeaderBytes && readWord(blob, sVersionAt) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
            && readWord(blob, sVendorAt) == properties.vendorID && readWord(blob, sDeviceAt) == properties.deviceID
            && std::memcmp(blob.data() + sUuidAt, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }

    PipelineCache::PipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties)
        : mDevice(device)
        , mPath(cachePath(properties))
        , mLoaded(readCache(mPath, properties))
    {
        const VkPipelineCacheCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = mLoaded.size(),
            .pInitialData = mLoaded.empty() ? nullptr : mLoaded.data(),
        };

        if (vkCreatePipelineCache(device, &describe, nullptr, &mHandle) != VK_SUCCESS)
        {
            Log(Debug::Warning) << "Rtx: no pipeline cache; every shader will be compiled from source";
            mHandle = VK_NULL_HANDLE;
        }
    }

    PipelineCache::~PipelineCache()
    {
        if (mHandle == VK_NULL_HANDLE)
            return;

        // A destructor, so nothing here may throw: allocating the blob can, and a cache that failed
        // to save is not worth taking the process down over.
        try
        {
            write();
        }
        catch (const std::exception& error)
        {
            Log(Debug::Warning) << "Rtx: the pipeline cache was not saved: " << error.what();
        }

        vkDestroyPipelineCache(mDevice, mHandle, nullptr);
    }

    void PipelineCache::write() const
    {
        if (mPath.empty())
            return;

        std::size_t bytes = 0;
        if (vkGetPipelineCacheData(mDevice, mHandle, &bytes, nullptr) != VK_SUCCESS || bytes == 0)
            return;

        std::vector<std::uint8_t> data(bytes);
        if (vkGetPipelineCacheData(mDevice, mHandle, &bytes, data.data()) != VK_SUCCESS)
            return;

        // A run that compiled nothing new has nothing to say, and a great many runs are that: every
        // `shot` after the first, every test binary after the first. Rewriting a megabyte to record
        // no change is how a cache comes to cost more than it saves.
        data.resize(bytes);
        if (data == mLoaded)
            return;

        // Through a temporary, because the alternative is a process dying mid-write and leaving half
        // a cache behind — which the next run would read, reject, and replace, so the cache would go
        // on working exactly until something crashed once.
        //
        // **The temporary's name is unique and not merely temporary.** A test binary and a tool can
        // easily be closing at the same moment, and two of them writing one path would interleave
        // into a file with a valid header and a mixed body — which is the one kind of corruption the
        // header check cannot catch. The rename that follows is atomic on both platforms, so the
        // loser of a race overwrites the winner rather than tearing it.
        std::filesystem::path partial = mPath;
        partial += "." + std::to_string(std::random_device{}()) + ".partial";

        bool written = false;
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            written = file
                && file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(bytes)).good();
        }

        std::error_code failed;
        if (written)
            std::filesystem::rename(partial, mPath, failed);

        // Whether the write failed or the rename did, what must not be left behind is the temporary:
        // a directory filling with abandoned near-copies of a megabyte is a worse fault than the one
        // that started it.
        if (!written || failed)
            std::filesystem::remove(partial, failed);
    }
}
