#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/guirenderer.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/sdlutil/vsyncmode.hpp>

namespace Rtx::Testing
{
    /// A renderer that records what it was given.
    ///
    /// **The decision is what is under test, so nothing here draws.** What `extendScene` and
    /// `setScene` do with the descriptions has its own tests against a real device
    /// (`apps/components_tests/rtx/visibility/`); what nothing else covers is which of
    /// them a frame picks, and that answer is the same one on every machine.
    class CountingRenderer final : public Rtx::Renderer
    {
    public:
        std::string describeDevice() const override { return "a renderer that counts rather than draws"; }
        const Rtx::RenderProfile& getProfile() const override { return mProfile; }
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
            countAt(slot) = static_cast<std::uint32_t>(scene.textures().getRows().size());
            heldAt(slot) = { true, scene.getIdentity(), scene.getStructureRevision() };
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
            mAppendedToWrongEnd |= countAt(slot) != scene.textures().getRows().size();
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
            return Rtx::SceneHeld{ .mBuilt = built.mBuilt,
                .mIdentity = built.mIdentity,
                .mStructureRevision = built.mRevision,
                .mTextureCount = countAt(slot) };
        }

        /// What the device refused, which a test fills to be what the next hand-over answers.
        std::span<const Rtx::Refusal> getRefusals(Rtx::SceneSlot) const override { return mRefusing; }

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
        void setUpscale(Upscale upscale) override { mProfile.mUpscaling.mMode = upscale; }
        void setVerticalSync(SDLUtil::VSyncMode) override {}
        bool pacesFrames() const override { return false; }
        void setPacing(const Rtx::Pacing&) override {}
        void awaitFrame() override {}
        void endSimulation(bool) override {}
        std::optional<Rtx::LatencyReport> describeLatency() const override { return std::nullopt; }
        Rtx::FrameExtents getExtents() const override { return {}; }
        Rtx::Reconstruction renderFrame(const Rtx::Shaders::VisibilityConstants&, const Rtx::FrameOptions&) override
        {
            ++mFrames;
            return {};
        }
        void skipFrame() override { ++mSkipped; }
        std::uint64_t getFrameCount() const override { return mFrames + mSkipped; }
        std::optional<Rtx::FrameResult> finishFrame() override { return std::nullopt; }
        std::optional<Rtx::FrameResult> collectFrame() override { return std::nullopt; }
        void presentFrame() override {}

        /// Slots go up, what a texture is sent is kept for a test to read, and nothing is drawn.
        Rtx::GuiSlot addGuiTexture(std::uint32_t, std::uint32_t) override { return Rtx::GuiSlot::at(mGuiTextures++); }
        std::span<std::uint8_t> lendGuiTexture(Rtx::GuiSlot, const Rtx::GuiRegion& region) override
        {
            mLent.push_back(region);
            mLending.assign(std::size_t{ region.mWidth } * region.mHeight * 4, 0);
            return mLending;
        }
        void sendGuiTexture(Rtx::GuiSlot) override { ++mGuiSent; }
        void dropGuiTexture(Rtx::GuiSlot) override {}
        void drawGui(std::span<const Rtx::GuiVertex>, std::span<const Rtx::GuiBatch>) override {}
        void traceGuiTexture(
            Rtx::GuiSlot, const Rtx::Shaders::VisibilityConstants& camera, const Rtx::GuiTraceOptions&) override
        {
            mTraced = camera;
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
            std::uint64_t mIdentity = 0;
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

        void dropViewScene(Rtx::SceneSlot slot) override { mViewDropped.push_back(slot.getViewIndex()); }
        bool takeGuiCopy(Rtx::GuiSlot, std::span<std::uint8_t>) override { return false; }
        void finishGuiTraces() override {}
        void readPixels(std::vector<std::uint8_t>&) override {}

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

        /// Every view scene given back, in the order it was, so a test can say a slot went back
        /// once and no more.
        std::vector<std::uint32_t> mViewDropped;

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
        std::uint64_t mFrames = 0;
        std::uint64_t mSkipped = 0;
        std::uint32_t mDropCalls = 0;

        /// What `getRefusals` answers, as a device short of room would after a hand-over.
        std::vector<Rtx::Refusal> mRefusing;

        /// What `getProfile` answers, which a test sets to say what the run decided.
        Rtx::RenderProfile mProfile;

        /// Every rectangle of a GUI texture lent, in order, the bytes written into the last one,
        /// and how many were sent back: what a mirror of a picture the game holds sent the device.
        std::vector<Rtx::GuiRegion> mLent;
        std::vector<std::uint8_t> mLending;
        std::uint32_t mGuiSent = 0;

        /// The constants the last picture inside the interface was traced with, or nothing before
        /// the first: what says a picture was traced under the run's rules.
        std::optional<Rtx::Shaders::VisibilityConstants> mTraced;

    private:
        Rtx::SceneStats mStats;
        std::uint32_t mGuiTextures = 0;
    };
}
