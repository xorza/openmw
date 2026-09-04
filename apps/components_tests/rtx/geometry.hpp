#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/index.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/skinning.h>

namespace Rtx::Testing
{
    /// Two triangles of a quad, wound so its face points the way its corners were listed.
    inline constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

    /// The unit square, in the same corner order — a texture laid once across a quad.
    inline const std::array<osg::Vec2f, 4> sQuadUv{
        osg::Vec2f(0.0f, 0.0f),
        osg::Vec2f(1.0f, 0.0f),
        osg::Vec2f(1.0f, 1.0f),
        osg::Vec2f(0.0f, 1.0f),
    };

    /// A level square of `extent` about the origin at height `z`, facing up.
    inline std::array<osg::Vec3f, 4> sheetAt(float extent, float z)
    {
        return {
            osg::Vec3f(-extent, -extent, z),
            osg::Vec3f(extent, -extent, z),
            osg::Vec3f(extent, extent, z),
            osg::Vec3f(-extent, extent, z),
        };
    }

    /// A square in the xz plane at y = 0, facing along -Y, four hundred units across, which is
    /// larger than any frame at the distances most of these tests use.
    inline const std::array<osg::Vec3f, 4> sWallQuad{
        osg::Vec3f(-200.0f, 0.0f, -200.0f),
        osg::Vec3f(200.0f, 0.0f, -200.0f),
        osg::Vec3f(200.0f, 0.0f, 200.0f),
        osg::Vec3f(-200.0f, 0.0f, 200.0f),
    };

    /// A wall across the view `away` units ahead of an eye at the origin looking along +Y, and
    /// behind it where `away` is negative — so a frame either hits every pixel or none, which is
    /// what tells the frames apart.
    ///
    /// Sixteen thousand units across, which fills the frame from anywhere these cameras stand.
    inline std::array<osg::Vec3f, 4> wallAt(float away)
    {
        return {
            osg::Vec3f(-8000.0f, away, -8000.0f),
            osg::Vec3f(8000.0f, away, -8000.0f),
            osg::Vec3f(8000.0f, away, 8000.0f),
            osg::Vec3f(-8000.0f, away, 8000.0f),
        };
    }

    /// A skin of one bone over `vertices` vertices, every weight one, so a pose is the bone's own
    /// transform and nothing else — what a test expects is what it moved the bone by.
    ///
    /// A run word is `first << RUN_COUNT_BITS | count`, and every vertex here names the one
    /// influence at nought: a word of one.
    inline Index addOneBoneRig(SceneDesc& scene, std::uint32_t vertices)
    {
        const std::vector<std::uint32_t> runs(vertices, 1u);
        const std::array influences{ Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f } };
        return scene.addRig(runs, influences, 1);
    }

    /// Poses `mesh`, a mesh on a one-bone rig, by `bone`, with the box its bind pose reaches
    /// carried through the same transform.
    inline void poseByOneBone(SceneDesc& scene, Index mesh, const osg::Matrixf& bone)
    {
        osg::BoundingBoxf reach;
        for (const osg::Vec3f& vertex : scene.getMeshPositions(mesh))
            reach.expandBy(vertex * bone);

        const std::array rows{ toGpuBone(bone) };
        scene.poseRig(mesh, rows, reach);
    }
}
