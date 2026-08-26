#pragma once

#include <osg/PositionAttitudeTransform>
#include <osg/ref_ptr>

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    /// `MWRender::Mask_Water`, which `SceneExtractor::setWaterMask` has to be told before a walk can
    /// tell the sea from anything else.
    ///
    /// **The whole of the mask and not a bit inside it.** A node mask is a filter over passes and
    /// its default is every bit set, so what names the water is that no *other* pass may see it.
    constexpr unsigned int sWaterMask = 1u << 6;

    /// The world's water, as one plane the way the game builds it.
    ///
    /// **Not a quad per cell.** `MWRender::Water` makes a single sheet a hundred and fifty cells
    /// across and moves it to whichever cell the camera stands in, so the sea reaches the horizon
    /// whatever is loaded — and the mirror finds it by walking the graph, exactly as it finds the
    /// game's. A harness that placed a footprint per cell instead was measuring caustics, the
    /// glitter path and every shoreline against a surface the game does not have, and had to hold
    /// each quad against the sweep and let go of it by hand when its cell left.
    class WaterPlane
    {
    public:
        /// Builds the sheet and hangs it under `root`. It is there whether or not the cell it opens
        /// on has water; `follow` is what decides.
        explicit WaterPlane(osg::Group& root);

        WaterPlane(const WaterPlane&) = delete;
        WaterPlane& operator=(const WaterPlane&) = delete;

        /// Moves the sheet to `cell` and returns where its surface now is, or minus infinity where
        /// the cell holds no water — which everything downstream reads as a depth that is never
        /// positive.
        ///
        /// **An exterior's sea is at zero and centred on the cell**, because the sheet is finite and
        /// a camera that walked far enough would otherwise reach its edge. An interior's pool sits
        /// at the height its record names, over the origin, which is where a room's geometry is.
        float follow(const ESM::Cell& cell);

    private:
        osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
    };
}
