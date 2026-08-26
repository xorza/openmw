#pragma once

#include <cstdint>
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
    /// (`apps/components_tests/rtx/visibilitypass.cpp`); what nothing else covers is which of
    /// them a frame picks, and that answer is the same one on every machine.
    class CountingRenderer final : public Rtx::Renderer
    {
    public:
        std::string describeDevice() const override { return "a renderer that counts rather than draws"; }
        bool isValidating() const override { return false; }

        /// Counted rather than acted on: what a caller has to prove is that the discontinuity
        /// reaches the renderer at all, and this double has no history to throw away.
        void resetHistory() override { ++mHistoryResets; }

        std::uint32_t mHistoryResets = 0;

        void setScene(std::uint32_t slot, const Rtx::SceneDesc& scene, std::span<const Rtx::TextureData> textures,
            const Rtx::SeaState&) override
        {
            ++mRebuilt;
            mDescribed = textures.size();

            // What the backend does: the array is made again and ends where the scene's table
            // does, whatever it held before.
            countAt(slot) = static_cast<std::uint32_t>(scene.getTextures().size());
        }

        void extendScene(std::uint32_t slot, const Rtx::SceneDesc& scene, std::span<const Rtx::TextureData> arrived,
            const Rtx::SeaState&) override
        {
            ++mExtended;
            mDescribed = arrived.size();
            countAt(slot) += static_cast<std::uint32_t>(arrived.size());

            // The contract `extendScene` is given rather than one it checks: appending only the
            // arrivals has to leave the array exactly as long as the scene's table.
            mAppendedToWrongEnd |= countAt(slot) != scene.getTextures().size();
        }

        void placeScene(std::uint32_t, const Rtx::SceneDesc&, const Rtx::SeaState&) override
        {
            ++mPlaced;
            mDescribed = 0;
        }

        std::uint32_t getTextureCount(std::uint32_t slot) const override
        {
            return const_cast<CountingRenderer*>(this)->countAt(slot);
        }

        /// The slots the scene gave back, in the order it named them, across every call.
        ///
        /// **The array does not shrink**, which is what `mTextures` staying put records: a slot goes
        /// on being where an append begins from whether or not it holds an image.
        void dropTextures(std::uint32_t, std::span<const std::uint32_t> slots) override
        {
            ++mDropCalls;
            mDropped.insert(mDropped.end(), slots.begin(), slots.end());
        }

        const Rtx::SceneStats& getSceneStats() const override { return mStats; }
        void resize(std::uint32_t, std::uint32_t) override {}
        Rtx::FrameExtents getExtents() const override { return {}; }
        Rtx::FrameResult renderFrame(const Rtx::Shaders::VisibilityConstants&, const Rtx::FrameOptions&) override
        {
            return {};
        }
        Rtx::SharedFrame shareFrame() override { return {}; }
        bool presentFrame() override { return true; }

        /// The GUI is not what this counts. Slots go up and nothing is drawn.
        std::uint32_t addGuiTexture(std::uint32_t, std::uint32_t) override { return mGuiTextures++; }
        void writeGuiTexture(std::uint32_t, const Rtx::Renderer::GuiRegion&, std::span<const std::uint8_t>) override {}
        void dropGuiTexture(std::uint32_t) override {}
        void drawGui(std::span<const Rtx::GuiVertex>, std::span<const Rtx::GuiBatch>) override {}
        void traceGuiTexture(
            std::uint32_t, const Rtx::Shaders::VisibilityConstants&, const Rtx::GuiTraceOptions&) override
        {
        }
        std::uint32_t addViewScene() override
        {
            mViewTextures.push_back(0);
            return mViewScenes++;
        }

        /// **A table a slot, as a real backend keeps.** An uploader that mixed the world's count
        /// with a doll's would begin one scene's descriptions inside the other's table, which is the
        /// overrun `aSecondSceneOnOneRendererIsBuiltRatherThanAppendedTo` exists for.
        std::uint32_t& countAt(std::uint32_t slot) { return slot == Rtx::sWorld ? mTextures : mViewTextures[slot]; }

        std::vector<std::uint32_t> mViewTextures;
        void dropViewScene(std::uint32_t) override {}
        void readGuiTexture(std::uint32_t, std::vector<std::uint8_t>&) override {}
        void readPixels(std::vector<std::uint8_t>&) override {}
        void readChannel(Rtx::Channel, std::vector<float>&) override {}
        void takeValidationErrors(std::vector<std::string>&) override {}

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
    };
}
