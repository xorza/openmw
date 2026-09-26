#pragma once

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/mipchainpass.hpp>
#include <components/rtxvulkan/shadingpass.hpp>
#include <components/rtxvulkan/spritelightpass.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "harness.hpp"

namespace Rtx::Testing
{
    /// The three passes a texture is made with, over the test's device, and the bundle an array
    /// or a `Texture` takes them as: what every test that stands a texture needs and none is
    /// about.
    struct TexturePassSet
    {
        explicit TexturePassSet(const Device& device)
            : mChain(device, getShaderDirectory())
            , mShading(device, getShaderDirectory())
            , mBake(device, getShaderDirectory())
            , mPasses{ mChain, mShading, mBake }
        {
        }

        MipChainPass mChain;
        ShadingPass mShading;
        SpriteLightPass mBake;
        TexturePasses mPasses;
    };
}
