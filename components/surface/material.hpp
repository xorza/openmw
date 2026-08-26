#ifndef OPENMW_COMPONENTS_SURFACE_MATERIAL_H
#define OPENMW_COMPONENTS_SURFACE_MATERIAL_H

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

namespace osg
{
    class StateSet;
}

namespace Surface
{
    /// What a texture is for, as the content file said.
    ///
    /// **A role, not a texture unit.** A NIF says a map is a glow map and the loader has always known
    /// it; what it used to do with that knowledge was name a texture unit `emissiveMap` on an
    /// `osg::StateSet`, leaving every renderer that does not bind texture units to read the name back
    /// out and guess. The role is the fact. Where a unit is bound, and whether there are units at
    /// all, is a renderer's business.
    enum class TextureRole
    {
        Diffuse,
        Normal,

        /// A normal map carrying height in its alpha, for parallax. The same slot as `Normal` to a
        /// renderer that does not do parallax, and a different sampler to one that does.
        NormalHeight,

        Emissive,
        Specular,
        Dark,
        Detail,
        Decal,
        Gloss,
        Bump,
        Environment,
    };

    inline constexpr std::size_t sTextureRoleCount = 11;

    /// The name the OpenGL renderer binds this role under, which is also the name the content
    /// pipeline has used since before there was anything else to call it.
    ///
    /// **One table, because there were fifty literals.** The same handful of strings were spelled out
    /// in `nifloader.cpp` (17), `shadervisitor.cpp` (15), `terrain/material.cpp` (4) and
    /// `rtx/sceneextractor.cpp` (4), and a typo in any of them silently produced an untextured surface
    /// rather than a build error.
    std::string_view textureRoleName(TextureRole role);

    /// The role a texture unit's name means, or nothing for a name that is not a role — `blendMap`
    /// and the shadow maps are bound the same way and are not what a surface is made of.
    std::optional<TextureRole> textureRoleNamed(std::string_view name);

    /// What the alpha channel means here.
    ///
    /// `Cutout` and `Blend` are not exclusive in a NIF — `NiAlphaProperty` can ask for both — but no
    /// renderer benefits from honouring both, and the rasterizer already resolves them this way:
    /// blending wins, and the test threshold survives for a renderer that would rather cut out.
    enum class AlphaMode
    {
        Opaque,
        Cutout,
        Blend,
    };

    /// What a surface is, as the content said and before any renderer has an opinion.
    ///
    /// **Authored where the fact is known** — `NifOsg` from the NIF properties, `Terrain` from its
    /// texture layers, `Shader::ShaderVisitor` for the maps it discovers by filename — and hung on
    /// the `osg::StateSet` that describes the same surface. Nothing in here was ever unavailable;
    /// it was written into OpenGL pipeline state and read back out by whoever needed it again.
    ///
    /// **A value, and cheap to copy.** It is inherited down `NifOsg`'s node recursion by assignment,
    /// which is how a texturing property on a parent reaches the shape three levels down, and it is
    /// replaced rather than mutated wherever it is already attached — a state set can be shared by
    /// thousands of drawables and a material edited in place would reach all of them.
    struct Material
    {
        /// One texture per role, null where the content has none.
        ///
        /// **The image, and not an `osg::Texture2D`.** After a model is loaded,
        /// `osgDB::SharedStateManager` canonicalises equal textures across every model in the cache,
        /// so the object `NifOsg` bound is replaced by one it never saw and a description holding it
        /// would be pointing at a texture nothing uses any more. The image survives that: it is what
        /// `Resource::ImageManager` caches by path, it is what makes two textures compare equal in
        /// the first place, and it carries the file name a renderer identifies a texture by.
        ///
        /// Sampler state — the wrap modes, the filters — stays on the `osg::Texture2D` for now.
        /// It joins this when the OpenGL renderer starts building those from the description.
        std::array<osg::ref_ptr<const osg::Image>, sTextureRoleCount> mTextures;

        AlphaMode mAlphaMode = AlphaMode::Opaque;

        /// What `Cutout` cuts at, in the zero-to-one range the content uses rather than the bytes
        /// `NiAlphaProperty` stores. Meaningful whenever the content asked for alpha testing, which
        /// includes surfaces that also blend.
        float mAlphaRef = 0.0f;

        /// Whether both faces of this surface are drawn and lit.
        ///
        /// **True unless something says otherwise, because that is what the content means.**
        /// OpenGL culls nothing until it is told to, and the only record in a NIF that tells it to
        /// is a `NiStencilProperty` with a draw mode other than `Both` — a shader property's
        /// "double sided" flag and a material file's can turn culling off and never on. So a
        /// surface nothing has spoken about is two-sided, which Morrowind leans on heavily: every
        /// leaf, banner and sheet in the game is one quad meant to be seen from behind.
        bool mTwoSided = true;

        /// Alpha included: `NiMaterialProperty` keeps the surface's opacity here and nowhere else.
        osg::Vec4f mDiffuseColour{ 1.0f, 1.0f, 1.0f, 1.0f };
        osg::Vec3f mAmbientColour{ 1.0f, 1.0f, 1.0f };
        osg::Vec3f mEmissiveColour{ 0.0f, 0.0f, 0.0f };
        osg::Vec3f mSpecularColour{ 0.0f, 0.0f, 0.0f };

        /// Clamped to OpenGL's limit at the point of authoring, because content routinely exceeds it
        /// and the number above the clamp never meant anything.
        float mGlossiness = 0.0f;

        /// A separate multiplier rather than folded into `mEmissiveColour`, because a
        /// `NiMaterialColorController` animates the colour and leaves this alone.
        float mEmissiveMult = 1.0f;

        /// How texture coordinates are transformed before the surface is sampled: scaled about the
        /// middle of the texture, then offset.
        ///
        /// **One convention, because the content has two.** A `BSShaderProperty` offsets by the
        /// negative of both its recorded components and `NiUVController` negates only U, so the raw
        /// numbers mean different things depending on which record they came from. What is stored
        /// is the resolved translation, which is the same thing either way and the only thing a
        /// renderer can use without knowing where it came from.
        ///
        /// Animated: `NifOsg::UVController` rewrites this every frame it is applied, exactly as it
        /// rewrites the `osg::TexMat` the OpenGL renderer reads.
        osg::Vec2f mTextureScale{ 1.0f, 1.0f };
        osg::Vec2f mTextureOffset{ 0.0f, 0.0f };

        const osg::Image* getTexture(TextureRole role) const { return mTextures[static_cast<std::size_t>(role)].get(); }

        void setTexture(TextureRole role, const osg::Image* image)
        {
            mTextures[static_cast<std::size_t>(role)] = image;
        }

        /// The same, taking whatever the texture was bound as. Null and imageless textures clear the
        /// role, which is what a placeholder a flip controller has not filled in yet amounts to.
        void setTexture(TextureRole role, const osg::Texture* texture);

        bool operator==(const Material& other) const = default;
    };

    /// Hangs `material` off the state set describing the same surface, replacing any already there.
    ///
    /// **Safe under `osgDB::SharedStateManager`, and not by accident.** After load, equal state sets
    /// across every model in the cache are collapsed into one, and it compares pipeline state
    /// without looking at what hangs off it — so two surfaces merged into one must not have had
    /// different descriptions. They cannot: every field here is authored from a record that also
    /// produced part of the state being compared, so state sets that compare equal describe the
    /// same surface. `apps/components_tests/rtxtool/material.cpp` is what keeps that true.
    ///
    /// **On the state set and not on the drawable**, because that is where the content's own
    /// inheritance already lands: a `NiTexturingProperty` three nodes up and a `NiMaterialProperty`
    /// on the shape both end on state sets, drawables sharing one surface share it, and everything
    /// that resolves a drawable's appearance already walks that chain.
    void setMaterial(osg::StateSet& stateSet, const Material& material);

    /// What `setMaterial` left on this state set, or null. Does not look at parents.
    ///
    /// The pointer is good until the next `setMaterial` or `getWritableMaterial` on the same state
    /// set, which is long enough for every caller: a material is read into a renderer's own form and
    /// not held.
    const Material* getMaterial(const osg::StateSet& stateSet);

    /// The same material, ready to be written to, or null where nothing authored one.
    ///
    /// **This is what keeps an animated surface off the allocator.** A `NifOsg` controller rewrites
    /// its material every frame it is applied, and replacing the whole attachment each time would be
    /// one allocation per animated surface per frame. Where the state set is the only holder the
    /// material is edited in place; where a copy shares it — the state set was cloned, and a
    /// `SceneUtil::StateSetUpdater`'s scratch always starts that way — it is duplicated once and
    /// edited in place from then on.
    Material* getWritableMaterial(osg::StateSet& stateSet);
}

#endif
