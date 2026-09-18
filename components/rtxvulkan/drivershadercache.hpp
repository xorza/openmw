#pragma once

namespace Rtx
{
    /// Keeps the driver's own shader disk cache out of this process, for a process whose pictures
    /// are compared, so that every pipeline it makes is a compile of the SPIR-V. The other half is
    /// that such a process keeps no `PipelineCache` of its own either: `RtxRenderer` says where.
    ///
    /// **A host calls it at start, before its window, and not a `RendererOptions` field.** The
    /// driver reads the word as it loads, and the `SDL_WINDOW_VULKAN` window loads it: measured,
    /// with the call moved to `createVulkanRenderer`, after `RtxRenderer` had its window, `shot`
    /// wrote fourteen megabytes into a fresh `__GL_SHADER_DISK_CACHE_PATH` although the default
    /// took, where the call at start writes nothing. A renderer is made after the window, so the
    /// seam is too late to say it. Named apart from `instance.hpp` so that a host compiles no
    /// Vulkan header for it.
    ///
    /// **Measured, because a pipeline out of a cache is not the compile's code.** Over
    /// `one-cell-walk` with the world held identical, a run whose launches came out of the
    /// driver's disk cache drew 59 of 360 pictures a part in 255 apart from a run that compiled
    /// them — single pixels anywhere in the frame, each one a value the two codes round across a
    /// byte's edge differently. Five cold compiles in five processes agreed to the byte, as did
    /// every run off the disk cache; and a run that loaded the fork's own blob started on the
    /// compile's code and was switched to the cache's a few seconds in, at frame 36, 41, 62 or
    /// 336 of its walk, the driver finishing the same work in the background. Those 59 pictures
    /// from frame 13, and the switches, are what the gate's repeat pairs failed on after a
    /// rebuild. A compile is one code and stays it, over thirty seconds of frames.
    ///
    /// The shell's word stands over this default, and the answer says whether it did: a run
    /// under a shell that turned the cache on is a run that may not repeat. The cache is
    /// NVIDIA's `__GL_SHADER_DISK_CACHE`, which its Vulkan driver reads as its GL one does —
    /// measured on Linux; whether the Windows driver reads it is not, which is what
    /// `VulkanRenderer` holds the launches' creation times against.
    bool refuseDriverShaderCache();

    /// Whether the driver was told to keep its shader disk cache out, by this process or by the
    /// shell: the word as the driver reads it, which is what `VulkanRenderer` holds the driver
    /// to when the launches come back too fast to have been compiled.
    bool driverShaderCacheRefused();
}
