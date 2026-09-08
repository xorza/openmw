#pragma once

#include <cstdint>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/surface/alphamode.hpp>

#include "index.hpp"
#include "run.hpp"

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

    /// How a surface is shaded, as recovered from the model.
    ///
    /// Vanilla textures are pre-lit, so `mDiffuse` is not an albedo yet; recovering one is M9. What
    /// is here is what the file says.
    struct Material
    {
        MaterialKind mKind = MaterialKind::Surface;

        Index mDiffuse = sNoIndex;
        Index mNormal = sNoIndex;
        Index mEmissive = sNoIndex;

        osg::Vec4f mDiffuseColour{ 1.0f, 1.0f, 1.0f, 1.0f };

        /// How much the surface glows on its own, with the material's own multiplier folded in.
        ///
        /// The multiplier is not kept apart because nothing wants it apart: the game's own shader
        /// only ever uses their product, and carrying two numbers would be carrying one of them for
        /// the sake of it.
        osg::Vec3f mEmissiveColour{ 0.0f, 0.0f, 0.0f };

        float mAlphaRef = 0.0f;

        Surface::AlphaMode mAlphaMode = Surface::AlphaMode::Opaque;

        /// Sheet geometry lit and hit from both faces. Morrowind leans on this heavily and a ray
        /// tracer has to be told, because back-face culling is not free the way a rasterizer's is.
        bool mTwoSided = false;

        /// Mesh texture coordinates to this material's, as `uv * xy + zw` — the same form the
        /// terrain layers use, so one sampler helper serves both.
        ///
        /// **A surface whose shading animates by scrolling.** Morrowind moves lava, waterfalls,
        /// banners and smoke by rewriting a texture matrix rather than by moving geometry, and
        /// `NifOsg::UVController` rewrites it every frame — 432 surfaces in Vivec alone. Held on the
        /// material rather than the instance because that is what changes: the same mesh under two
        /// controllers is two materials and one geometry, and `setMaterial` is built for shading
        /// that moves.
        osg::Vec4f mTextureTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

        /// Where this material's terrain layers sit in the scene's layer table.
        ///
        /// Empty for everything that is not terrain, which is all but a handful of materials in a
        /// cell — so the layered path costs the rest of them one comparison and no indirection.
        Run mLayers;

        /// Whether this chunk is wide enough that its stack is worth flattening into one texture.
        ///
        /// **Asked for here and answered later, which is the whole of why it is a flag.** A
        /// composite costs tens of milliseconds and a cell boundary wants several, so the bake
        /// cannot be done by the walk that meets the chunk. Until one arrives `mDiffuse` stays
        /// unset and the chunk shades from the stack below — the branch the shader already takes for
        /// every near chunk — so the picture is right throughout and only the cost per hit differs.
        bool mFlatten = false;

        /// Whether a controller rewrites this material's state set every frame, so what it says
        /// now is not what it will say next frame.
        ///
        /// **Constant for the material's whole life**, because it is a fact about the state set the
        /// material is keyed on: `SceneExtractor::animate` gives a node with a controller a state
        /// set of its own, and every material read off that state set is read off it again each
        /// frame. A backend bakes nothing against a mask that scrolls — the bake is against what
        /// the texture coordinates land on, and a `UVController` moves that every frame — so this
        /// is what refuses one. `MeshRange::mMaterial` says why a mesh has one material to ask.
        bool mAnimated = false;

        /// Whether the diffuse map's alpha never reaches solid anywhere on it — `reachesSolid`.
        ///
        /// **A fact about the texture, kept on the material because the material is what asks.**
        /// It is what separates a cloud from a pane among surfaces that carry the same alpha mode
        /// and the same kind of alpha, and it is measured once for an image however many materials
        /// name it.
        ///
        /// False for a material with no diffuse map at all, which is an untextured pane: all glass,
        /// no paint, and a surface wherever it stands.
        bool mDiffuseNeverSolid = false;

        /// Two materials are the same when every field is.
        ///
        /// **For telling a rewrite from a no-op.** A state set with a controller on it is re-read
        /// every frame and usually says exactly what it said last time; treating that as a change
        /// would write the row to the device for nothing.
        bool operator==(const Material& other) const = default;

        /// The alpha below which a texel is a hole, or zero where the surface has none.
        ///
        /// A blended material that never asked for a test gets a stand-in, because that is where
        /// the game keeps its foliage. Right for a leaf and wrong for a pane of glass, until
        /// ordered transparency gives the second one somewhere else to go.
        float getAlphaCutoff() const;

        /// Whether traversal has to stop and ask this material whether a hit is a hole.
        ///
        /// The one predicate: the build marks an instance non-opaque by this and the shader tests
        /// against the same cutoff, so the two cannot disagree about which triangles reach the
        /// candidate loop. A cutoff with no texture to sample is not one — the mask lives in the
        /// diffuse map's alpha and there is nothing else to read.
        bool isCutout() const { return getAlphaCutoff() > 0.0f && mDiffuse != sNoIndex; }

        /// Whether what is behind this surface is meant to show through it.
        ///
        /// **`Surface::AlphaMode::Blend` alone does not say so, and this is the whole difficulty.** Morrowind
        /// keeps its foliage under `NiAlphaProperty`, so a leaf card and a pane of glass carry the
        /// same mode: the leaf is fully opaque where its painted mask is opaque, and the pane is
        /// translucent everywhere. What tells them apart is the *material's* own alpha, which
        /// `NiMaterialProperty` records and `NifOsg::AlphaController` animates.
        ///
        /// Told apart because the two want opposite answers from traversal. A mask averaged over the
        /// ray cone and tested is right for the leaf and wrong for the pane; light attenuated as it
        /// passes is right for the pane and turns the leaf to gauze.
        ///
        /// **Not the opposite of `isCutout`, and a pane is both.** `getAlphaCutoff` hands a blended
        /// material a stand-in threshold, so the build marks a pane non-opaque and traversal stops
        /// for it — which is what a transmittance needs anyway. A reader deciding what to do with a
        /// candidate asks this one first.
        bool isTranslucent() const { return mAlphaMode == Surface::AlphaMode::Blend && mDiffuseColour.a() < 1.0f; }

        /// Whether the eye passes through this rather than meeting it: a medium, not a surface.
        ///
        /// **Two facts, and neither alone.** The material's own alpha says the content meant to be
        /// seen through it everywhere, which a leaf's does not. The texture says the paint never
        /// closes anywhere on it, which a pane's lead came does. Where both hold there is nothing
        /// for a ray to stop on, and the layers are composited as depth along it — `mediumAlong`.
        bool isMedium() const { return isTranslucent() && mDiffuseNeverSolid; }

        /// What a placement's row tells traversal about the material it wears.
        ///
        /// **Stated once, because two things read it.** The record builder puts these three answers
        /// into the acceleration structure's row, and `setMaterial` has to know whether a rewrite
        /// changed any of them — a fade crossing opaque does, a flipbook turning does not — so the
        /// placements wearing the material can be rewritten. Two lists of the same three fields
        /// would drift.
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

    /// One layer of a terrain material: a ground texture and the weights that place it.
    ///
    /// Morrowind's ground is a stack of tiling textures, each masked by a small grid of weights that
    /// `ESMTerrain` derives from the land records — and OpenMW draws that stack as one alpha-blended
    /// pass per layer over the same triangles. A ray tracer has one hit and shades it once, so the
    /// stack is read back into layers and summed at the hit instead.
    struct MaterialLayer
    {
        /// The ground texture, which tiles many times across a chunk.
        Index mDiffuse = sNoIndex;

        /// This layer's weights in the scene's mask table, and the grid they form.
        ///
        /// An empty run means the layer covers everything: a chunk of a single ground type is given
        /// no mask at all, because there is nothing for it to blend against. The run holds
        /// `mMaskWidth * mMaskHeight` weights, and it is kept rather than rebuilt from the sides so
        /// that what is given back is what was taken.
        Run mMask;
        std::uint16_t mMaskWidth = 0;
        std::uint16_t mMaskHeight = 0;

        /// Chunk texture coordinates to this layer's, as `uv * xy + zw`.
        ///
        /// Read off the texture matrices the terrain builder attached rather than recomputed: the
        /// mask's carries a half-texel inset and a nudge that exist to match the original game, and
        /// deriving them again from the tile size is how the two quietly stop agreeing.
        osg::Vec4f mDiffuseTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
        osg::Vec4f mMaskTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

        /// Two layers are the same when every field is, which is what says a chunk still stands
        /// where a bake of it began.
        bool operator==(const MaterialLayer& other) const = default;
    };
}
