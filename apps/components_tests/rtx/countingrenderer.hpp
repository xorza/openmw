#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>

namespace Rtx::Testing
{
    /// A renderer that records which of the three calls it was given, and nothing else.
    ///
    /// **The decision is what is under test, so nothing here draws.** What `extendScene` and
    /// `setScene` do with the descriptions has its own tests against a real device
    /// (`apps/components_tests/rtx/visibility/`); what nothing else covers is which of
    /// them a frame picks, and that answer is the same one on every machine.
    class CountingRenderer final : public Rtx::Renderer
    {
    public:
        std::string describeDevice() const override { return "a renderer that counts rather than draws"; }
        bool isValidating() const override { return false; }

        /// Counted rather than acted on: what a caller has to prove is that the discontinuity
        /// reaches the renderer at all, and this double has no history to throw away.
        void resetHistory() override { ++mHistoryResets; }

        void setScene(
            Rtx::SceneSlot slot, const Rtx::SceneDesc& scene, std::span<const Rtx::TextureData> textures) override
        {
            ++mRebuilt;
            mDescribed = textures.size();
            recordSlots(textures);

            // What the backend does: the array is made again and ends where the scene's table
            // does, whatever it held before.
            countAt(slot) = static_cast<std::uint32_t>(scene.textures().getPaths().size());
            heldAt(slot) = { true, scene.getStructureRevision() };
        }

        void extendScene(
            Rtx::SceneSlot slot, const Rtx::SceneDesc& scene, std::span<const Rtx::TextureData> arrived) override
        {
            ++mExtended;
            mDescribed = arrived.size();
            recordSlots(arrived);

            // **The array reaches the highest slot written, and is not a count of what arrived.**
            // A slot the table freed is handed out again, so an arrival can land below the end and
            // lengthen nothing at all.
            for (const Rtx::TextureData& one : arrived)
                countAt(slot) = std::max(countAt(slot), one.mSlot + 1);

            // The contract `extendScene` is given rather than one it checks: appending only the
            // arrivals has to leave the array exactly as long as the scene's table.
            mAppendedToWrongEnd |= countAt(slot) != scene.textures().getPaths().size();
            heldAt(slot).mRevision = scene.getStructureRevision();
        }

        void placeScene(Rtx::SceneSlot, const Rtx::SceneDesc&) override
        {
            ++mPlaced;
            mDescribed = 0;
        }

        Rtx::SceneHeld describeHeld(Rtx::SceneSlot slot) const override
        {
            const Built& built = heldAt(slot);
            return Rtx::SceneHeld{
                .mBuilt = built.mBuilt, .mStructureRevision = built.mRevision, .mTextureCount = countAt(slot)
            };
        }

        /// **The texture array does not shrink**, which is what `mTextures` staying put records: a
        /// slot goes on being where an append begins from whether or not it holds an image.
        void dropTextures(Rtx::SceneSlot, std::span<const Rtx::Index> slots) override
        {
            ++mDropCalls;
            mDropped.insert(mDropped.end(), slots.begin(), slots.end());
        }

        const Rtx::SceneStats& getSceneStats() const override { return mStats; }
        Rtx::MemoryReport getMemoryReport() const override { return {}; }
        void resize(std::uint32_t, std::uint32_t) override {}
        void setUpscale(Upscale upscale) override { mUpscale = upscale; }
        Upscale getUpscale() const override { return mUpscale; }
        void setSea(const Rtx::SeaState&) override {}
        void setVerticalSync(SDLUtil::VSyncMode) override {}
        Rtx::FrameExtents getExtents() const override { return {}; }
        Rtx::Reconstruction renderFrame(const Rtx::Shaders::VisibilityConstants&, const Rtx::FrameOptions&) override
        {
            return {};
        }
        std::optional<Rtx::FrameResult> finishFrame() override { return std::nullopt; }
        bool presentFrame() override { return true; }

        /// The GUI is not what this counts. Slots go up and nothing is drawn.
        Rtx::GuiSlot addGuiTexture(std::uint32_t, std::uint32_t) override { return Rtx::GuiSlot::at(mGuiTextures++); }
        std::span<std::uint8_t> lendGuiTexture(Rtx::GuiSlot, const Rtx::GuiRegion&) override { return {}; }
        void sendGuiTexture(Rtx::GuiSlot) override {}
        void dropGuiTexture(Rtx::GuiSlot) override {}
        void drawGui(std::span<const Rtx::GuiVertex>, std::span<const Rtx::GuiBatch>) override {}
        void traceGuiTexture(
            Rtx::GuiSlot, const Rtx::Shaders::VisibilityConstants&, const Rtx::GuiTraceOptions&) override
        {
        }
        Rtx::SceneSlot addViewScene() override
        {
            mViewTextures.push_back(0);
            mViewBuilt.emplace_back();
            return Rtx::SceneSlot::view(mViewScenes++);
        }

        /// What a slot was built from, which is what says whether an uploader may append.
        struct Built
        {
            bool mBuilt = false;
            std::uint64_t mRevision = 0;
        };

        Built& heldAt(Rtx::SceneSlot slot) { return slot.isWorld() ? mBuilt : mViewBuilt[slot.getViewIndex()]; }
        const Built& heldAt(Rtx::SceneSlot slot) const
        {
            return slot.isWorld() ? mBuilt : mViewBuilt[slot.getViewIndex()];
        }

        /// **A table a slot, as a real backend keeps.** An uploader that mixed the world's count
        /// with a doll's would begin one scene's descriptions inside the other's table, which is the
        /// overrun `aSecondSceneOnOneRendererIsBuiltRatherThanAppendedTo` exists for.
        std::uint32_t& countAt(Rtx::SceneSlot slot)
        {
            return slot.isWorld() ? mTextures : mViewTextures[slot.getViewIndex()];
        }

        /// The same for a caller that only reads, so that `describeHeld` needs no cast.
        std::uint32_t countAt(Rtx::SceneSlot slot) const
        {
            return slot.isWorld() ? mTextures : mViewTextures[slot.getViewIndex()];
        }

        void recordSlots(std::span<const Rtx::TextureData> described)
        {
            mDescribedSlots.clear();
            for (const Rtx::TextureData& texture : described)
                mDescribedSlots.push_back(texture.mSlot);
        }

        void dropViewScene(Rtx::SceneSlot) override {}
        bool takeGuiCopy(Rtx::GuiSlot, std::span<std::uint8_t>) override { return false; }
        void finishGuiTraces() override {}
        void readGuiTexture(Rtx::GuiSlot, std::vector<std::uint8_t>&) override {}
        void readPixels(std::vector<std::uint8_t>&) override {}
        void readChannel(Rtx::Channel, std::vector<float>&) override {}
        void readFrameImage(Rtx::FrameImage, std::vector<float>&) override {}
        void takeValidationErrors(std::vector<std::string>&) override {}

        std::uint32_t mHistoryResets = 0;

        /// Which slots the last hand-over described, in the order it described them.
        ///
        /// **What says the loader answered about this scene and not the last one.** `SceneTextures`
        /// is held by the uploader and cleared per arrival, so a buffer left unclear would show up
        /// here as a slot belonging to a scene that has gone.
        std::vector<std::uint32_t> mDescribedSlots;

        std::vector<std::uint32_t> mViewTextures;
        std::vector<Built> mViewBuilt;
        Built mBuilt;
        std::uint32_t mViewScenes = 0;
        std::uint32_t mPlaced = 0;
        std::uint32_t mExtended = 0;
        std::uint32_t mRebuilt = 0;

        /// How many descriptions the last call was handed, which is what says whether a texture
        /// already uploaded was decoded and shading-estimated a second time.
        std::size_t mDescribed = 0;

        std::uint32_t mTextures = 0;
        bool mAppendedToWrongEnd = false;

        /// Every texture slot given back, across every call, in the order it was named.
        std::vector<std::uint32_t> mDropped;
        std::uint32_t mDropCalls = 0;

    private:
        Rtx::SceneStats mStats;
        std::uint32_t mGuiTextures = 0;

        /// Whatever it was last told, since nothing here traces and no mode can be refused.
        Upscale mUpscale = Upscale::Off;
    };
}
