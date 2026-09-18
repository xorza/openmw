#pragma once

#include <cstdint>

#include <osg/Vec4f>

#include <components/rtx/material.hpp>
#include <components/rtx/runs.hpp>

namespace Rtx::Testing
{
    /// One terrain layer over a mask the scene already holds: `wholeLayer` with its texture, its
    /// mask and its grid filled in, and the identity transforms unless a tiling is named. A
    /// helper rather than a designated initialiser, because the row is the device's and a
    /// value-initialised transform places the texture at one texel.
    inline MaterialLayer layerOf(const Index diffuse, const Run mask = {}, const std::uint32_t maskWidth = 0,
        const std::uint32_t maskHeight = 0, const osg::Vec4f& diffuseTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f))
    {
        MaterialLayer layer = wholeLayer();
        layer.mDiffuse = diffuse;
        layer.mMaskOffset = mask.mOffset;
        layer.mMaskWidth = maskWidth;
        layer.mMaskHeight = maskHeight;
        layer.mDiffuseTransform = diffuseTransform;
        return layer;
    }
}
