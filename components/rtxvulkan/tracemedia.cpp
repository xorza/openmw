#include "tracemedia.hpp"

#include <cmath>
#include <cstdint>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>

#include "devicescene.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    TraceMedia::TraceMedia(const Device& device, const std::filesystem::path& shaders)
        : mWaves(device, shaders)
        , mRipples(device, shaders)
        , mFog(device)
        , mNoSprites(Buffer::hostWritten(
              device, 2 * sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "no sprites"))
    {
        // `SPRITE_LIST_UNBINNED` and a count of nought are both nought.
        mNoSprites.clear();
    }

    VisibilityInputs TraceMedia::describe(const DeviceScene& held, const Shaders::VisibilityConstants& camera,
        const Image& shown, const Buffer& counts, const Buffer& sunGlare, const FrameSlot traceSlot) const
    {
        return VisibilityInputs{
            .mScene = held.getAcceleration().getTopLevel(),
            .mBuffers = &held.getBuffers(),
            .mSlot = held.getSlot(),
            .mTraceSlot = traceSlot,
            .mCounts = &counts,
            .mIndexBlocks = held.getAcceleration().getIndexBlocks(),
            .mPoseBlocks = held.getAcceleration().getPoseBlocks(held.getSlot()),
            .mPreviousPoseBlocks = held.getAcceleration().getPreviousPoseBlocks(held.getSlot()),
            .mTextures = held.getTextures(),
            .mTextureTexels = held.getTextureTexels(),
            .mWaves = &mWaves,
            .mRipples = &mRipples,
            .mFog = &mFog,
            .mSpriteList = (camera.mRayMask & Shaders::MASK_PARTICLE) != 0 ? 0 : mNoSprites.addressFor(),
            .mShown = &shown,
            .mSunGlare = &sunGlare,
            .mSea = held.getCounts().mWater > 0 || !std::isinf(camera.mWaterLevel),
            .mMapped = held.getCounts().mMapped > 0,
        };
    }

    void TraceMedia::keepRipples(const SceneDesc& scene)
    {
        mImpulses.assign(scene.ripples().begin(), scene.ripples().end());
    }

    void TraceMedia::stepRipples(const VkCommandBuffer commands, const FrameSlot slot, const osg::Vec2f& eye,
        const double skySeconds, GpuTimer* const timer)
    {
        mRipples.record(commands, slot, mImpulses, eye, skySeconds, timer);
    }

    void TraceMedia::placeRipples(Shaders::VisibilityConstants& sampled) const
    {
        sampled.mRippleOrigin = mRipples.getOrigin();
        sampled.mRippleExtent = RipplePass::getExtent();
    }
}
