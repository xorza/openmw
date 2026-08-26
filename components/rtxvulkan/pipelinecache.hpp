#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// A `VkPipelineCache` that outlives the process, kept in a file beside the system's temporaries.
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
    /// works without it: an unwritable temporary directory, a half-written file, a cache from
    /// another machine — each of them means compiling from scratch and nothing worse, so each is
    /// swallowed rather than thrown.
    class PipelineCache
    {
    public:
        /// @param device the handle pipelines will be created on.
        /// @param properties identifies the driver the cache was built by. Vulkan will reject a blob
        ///        that does not match, and the file is named for it so that a driver update starts a
        ///        new one instead of rejecting the old one on every run.
        PipelineCache(VkDevice device, const VkPhysicalDeviceProperties& properties);
        ~PipelineCache();

        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        /// Null when the cache could not be created, which every `vkCreate*Pipelines` accepts as
        /// "no cache" — so a caller passes this without asking whether it worked.
        VkPipelineCache getHandle() const { return mHandle; }

        /// Whether a stored blob is one this driver wrote and can therefore read back.
        ///
        /// **Checked here as well as by the driver.** Handing a blob to `vkCreatePipelineCache` is
        /// handing it untrusted data — the file sits in a world-writable directory and may be a
        /// truncated write from a process that died — and while the specification requires the
        /// implementation to validate the header, four comparisons are cheaper than relying on every
        /// driver to have got that right.
        ///
        /// Public because it is the one part of this worth testing without a file: an offset off by
        /// four would reject every blob the driver ever wrote, and the only symptom would be a cache
        /// that silently never hit.
        static bool accepts(std::span<const std::uint8_t> blob, const VkPhysicalDeviceProperties& properties);

    private:
        /// Writes the driver's current blob back, through a temporary and a rename.
        void write() const;

        VkDevice mDevice = VK_NULL_HANDLE;
        VkPipelineCache mHandle = VK_NULL_HANDLE;
        std::filesystem::path mPath;

        /// What was loaded, kept so that a run which compiled nothing new rewrites nothing.
        std::vector<std::uint8_t> mLoaded;
    };
}
