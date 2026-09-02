#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// The map entries a table of specialization words needs, and the `VkSpecializationInfo` over
    /// them.
    ///
    /// **Built here rather than by the caller**, because it is the same table every time and its
    /// contents are the caller's own indices: `constant_id` `i` takes word `i`, at word `i`'s
    /// offset. Words because that is what every constant this renderer specializes on is — a `bool`
    /// reaches SPIR-V as a 32-bit value like a `uint` does.
    ///
    /// Does not outlive the words it was made from, and neither does what `getInfo` points at.
    class Specialization
    {
    public:
        explicit Specialization(std::span<const std::uint32_t> words)
            : mEntries(words.size())
        {
            for (std::uint32_t at = 0; at < mEntries.size(); ++at)
                mEntries[at] = VkSpecializationMapEntry{ at, at * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                    sizeof(std::uint32_t) };

            mInfo = VkSpecializationInfo{
                .mapEntryCount = static_cast<std::uint32_t>(mEntries.size()),
                .pMapEntries = mEntries.data(),
                .dataSize = words.size_bytes(),
                .pData = words.data(),
            };
        }

        Specialization(const Specialization&) = delete;
        Specialization& operator=(const Specialization&) = delete;

        /// What a stage's `pSpecializationInfo` takes, or null where nothing was specialized.
        const VkSpecializationInfo* getInfo() const { return mEntries.empty() ? nullptr : &mInfo; }

    private:
        std::vector<VkSpecializationMapEntry> mEntries;
        VkSpecializationInfo mInfo{};
    };
}
