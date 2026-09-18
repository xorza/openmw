#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    /// Where a pipeline cache is kept, and what it is keyed on.
    struct PipelineCacheSpec
    {
        /// The directory the file goes in, made if it is not there. Empty keeps no cache at all —
        /// no file, and no `VkPipelineCache` object either — which is a renderer that compiles
        /// every stage of every pipeline from source: every measuring process, for the reason
        /// `RtxRenderer` gives where it leaves this empty.
        ///
        /// **No object, not merely no file**, because the object is shared by every pipeline
        /// created against it, and the pipelines are created in parallel. A stage two of them
        /// share — the hit and miss shaders under every visibility variant — is compiled by
        /// whichever asked first and handed back from the object to whichever asked later, and
        /// what is handed back is not the code a compile of the same SPIR-V makes. Which
        /// pipelines got which was the hands' timing: a third of the runs of one scene drew a
        /// pixel a part in 255 apart on one frame in two hundred, with every input the same, and
        /// none of twelve did with the object gone. The compiles are faster without it as well.
        std::filesystem::path mDirectory;

        /// The compiled shaders the pipelines are built from, digested into the file's name.
        std::filesystem::path mShaderDirectory;
    };

    /// A `VkPipelineCache` that outlives the process, kept in a file in the user's cache directory,
    /// so an edited shader is compiled once rather than once per process. The driver already keeps
    /// its own, so a warm run gains a few per cent; where this shows is the first run after an
    /// edit. Nothing here is allowed to fail loudly: a cache that cannot be read or written means
    /// compiling from scratch and nothing worse.
    class PipelineCache
    {
    public:
        /// @param device the handle pipelines will be created on.
        /// @param properties identifies the driver the cache was built by. Vulkan will reject a blob
        ///        that does not match, and the name carries it so that a driver update starts a new
        ///        cache instead of rejecting the old one on every run.
        PipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties, const PipelineCacheSpec& spec);
        ~PipelineCache();

        /// Null where the spec keeps no cache or the cache could not be created, which every
        /// `vkCreate*Pipelines` accepts as "no cache" — so a caller passes this without asking.
        VkPipelineCache getHandle() const { return mHandle.get(); }

        /// The most a blob may hold before a run throws it away and starts one again — a backstop
        /// and not the eviction, which the name is. Well clear of the largest live set, because a
        /// cap that trips on a working cache throws it away every run for ever: one shader
        /// generation's set is tens of megabytes, about double once `shot` has added its extents.
        static constexpr std::size_t sMostBytes = std::size_t{ 256 } << 20;

        /// Whether a stored blob is one this driver wrote, and one small enough to go on keeping.
        /// Checked here as well as by the driver, because the file is untrusted data and four
        /// comparisons are cheaper than relying on every driver. Public because an offset off by
        /// four would reject every blob the driver ever wrote, with no symptom but a cache that
        /// never hit.
        static bool accepts(std::span<const std::uint8_t> blob, const VkPhysicalDeviceProperties& properties);

    private:
        /// Writes the driver's current blob back, through a temporary and a rename — or deletes the
        /// file where the blob has outgrown `sMostBytes`, which is what the next run would do with
        /// it anyway.
        void write() const;

        /// Removes every other pipeline cache of this renderer's in the same directory — the
        /// eviction, since anything else under `rtx-` is for a driver or shaders this build no
        /// longer has. A driver rollback compiles from source once; so does a process whose partial
        /// write is swept in the one-rename window.
        void sweep() const;

        VkDevice mDevice = VK_NULL_HANDLE;
        Owned<VkPipelineCache, vkDestroyPipelineCache> mHandle;
        std::filesystem::path mPath;

        /// What was loaded, kept so that a run which compiled nothing new rewrites nothing.
        std::vector<std::uint8_t> mLoaded;
    };
}
