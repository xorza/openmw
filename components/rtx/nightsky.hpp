#pragma once

#include <array>
#include <cstdint>

#include <osg/Vec3f>

#include "scenedesc.hpp"
#include "shaders/visibility.h"

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    /// Morrowind's night sky, read off the mesh the rasterizer draws it with.
    ///
    /// **Seven layers and only one of them is the stars.** `Models/skynight01` holds a star field
    /// over the whole dome and six more meshes over patches of it — three nebulae, which are most of
    /// what gives a Morrowind night its colour, and the warrior, the mage and the thief. A renderer
    /// drawing the field alone draws stars on black.
    ///
    /// **Read rather than transcribed.** Every number here comes out of the file at load: where each
    /// patch sits and how wide it is, how much sky one tile of the field's sheet covers, and how far
    /// down the field fades out. Writing them down instead would have been a table that a replaced
    /// mesh makes silently wrong, and this renderer does not own that mesh.
    struct NightSky
    {
        /// One painted patch: a sheet laid once across a piece of the sky.
        struct Patch
        {
            Index mTexture = sNoIndex;

            /// Where its middle points, and how far it reaches from there in radians.
            osg::Vec3f mDirection{ 0.0f, 0.0f, 1.0f };
            float mAngularRadius = 0.0f;
        };

        /// The field over the whole dome, and the sheet it is painted with.
        Index mField = sNoIndex;

        /// How much sky one tile of that sheet covers, in radians.
        ///
        /// **The unwrap is isotropic**, which is what keeps a star round: the mesh advances its
        /// texture by the same amount for a degree of azimuth as for a degree of elevation. This is
        /// the median of that rate over every edge of it, so a mesh that disagrees with itself
        /// somewhere still lands where most of it says.
        float mTile = 0.0f;

        /// The elevation the field fades out below, in radians.
        ///
        /// `MWRender::ModVertexAlphaVisitor` draws a vertex of that dome only where its authored
        /// colour is white, so the ring the engine keeps is the lowest one this reaches.
        float mHorizon = 0.0f;

        std::array<Patch, Shaders::SKY_PATCH_COUNT> mPatches;

        /// What every sheet on this mesh adds to the sky's mean radiance, linear, at full fade.
        ///
        /// **The night sky as a light and not as a picture.** A bounce that escapes takes `skyGlow`,
        /// which had the dome and nothing laid over it — so a star lit nothing, and neither did the
        /// three nebulae that are most of what gives a Morrowind night its colour. This is what they
        /// are worth summed: each sheet's own mean texel times the share of the hemisphere it covers.
        ///
        /// **A mean rather than the sheets themselves**, because a cosine lobe is a hemisphere and a
        /// ray is a direction. A coarse mip would average a sheet near where a ray points, which is
        /// still a different answer for every ray and still a fetch apiece; the mean over the whole
        /// sky is the term a diffuse gather actually wants, it is exact for a sheet as near uniform
        /// as these are, and it is read once at load.
        osg::Vec3f mGlow;
    };

    /// Reads it, adding every texture it names to `scene` and holding them there.
    ///
    /// A missing or unreadable mesh comes back with nothing in it, which draws no night sky rather
    /// than failing: the file is content and content is what a mod replaces.
    NightSky readNightSky(SceneDesc& scene, Resource::SceneManager& scenes);
}
