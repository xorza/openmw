#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// One side of an image barrier: the layout an image is in, and the stage and access that last
    /// touched it or will next. A transition names two of these rather than seven flags, and the
    /// pairs a pass reaches for are named once below.
    struct ImageUse
    {
        VkImageLayout mLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 mStage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 mAccess = VK_ACCESS_2_NONE;
    };

    /// One side of a buffer barrier, or of a memory barrier over everything: the stage and access
    /// that last touched it or will next. `Buffer::describeBarrier` takes two, as `Image`'s takes
    /// two `ImageUse`s, and `memoryBarrier` takes two over no resource in particular.
    struct BufferUse
    {
        VkPipelineStageFlags2 mStage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 mAccess = VK_ACCESS_2_NONE;
    };

    namespace Use
    {
        /// Nothing before this: the first write into a fresh image, or a discard at the start of a
        /// frame. A discard waits for nothing of its own because the head barrier every command
        /// buffer opens with (`CommandPool::begin`) has already ordered it after whatever the last
        /// frame did.
        inline constexpr ImageUse sUndefined{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };

        inline constexpr ImageUse sComputeWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr ImageUse sComputeRead{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
        inline constexpr ImageUse sComputeSample{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
        inline constexpr ImageUse sComputeReadWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr ImageUse sComputeReadOrSample{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };

        inline constexpr ImageUse sTraceWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr ImageUse sTraceRead{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
        inline constexpr ImageUse sTraceReadWrite{ VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr ImageUse sTraceSample{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };

        /// Sampled by the trace and by a dispatch alike, in `GENERAL`: what the wave tiles and the
        /// fog volume's slices are left as.
        inline constexpr ImageUse sShaderSample{ VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };

        /// Loaded by the trace and by a dispatch alike, in `GENERAL`.
        inline constexpr ImageUse sShaderStorageRead{ VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT };

        /// Sampled as a texture from the layout a sampler wants, by the trace and by a dispatch.
        inline constexpr ImageUse sTextureSample{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };

        inline constexpr ImageUse sTransferWrite{ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
        inline constexpr ImageUse sCopyWrite{ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT };
        inline constexpr ImageUse sCopyRead{ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT };
        inline constexpr ImageUse sBlitWrite{ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT };

        /// A level about to be blitted over whole, holding whatever the last frame left in it.
        inline constexpr ImageUse sDiscardForBlit{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_BLIT_BIT, 0 };
        inline constexpr ImageUse sBlitRead{ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT };
        inline constexpr ImageUse sClearWrite{ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT };

        inline constexpr ImageUse sFragmentSample{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
        inline constexpr ImageUse sColourAttachment{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };

        /// Handed to the presentation engine, which reads it at no stage a barrier can name.
        inline constexpr ImageUse sPresent{ VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            0 };

        /// Whatever came before or comes after, in `GENERAL`: the widest dependency, for an image
        /// handed between owners that do not know each other.
        inline constexpr ImageUse sAnyGeneral{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
        inline constexpr ImageUse sAnyGeneralWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT };
        inline constexpr ImageUse sAnyGeneralRead{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT };

        // The buffer sides, named with the resource in them because the image sides above share
        // the namespace. `vkCmdFillBuffer` and `vkCmdUpdateBuffer` are filed under the clear stage
        // by the specification, which is why a fill and an inline write are `sBufferClearWrite`.
        inline constexpr BufferUse sBufferCopyWrite{ VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
        inline constexpr BufferUse sBufferClearWrite{ VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
        inline constexpr BufferUse sBufferComputeRead{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
        inline constexpr BufferUse sBufferComputeWrite{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr BufferUse sBufferComputeReadWrite{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };

        /// Read by the trace and by a dispatch alike, and written by both: the sprite table a
        /// launch shelters before a dispatch shades.
        inline constexpr BufferUse sBufferShaderRead{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
        inline constexpr BufferUse sBufferShaderReadWrite{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        inline constexpr BufferUse sBufferUniformRead{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_UNIFORM_READ_BIT };

        /// Read back on the host after a wait. A wait makes nothing visible to the host — its access
        /// scope holds device access only — so the host's read has to be named where the write is.
        inline constexpr BufferUse sBufferHostRead{ VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT };

        /// Whatever comes next reads it: the widest destination, for a copy whose reader is a
        /// build, a trace or a dispatch recorded after it in the same batch.
        inline constexpr BufferUse sBufferAnyRead{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT };

        /// Whatever came before, reading or writing: the widest source, for a table the queue last
        /// touched two frames ago in a way this pass does not know.
        inline constexpr BufferUse sBufferAnyReadWrite{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
    }

    /// The dependency between one use of `count` levels of `image` from `base` and the next —
    /// `Image::describeTransition` for an image that is not an `Image`, which a swapchain's are.
    constexpr VkImageMemoryBarrier2 imageBarrier(const VkImage image, const std::uint32_t base,
        const std::uint32_t count, const ImageUse& from, const ImageUse& to)
    {
        return VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = from.mStage,
            .srcAccessMask = from.mAccess,
            .dstStageMask = to.mStage,
            .dstAccessMask = to.mAccess,
            .oldLayout = from.mLayout,
            .newLayout = to.mLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1 },
        };
    }

    /// A memory barrier over everything: the barrier `handOver` records, as a value a `Barriers`
    /// can take beside image and buffer ones.
    constexpr VkMemoryBarrier2 memoryBarrier(const BufferUse& from, const BufferUse& to)
    {
        return VkMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = from.mStage,
            .srcAccessMask = from.mAccess,
            .dstStageMask = to.mStage,
            .dstAccessMask = to.mAccess,
        };
    }
}
