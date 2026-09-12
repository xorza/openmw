#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/ref_ptr>

namespace Surface
{
    /// What the alpha channel of a surface's diffuse texture means.
    ///
    /// `Cutout` and `Blend` are not exclusive in a NIF — `NiAlphaProperty` can ask for both — but no
    /// renderer benefits from honouring both, and the rasterizer already resolves them this way:
    /// blending wins, and the test threshold survives for a renderer that would rather cut out.
    enum class AlphaMode
    {
        /// Ignore it. The overwhelming majority of Morrowind's geometry.
        Opaque,

        /// Test against the material's own threshold.
        Cutout,

        /// Blend. **This is where the foliage is**, and a ray tracer reads it the opposite of the
        /// obvious way: barely any of Morrowind's material set is alpha-tested outright — a canopy,
        /// a grate or a banner is an `NiAlphaProperty` over a texture whose alpha is all but binary,
        /// and the original renderer sorted it rather than testing it.
        /// `Rtx::Material::getAlphaCutoff` says what a renderer with no sort does about that.
        Blend,
    };

    /// A colour as a content file states one: three channels from nought to one, display-encoded.
    ///
    /// **A type, because the space is the whole of what a reader gets wrong.** Morrowind's records
    /// are written in the space the artist saw, and a renderer working in light has to divide the
    /// display curve out of every one of them — `Rtx::decodeColour` is that crossing. An
    /// `osg::Vec3f` says nothing about which side of it a value is on, so a reader that copied one
    /// across was right by inspection and wrong in fact. This is the side a renderer cannot copy
    /// out of.
    ///
    /// **No arithmetic, on purpose.** Nothing weighs, sums or scales a colour on this side of the
    /// crossing: the content states it, a controller replaces it, and a renderer decodes it. A gain
    /// belongs past the decode, where the numbers are light.
    struct Colour
    {
        float mRed = 0.0f;
        float mGreen = 0.0f;
        float mBlue = 0.0f;

        bool operator==(const Colour& other) const = default;
    };

    /// What a surface's per-vertex colour is for, as the content said.
    ///
    /// **`NiVertexColorProperty`'s three vertex modes, resolved against its light mode.**
    /// `SceneUtil::VertexColorModes` carries six, because a loaded `osg::Material` can name the
    /// ambient, the diffuse or the specular alone — but a NIF states only these three. A renderer
    /// with one albedo folds the first two of those into the tint, and the specular has nowhere to
    /// go in a model with no specular lobe.
    enum class VertexColour
    {
        /// The colours are there and mean nothing, or there are none at all. `NifOsg` resolves both
        /// to this, so a surface that says nothing here has nothing to apply.
        None,

        /// It replaces the material's diffuse and ambient colour, which is what
        /// `glColorMaterial(GL_AMBIENT_AND_DIFFUSE)` does and what the game's own shader reads
        /// through `getDiffuseColor`. Every piece of ground is this, and so is every model that
        /// carries colours and says nothing about them.
        Tint,

        /// It replaces the material's emissive colour. The light mode that goes with it also takes
        /// the diffuse and the ambient to nought, so such a surface is its glow and nothing else.
        Glow,
    };

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

    /// What a surface is, as the content said and before any renderer has an opinion.
    ///
    /// **Read off the finished `osg::StateSet`, by `describe`.** Everything here was written into
    /// OpenGL pipeline state by whoever loaded the content — `NifOsg` from the NIF properties,
    /// `Terrain` from its texture layers, `Shader::ShaderVisitor` for the maps it discovers by
    /// filename — and the state set is the one place the fact is kept, whatever a controller has
    /// done to it since.
    ///
    /// **A value, and cheap to copy.** A chain of state sets is folded into one of these in order,
    /// which is how a texturing property on a parent reaches the shape three levels down.
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

        /// What the surface's per-vertex colour is for.
        ///
        /// **A property of the surface, where the colours are a property of the geometry.** The two
        /// arrive apart — `NiVertexColorProperty` hangs above the shape and the array is inside its
        /// data — and a renderer needs both to know whether an array means anything.
        ///
        /// **Constant for the material's life.** `NifOsg::AlphaController` and
        /// `NifOsg::MaterialColorController` rewrite the colours beside this every frame they run
        /// and neither touches the mode, which is what lets a reader settle it once.
        VertexColour mVertexColour = VertexColour::None;

        /// What `Cutout` cuts at, in the zero-to-one range the content uses rather than the bytes
        /// `NiAlphaProperty` stores. Meaningful whenever the content asked for alpha testing, which
        /// includes surfaces that also blend.
        float mAlphaRef = 0.0f;

        /// Whether both faces of this surface are drawn and lit.
        ///
        /// **False unless the content says otherwise, because that is what the game draws.** The
        /// scene root turns `GL_CULL_FACE` on for everything under it
        /// (`apps/openmw/mwrender/renderingmanager.cpp`), so a surface nothing has spoken about
        /// shows one face, and only two records speak: a `NiStencilProperty` whose draw mode is
        /// `Both`, and a material file's two-sided flag, which can turn culling off and never on.
        /// Vanilla Morrowind has neither — none of its three archives holds a `NiStencilProperty`
        /// — and a leaf or banner meant to be seen from behind is modelled as a second copy wound
        /// the other way, which is `Rtx::ShapeFold`'s business and not this flag's.
        bool mTwoSided = false;

        /// The four colours a `NiMaterialProperty` states for a surface.
        ///
        /// **Display-encoded, and `Surface::Colour` is what says so.** A renderer working in light
        /// divides the curve out through `Rtx::decodeColour`, and one drawing in the game's own
        /// space uses them as they stand.
        Colour mDiffuseColour{ 1.0f, 1.0f, 1.0f };
        Colour mAmbientColour{ 1.0f, 1.0f, 1.0f };
        Colour mEmissiveColour;
        Colour mSpecularColour;

        /// How much of the surface is there, before its texture is read.
        ///
        /// **Beside the diffuse colour and not inside it, because that is where the record keeps
        /// it.** `NiMaterialProperty` states an alpha of its own next to four colours, and it is
        /// `osg::Material`'s four-component diffuse that merged the two — a shape a description of
        /// what the content said has no reason to copy. `NifOsg::AlphaController` animates this
        /// one field and leaves the colour alone.
        float mOpacity = 1.0f;

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

    /// What the shader visitor is told a GPU offers, for a host that has no GL context to ask.
    ///
    /// **A stand-in and not a capability.** The visitor runs on every model OpenMW loads and needs a
    /// number to fit texture slots into. The value decides only how many slots it is willing to use;
    /// the roles it labels them with, which is the whole of what a described surface carries, are the
    /// same for any number this large.
    constexpr int sAssumedTextureUnits = 32;
}
