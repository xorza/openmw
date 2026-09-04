#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Math>
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

    /// A texture laid once across a quad, in the same corner order.
    inline const std::array<osg::Vec2f, 4> sQuadUv{
        osg::Vec2f(0.0f, 0.0f),
        osg::Vec2f(1.0f, 0.0f),
        osg::Vec2f(1.0f, 1.0f),
        osg::Vec2f(0.0f, 1.0f),
    };

    /// The unit square in the xy plane, its first corner at the origin, wound the way
    /// `sQuadIndices` reads it.
    inline const std::array<osg::Vec3f, 4> sUnitQuad{
        osg::Vec3f(0.0f, 0.0f, 0.0f),
        osg::Vec3f(1.0f, 0.0f, 0.0f),
        osg::Vec3f(1.0f, 1.0f, 0.0f),
        osg::Vec3f(0.0f, 1.0f, 0.0f),
    };

    /// The unit right triangle in the xy plane, its right angle at the origin, its second corner
    /// along x and its third along y.
    inline const std::array<osg::Vec3f, 3> sUnitTriangle{
        osg::Vec3f(0.0f, 0.0f, 0.0f),
        osg::Vec3f(1.0f, 0.0f, 0.0f),
        osg::Vec3f(0.0f, 1.0f, 0.0f),
    };

    /// One triangle, wound the way its corners were listed.
    inline constexpr std::array<std::uint32_t, 3> sTriangleIndices{ 0, 1, 2 };

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

    /// A square in the xz plane at y = `away`, `halfExtent` from the axis on every side. Its face
    /// points along -Y, back at an eye standing on the negative side and looking along +Y, which is
    /// where every camera in these tests stands.
    inline std::array<osg::Vec3f, 4> uprightQuadAt(float halfExtent, float away)
    {
        return {
            osg::Vec3f(-halfExtent, away, -halfExtent),
            osg::Vec3f(halfExtent, away, -halfExtent),
            osg::Vec3f(halfExtent, away, halfExtent),
            osg::Vec3f(-halfExtent, away, halfExtent),
        };
    }

    /// One such square at y = 0, four hundred units across, which is larger than any frame at the
    /// distances most of these tests use.
    inline const std::array<osg::Vec3f, 4> sWallQuad = uprightQuadAt(200.0f, 0.0f);

    /// A wall across the view `away` units ahead of an eye at the origin looking along +Y, and
    /// behind it where `away` is negative — so a frame either hits every pixel or none, which is
    /// what tells the frames apart.
    ///
    /// Sixteen thousand units across, which fills the frame from anywhere these cameras stand.
    inline std::array<osg::Vec3f, 4> wallAt(float away)
    {
        return uprightQuadAt(8000.0f, away);
    }

    /// Half the width a sixty-degree frame covers a hundred units from the eye.
    ///
    /// **Derived rather than pinned, because it is a fact about the camera and not a choice.** A
    /// card built to it exactly fills the frame, which is what the tests that use it are built on:
    /// each quadrant of the card's texture is then a quadrant of the picture, and the seams fall
    /// between pixel columns and rows rather than on them.
    inline const float sCardHalfExtent = 100.0f * std::tan(osg::DegreesToRadians(30.0f));

    /// That card, at y = `away`, so an eye a hundred units in front of it sees nothing else.
    inline std::array<osg::Vec3f, 4> cardAt(float away)
    {
        return uprightQuadAt(sCardHalfExtent, away);
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
