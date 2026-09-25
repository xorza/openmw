#pragma once

#include <cstdint>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "runs.hpp"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "surface.hpp"

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

    /// Whether what is behind a surface is meant to show through it. `AlphaMode::Blend` alone
    /// does not say so: Morrowind keeps its foliage under `NiAlphaProperty`, so a leaf card and a
    /// pane of glass carry the same mode, and what tells them apart is the surface's own alpha. An
    /// additive surface is neither: it covers nothing at any alpha. The one rule, over the three
    /// facts as the content states them, so a reading made off a description and a material made
    /// from it cannot answer differently.
    inline bool translucentSurface(const AlphaMode mode, const float opacity, const BlendKind blend)
    {
        return mode == AlphaMode::Blend && opacity < 1.0f && blend == BlendKind::Over;
    }

    /// Whether a surface adds to what is behind it and covers nothing — `BlendKind::Add` or
    /// `AddWhole` under a blend. The same rule over the same two facts, for the same reason.
    inline bool additiveSurface(const AlphaMode mode, const BlendKind blend)
    {
        return mode == AlphaMode::Blend && blend != BlendKind::Over;
    }

    /// What traversal is told about one placement: the facts of the material it wears, with the
    /// placement's own fade applied (`Material::Traversed::placedAt`). The one rule, which the
    /// counts that gate the trace and the instance records that drive traversal both read, so the
    /// two cannot disagree about a placement.
    struct PlacedTraversal
    {
        /// Whether traversal stops to ask whether a hit is a hole, where it would not stop for the
        /// placement anyway: a translucent one is stopped for and never ends the ray.
        bool mCutout = false;

        /// Whether traversal stops to ask how much of a hit there is. Earned by the material, for a
        /// pane, or by the placement, for an actor the game is fading — and never by a surface that
        /// adds, whose alpha weights what it adds rather than deciding how much of it is there.
        bool mTranslucent = false;

        bool mMedium = false;
        bool mAdditive = false;
    };

    /// How a surface is shaded, as the file says it. Vanilla textures are pre-lit, so `mDiffuse` is
    /// not an albedo yet.
    ///
    /// **The normal and specular maps are the content's companions, found by name.** A Morrowind
    /// NIF has no slot for either, so every one arrives as an `_n`, `_nh` or `_spec` file beside the
    /// diffuse, which `Shader::MapVisitor` attaches at load under the same `[Shaders]` switches
    /// the rasterizer reads. A surface that has none is vanilla, and vanilla pictures do not change:
    /// that is the rule these maps enter under.
    struct Material
    {
        MaterialKind mKind = MaterialKind::Surface;

        Index mDiffuse = sNoIndex;
        Index mEmissive = sNoIndex;

        /// A sphere-mapped sheet the surface adds past its albedo, indexed by where the eye is —
        /// a `NiTextureEffect`, or the caustic sheet an enchanted item shimmers with — and what it
        /// is tinted by, in linear light. `sNoIndex` for the surfaces that carry none, which is
        /// nearly all of them.
        Index mEnvironment = sNoIndex;
        osg::Vec3f mEnvironmentColour{ 1.0f, 1.0f, 1.0f };

        /// A map the albedo is multiplied by, read at the texture unit the content bound it at,
        /// because half the vanilla dark maps read the geometry's second set of coordinates and
        /// which set a unit reads is the mesh's to say — `GpuMesh::mUnitStreams`.
        Index mDark = sNoIndex;
        std::uint8_t mDarkUnit = 0;

        /// The companion maps, in the texture table's data encoding: a tangent-space normal, and a
        /// specular map in `SpecularLayout::MetalRoughness`. `sNoIndex` where the content has none,
        /// or, for the specular map, where the layout is `Ignore`.
        Index mNormal = sNoIndex;
        Index mSpecular = sNoIndex;

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

        AlphaMode mAlphaMode = AlphaMode::Opaque;

        /// How a blended surface composites: over what is behind it, or added to it. Meaningful
        /// under `AlphaMode::Blend`, and what tells a magic effect's sheet from a pane of glass.
        BlendKind mBlend = BlendKind::Over;

        /// What this surface's per-vertex colour is for — the tint that replaces `mDiffuseColour`,
        /// the glow that replaces `mEmissiveColour`, or nothing. On the material and not on the
        /// mesh, because a `NiVertexColorProperty` hangs above a shape and two shapes sharing one
        /// state set share the mode. What a mesh carries is the colours themselves.
        VertexColour mVertexColour = VertexColour::None;

        /// Both faces of this surface are drawn, which the content says by turning `GL_CULL_FACE`
        /// off. Vanilla says it by doubling a shape instead, and `InstanceRecord::mTwoSided` is
        /// where the two ways of saying it meet and a ray that draws reads them.
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

        /// Whether any of this chunk's layers reads a map: a normal map, or an authored albedo with
        /// its roughness beside it (`Shaders::LAYER_AUTHORED`). A layer run is the scene's and not
        /// the row's, so the placer that fills the run says so here, where `Traversed::mMapped`
        /// can read it.
        bool mLayersMapped = false;

        /// Whether a controller rewrites this material's state set every frame. Constant for the
        /// material's whole life, because `SceneExtractor::animate` gives a node with a controller
        /// a state set of its own. It is what refuses such a material to the replay, which reuses
        /// what it read last frame.
        bool mAnimated = false;

        /// Whether the diffuse map's alpha never reaches solid anywhere on it — `reachesSolid` —
        /// which is what separates a cloud from a pane among surfaces with the same alpha mode.
        /// False for a material with no diffuse map at all, which is an untextured pane.
        bool mDiffuseNeverSolid = false;

        /// What one texel of the diffuse map adds on average under this material's blend, in
        /// linear light: weighted by its own alpha where the blend reads one and whole where it
        /// does not — `meanTexel`. Read for an additive material and nothing else, because what
        /// asks is a magic effect's glow, and left at the untextured grey for one with no map,
        /// which is what its sheets are drawn with. Nought for a map nothing here can decode.
        osg::Vec3f mDiffuseMean = Shaders::NO_TEXTURE_ALBEDO;

        /// For telling a rewrite from a no-op: a state set with a controller on it is re-read every
        /// frame and usually says exactly what it said last time.
        bool operator==(const Material& other) const = default;

        /// What a blended material is tested against when it named no threshold of its own. Half,
        /// because Morrowind's masks are painted rather than anti-aliased, so the alpha is very
        /// nearly binary and the fringe a filter puts on it is a texel wide.
        static constexpr float sBlendCutoff = 0.5f;

        /// The alpha below which a texel is a hole, or zero where the surface has none. A blended
        /// material that never asked for a test gets `sBlendCutoff`, because that is where the game
        /// keeps its foliage.
        float getAlphaCutoff() const
        {
            switch (mAlphaMode)
            {
                case AlphaMode::Opaque:
                    return 0.0f;
                case AlphaMode::Cutout:
                    return mAlphaRef;
                case AlphaMode::Blend:
                    return mAlphaRef > 0.0f ? mAlphaRef : sBlendCutoff;
            }
            return 0.0f;
        }

        /// Every texture slot this material names in its own right, each once, whichever of them
        /// is set.
        ///
        /// **Beside the fields, because the list and the fields fall out of step nowhere else.**
        /// What keeps a slot alive is the material row that names it — the walk's own hold on an
        /// image goes on the frame after the material arrived — so a map missing from this list is
        /// a slot freed under a live material and handed to the next texture that arrives, and the
        /// surface then wears whatever took it. A map added to the struct is added here, and
        /// `MaterialTable` adds the layers' own to what this walks.
        template <class Visit>
        void forEachTexture(Visit visit) const
        {
            visit(mDiffuse);
            visit(mEmissive);
            visit(mEnvironment);
            visit(mDark);
            visit(mNormal);
            visit(mSpecular);
        }

        /// Whether traversal has to stop and ask this material whether a hit is a hole — the one
        /// predicate the build marks an instance non-opaque by and the shader tests against. A
        /// cutoff with no texture to sample is not one, and neither is an additive surface, whose
        /// alpha weights what it adds rather than deciding whether it is there.
        bool isCutout() const { return getAlphaCutoff() > 0.0f && mDiffuse != sNoIndex && !isAdditive(); }

        /// Whether this surface adds to what is behind it and covers nothing — `BlendKind::Add`
        /// or `AddWhole` under a blend. Such a surface is no pane and no mask: it is gathered by
        /// `additiveAlong` on a mask of its own and met by no other ray.
        bool isAdditive() const { return additiveSurface(mAlphaMode, mBlend); }

        /// Whether `mOpacity` is a number the trace reads at all: what the content asked a blend to
        /// weigh by, which is coverage where the surface covers and strength where it adds. An
        /// opaque or cutout surface is all there, and what its alpha holds is not a coverage.
        bool isBlended() const { return mAlphaMode == AlphaMode::Blend; }

        /// `translucentSurface` of this material. The two answers want opposite things from
        /// traversal — a mask averaged and tested is right for the leaf, light attenuated as it
        /// passes is right for the pane and turns the leaf to gauze. Not the opposite of
        /// `isCutout`, and a pane is both.
        bool isTranslucent() const { return translucentSurface(mAlphaMode, mOpacity, mBlend); }

        /// Whether the eye passes through this rather than meeting it: a medium, not a surface.
        /// Two facts and neither alone — the material's own alpha, which a leaf's does not say, and
        /// a texture whose paint never closes, which a pane's lead came does. Where both hold the
        /// layers are composited as depth along the ray — `mediumAlong`.
        bool isMedium() const { return isTranslucent() && mDiffuseNeverSolid; }

        /// What a placement keeps of the material it wears — what its row tells traversal, and what
        /// the scene's counts read — stated once because the record builder writes it into the row
        /// and `setMaterial` has to know whether a rewrite changed it.
        struct Traversed
        {
            MaterialKind mKind = MaterialKind::Surface;
            bool mCutout = false;
            bool mTranslucent = false;

            /// Whether the placements wearing this material go into the structure under
            /// `MASK_MEDIUM` as well, which is the one ray that gathers them.
            bool mMedium = false;

            /// Whether they go in under `MASK_ADDITIVE` and nothing else: seen by the one query
            /// that gathers what adds, and by no ray that shades, shadows or bounces.
            bool mAdditive = false;

            /// What `Material::mTwoSided` says, carried here because the row that culls by it is
            /// the placement's and not the material's.
            bool mTwoSided = false;

            /// Whether the material wears a normal map or a specular map: what the trace compiles
            /// the maps' code in for, and only for a scene that places such a material
            /// (`InstanceCounts::mMapped`).
            bool mMapped = false;

            bool operator==(const Traversed& other) const = default;

            /// What a placement wearing this at `opacity` tells traversal.
            PlacedTraversal placedAt(const float opacity) const
            {
                const bool translucent = !mAdditive && (opacity < 1.0f || mTranslucent);
                return PlacedTraversal{
                    .mCutout = mCutout && !translucent,
                    .mTranslucent = translucent,
                    .mMedium = mMedium,
                    .mAdditive = mAdditive,
                };
            }
        };

        Traversed getTraversed() const
        {
            return Traversed{
                .mKind = mKind,
                .mCutout = isCutout(),
                .mTranslucent = isTranslucent(),
                .mMedium = isMedium(),
                .mAdditive = isAdditive(),
                .mTwoSided = mTwoSided,
                .mMapped = mNormal != sNoIndex || mSpecular != sNoIndex || mLayersMapped,
            };
        }
    };

    /// One layer of a terrain material: a ground texture and the weights that place it. The
    /// device's own row, carried whole from the land record to the scene's layer run and the
    /// bake — `Shaders::GpuLayer` says what each field is. A fresh one is all nought, which is a
    /// layer with no texture and no transform: `wholeLayer` is what a reader starts from.
    using MaterialLayer = Shaders::GpuLayer;

    /// The weights a layer placed, as the run the mask table handed out — what is given back is
    /// what was taken, because the count is the grid's area. Here and not on the row, because the
    /// row is a shared header's and `Run` is the host's.
    inline Run maskOf(const MaterialLayer& layer)
    {
        return Run{ .mOffset = layer.mMaskOffset, .mCount = layer.mMaskWidth * layer.mMaskHeight };
    }

    /// A layer that covers the whole chunk at its cell coordinates and names no texture yet: no
    /// grid, and both transforms the identity `uv * (1, 1) + (0, 0)`. What every layer starts as
    /// before the land record says otherwise, because a value-initialised row would place its
    /// texture at one texel.
    inline MaterialLayer wholeLayer()
    {
        return MaterialLayer{
            .mDiffuse = sNoIndex,
            .mMaskOffset = 0,
            .mMaskWidth = 0,
            .mMaskHeight = 0,
            .mDiffuseTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f),
            .mMaskTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f),
            .mNormal = sNoIndex,
            .mFlags = 0,
        };
    }
}
