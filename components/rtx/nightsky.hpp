#pragma once

#include <array>
#include <cstdint>

#include <osg/Vec3f>

#include <components/vfs/pathutil.hpp>

#include "runs.hpp"
#include "shaders/visibility.h"

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    class SceneDesc;

    /// Morrowind's night sky, read off the mesh the rasterizer draws it with: a star field over
    /// the whole dome and six more sheets over patches of it — three nebulae, which are most of
    /// what gives a Morrowind night its colour, and the warrior, the mage and the thief. Read at
    /// load rather than transcribed, because a replaced mesh would make a table silently wrong.
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

        /// How much sky one tile of that sheet covers, in radians — the unwrap is isotropic, which
        /// is what keeps a star round. The median of that rate over every edge, so a mesh that
        /// disagrees with itself somewhere still lands where most of it says.
        float mTile = 0.0f;

        /// The elevation the field fades out below, in radians.
        ///
        /// `MWRender::ModVertexAlphaVisitor` draws a vertex of that dome only where its authored
        /// colour is white, so the ring the engine keeps is the lowest one this reaches.
        float mHorizon = 0.0f;

        std::array<Patch, Shaders::SKY_PATCH_COUNT> mPatches;

        /// What every sheet on this mesh adds to the sky's mean radiance, linear, at full fade:
        /// each sheet's mean texel times the share of the hemisphere it covers. A bounce that
        /// escapes takes `skyGlow`, which had the dome and nothing laid over it, so the nebulae lit
        /// nothing. A mean rather than the sheets, because a cosine lobe is a hemisphere and the
        /// mean over the whole sky is the term a diffuse gather wants — exact for sheets this near
        /// uniform, and read once at load.
        osg::Vec3f mGlow;
    };

    /// Reads it, adding every texture it names to `scene` and holding them there. A missing or
    /// unreadable mesh comes back with nothing in it, because the file is content.
    ///
    /// @param mesh the star dome the configuration names.
    /// @param fallback the dome to read where the archives hold no `mesh`: Tribunal ships the
    ///        second one and Morrowind alone does not, and the rasterizer picks by the same test.
    NightSky readNightSky(SceneDesc& scene, Resource::SceneManager& scenes, VFS::Path::NormalizedView mesh,
        VFS::Path::NormalizedView fallback);
}
