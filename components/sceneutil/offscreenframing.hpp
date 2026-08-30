#pragma once

#include <osg/Vec3f>
#include <osg/Vec4f>

namespace SceneUtil
{
    // What the game frames and lights its two fixed offscreen pictures with — the character preview
    // and one tile of the local map.
    //
    // **One description and two renderers.** The rasterizer draws both of these and the ray tracer
    // draws them again, `openmw-rtxtool doll` and `map` being how the second is looked at beside the
    // first. A number each would make the comparison answer a question about the framing rather than
    // about the light transport, which is the one thing it exists to answer.

    /// A directional light with no position, which is all an offscreen picture is lit by.
    struct FlatLight
    {
        osg::Vec3f mDirection;
        osg::Vec4f mDiffuse;
        osg::Vec4f mAmbient;
    };

    /// Where a picture of one figure is taken from.
    struct PreviewCamera
    {
        osg::Vec3f mOrigin;
        osg::Vec3f mTarget;
    };

    /// What every character preview is drawn with: the inventory doll and the race selection both.
    inline constexpr float sPreviewFieldOfView = 12.3f;
    inline constexpr float sPreviewNear = 4.0f;
    inline constexpr float sPreviewFar = 10000.0f;

    /// The inventory doll's own size, which the race selection does not share.
    inline constexpr int sInventoryWidth = 512;
    inline constexpr int sInventoryHeight = 1024;

    /// Seven hundred units in front of the figure, at the height of its head.
    PreviewCamera inventoryCamera();

    /// What the inventory is lit by, out of Morrowind's own four `Inventory_Directional*` keys.
    FlatLight inventoryLight();

    /// The near plane one map tile is drawn with. How far it sees is the caller's: the game fits
    /// the segment it is about to draw, and nothing else knows that extent.
    inline constexpr float sMapNear = 5.0f;

    /// Flat and from nowhere in particular: a chart is read for what is where, and a sun angle that
    /// made shadows would only make it harder to read.
    FlatLight mapLight();
}
