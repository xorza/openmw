#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include <components/rtx/memoryreport.hpp>
#include <components/rtx/result.hpp>

struct VmaAllocation_T;
struct VmaAllocator_T;
struct VmaPool_T;

namespace Rtx
{
    class MemoryAllocator;

    /// `value` rounded up to the next multiple of `alignment`.
    inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    /// Video memory the host writes into and the device reads — one statement, because
    /// `Buffer::hostWritten` asks for it, `MemoryAllocator::report` counts it, and a device
    /// offering no such type is refused.
    inline constexpr VkMemoryPropertyFlags sHostWritten = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /// What a range of memory is for, which says what becomes of the content where the device has
    /// no room for it — and so the order the room is given in. Each use stops where every use
    /// before it could be made once more: what the frame holds is what a change of mode makes
    /// again, and a table grows by making itself again while the frame behind reads the old one.
    enum class MemoryUse : std::uint8_t
    {
        /// What the frame cannot go without: its targets, its tables, and the geometry every hit
        /// reads. Nothing stands in for one, so it is never refused here, and a device that
        /// refuses it is the end of the renderer.
        Essential,

        /// A mesh's bottom-level structure. Refused, the mesh is left out, and every placement of
        /// it hits nothing.
        Structure,

        /// A texture's images. Refused, its slot draws the stand-in.
        Texture,
    };

    inline constexpr std::size_t sMemoryUses = static_cast<std::size_t>(MemoryUse::Texture) + 1;

    /// A range of one device allocation, and the allocator that hands it back. Not an allocation
    /// of its own: one `vkAllocateMemory` per image was 1554 for the cell the game starts in, most
    /// for a shading map of two kilobytes; sub-allocated, that cell takes a couple of dozen.
    class DeviceMemory
    {
    public:
        DeviceMemory() = default;
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        /// Written out, because a moved-from range must stop naming the allocation. Everything else
        /// here empties itself; a range the source still described would be given back twice.
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        VkDeviceSize getOffset() const { return mOffset; }

        /// This range, mapped, or null where the memory is not host-visible. Mapped for as long as
        /// the range stands, and memory need not be unmapped before it is freed.
        void* map() const { return mMapped; }

    private:
        friend class MemoryAllocator;

        DeviceMemory(MemoryAllocator* owner, VmaAllocation_T* allocation, VkDeviceMemory handle, VkDeviceSize offset,
            void* mapped, VkDeviceSize size, std::uint32_t heap, MemoryUse use);

        /// Gives the range back, where it holds one.
        void release();

        MemoryAllocator* mOwner = nullptr;
        VmaAllocation_T* mAllocation = nullptr;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
        void* mMapped = nullptr;

        /// What the range counts for in `MemoryAllocator::getHeld`, taken off again as it goes.
        VkDeviceSize mSize = 0;
        std::uint32_t mHeap = 0;
        MemoryUse mUse = MemoryUse::Essential;
    };

    /// Every `vkAllocateMemory` the renderer holds, and the ranges of them nothing is using:
    /// Vulkan Memory Allocator behind a face of this fork's own, so that a buffer and an image ask
    /// for room the same way and a report reads one set of figures. Every allocation carries the
    /// device-address flag, because every buffer here may be addressed; a host-visible range is
    /// mapped as it is made. Thread-safe, because `VisibilityPass::compileEvery` reaches `take`
    /// from a thread per core.
    class MemoryAllocator
    {
    public:
        /// @param memory the device's heaps and types, read once when the device was chosen. Kept
        ///        for the report, which says what each heap is beside what it holds.
        /// @param budget whether `VK_EXT_memory_budget` was enabled. Optional, because a driver
        ///        without it only leaves the ceilings on the library's own estimate — four fifths
        ///        of each heap, less what it holds itself — and the report without its budget.
        MemoryAllocator(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
            const VkPhysicalDeviceMemoryProperties& memory, bool budget);
        ~MemoryAllocator();

        MemoryAllocator(const MemoryAllocator&) = delete;
        MemoryAllocator& operator=(const MemoryAllocator&) = delete;

        /// Room for `buffer`, in memory that is `properties`, ready to bind, at the coarser of the
        /// driver's alignment for binding it and `alignment` — what the buffer's use owes beyond the
        /// bind, which `Buffer` reads off its usage. Essential, so never refused here: throws
        /// `DeviceError` where the device has no room, and `Unsupported` where it offers no memory
        /// type that is `properties`, which on hardware that meets the requirements means the
        /// request was wrong.
        DeviceMemory take(VkBuffer buffer, VkMemoryPropertyFlags properties, VkDeviceSize alignment);

        /// The same for an image, at the driver's alignment, which the allocator keeps its
        /// granularity away from a buffer's.
        DeviceMemory take(VkImage image, VkMemoryPropertyFlags properties);

        /// Room for `buffer` as `use`, or why there is none. Out of the blocks content already
        /// holds wherever it fits there, because that costs the heap nothing; otherwise out of new
        /// memory — a block, or an allocation of its own for a resource larger than half of one —
        /// only where the heap stays below `use`'s ceiling with it. Essential memory is `take`,
        /// which is never refused and throws instead, and so does every failure but the device
        /// having no room.
        ///
        /// **Content never shares a block with what the frame holds.** A range of content in a
        /// block the frame's own memory opened would take room the frame had already been given,
        /// past every ceiling; in blocks of their own, what each use holds and what it may take
        /// are both exact to the block.
        Result<DeviceMemory, std::string_view> tryTake(
            VkBuffer buffer, VkMemoryPropertyFlags properties, VkDeviceSize alignment, MemoryUse use);

        /// The same for an image.
        Result<DeviceMemory, std::string_view> tryTake(VkImage image, VkMemoryPropertyFlags properties, MemoryUse use);

        /// About how many bytes of video memory `use` may still take: what content's blocks have
        /// free, and the whole blocks its ceiling leaves room for. For a caller sizing what it asks
        /// for, and not a promise: a free range may be too small for the resource that asks, so
        /// `tryTake` has the last word.
        VkDeviceSize getRoom(MemoryUse use) const;

        /// What `use` holds on `heap`, the ranges and not the blocks around them.
        VkDeviceSize getHeld(std::uint32_t heap, MemoryUse use) const;

        /// The heap video memory is taken out of, whose room `getRoom` measures.
        std::uint32_t getVideoHeap() const { return mVideoHeap; }

        /// Holds every heap to `bytes` where the driver states more, or to the driver's word where
        /// nothing: for a run that asks what a smaller card does, and for a test that drives
        /// content past the room. The ceilings move with it and essential memory does not, which
        /// is never refused here.
        void limitBudget(std::optional<VkDeviceSize> bytes);

        /// Has the library ask the driver its budget again, which it otherwise does only every
        /// few dozen allocations. Before a decision about what content fits, so the decision is
        /// made against what the device says now.
        void refreshBudget(std::uint64_t frame);

        /// How many calls to `vkAllocateMemory` stand behind everything handed out.
        std::size_t getBlockCount() const;

        /// How many ranges stand, which is what a device may not be taken apart under.
        std::size_t getLiveCount() const;

        /// Every heap of the device, what this allocator took out of each, and what the driver says
        /// is left. Walks every allocation for the host-written figures, so it is asked once a place
        /// and never once a frame.
        MemoryReport report() const;

    private:
        friend class DeviceMemory;

        /// `tryTake`'s one path for either resource: `allocate` asks the library with the flags it
        /// is handed, and `size` and `typeBits` are the resource's requirements. `own` for a
        /// resource the driver will only bind to an allocation of its own.
        template <class Allocate>
        Result<DeviceMemory, std::string_view> tryAllocate(VkDeviceSize size, std::uint32_t typeBits, bool own,
            VkMemoryPropertyFlags properties, MemoryUse use, Allocate&& allocate);

        /// What the library placed, as a range counted for `use`.
        DeviceMemory hold(VmaAllocation_T* allocation, VkDeviceMemory handle, VkDeviceSize offset, void* mapped,
            VkDeviceSize size, std::uint32_t type, MemoryUse use);

        /// Frees what `memory` holds and takes it off what its use holds.
        void give(DeviceMemory& memory);

        /// How far up `heap` a use after the first may go, from the library's figures for it: the
        /// budget, less what every use before `use` holds and less what the process holds outside
        /// the library — the swapchain, the upscaler's own memory and the driver's, which the frame
        /// cannot do without either. What is held is owed once more, for the reason `MemoryUse`
        /// gives.
        VkDeviceSize ceilingOf(
            std::uint32_t heap, MemoryUse use, VkDeviceSize budget, VkDeviceSize usage, VkDeviceSize blockBytes) const;

        VkDevice mDevice = VK_NULL_HANDLE;
        const VkPhysicalDeviceMemoryProperties& mMemory;

        /// Whether `report` may ask the driver what this process holds, rather than only counting
        /// what it asked for itself.
        bool mBudget = false;

        VmaAllocator_T* mAllocator = nullptr;

        /// The type video memory is taken out of, and its heap, which is the one a use's room is
        /// measured on.
        std::uint32_t mVideoType = 0;
        std::uint32_t mVideoHeap = 0;

        /// Content's blocks, one pool for each type of video memory, every block `sBlockBytes`: a
        /// size of the pool's own, so new memory is exactly a block. Null for the other types,
        /// which content never asks for.
        std::array<VmaPool_T*, VK_MAX_MEMORY_TYPES> mContentPools{};

        /// What each use holds on each heap. Atomic, for the threads `take` is reached from.
        std::array<std::array<std::atomic<VkDeviceSize>, sMemoryUses>, VK_MAX_MEMORY_HEAPS> mHeld{};

        std::optional<VkDeviceSize> mBudgetLimit;
    };
}
