#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// Where a pipeline cache is kept, and what it is keyed on.
    struct PipelineCacheSpec
    {
        /// The directory the file goes in, made if it is not there. Empty keeps no file at all,
        /// which is a renderer that compiles from source every run.
        std::filesystem::path mDirectory;

        /// The compiled shaders the pipelines are built from, digested into the file's name.
        std::filesystem::path mShaderDirectory;
    };

    /// A `VkPipelineCache` that outlives the process, kept in a file in the user's cache directory.
    ///
    /// **Creating a pipeline is compiling a program**, and this renderer's is one large ray-query
    /// shader. Keeping the result means an edited shader is compiled once rather than once per
    /// process that runs it: the driver keys its entries on the module, so a change misses and is
    /// built afresh, which is exactly the behaviour wanted.
    ///
    /// **Worth measuring before believing, because a driver may already be doing it.** This one
    /// does: on the machine this was written on the test suite runs in 1816 ms with the cache and
    /// 1862 ms with none at all, which is two and a half per cent and not the order of magnitude
    /// the idea invites. Where it shows is the first run after a shader edit — 4.0 seconds against
    /// 1.9 — and in not depending on a driver choosing to keep something it is not obliged to.
    ///
    /// **Nothing here is allowed to fail loudly.** The cache is an optimisation over a renderer that
    /// works without it: a directory that cannot be made or written, a half-written file, a cache
    /// from another machine — each of them means compiling from scratch and nothing worse, so each
    /// is swallowed rather than thrown.
    class PipelineCache
    {
    public:
        /// @param device the handle pipelines will be created on.
        /// @param properties identifies the driver the cache was built by. Vulkan will reject a blob
        ///        that does not match, and the name carries it so that a driver update starts a new
        ///        cache instead of rejecting the old one on every run.
        PipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties, const PipelineCacheSpec& spec);
        ~PipelineCache();

        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        /// Null when the cache could not be created, which every `vkCreate*Pipelines` accepts as
        /// "no cache" — so a caller passes this without asking whether it worked.
        VkPipelineCache getHandle() const { return mHandle; }

        /// The most a blob may hold before a run throws it away and starts one again.
        ///
        /// **A backstop and not the eviction, which the name is.** A blob is monolithic and Vulkan
        /// offers no way to drop one entry of it, so what stops this growing is that the file is
        /// named for the shaders as well as the driver — an edit starts a new one and `sweep`
        /// removes what it started from. What is left for a cap to catch is one build accumulating
        /// entries within its own lifetime, and a file that is not a cache at all.
        ///
        /// **Well clear of the largest live set, because firing is the failure.** Once the name is
        /// the eviction, a cap that trips on a working cache does not save space — it throws away a
        /// cache that was doing its job, every run, for ever. The measured sets on one shader
        /// generation are 35 MiB for the test suite alone and 73 MiB once `shot` has added its own
        /// extents and upscaler to the same file, so a cap at 96 MiB was one more host away from
        /// tripping. Before the name carried a digest the same file reached 3.9 GiB.
        static constexpr std::size_t sMostBytes = std::size_t{ 256 } << 20;

        /// Whether a stored blob is one this driver wrote, and one small enough to go on keeping.
        ///
        /// **Checked here as well as by the driver.** Handing a blob to `vkCreatePipelineCache` is
        /// handing it untrusted data — the file may be a truncated write from a process that died,
        /// or one a cache cleaner half-removed — and while the specification requires the
        /// implementation to validate the header, four comparisons are cheaper than relying on every
        /// driver to have got that right. The driver has no opinion at all about the second half:
        /// a blob past `sMostBytes` is refused here and nowhere else.
        ///
        /// Public because it is the one part of this worth testing without a file: an offset off by
        /// four would reject every blob the driver ever wrote, and the only symptom would be a cache
        /// that silently never hit.
        static bool accepts(std::span<const std::uint8_t> blob, const VkPhysicalDeviceProperties& properties);

    private:
        /// Writes the driver's current blob back, through a temporary and a rename — or deletes the
        /// file where the blob has outgrown `sMostBytes`, which is what the next run would do with
        /// it anyway.
        void write() const;

        /// Removes every other pipeline cache of this renderer's in the same directory.
        ///
        /// **This is the eviction.** The name carries the driver and the shaders, so anything else
        /// under `rtx-` is a cache for a driver this machine no longer runs or for shaders this
        /// build no longer has — and nothing used to remove either, so a machine kept one blob per
        /// combination it had ever run. What it costs is a driver rollback, or a jump back to an
        /// older build, compiling from source once.
        ///
        /// A partial write another process has in flight is swept too, and that process then fails
        /// to save its cache. The window is one rename wide and the cost is one compile.
        void sweep() const;

        VkDevice mDevice = VK_NULL_HANDLE;
        VkPipelineCache mHandle = VK_NULL_HANDLE;
        std::filesystem::path mPath;

        /// What was loaded, kept so that a run which compiled nothing new rewrites nothing.
        std::vector<std::uint8_t> mLoaded;
    };
}
