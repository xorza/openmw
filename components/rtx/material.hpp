#pragma once

#include <cstdint>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/surface/material.hpp>

#include "runs.hpp"

namespace Rtx
{
    /// What shading a hit takes, which is not a variation on one path but three different ones.
    enum class MaterialKind
    {
        /// One diffuse texture over a lit surface, which is nearly everything in the game.
        Surface,

        /// A stack of tiling ground textures, each masked by its own grid of weights.
        Terrain,

        /// Water, which has no albedo at all: it reflects, refracts and absorbs, and its colour is
        /// what is behind and above it rather than anything of its own.
        Water,
    };

    /// How a surface is shaded, as the file says it. Vanilla textures are pre-lit, so `mDiffuse` is
    /// not an albedo yet.
    struct Material
    {
        MaterialKind mKind = MaterialKind::Surface;

        Index mDiffuse = sNoIndex;
        Index mNormal = sNoIndex;
        Index mEmissive = sNoIndex;

        /// What the texture is tinted by, in linear light. Three channels and not the record's
        /// four: the alpha beside it is `mOpacity` and is not a colour.
        osg::Vec3f mDiffuseColour{ 1.0f, 1.0f, 1.0f };

        /// How much the surface glows on its own, with the material's own multiplier folded in,
        /// because the game's own shader only ever uses their product.
        osg::Vec3f mEmissiveColour{ 0.0f, 0.0f, 0.0f };

        /// How much of the surface is there, as the content stated it and before its texture is
        /// read. One for everything that is all there, which is nearly everything.
        float mOpacity = 1.0f;

        float mAlphaRef = 0.0f;

        Surface::AlphaMode mAlphaMode = Surface::AlphaMode::Opaque;

        /// What this surface's per-vertex colour is for — the tint that replaces `mDiffuseColour`,
        /// the glow that replaces `mEmissiveColour`, or nothing. On the material and not on the
        /// mesh, because a `NiVertexColorProperty` hangs above a shape and two shapes sharing one
        /// state set share the mode. What a mesh carries is the colours themselves.
        Surface::VertexColour mVertexColour = Surface::VertexColour::None;

        /// Sheet geometry lit and hit from both faces. Morrowind leans on this heavily and a ray
        /// tracer has to be told, because back-face culling is not free the way a rasterizer's is.
        bool mTwoSided = false;

        /// Mesh texture coordinates to this material's, as `uv * xy + zw` — the same form the
        /// terrain layers use. Morrowind moves lava, waterfalls, banners and smoke by rewriting a
        /// texture matrix every frame — 432 surfaces in Vivec alone — so this is on the material,
        /// which `setMaterial` rewrites, and not on the instance.
        osg::Vec4f mTextureTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

        /// Where this material's terrain layers sit in the scene's layer table. Empty for everything
        /// that is not terrain, which is all but a handful of materials in a cell, so the layered
        /// path costs the rest of them one comparison and no indirection.
        Run mLayers;

        /// Whether this chunk is wide enough that its stack is worth flattening into one texture.
        /// Asked for here and answered later, because a composite costs tens of milliseconds; until
        /// one arrives `mDiffuse` stays unset and the chunk shades from the stack, so only the cost
        /// per hit differs.
        bool mFlatten = false;

        /// Whether a controller rewrites this material's state set every frame. Constant for the
        /// material's whole life, because `SceneExtractor::animate` gives a node with a controller
        /// a state set of its own. It is what refuses such a material to the replay, which reuses
        /// what it read last frame.
        bool mAnimated = false;

        /// Whether the diffuse map's alpha never reaches solid anywhere on it — `reachesSolid` —
        /// which is what separates a cloud from a pane among surfaces with the same alpha mode.
        /// False for a material with no diffuse map at all, which is an untextured pane.
        bool mDiffuseNeverSolid = false;

        /// For telling a rewrite from a no-op: a state set with a controller on it is re-read every
        /// frame and usually says exactly what it said last time.
        bool operator==(const Material& other) const = default;

        /// The alpha below which a texel is a hole, or zero where the surface has none. A blended
        /// material that never asked for a test gets a stand-in, because that is where the game
        /// keeps its foliage.
        float getAlphaCutoff() const;

        /// Whether traversal has to stop and ask this material whether a hit is a hole — the one
        /// predicate the build marks an instance non-opaque by and the shader tests against. A
        /// cutoff with no texture to sample is not one.
        bool isCutout() const { return getAlphaCutoff() > 0.0f && mDiffuse != sNoIndex; }

        /// Whether what is behind this surface is meant to show through it. `AlphaMode::Blend`
        /// alone does not say so: Morrowind keeps its foliage under `NiAlphaProperty`, so a leaf
        /// card and a pane of glass carry the same mode, and what tells them apart is the
        /// *material's* own alpha. The two want opposite answers from traversal — a mask averaged
        /// and tested is right for the leaf, light attenuated as it passes is right for the pane
        /// and turns the leaf to gauze. Not the opposite of `isCutout`, and a pane is both.
        bool isTranslucent() const { return mAlphaMode == Surface::AlphaMode::Blend && mOpacity < 1.0f; }

        /// Whether the eye passes through this rather than meeting it: a medium, not a surface.
        /// Two facts and neither alone — the material's own alpha, which a leaf's does not say, and
        /// a texture whose paint never closes, which a pane's lead came does. Where both hold the
        /// layers are composited as depth along the ray — `mediumAlong`.
        bool isMedium() const { return isTranslucent() && mDiffuseNeverSolid; }

        /// What a placement's row tells traversal about the material it wears, stated once because
        /// the record builder writes it into the row and `setMaterial` has to know whether a
        /// rewrite changed it.
        struct Traversed
        {
            MaterialKind mKind = MaterialKind::Surface;
            bool mCutout = false;
            bool mTranslucent = false;

            /// Whether the placements wearing this material go into the structure under
            /// `MASK_MEDIUM` as well, which is the one ray that gathers them.
            bool mMedium = false;

            bool operator==(const Traversed& other) const = default;
        };

        Traversed getTraversed() const
        {
            return Traversed{
                .mKind = mKind, .mCutout = isCutout(), .mTranslucent = isTranslucent(), .mMedium = isMedium()
            };
        }
    };

    /// One layer of a terrain material: a ground texture and the weights that place it. OpenMW
    /// draws the stack as one alpha-blended pass per layer; a ray tracer has one hit and sums the
    /// layers at it instead.
    struct MaterialLayer
    {
        /// The ground texture, which tiles many times across a chunk.
        Index mDiffuse = sNoIndex;

        /// This layer's weights in the scene's mask table, and the grid they form. An empty run
        /// means the layer covers everything. The run holds `mMaskWidth * mMaskHeight` weights and
        /// is kept rather than rebuilt from the sides, so that what is given back is what was taken.
        Run mMask;
        std::uint16_t mMaskWidth = 0;
        std::uint16_t mMaskHeight = 0;

        /// Cell texture coordinates to this layer's, as `uv * xy + zw`. `GroundReader` derives both
        /// from the tile count as `Terrain::createPasses` does, and a test holds the numbers.
        osg::Vec4f mDiffuseTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
        osg::Vec4f mMaskTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

        /// Two layers are the same when every field is, which is what says a chunk still stands
        /// where a bake of it began.
        bool operator==(const MaterialLayer& other) const = default;
    };
}
