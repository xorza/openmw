#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/vfs/pathutil.hpp>

#include "index.hpp"
#include "meshinstance.hpp"
#include "placementtable.hpp"
#include "shaders/scene.h"
#include "shaders/skinning.h"
#include "shapefold.hpp"
#include "slotchanges.hpp"
#include "slotset.hpp"
#include "spanallocator.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// How a mesh's vertices are re-posed every frame, where they are.
    ///
    /// **The kind names the kernel and the pose it takes.** A skinned body is posed by bone rows
    /// through a rig, a morphed face by target weights through a morph, and a mesh that stands is
    /// neither. The loader never makes a geometry both: `NifOsg` skips the morpher where a skin
    /// exists.
    enum class Deform : std::uint8_t
    {
        None,
        Rig,
        Morph,
    };

    /// Where one mesh's vertices and indices sit in the scene's shared buffers.
    ///
    /// The buffers are shared rather than per-mesh because a cell holds thousands of meshes and a
    /// `vector` of `vector`s would pay an allocation for each one — and because the GPU wants one
    /// buffer anyway, so a per-mesh vector would only have to be flattened again on the way up.
    struct MeshRange
    {
        Index mVertexOffset = 0;
        Index mVertexCount = 0;
        Index mIndexOffset = 0;
        Index mIndexCount = 0;

        /// What the fold found this mesh's triangles to be. `Rtx::FoldedShape` says what each half
        /// means; the scene keeps them and draws nothing from them.
        FoldedShape mShape;

        /// Whether this mesh is re-posed by `poseRig` or `poseMorph` — a skinned body, a morphed
        /// face — which is what tells a backend to build its structure so it can be refitted rather
        /// than built again, and which kernel poses it. The caller's finding, like `mShape`.
        Deform mDeform = Deform::None;

        /// The rig or the morph that poses it, into `getRigs` or `getMorphs`. `sNoIndex` for a mesh
        /// that stands.
        Index mDeformer = sNoIndex;

        /// The material this mesh arrived wearing, or `sNoIndex` for one that arrived with none.
        ///
        /// **A mesh's, and not a placement's, because a static mesh wears one material by
        /// construction.** `SceneUtil::CopyOp` copies nodes and shares drawables and state sets, so
        /// a hundred crates are a hundred nodes over one drawable and one state set — the material
        /// the extractor keys on is the same object under every placement. What a backend bakes
        /// against the mask a mesh is worn with, it bakes against this; a placement wearing
        /// another is `ExtractionStats::mWornOtherwise`, and the loader says there is none. The
        /// caller's finding, like `mShape`.
        Index mMaterial = sNoIndex;

        /// Where this mesh's bind pose sits among the deforming meshes' vertices, which is what a
        /// backend's bind table is indexed by. **A run of `mVertexCount` beside the mesh's own**,
        /// allocated only for a mesh that deforms: the shared vertex buffers hold every mesh, and a
        /// bind table that mirrored them would hold megabytes of the cell for a few bodies.
        Index mBindOffset = 0;

        /// Where this mesh's bone rows or morph weights start in `getBones` or `getWeights`. The
        /// count is the rig's or the morph's.
        Index mPoseOffset = 0;

        /// Whether a pose has been written since the mesh arrived. The first pose names the mesh
        /// whatever it is, so a body whose first pose happens to equal the zeroed rows still
        /// reaches the device.
        bool mPosed = false;

        /// The box this mesh's vertices fit in, in the space they are stated in. Invalid where the
        /// slot is free.
        ///
        /// **Written where the vertices are, and nowhere else.** A mesh's own extent is a fact about
        /// its positions, so it is taken once as they arrive — and for a mesh that deforms, taken
        /// again from what the caller says the pose reaches, because the posed vertices are on the
        /// device and nowhere else. That is what lets a question about where a scene reaches be
        /// eight transforms per instance rather than a walk over every vertex in the table.
        osg::BoundingBoxf mBounds;

        Index getTriangleCount() const { return mIndexCount / 3; }
    };

    /// What skins one bind pose: a run word per vertex and the influences the runs name, laid in the
    /// scene's shared tables. `Shaders::GpuInfluence` says what a run is.
    ///
    /// **Shared by every mesh built from one skin**, because that is what the content shares:
    /// `SceneUtil::RigGeometry` copies keep one `InfluenceData` between them, and a body part worn by
    /// a hundred people is one rig here and a hundred meshes. A rig outlives its last mesh by one
    /// sweep and goes with it.
    struct Rig
    {
        Index mRunOffset = 0;
        Index mInfluenceOffset = 0;
        Index mInfluenceCount = 0;

        /// Rows one pose of this rig takes, which is what every mesh on it is given.
        Index mBoneCount = 0;

        /// Vertices this rig skins, which every mesh on it must have exactly.
        Index mVertexCount = 0;

        /// How many meshes stand on it. Nought is a free slot.
        Index mUses = 0;
    };

    /// What morphs one base: every target's offsets laid end to end, target by target, in the
    /// scene's shared table. A pose is one weight per target.
    struct Morph
    {
        Index mOffsetsAt = 0;
        Index mTargetCount = 0;
        Index mVertexCount = 0;

        /// How many meshes stand on it. Nought is a free slot.
        Index mUses = 0;
    };

    /// How the alpha channel of a surface's diffuse texture is meant to be read.
    enum class AlphaMode
    {
        /// Ignore it. The overwhelming majority of Morrowind's geometry.
        Opaque,

        /// Test against `Material::mAlphaRef`, which costs every candidate a loop into the shader.
        Cutout,

        /// Blend, and read the opposite of the obvious way: **this is where the foliage is.** Barely
        /// any of Morrowind's material set is alpha-tested outright — a canopy, a grate or a banner
        /// is an `NiAlphaProperty` over a texture whose alpha is all but binary, and the original
        /// renderer sorted it rather than testing it. Marking only the tested ones would look
        /// correct and leave every tree a solid card.
        Blend,
    };

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
        AlphaMode mAlphaMode = AlphaMode::Opaque;

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
        Index mLayerOffset = 0;
        Index mLayerCount = 0;

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
        /// **`AlphaMode::Blend` alone does not say so, and this is the whole difficulty.** Morrowind
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
        bool isTranslucent() const { return mAlphaMode == AlphaMode::Blend && mDiffuseColour.a() < 1.0f; }

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

        /// Where this layer's weights begin in the scene's mask table, and the grid they form.
        ///
        /// A zero-sized grid means the layer covers everything: a chunk of a single ground type is
        /// given no mask at all, because there is nothing for it to blend against.
        Index mMaskOffset = 0;
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

    /// One point light, placed in the world.
    ///
    /// Everything here is derived rather than read. A `LIGH` record carries a colour and a radius
    /// and **no intensity at all** — the original renderer had a fixed attenuation curve and no
    /// physical units, so brightness fell out of the curve and there is no authored value to be
    /// faithful to.
    struct Light
    {
        osg::Vec3f mPosition;

        /// Radiant intensity, linear, with the colour folded in.
        ///
        /// Scaled by the square of the recorded radius, which is what makes a lantern and a candle
        /// differ by their size rather than by an arbitrary per-light number.
        osg::Vec3f mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing.
        ///
        /// **Not the recorded radius.** Morrowind's radii run 64 to 256 units in an interior — a
        /// metre to three and a half — because a fixed falloff curve lit the lamp's own post and an
        /// ambient term filled the room. Here the ambient is real light and the lamps have to be
        /// what lights the place, so the reach is stretched while the brightness is not.
        float mReach = 0.0f;

        /// How big the glowing part is, in world units.
        ///
        /// **Not the recorded radius**, and for once not a stretched version of it: this is the
        /// flame rather than the room it lights. `makeLight` is the one place it is derived and
        /// says what from; zero, which is what a light built by hand carries, is a point.
        ///
        /// A shadow ray opens to it, so a lamp with one casts a penumbra as wide as it is. It is
        /// also what stops the falloff running away at the lamp itself, which is where the air
        /// beside one is sampled — `falloff` says what that drew before.
        float mSourceRadius = 0.0f;

        /// How far short of the centre that ray stops.
        ///
        /// **A clearance, and it is not the same question as the size.** A lamp sits inside its own
        /// fitting — a lantern's frame, a sconce's bracket, a candle's holder — so a ray that runs
        /// all the way to the light ends among that fitting and comes back as fully shadowed. What
        /// `makeLight` estimates the fitting to be is the wider of the two.
        float mClearance = 0.0f;
    };

    /// The sun, as a directional light and as something to look at.
    ///
    /// Far enough away that its rays are parallel, so it has a direction and no position, and its
    /// shadow ray runs to the end of the world rather than to an emitter.
    ///
    /// **One sun, where the game keeps five dials for one**, and the one thing to know here is that
    /// nothing may fill these fields itself: `makeSkylight` builds every one of them and its
    /// header says what goes wrong when they are set apart.
    struct Sun
    {
        /// Where the sun stands, unit — and so `-mPosition` is where its light travels.
        ///
        /// **Meaningful even where `mIrradiance` is nothing**, because a moon's crescent points at
        /// where the sun would be. Where it *is* and whether it is there are two questions, and this
        /// answers only the first.
        osg::Vec3f mPosition{ 0.0f, 0.0f, 1.0f };

        /// Irradiance on a surface square to it, linear.
        ///
        /// **Zero exactly when there is no sun**, which is the invariant the whole type exists for:
        /// an interior, a night, and either end of the day once the disc has gone into the horizon.
        /// Everything the sun does is gated on this one test, so a sun cannot shadow without being
        /// drawn or be drawn without lighting — which is what a second field saying whether the
        /// disc is up allowed, for as long as there was one.
        osg::Vec3f mIrradiance;

        /// What the disc is painted with, linear — **not the hue of `mIrradiance`.** The colour a
        /// weather gives its sunlight is the sky's as much as the sun's, which is why it is blue at
        /// night; the disc has its own, and `Sky::sunDiscAt` is where it comes from. The weather's
        /// own glare is folded in, so an overcast sun is a paler one.
        osg::Vec3f mDiscColour{ 1.0f, 1.0f, 1.0f };
    };

    /// One mesh placed in the world: a row of the top-level acceleration structure.
    ///
    /// One live particle, drawn as a disc facing the eye.
    ///
    /// **A particle system carries no triangles at all** — the sprites are the whole of the drawing —
    /// so nothing here reaches an acceleration structure. The layer is marched against the primary
    /// ray and composited instead, which is also what lets it blend in depth order without the
    /// candidate loop an alpha-blended hit would cost traversal.
    struct Sprite
    {
        osg::Vec3f mPosition;

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size: its
        /// quad runs from `-size` to `+size` about the particle and its bounds are expanded by it.
        float mRadius = 0.0f;

        /// The streak's own axis in the world, per unit of `mRadius` — or **zero for a sprite that
        /// faces the eye**, which is nearly every one. `SpriteEmitter::mWidth` is the other half of
        /// the shape and is the emitter's, because a rotation cannot change it.
        ///
        /// **Per particle, because the rotation is.** `osgParticle` turns both of a quad's axes by
        /// the angle the particle carries before it draws them, and `Weather::RainShooter` is what
        /// leans a raindrop into the wind with it — so two drops fired under different winds hang at
        /// different angles in one frame, and an axis held once for the emitter drew the whole storm
        /// falling straight down.
        ///
        /// **Not normalised**, because its length is the shape: rain's is a whole radius against a
        /// width of a tenth, which is what makes a drop a streak.
        osg::Vec3f mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        osg::Vec3f mColour{ 1.0f, 1.0f, 1.0f };

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha = 1.0f;

        /// Where the particle stood on the previous frame, less where it stands now — see
        /// `Shaders::GpuSprite::mMoved` for why it is the difference that is carried.
        ///
        /// **The particle's own answer.** `osgParticle` keeps a previous position per particle for
        /// its own line rendering, so nothing here has to track a particle across frames or care
        /// that births and deaths reshuffle the array.
        osg::Vec3f mMoved;
    };

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them.
    ///
    /// **The sphere is the whole spatial structure and it is enough.** A light is asked for by a
    /// shading *point*, which the uniform grid answers in a lookup; an emitter is asked for by a
    /// whole *ray*, which would have to walk that grid cell by cell. There are tens of emitters in a
    /// cell against hundreds of lamps and each is small, so one rejection throws an emitter away for
    /// almost every pixel of the frame.
    struct SpriteEmitter
    {
        osg::Vec3f mCentre;

        /// Far enough from `mCentre` to contain every sprite in the range, rim included.
        float mReach = 0.0f;

        Index mFirst = 0;
        Index mCount = 0;

        /// The sprite texture, or `sNoIndex` where the emitter had none — which draws nothing, since
        /// a particle's whole silhouette is in that texture's alpha.
        Index mTexture = sNoIndex;

        /// What that texture's alpha leaves of the light crossing a sprite — a `SpriteLightMap` —
        /// or `sNoIndex` for one lit as a flat card.
        Index mLighting = sNoIndex;

        /// `SRC_ALPHA, ONE`: a flame, which adds light and hides nothing behind it. The rest blend
        /// over, which is smoke and needs its colour ramp to fade it.
        bool mAdditive = false;

        /// How wide this emitter's quads are against their own axis, per unit of `Sprite::mRadius`
        /// — or **nought for sprites that face the eye**, which is nearly every emitter in the game.
        ///
        /// `osgParticle` draws a particle as `position ± axisX * size ± axisY * size` and offers two
        /// ways of choosing those axes. A `BILLBOARD` system's are the screen's, transformed into
        /// view space every frame — that is a disc facing the eye and needs nothing carried here. A
        /// `FIXED` one's are used as they were authored, so the quad hangs in the world at an
        /// orientation of its own, and Morrowind's rain is the reason the mode exists: an X axis
        /// squashed to a tenth against a Y axis pointing straight down is a falling streak rather
        /// than a round drop.
        ///
        /// **The length of that X axis and not its direction**, because the march swings the width
        /// about the sprite's own axis to meet the ray rather than committing it to the plane the
        /// content picked. `Sprite::mAxis` carries the rest of the shape, and carries it per
        /// particle because a particle's own rotation turns it.
        float mWidth = 0.0f;
    };

    /// Everything the renderer needs to know about a world, with no Vulkan and no scene graph in it.
    ///
    /// Lights come from ESM `Light` records rather than from the graph: `NifOsg` never reads
    /// `NiLight`, so a model carries none — a candle's mesh and the light it casts arrive by
    /// different routes and are placed by the same reference.
    ///
    /// Deliberately dumb: it appends and it dedups paths, and nothing else. Deciding that two
    /// drawables are the same mesh belongs to whoever is reading the scene graph, which knows what
    /// identity means there; this type would have to guess.
    class SceneDesc
    {
    public:
        /// How many vertices one block of the position, normal and texture-coordinate buffers holds.
        ///
        /// One block of the shared vertex buffers, and of the index buffer.
        ///
        /// **The shaders' own numbers**, because a run this places against a block is resolved back
        /// to that block by a shader dividing by the same figure. `Shaders::VERTEX_BLOCK` says at
        /// length what they are for.
        static constexpr Index sVertexBlock = Shaders::VERTEX_BLOCK;
        static constexpr Index sIndexBlock = Shaders::INDEX_BLOCK;

        /// Copies the vertex data into the shared buffers and returns the new mesh's index.
        ///
        /// `normals` and `texCoords` may be empty; when they are not they must match `positions` in
        /// length, and `indices` must be a whole number of triangles addressing only those vertices.
        /// All three are contracts on the caller, so they are asserted rather than reported.
        ///
        /// Throws where the mesh is longer than a block. **Named rather than asserted**, because a
        /// vertex count comes out of a content file and a run that straddled a block would be
        /// written across two device allocations that are not next to each other.
        ///
        /// `shape` is `MeshRange::mShape` and `deform` is `MeshRange::mDeform`, and both are the
        /// caller's findings: the scene keeps them and draws no conclusion of its own from the
        /// triangles it was handed. A mesh that deforms names the rig or the morph that poses it,
        /// whose vertex count must be this mesh's, and hands over its **bind pose**: the vertices a
        /// pose is computed from, which stay in the shared buffers for as long as the mesh does.
        /// `material` is `MeshRange::mMaterial`, the caller's finding likewise, and must be a
        /// material the scene holds or `sNoIndex`.
        Index addMesh(std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals,
            std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, FoldedShape shape = {},
            Deform deform = Deform::None, Index deformer = sNoIndex, Index material = sNoIndex);

        /// Copies a skin's runs and influences into the shared tables and returns the rig's index.
        ///
        /// `runs` is one word per vertex, `Shaders::RUN_COUNT_BITS` says how it is packed, and every
        /// run it names must lie inside `influences`; every `GpuInfluence::mBone` must be under
        /// `boneCount`. Contracts on the caller, asserted.
        Index addRig(
            std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount);

        /// Copies a morph's offsets — `targets` targets of `offsets.size() / targets` vertices each,
        /// laid end to end — into the shared table and returns the morph's index. `targets` must be
        /// at least one and divide `offsets.size()`.
        Index addMorph(std::span<const osg::Vec3f> offsets, Index targets);

        /// Poses one skinned mesh: its bone rows, and the box the pose reaches.
        ///
        /// **What a skinned body is, and the whole of what the host says about one per frame.** Its
        /// triangles never change and its vertices are computed on the device from the bind pose it
        /// arrived with, so what a frame hands over is one row per bone — a few dozen — and the
        /// mesh keeps the slot in the shared buffers every instance already names.
        ///
        /// `bones.size()` must be the rig's `mBoneCount`, a contract on the caller. `bounds` is the
        /// box the pose reaches in the mesh's own space, which the caller reads off the drawable
        /// rather than this walking vertices it does not have.
        ///
        /// The mesh joins `getDeformed` for the frame, which is what tells a backend whose vertices
        /// to pose and whose structure to refit — unless nothing moved: rows equal to the ones
        /// already held write nothing and name nothing, so an actor standing still two cells away
        /// costs no dispatch and no refit. Compared rather than trusted, because the walk poses
        /// every rig it meets and cannot know which of them the engine animated.
        void poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds);

        /// The same for a morphed mesh: one weight per target of its morph, the base's included and
        /// ignored, which is how `SceneUtil::MorphGeometry` numbers them.
        void poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds);

        Index addMaterial(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it.
        ///
        /// **For shading that animates rather than for a mistake.** A `NifOsg` flipbook, UV, alpha
        /// or material-colour controller rewrites its state set every frame and the surface wearing
        /// it is the same surface: adding a material and dropping the old one would churn a slot a
        /// frame and leave every placement pointing at it to be found and repointed.
        ///
        /// Writing back what is already there costs nothing — the row is only reported as written
        /// when something actually changed, so a paused game re-reads its fires and uploads none
        /// of them.
        ///
        /// **A material that changes what traversal is told rewrites every placement wearing it.**
        /// Its kind, whether it is a cutout and whether it is translucent go into the acceleration
        /// structure's row and not only into the material table; an alpha controller fading a
        /// surface past opaque changes those rows, so the slots wearing the material join
        /// `getMoved`. A flipbook turning or a texture scrolling changes none of them and costs the
        /// placements nothing.
        void setMaterial(Index material, const Material& what);

        /// Copies `weights` into the shared mask table and returns where they landed.
        ///
        /// One float per weight rather than the byte the source holds: a mask is a few hundred
        /// texels and a whole cell's worth is tens of kilobytes, which is not worth requiring
        /// 8-bit storage of the device for.
        ///
        /// How long the run is is not stored beside the offset: a mask is a grid, and the layer
        /// that names this carries the two sides of it. `release` reconstructs the length from
        /// them, so a caller whose weights are not `mMaskWidth * mMaskHeight` long leaks the
        /// difference.
        Index addMask(std::span<const float> weights);

        /// Copies a material's layers into the shared layer table and returns where they landed.
        ///
        /// **All of them at once, because a run is allocated as a run.** They were appended one at
        /// a time when the table only ever grew and a material took whatever length the table
        /// happened to be at; a run that can be given back has to be asked for by length.
        Span addLayers(std::span<const MaterialLayer> layers);

        void addLight(const Light& light);

        /// Returns the slot of an image this renderer made, adding it only if `key` is not known.
        ///
        /// **A texture with no file behind it, which the table has to be able to hold.** A composite
        /// baked for a distant terrain chunk is an image nothing can open: the bytes belong to
        /// whatever made it, and what the scene keeps is the slot, because a slot is what a material
        /// points at and what a backend uploads into. Two chunks that would bake the same image must
        /// find the same slot, which is what `key` is for and why it has to be stable across frames.
        ///
        /// The same slots, the same free list and the same reference counting as a file's — this is a
        /// second way in and not a second table. `holdTexture` and `dropTexture` do not care which
        /// kind a slot is.
        Index addBakedTexture(std::string_view key);

        /// Returns the index of `path`, adding it only if it is not already known.
        ///
        /// **The slot is live from here**, before anything names it, and stays live until the last
        /// thing that named it lets go. A caller that adds a texture and then puts it on no material
        /// and takes no hold of it keeps that slot for the rest of the scene, which is a caller
        /// asking for a texture it did not want.
        Index addTexture(VFS::Path::NormalizedView path);

        /// Names a texture for something no material can speak for, and stops.
        ///
        /// **A particle emitter's sprite, and nothing else so far.** An emitter is a placement — it
        /// is thrown away and rebuilt every frame — so the texture it draws with hangs off no
        /// material and no table the scene owns; whatever recognises the emitter between frames is
        /// what has to hold it. The alternative was a keep set handed over on every sweep, which
        /// could only be looked at on the frames a mesh or a material also died.
        ///
        /// `sNoIndex` is allowed and does nothing, so a caller need not test what it got.
        void holdTexture(Index texture);

        /// Gives back one `holdTexture`. The slot is freed here where nothing else names it.
        void dropTexture(Index texture);

        /// Whether nothing stands in `slot`: the last thing naming it gave it back, and it is
        /// waiting for the next `addTexture` to take it over.
        ///
        /// **The name and not the reference count**, which is the same answer except for the window
        /// between a slot being handed out and whatever is about to name it doing so. A reader that
        /// asked the count would find a texture it was in the middle of building.
        bool isTextureFree(Index texture) const { return mTextures.isFree(texture); }

        /// Places `instance` in a slot and returns it.
        ///
        /// **The slot is the placement's name for as long as it stands.** It is the custom index a
        /// hit reads back, the row a shader looks its material up in, and — because it outlives the
        /// walk that made it — what lets a mirror move a placement instead of rebuilding the list
        /// it was in. A slot freed by `dropInstance` is handed out again; one that is still standing
        /// never is.
        Index addInstance(const MeshInstance& instance);

        /// Moves the placement in `slot`, and says whether that changed anything.
        ///
        /// A transform equal to the one already there is not a move: it writes nothing, records
        /// nothing, and leaves the slot reporting no motion. That is the ordinary case — most of a
        /// world stands still — and making it the cheap one is the point of addressing placements
        /// by slot at all.
        bool moveInstance(Index slot, const osg::Matrixf& transform);

        /// Fades the placement in `slot`.
        ///
        /// Separate from `moveInstance` because the two are separate facts: an actor fading on the
        /// spot has not moved, and an actor walking is not fading. A fade that changed the number
        /// joins `getMoved` all the same, because it is a row to rewrite — the opacity a shader
        /// reads and the translucency traversal is told — and its previous transform stays equal to
        /// its current one, so it carries no motion.
        void fadeInstance(Index slot, float opacity);

        /// Empties `slot`. Its index is not reused until the next `addInstance` asks for one.
        ///
        /// The slot joins `getMoved`: a backend has to write its row inactive, or the structure
        /// goes on tracing what stood there.
        void dropInstance(Index slot);

        /// Ends a frame's placement: what moved becomes where things were.
        ///
        /// **Costs what moved and not what stands.** Only a slot that reported a move can have a
        /// previous transform that differs from its current one, so only those have to be caught
        /// up — which is what makes a world of fifty thousand placements and three hundred movers
        /// cost three hundred. What was moved becomes `getSettled`, and `getMoved` starts empty.
        void advancePlacement();

        /// Appends one particle system's live sprites, and the emitter that names them.
        ///
        /// The sphere is derived here rather than passed in, so the rejection test a ray makes and
        /// the sprites it would then walk cannot disagree about where they are. Nothing is added for
        /// an emitter with no live particles, which is most of them for most of a frame.
        /// @param width how wide the quads are against their own axis, per unit of
        ///        `Sprite::mRadius`, or nought for sprites that face the eye. Every sprite carries
        ///        an axis where this is set and none where it is not — `SpriteEmitter::mWidth`.
        /// @param lighting the bake of `texture`'s alpha, or `sNoIndex`. `SpriteEmitter::mLighting`.
        void addEmitter(std::span<const Sprite> sprites, Index texture, bool additive, float width = 0.0f,
            Index lighting = sNoIndex);

        /// Drops every mesh and material the caller did not name.
        ///
        /// **The only way a scene loses geometry, and nothing is renumbered by it.** A freed entry
        /// keeps its index and its room; the index goes on a free list and the next arrival that
        /// fits takes the slot. Compacting instead — closing the gaps and renaming what pointed into
        /// them — is what made a cell boundary cost a full rebuild: every bottom-level acceleration
        /// structure in the world is named by a mesh index, and every material a hit reads is named
        /// by another.
        ///
        /// **Textures are not swept here and are not named here.** A material freed below gives back
        /// what it named on its way out, which is the same thing `setMaterial` does when a shading
        /// animation stops naming an image and the same thing `dropTexture` does for an emitter's
        /// sprite. Sweeping them instead meant asking on the frames a mesh or a material happened to
        /// die as well, and a texture that stopped being named on any other frame was never noticed.
        ///
        /// Layers and masks have no keep set either: they belong to the material that owns them, so
        /// a freed material hands both runs back to their allocators on its way out. A terrain
        /// chunk's masks are tens of kilobytes and a player can cross the whole continent through
        /// one `SceneDesc`, so leaving them until `clear` was a session-long growth.
        ///
        /// **Placements do not go**, and they no longer have to be carried anywhere either: a slot
        /// is a name, and what it names has stopped moving.
        ///
        /// @param meshes every mesh to keep, each once, in any order.
        /// @param materials the same for materials.
        /// @return whether anything was freed. False is the ordinary frame, and it costs two
        ///         comparisons: a scene that lost nothing has as many survivors as it had entries.
        bool release(std::span<const Index> meshes, std::span<const Index> materials);

        /// Empties every table while keeping the capacity, so rebuilding a scene does not go back to
        /// the allocator for buffers it already had.
        void clear();

        /// Empties the per-frame lists a walk rebuilds wholesale: lights, deformed meshes, sprites
        /// and emitters.
        ///
        /// **Placements are not among them.** They are addressed by slot and reconciled in place —
        /// `addInstance` for one that has appeared, `moveInstance` for one that has shifted,
        /// `dropInstance` for one that has gone — because a slot index is what a hit reads back and
        /// because a world of fifty thousand placements of which three hundred move should cost
        /// three hundred. Everything above is small enough per frame that rebuilding it is cheaper
        /// than reconciling it.
        void clearPlacement();

        /// **Every span below is into a table that grows, and lives until the table does.** A span
        /// is valid until the next `add` into its table — `addMesh` grows the geometry and the mesh
        /// table, `addEmitter` the sprites and the emitters, and each of the others the table it
        /// names — and until `clear` or `clearPlacement`, which empty them. Take it after the add
        /// and never in the same expression as one: `getMeshes()[addMesh(...)]` sequences the span
        /// before the add, and indexes a table that has moved.
        std::span<const osg::Vec3f> getPositions() const { return mPositions; }
        std::span<const osg::Vec3f> getNormals() const { return mNormals; }
        std::span<const osg::Vec2f> getTexCoords() const { return mTexCoords; }
        std::span<const std::uint32_t> getIndices() const { return mIndices; }

        /// Every mesh slot, live or free. A freed one has a zero count and keeps its room, so a
        /// backend that walks these builds a structure over nothing rather than over somebody else's
        /// triangles — and the top level a frame rebuilds is what stops it being traced.
        std::span<const MeshRange> getMeshes() const { return mMeshes; }

        /// Which meshes changed shape since the last `clearPlacement`, each named once and in no
        /// particular order. Empty for a world that only moves.
        std::span<const Index> getDeformed() const { return mDeformed.getSlots(); }

        /// Every rig slot, live or free — `Rig::mUses` tells them apart — and the two tables the rigs
        /// index.
        std::span<const Rig> getRigs() const { return mRigs; }
        std::span<const std::uint32_t> getRuns() const { return mRuns; }
        std::span<const Shaders::GpuInfluence> getInfluences() const { return mInfluences; }

        /// The same for the morphs.
        std::span<const Morph> getMorphs() const { return mMorphs; }
        std::span<const osg::Vec3f> getMorphOffsets() const { return mMorphOffsets; }

        /// Every deforming mesh's pose, laid end to end: a run of rows per skinned mesh, and a run
        /// of weights per morphed one. `MeshRange::mPoseOffset` says where each starts.
        std::span<const Shaders::GpuBone> getBones() const { return mBones; }
        std::span<const float> getWeights() const { return mWeights; }

        /// One mesh's pose, for a backend writing that mesh's rows or a test reading them back.
        std::span<const Shaders::GpuBone> getMeshBones(Index mesh) const;
        std::span<const float> getMeshWeights(Index mesh) const;

        /// How many vertices the deforming meshes' bind poses take between them, which is how long
        /// a backend's bind table has to be. `MeshRange::mBindOffset` says where each mesh's run is.
        Index getBindVertexCount() const { return mBindRuns.getEnd(); }

        /// Which rig and morph slots have been written since the last `clearArrivals`, for a
        /// backend to upload. A freed slot is named by nothing: nothing reads it until the next
        /// arrival lands in it, and that arrival names it.
        std::span<const Index> getArrivedRigs() const { return mArrivedRigs; }
        std::span<const Index> getArrivedMorphs() const { return mArrivedMorphs; }
        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getInstances() const { return mPlacements.getAll(); }

        /// How many slots hold a placement, which is what reaches an acceleration structure.
        std::uint32_t getPlacedCount() const { return mPlacements.getPlacedCount(); }

        /// Where each slot stood before the last `advancePlacement`, indexed alongside the slots.
        std::span<const osg::Matrixf> getPrevious() const { return mPlacements.getPrevious(); }

        /// The slots whose row changed since the last `advancePlacement`: placed, moved, faded,
        /// dropped, or wearing a material that changed what traversal is told.
        ///
        /// **What a backend rewrites, and all it rewrites.** A world is tens of thousands of
        /// placements and a frame changes hundreds; a row table written whole every frame was a
        /// millisecond of the game's CPU to change nothing. A slot can appear more than once where
        /// two facts about it changed in one frame, which costs one row written twice.
        std::span<const Index> getMoved() const { return mPlacements.getMoved(); }

        /// The slots the last `advancePlacement` caught up, whose motion is now still.
        ///
        /// **The other half of what a backend rewrites.** A row carries the motion between where a
        /// placement stood and where it stands, and that motion goes back to nothing on the frame
        /// after the move — which is a frame on which the slot did not move. Without this list a
        /// backend writing only `getMoved` would leave last frame's motion in the row for ever.
        std::span<const Index> getSettled() const { return mPlacements.getSettled(); }
        std::span<const Material> getMaterials() const { return mMaterials; }
        std::span<const MaterialLayer> getLayers() const { return mLayers; }
        std::span<const Light> getLights() const { return mLights; }

        /// Puts the lights in an order that depends on the lights and not on the walk that found
        /// them. Once, where a walk ends.
        ///
        /// **A picture must not depend on the order cells were loaded in.** A walk meets lights in
        /// graph order, and a graph gains and loses cells as a player moves — so the same place
        /// walked twice hands the same lights over in a different order. Every one of them is still
        /// there, and the picture still changes: the grid bins them in that order and a reservoir
        /// streams them in it, so one cell's sample falls on a different lamp and its neighbourhood
        /// moves by a level or two. It is a handful of pixels around one lamp, which is small enough
        /// to be read as noise and is not.
        void orderLights();

        /// How many times the scene's **structure** has changed: its meshes and its textures.
        ///
        /// **What a rebuild costs is why this is separate from the tables.** A mesh appearing means
        /// a bottom-level acceleration structure that does not exist yet, and a texture appearing
        /// means an array that has to be made again — hundreds of milliseconds between them, and
        /// the temporal history goes with them. Nothing else in the scene is worth that.
        ///
        /// **The only honest test for it**: comparing table sizes misses a cell that left as another
        /// arrived, which is exactly what walking across a boundary does — and it misses a freed
        /// slot taken over by something else entirely, which is what one does now. Bumped by a mesh
        /// or a texture appearing, whether at the end of the table or into a slot something else
        /// left, and by `clear`; never by a placement, which is rewritten every frame anyway.
        std::uint64_t getStructureRevision() const { return mStructureRevision + mTextures.getRevision(); }

        /// Forgets what has arrived and what has gone, for a caller that has applied both.
        ///
        /// **Whoever hands the scene to a backend owns this**, not the frame: an arrival lives from
        /// the walk that made it until something has taken it, and a walk that is never handed over
        /// must not lose what it added.
        void clearArrivals();

        /// Which texture slots have been written since the last `clearArrivals`.
        ///
        /// **A list and not a count, because a slot is taken over wherever it sits.** A backend used
        /// to be handed the tail of the table and told to append; reclaiming a slot means an arrival
        /// can be anywhere, so the arrivals say where each one goes and the backend writes those and
        /// nothing else.
        std::span<const Index> getArrivedTextures() const { return mTextures.getArrived(); }

        /// Which mesh slots have been written since the last `clearArrivals`.
        ///
        /// The same list for the expensive half. `getMeshRevision` says *that* a mesh arrived and a
        /// backend hearing it had nothing to do but build the scene again; this says *which*, which
        /// is what lets it build those structures and leave the rest standing.
        std::span<const Index> getArrivedMeshes() const { return mMeshChanges.getArrived(); }

        /// Which mesh slots `release` has given up since the last `clearArrivals`.
        std::span<const Index> getFreedMeshes() const { return mMeshChanges.getFreed(); }

        /// Which texture slots `release` has given up since the last `clearArrivals`.
        ///
        /// **What lets a backend stop holding a departed cell's images.** An array that is never
        /// told a slot went keeps whatever was in it until something takes the slot over, so a
        /// region walked away from goes on costing its texture memory.
        std::span<const Index> getFreedTextures() const { return mTextures.getFreed(); }

        /// How many times a **mesh** has appeared, which is the expensive half of the above.
        ///
        /// A texture arriving is an upload; a mesh arriving is a bottom-level acceleration structure
        /// that does not exist yet. Told apart because a body texture nobody has worn yet must not
        /// cost the structures of a whole cell.
        std::uint64_t getMeshRevision() const { return mMeshRevision; }

        /// How many times the scene has been **replaced outright** by `clear`.
        ///
        /// **The one thing that still renumbers, and it is travel.** Walking out of the world you
        /// were in — a door, a fast travel, a load — is not a ring arriving, and rebuilding is both
        /// correct and what the player is already waiting for. Everything short of that appends.
        std::uint64_t getResetRevision() const { return mResetRevision; }

        /// Which material slots `addMaterial` or `setMaterial` wrote since the last `clearArrivals`,
        /// each once.
        ///
        /// **Rows and not a revision, because a row is what a backend writes.** A counter that moved
        /// on any material said "the shading changed" and the answer to that was every material,
        /// every layer and every mask copied to the device — megabytes, every frame a flipbook
        /// turned, to change eighty bytes. A material the sweep freed is not here: nothing stands on
        /// it, so its row is never read and need not be written.
        std::span<const Index> getWrittenMaterials() const { return mWrittenMaterials.getSlots(); }

        /// The runs `addLayers` placed since the last `clearArrivals`, and the same for `addMask`.
        ///
        /// **Runs and not a flag over the table**, for the reason the materials are rows: a chunk
        /// arriving writes its own layers and its own weights, and the rest of both tables is what
        /// it was. A run the sweep gave back is not named here either — nothing reads it until the
        /// next chunk lands in it, and that chunk's arrival is what names it.
        std::span<const Span> getArrivedLayers() const { return mArrivedLayers; }
        std::span<const Span> getArrivedMasks() const { return mArrivedMasks; }

        std::span<const Sprite> getSprites() const { return mSprites; }
        std::span<const SpriteEmitter> getEmitters() const { return mEmitters; }
        std::span<const float> getMasks() const { return mMasks; }
        /// The file each slot was read from, empty where it was not read from one.
        std::span<const VFS::Path::Normalized> getTextures() const { return mTextures.getPaths(); }

        /// What made each slot, for the ones nothing opened — empty for every slot that is a file.
        ///
        /// **Parallel to `getTextures` and not instead of it**, because the two are different facts
        /// about a slot and nearly every reader wants only the first. A slot with neither is one
        /// nothing stands in, which is what `isTextureFree` answers.
        std::span<const std::string> getBakedTextures() const { return mTextures.getBaked(); }

        /// The vertices of one mesh, for a test or a build that wants to read back what it appended.
        std::span<const osg::Vec3f> getMeshPositions(Index mesh) const;
        std::span<const std::uint32_t> getMeshIndices(Index mesh) const;

        std::uint32_t getTriangleCount() const;

        /// The world-space extent of everything placed. Invalid when nothing is.
        ///
        /// Computed from each mesh's local box carried through its instances rather than from every
        /// vertex of every instance, which is the difference between eight transforms per instance
        /// and several hundred.
        ///
        /// **Backdrops included, and a far plane is what wants that**: a ray has to reach the sea,
        /// so what the frame must span is everything there is. `getContentBounds` is the other
        /// question.
        osg::BoundingBoxf getBounds() const;

        /// The extent of what stands inside `region`, backdrops left out. Invalid where nothing does.
        ///
        /// **What a camera is placed from, and neither half of it is optional.** The sea is one sheet
        /// a hundred and fifty cells across and the ground now reaches four cells past the one being
        /// looked at, so a camera framing the whole scene went a million and a half units out and
        /// photographed water — and framing everything that is not the sea would still go two hundred
        /// thousand out and photograph a region. A view of a place is a view of that place.
        ///
        /// `region` is asked in world units and clips what it meets, so a chunk straddling its edge
        /// contributes the part inside it rather than dragging the answer a cell wide. Its height is
        /// the caller's to leave open: how high the ground is there is exactly what this is for.
        ///
        /// Water is the only backdrop today and `MaterialKind` is what says so; a sky dome would join
        /// it here rather than teaching every caller a second exception.
        osg::BoundingBoxf getContentBoundsWithin(const osg::BoundingBoxf& region) const;

        /// Bytes held by the vertex and index buffers. What the upload at M3 will cost.
        std::size_t getGeometryBytes() const;

    private:
        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;
        std::vector<std::uint32_t> mIndices;
        std::vector<MeshRange> mMeshes;

        /// Which meshes were posed this frame. Grown with `mMeshChanges` and emptied with the
        /// placement.
        SlotSet mDeformed;

        /// What poses the deforming meshes, and the poses themselves. `Rig` and `Morph` say what
        /// each table holds; the runs behind them are handed out by the allocators below and given
        /// back when the last mesh on a rig or a morph goes.
        std::vector<Rig> mRigs;
        std::vector<std::uint32_t> mRuns;
        std::vector<Shaders::GpuInfluence> mInfluences;
        std::vector<Morph> mMorphs;
        std::vector<osg::Vec3f> mMorphOffsets;
        std::vector<Shaders::GpuBone> mBones;
        std::vector<float> mWeights;

        std::vector<Index> mFreeRigs;
        std::vector<Index> mFreeMorphs;
        std::vector<Index> mArrivedRigs;
        std::vector<Index> mArrivedMorphs;

        /// Gives a deforming mesh's runs back: its bind run, its pose run, and its rig's or morph's
        /// where this was the last mesh standing on it.
        void releaseDeformer(MeshRange& range);

        /// What a pose that changed does beside its rows: the reach, and the mesh named for the
        /// frame, once.
        void notePosed(Index mesh, const osg::BoundingBoxf& bounds);

        // Slot-addressed and parallel: the placement, and where it stood before the last advance.
        // Two flat arrays rather than one struct, because the previous transform is read only for
        // what moved and a frame walks the placements for other reasons.
        /// Where everything stands and which rows a backend has to write again. Its own type,
        /// because slots that are never moved, a free list and two change lists are one invariant.
        PlacementTable mPlacements;

        std::vector<Material> mMaterials;
        std::vector<MaterialLayer> mLayers;
        std::vector<Light> mLights;
        std::vector<Sprite> mSprites;
        std::vector<SpriteEmitter> mEmitters;
        std::vector<float> mMasks;

        /// Every texture the scene names, and what still names each. Its own type, because a
        /// reference-counted table with a free list and two lookups into it is a thing with an
        /// invariant rather than a set of parallel vectors.
        TextureTable mTextures;

        /// **The share of the structure revision this counts, and not the whole of it.** The rest
        /// belongs to `TextureTable`, which takes its own slots — `getStructureRevision` adds the
        /// two.
        std::uint64_t mStructureRevision = 0;
        std::uint64_t mMeshRevision = 0;
        std::uint64_t mResetRevision = 0;

        /// Copies one mesh's arrays into the room `range` names. Zero-fills an attribute the mesh
        /// did not bring, because a reused slot still holds its last tenant's.
        void writeMesh(const MeshRange& range, std::span<const osg::Vec3f> positions,
            std::span<const osg::Vec3f> normals, std::span<const osg::Vec2f> texCoords,
            std::span<const std::uint32_t> indices);

        /// Slots nothing stands in. **Any of them will do**, whichever table it is: a slot is one
        /// row and every row is the same size, and what varies is the run of geometry, layers or
        /// weights behind it — which `mVertexRuns` and the rest hand out separately. Taken from the
        /// back, because there is no fit to find.
        ///
        /// **A list and not a hole map**, because what goes on them is what one departing ring left
        /// — tens of entries, not the table. Nothing is ever moved, so a slot that is taken over
        /// keeps its index and every placement standing on it stays where it is.
        std::vector<Index> mFreeMeshes;
        std::vector<Index> mFreeMaterials;

        /// Which slots a sweep was told to keep, one flag per row of the table beside it.
        ///
        /// **Held rather than made, because a sweep runs on the frame a cell left** — the frame that
        /// is already giving thousands of runs back to the allocators, and the last one that should
        /// also be sizing two buffers to the whole table. Refilled by `release` and read by nobody
        /// else.
        std::vector<std::uint8_t> mKeptMeshes;
        std::vector<std::uint8_t> mKeptMaterials;

        /// Where a mesh's vertices and indices, a material's layers and a layer's weights live.
        ///
        /// **Runs and not slots**, which is why these are allocators and the three lists above are
        /// not: a material is one material's worth of room and a mesh slot is one row of a table,
        /// but the geometry behind a mesh is as long as the model, a terrain chunk's layer run is as
        /// long as the ground types under it, and its masks are as big as the blend maps. A list of
        /// slots cannot give a variable length back.
        ///
        /// One for the vertices because the position, normal and texture-coordinate buffers are
        /// parallel and a vertex id indexes all three.
        SpanAllocator mVertexRuns{ sVertexBlock };
        SpanAllocator mIndexRuns{ sIndexBlock };
        SpanAllocator mLayerRuns;
        SpanAllocator mMaskRuns;

        /// The deforming meshes' bind poses and their bone rows and weights, and the rigs' and the
        /// morphs' own runs. Unblocked: a backend reaches each run by an address it is handed per
        /// dispatch, so nothing here has to keep an address across a growth.
        SpanAllocator mBindRuns;
        SpanAllocator mBoneRuns;
        SpanAllocator mWeightRuns;
        SpanAllocator mRigRuns;
        SpanAllocator mInfluenceRuns;
        SpanAllocator mMorphRuns;

        /// Calls `visit(instance, worldBox)` for every placement, which is what both extents walk.
        template <class Visit>
        void forEachPlacement(Visit&& visit) const;

        /// Records `slot` as having arrived or gone, and grows the list to reach it.
        void noteMesh(Index slot, SlotNews what);

        /// Records that `slot`'s row was written, once however many times it is.
        void noteMaterial(Index slot);

        /// Takes and gives back the textures a material names — its three roles and every layer's.
        ///
        /// Only ever called in that pair, and `setMaterial` is why the order between them matters.
        void holdMaterialTextures(const Material& material);
        void dropMaterialTextures(const Material& material);

        /// Which mesh slots arrived and which were freed, since a backend last read them.
        SlotChanges mMeshChanges;

        /// Material rows written since the last `clearArrivals` — a flipbook that is added and then
        /// rewritten on one frame is one row, not two.
        SlotSet mWrittenMaterials;

        /// Runs placed in the layer and mask tables since the last `clearArrivals`.
        std::vector<Span> mArrivedLayers;
        std::vector<Span> mArrivedMasks;
    };
}
