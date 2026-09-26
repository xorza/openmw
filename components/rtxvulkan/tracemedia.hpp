#pragma once

#include <filesystem>
#include <vector>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/ripple.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/wavespectrum.hpp>

#include "buffer.hpp"
#include "fogvolume.hpp"
#include "frameslots.hpp"
#include "ripplepass.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    class Device;
    class DeviceScene;
    class GpuTimer;
    class Image;
    class SceneDesc;
    struct VisibilityInputs;

    /// What every trace reads beside its scene and its chain: the sea, the wake in it, the fog's
    /// field and the list of no sprites. One of each for everything traced, the doll and the map
    /// included, because none of them is a property of a scene.
    class TraceMedia
    {
    public:
        TraceMedia(const Device& device, const std::filesystem::path& shaders);

        /// What a trace of `held` under `camera` reads, for the copy its last placement wrote, and
        /// what its launches bind beside it — but for the chain's own images, which
        /// `TraceChain::record` names. One description for a frame and for a picture inside the
        /// interface, which differ in the arguments alone.
        ///
        /// @param shown what the puffs are composited over.
        /// @param counts the census the launches sum into.
        /// @param sunGlare the fader's counts, `DisplayChain::getGlareCounts`.
        /// @param traceSlot which of the chain's slots the trace takes.
        VisibilityInputs describe(const DeviceScene& held, const Shaders::VisibilityConstants& camera,
            const Image& shown, const Buffer& counts, const Buffer& sunGlare, FrameSlot traceSlot) const;

        /// What the sea's amplitudes were last drawn for.
        const SeaState& getSea() const { return mWaves.getSea(); }

        /// Draws another sea's amplitudes. Nothing may be in flight: `WavePass::describe` says why.
        void describeSea(const SeaState& sea) { mWaves.describe(sea); }

        /// Copies what the world's scene says disturbed the water, at the two places the world's
        /// scene passes through: built and placed. A copy and not a span, because the scene's list
        /// is cleared by the next walk and nothing here would say so.
        void keepRipples(const SceneDesc& scene);

        /// Drops the wake, for a world that was replaced rather than moved through.
        void resetRipples() { mRipples.reset(); }

        /// Steps the wake under the world's frame, anchored at `eye`, and presses in what
        /// `keepRipples` kept. Only for a trace with a sea: a frame with none leaves the tiles as
        /// they were.
        ///
        /// @param timer null where the run is not being timed.
        void stepRipples(
            VkCommandBuffer commands, FrameSlot slot, const osg::Vec2f& eye, double skySeconds, GpuTimer* timer);

        /// Tells `sampled` where the wake's field lies, as the last step left it.
        void placeRipples(Shaders::VisibilityConstants& sampled) const;

    private:
        WavePass mWaves;
        RipplePass mRipples;

        /// Refilled and never freed.
        std::vector<RippleImpulse> mImpulses;

        /// Drawn once for the life of the device. Nothing about it turns on the weather or the cell
        /// — those decide the extinction and the layer's height, which are numbers the shader
        /// already has.
        FogTile mFog;

        /// An empty sprite tiles' list, for a camera that draws no sprites and so binned none.
        /// `VisibilityInputs::mSpriteList` says why one buffer serves every extent.
        Buffer mNoSprites;
    };
}
