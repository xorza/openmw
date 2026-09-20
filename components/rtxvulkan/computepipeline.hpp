#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "pipeline.hpp"

namespace Rtx
{
    class Device;

    /// The map entries a table of specialization words needs, and the `VkSpecializationInfo` over
    /// them. Built here rather than by the caller, because it is the same table every time and
    /// its contents are the caller's own indices: `constant_id` `i` takes word `i`, at word `i`'s
    /// offset. Words because that is what every constant this renderer specializes on is — a `bool`
    /// reaches SPIR-V as a 32-bit value like a `uint` does. The words are copied, so nothing
    /// outlives the object but what `getInfo` points at, which is the object's own.
    class Specialization
    {
    public:
        /// @param words the pipeline's, one per constant from nought.
        /// @param more a stage's own after them, for a module compiled into several stages under
        ///        constants of its own — `TraceShaders::mHit`.
        explicit Specialization(std::span<const std::uint32_t> words, std::span<const std::uint32_t> more = {})
            : mWords(words.begin(), words.end())
        {
            mWords.insert(mWords.end(), more.begin(), more.end());

            mEntries.resize(mWords.size());
            for (std::uint32_t at = 0; at < mEntries.size(); ++at)
                mEntries[at] = VkSpecializationMapEntry{ at, at * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                    sizeof(std::uint32_t) };

            mInfo = VkSpecializationInfo{
                .mapEntryCount = static_cast<std::uint32_t>(mEntries.size()),
                .pMapEntries = mEntries.data(),
                .dataSize = mWords.size() * sizeof(std::uint32_t),
                .pData = mWords.data(),
            };
        }

        Specialization(const Specialization&) = delete;
        Specialization& operator=(const Specialization&) = delete;

        /// What a stage's `pSpecializationInfo` takes, or null where nothing was specialized.
        const VkSpecializationInfo* getInfo() const { return mEntries.empty() ? nullptr : &mInfo; }

    private:
        std::vector<std::uint32_t> mWords;
        std::vector<VkSpecializationMapEntry> mEntries;
        VkSpecializationInfo mInfo{};
    };

    /// A compute pipeline and its layout. `TracePipeline` is the same object for a launch.
    class ComputePipeline : public Pipeline
    {
    public:
        /// Neither span outlives the call.
        ///
        /// @param pushConstantBytes the whole range, at offset zero, visible to the compute stage.
        /// @param laterSets layouts bound after set zero. A pipeline layout has to name every set
        ///        it will ever be handed.
        /// @param module the compiled SPIR-V the build wrote, by path.
        /// @param name what a capture calls the pipeline.
        /// @param specialization one word per specialization constant, `constant_id` `i` taking
        ///        `specialization[i]` — a `bool` reaches SPIR-V as a 32-bit value like a `uint`.
        ComputePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::uint32_t pushConstantBytes, std::span<const VkDescriptorSetLayout> laterSets,
            const std::filesystem::path& module, std::string_view name,
            std::span<const std::uint32_t> specialization = {});
    };
}
