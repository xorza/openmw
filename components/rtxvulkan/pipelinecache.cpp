#include "pipelinecache.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <smhasher/MurmurHash3.h>

#include <components/debug/debuglog.hpp>
#include <components/files/hash.hpp>

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

        /// What every file this renderer keeps in the cache directory is called, before its key.
        constexpr std::string_view sPrefix = "rtx-";
        constexpr std::string_view sSuffix = ".pipelinecache";

        std::uint32_t readWord(std::span<const std::uint8_t> data, std::size_t at)
        {
            std::uint32_t word = 0;
            std::memcpy(&word, data.data() + at, sizeof(word));
            return word;
        }

        void appendHex(std::string& name, std::span<const std::uint8_t> bytes)
        {
            constexpr std::array<char, 16> digits{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd',
                'e', 'f' };

            for (const std::uint8_t byte : bytes)
            {
                name.push_back(digits[byte >> 4]);
                name.push_back(digits[byte & 0xF]);
            }
        }

        /// A number that changes when any compiled shader does, and with nothing else.
        ///
        /// **What makes the eviction exact.** A blob cannot be pruned entry by entry, so the only way
        /// a stale pipeline leaves is for the whole file to be replaced — and the file is replaced
        /// exactly when this changes, because it is what the name is built from. An edited shader
        /// therefore costs one cold compile and leaves nothing behind, where before it left every
        /// pipeline of every earlier edit in a file that grew until a cap threw the lot away.
        ///
        /// **The whole directory and not the modules a pipeline names**, because what the cache holds
        /// is every pipeline the run built and a module none of them named this time may be named by
        /// the next. Sorted, so the order is the directory listing's and not the filesystem's.
        ///
        /// Six megabytes over thirty-two files, hashed in six milliseconds. Nought where the directory
        /// cannot be read, which is a build with no shaders and a renderer about to fail anyway.
        std::array<std::uint64_t, 2> digestOfShaders(const std::filesystem::path& directory)
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

                // The name as well as the contents, so that renaming a shader is a change and two
                // files trading contents is not the same set.
                const std::string name = file.filename().string();
                std::array<std::uint64_t, 2> step{ 0, 0 };
                MurmurHash3_x64_128(name.data(), static_cast<int>(name.size()), digest.data(), step.data());
                digest = step;

                // **`Files::getHash` throws where a read fails**, and this one may not: it is called
                // from a constructor whose whole contract is that a cache which cannot be built is a
                // renderer that compiles from source. A shader that will not read is a renderer about
                // to fail for a better reason, and the digest of what did read is a key like any
                // other — it names a set nothing else will produce.
                try
                {
                    const std::array<std::uint64_t, 2> content = Files::getHash(name, stream);
                    MurmurHash3_x64_128(content.data(), static_cast<int>(sizeof(content)), digest.data(), step.data());
                    digest = step;
                }
                catch (const std::exception& error)
                {
                    Log(Debug::Warning) << "Rtx: " << name
                                        << " would not read for the pipeline cache's key: " << error.what();
                }
            }

            return digest;
        }

        /// What this cache is called: the driver that can read it back, and the shaders it was
        /// built from.
        ///
        /// **Both halves are the eviction.** Vulkan refuses a blob from another driver anyway, but a
        /// single filename would mean every run after an update reading a file it cannot use and
        /// overwriting it; and no part of Vulkan has an opinion about a blob full of pipelines for
        /// shaders that no longer exist. A name carrying both means the run knows exactly one file is
        /// live, which is what lets `sweep` remove the rest.
        std::filesystem::path cachePath(const PipelineCacheSpec& spec, const VkPhysicalDeviceProperties& properties)
        {
            if (spec.mDirectory.empty())
                return {};

            std::error_code failed;
            std::filesystem::create_directories(spec.mDirectory, failed);
            if (failed)
                return {};

            const std::array<std::uint64_t, 2> shaders = digestOfShaders(spec.mShaderDirectory);

            std::array<std::uint8_t, sizeof(shaders)> digest{};
            std::memcpy(digest.data(), shaders.data(), digest.size());

            std::string name(sPrefix);
            appendHex(name, properties.pipelineCacheUUID);
            name.push_back('-');
            appendHex(name, digest);
            name += sSuffix;

            return spec.mDirectory / name;
        }

        /// The file's contents, where there is a file and `PipelineCache::accepts` takes it.
        std::vector<std::uint8_t> readCache(
            const std::filesystem::path& path, const VkPhysicalDeviceProperties& properties)
        {
            if (path.empty())
                return {};

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};

            // **Both bounds before the read and not only after it**, because the file this refuses
            // for its size is the one it would be most expensive to read: `PipelineCache::sMostBytes`
            // says what has been seen in a directory nothing swept.
            const std::streamoff bytes = file.tellg();
            if (bytes < static_cast<std::streamoff>(sHeaderBytes)
                || bytes > static_cast<std::streamoff>(PipelineCache::sMostBytes))
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
        if (blob.size() < sHeaderBytes || blob.size() > sMostBytes)
            return false;

        return readWord(blob, 0) == sHeaderBytes && readWord(blob, sVersionAt) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
            && readWord(blob, sVendorAt) == properties.vendorID && readWord(blob, sDeviceAt) == properties.deviceID
            && std::memcmp(blob.data() + sUuidAt, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }

    PipelineCache::PipelineCache(
        VkDevice device, const VkPhysicalDeviceProperties& properties, const PipelineCacheSpec& spec)
        : mDevice(device)
        , mPath(cachePath(spec, properties))
    {
        // Before the read and not after it, so that a run which then fails to load its own file has
        // still taken the rest away.
        sweep();
        mLoaded = readCache(mPath, properties);

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

    void PipelineCache::sweep() const
    {
        if (mPath.empty())
            return;

        const std::filesystem::path::string_type mine = mPath.filename().native();
        const std::filesystem::path::string_type prefix = std::filesystem::path(sPrefix).native();

        std::error_code failed;
        for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(mPath.parent_path(), failed))
        {
            const std::filesystem::path::string_type name = entry.path().filename().native();
            if (name == mine || !name.starts_with(prefix))
                continue;

            // A directory somebody named `rtx-something` is not this renderer's to remove, and
            // neither is a link: what is swept is the kind of thing `write` leaves.
            std::error_code ignored;
            if (entry.is_regular_file(ignored))
                std::filesystem::remove(entry.path(), ignored);
        }
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

        // **Removed rather than written, where one run's own pipelines have outgrown the cap.** The
        // next run would refuse a blob this size and start again, so writing it is a hundred
        // megabytes spent to be thrown away — and leaving the smaller file that is already there
        // would have the run after that grow past the cap again from where this one did.
        if (bytes > sMostBytes)
        {
            Log(Debug::Warning) << "Rtx: the pipeline cache reached " << bytes / (1024 * 1024)
                                << " MiB and was dropped rather than kept";

            std::error_code tooBig;
            std::filesystem::remove(mPath, tooBig);
            return;
        }

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
