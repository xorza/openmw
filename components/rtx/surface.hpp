#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/CopyOp>
#include <osg/Image>
#include <osg/StateAttribute>
#include <osg/Vec2f>
#include <osg/ref_ptr>

#include "texturewrap.hpp"

namespace osg
{
    class StateSet;
    class Texture;
}

namespace Rtx
{
    /// What the alpha channel of a surface's diffuse texture means. `Cutout` and `Blend` are not
    /// exclusive in a NIF, and the rasterizer already resolves them this way: blending wins, and the
    /// test threshold survives for a renderer that would rather cut out.
    enum class AlphaMode
    {
        Opaque,

        /// Test against the material's own threshold.
        Cutout,

        /// Blend. This is where the foliage is: a canopy or a banner is an `NiAlphaProperty` over a
        /// texture whose alpha is all but binary, which the original renderer sorted rather than
        /// tested. `Rtx::Material::getAlphaCutoff` says what a renderer with no sort does.
        Blend,
    };

    /// A colour as a content file states one: three channels from nought to one, display-encoded.
    /// A type, because the space is the whole of what a reader gets wrong: an `osg::Vec3f` says
    /// nothing about which side of `Rtx::decodeColour` a value is on. No arithmetic, on purpose —
    /// a gain belongs past the decode, where the numbers are light.
    struct EncodedColour
    {
        float mRed = 0.0f;
        float mGreen = 0.0f;
        float mBlue = 0.0f;

        bool operator==(const EncodedColour& other) const = default;
    };

    /// What a surface's per-vertex colour is for: `NiVertexColorProperty`'s three vertex modes,
    /// resolved against its light mode. `SceneUtil::VertexColorModes` carries six, but a NIF states
    /// only these three.
    enum class VertexColour
    {
        /// The colours are there and mean nothing, or there are none at all.
        None,

        /// It replaces the material's diffuse and ambient colour, which is what the game's own
        /// shader reads through `getDiffuseColor`. Every piece of ground is this.
        Tint,

        /// It replaces the material's emissive colour, and the light mode that goes with it takes
        /// the diffuse and the ambient to nought, so such a surface is its glow and nothing else.
        Glow,
    };

    /// What a texture is for, as the content file said. A role and not a texture unit: where a unit
    /// is bound, and whether there are units at all, is a renderer's business.
    enum class TextureRole
    {
        Diffuse,
        Normal,

        /// A normal map carrying height in its alpha, for parallax. The same slot as `Normal` to a
        /// renderer that does not do parallax.
        NormalHeight,

        Emissive,
        Specular,
        Dark,

        /// Named so a unit that carries one is a role and not an unknown, and read by nothing:
        /// none of the four occurs in the shipped files, over every NIF the three archives hold.
        Detail,
        Decal,
        Gloss,
        Bump,

        Environment,
    };

    inline constexpr std::size_t sTextureRoleCount = 11;

    /// The name the OpenGL renderer binds this role under — one table, so a typo is a build error
    /// rather than an untextured surface.
    std::string_view textureRoleName(TextureRole role);

    /// One texture as a surface uses it: the image, and how it is addressed past its edges.
    ///
    /// **The image and not an `osg::Texture2D`**, for the reason `SurfaceDescription::mTextures`
    /// gives; **and the wrap beside it**, because that is the one piece of sampler state the
    /// content decides per texture. A `NiSourceTexture` states a clamp, `NifOsg` puts it on the
    /// texture, and a renderer that read the image alone repeated every banner's edge.
    struct TextureUse
    {
        osg::ref_ptr<const osg::Image> mImage;
        TextureWrap mWrap = TextureWrap::Repeat;

        const osg::Image* get() const { return mImage.get(); }

        bool operator==(const TextureUse& other) const = default;
    };

    /// How a blended surface composites over what is behind it, off its `BlendFunc`. Three and not
    /// the eleven factors OpenGL offers, because the shipped content states exactly these three:
    /// `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` on 3,808 records, `SRC_ALPHA, ONE` on 785 and
    /// `SRC_ALPHA, DST_ALPHA` on eight — which adds, since the frame's alpha is one — and
    /// `ONE, ONE` on one.
    enum class BlendKind : std::uint8_t
    {
        /// Covers what is behind it by its alpha.
        Over,

        /// Adds to what is behind it, weighted by its alpha, and covers nothing: a flame, a
        /// magic effect's sheet.
        Add,

        /// Adds whole, its alpha unread.
        AddWhole,
    };

    /// What a parent state set has claimed for the rest of a chain.
    ///
    /// **OpenGL resolves a chain root first, and a parent that set a value with `OVERRIDE` keeps
    /// it against every child that did not set its own `PROTECTED`.** A fold that let the child
    /// win every time lost the one texture the game overrides from above — `MWRender::overrideTexture`
    /// puts the blood's texture on an effect's root that way. One lock per thing the fold reads,
    /// carried across one fold and never further.
    struct SurfaceLocks
    {
        /// One bit per `TextureRole`.
        std::uint32_t mTextures = 0;

        bool mMaterial = false;
        bool mAlphaFunc = false;
        bool mBlend = false;
        bool mCull = false;
        bool mAlpha = false;
        bool mTextureMatrix = false;
        bool mAmbientOverride = false;
        bool mEnvironmentColour = false;

        /// Whether a value carrying `flags` is taken under `lock`, and claims the lock for the rest
        /// of the chain where it carries `OVERRIDE`. The whole of OpenGL's rule, in one place.
        static bool takes(bool& lock, osg::StateAttribute::OverrideValue flags)
        {
            if (lock && (flags & osg::StateAttribute::PROTECTED) == 0)
                return false;
            if ((flags & osg::StateAttribute::OVERRIDE) != 0)
                lock = true;
            return true;
        }

        bool takesTexture(TextureRole role, osg::StateAttribute::OverrideValue flags);
    };

    /// The role a texture unit's name means, or nothing for a name that is not a role: `blendMap`
    /// and the shadow maps are bound the same way and are not what a surface is made of.
    std::optional<TextureRole> textureRoleNamed(std::string_view name);

    /// What a surface is, as the content said and before any renderer has an opinion. Read off the
    /// finished `osg::StateSet` by `describeStateSet`, because that is the one place whoever loaded
    /// the content kept the fact, whatever a controller has done to it since. A value, and cheap to
    /// copy: a chain of state sets is folded into one of these in order.
    struct SurfaceDescription
    {
        /// One texture per role, null where the content has none. The image and not an
        /// `osg::Texture2D`: `osgDB::SharedStateManager` replaces the texture `NifOsg` bound by
        /// one it never saw, and the image is what `Resource::ImageManager` caches by path and what
        /// carries the file name a renderer identifies a texture by. The wrap comes across with it,
        /// and the rest of the sampler state stays on the texture.
        std::array<TextureUse, sTextureRoleCount> mTextures;

        /// How a blended surface composites, meaningful under `AlphaMode::Blend`.
        BlendKind mBlend = BlendKind::Over;

        /// Which texture unit the dark map is bound at, meaningful where there is one. Carried
        /// because half the vanilla dark maps read the geometry's second set of texture
        /// coordinates, and which set a unit reads is the geometry's to say.
        std::uint8_t mDarkUnit = 0;

        /// What the environment map is tinted by: `envMapColor`, which `NifOsg` sets to white
        /// under a `NiTextureEffect` and `SceneUtil::GlowUpdater` to the enchantment's colour.
        EncodedColour mEnvironmentColour{ 1.0f, 1.0f, 1.0f };

        /// The ambient light the game overrides for this surface, or nothing. `EffectManager` and
        /// `Animation::addEffect` give a magic effect a white `sun.ambient`, which is what makes
        /// it fully lit in a cave; the rasterizer sums it with the material's ambient colour.
        std::optional<EncodedColour> mAmbientOverride;

        AlphaMode mAlphaMode = AlphaMode::Opaque;

        /// What the surface's per-vertex colour is for: a property of the surface, where the
        /// colours are a property of the geometry, and constant for the material's life while the
        /// controllers rewrite the colours beside it.
        VertexColour mVertexColour = VertexColour::None;

        /// What `Cutout` cuts at, from nought to one. Meaningful whenever the content asked for
        /// alpha testing, which includes surfaces that also blend.
        float mAlphaRef = 0.0f;

        /// Whether both faces of this surface are drawn and lit. False unless the content says
        /// otherwise, because the scene root turns `GL_CULL_FACE` on for everything under it, and
        /// only a `NiStencilProperty` drawing `Both` or a material file's two-sided flag turns it
        /// off. Vanilla Morrowind has neither: a leaf meant to be seen from behind is a second copy
        /// wound the other way, which is `Rtx::ShapeFold`'s business.
        bool mTwoSided = false;

        /// The four colours a `NiMaterialProperty` states for a surface, display-encoded.
        EncodedColour mDiffuseColour{ 1.0f, 1.0f, 1.0f };
        EncodedColour mAmbientColour{ 1.0f, 1.0f, 1.0f };
        EncodedColour mEmissiveColour;
        EncodedColour mSpecularColour;

        /// How much of the surface is there, before its texture is read. Beside the diffuse colour
        /// and not inside it, because that is where `NiMaterialProperty` keeps it and
        /// `NifOsg::AlphaController` animates this one field alone.
        float mOpacity = 1.0f;

        /// Clamped to OpenGL's limit at the point of authoring, because content routinely exceeds it.
        float mGlossiness = 0.0f;

        /// A separate multiplier rather than folded into `mEmissiveColour`, because a
        /// `NiMaterialColorController` animates the colour and leaves this alone.
        float mEmissiveMult = 1.0f;

        /// How texture coordinates are transformed before the surface is sampled: scaled about the
        /// middle of the texture, then offset. The resolved translation, because a
        /// `BSShaderProperty` and a `NiUVController` negate different components of what they
        /// record. `NifOsg::UVController` rewrites this every frame it is applied.
        osg::Vec2f mTextureScale{ 1.0f, 1.0f };
        osg::Vec2f mTextureOffset{ 0.0f, 0.0f };

        const osg::Image* getTexture(TextureRole role) const { return mTextures[static_cast<std::size_t>(role)].get(); }

        const TextureUse& getTextureUse(TextureRole role) const { return mTextures[static_cast<std::size_t>(role)]; }

        /// Repeating, which is what a caller that has no texture to read a wrap off means.
        void setTexture(TextureRole role, const osg::Image* image, TextureWrap wrap = TextureWrap::Repeat)
        {
            mTextures[static_cast<std::size_t>(role)] = TextureUse{ .mImage = image, .mWrap = wrap };
        }

        /// The same, taking whatever the texture was bound as, its wrap included. Null and
        /// imageless textures clear the role, which is what a placeholder a flip controller has
        /// not filled in yet amounts to.
        void setTexture(TextureRole role, const osg::Texture* texture);

        bool operator==(const SurfaceDescription& other) const = default;
    };

    /// Folds what one state set says about a surface into `into`, and says whether it said
    /// anything at all. One state set at a time, nearest last, because that is how OpenGL resolves a
    /// chain. A texture's role is the `SceneUtil::TextureType` at its unit or the sampler uniform
    /// naming it, and a unit nothing names is not the surface's; the colours, the opacity, the
    /// glossiness and the vertex-colour mode come off the `SceneUtil::Material`, the opacity again
    /// off an `alpha` uniform unless an `actorFade` stands beside it, the alpha test off the
    /// `osg::AlphaFunc` or the `alphaRef` uniform the visitor moved it into, blending off a
    /// `BlendFunc` or `GL_BLEND` and how it composites off the `BlendFunc`'s factors, two-sidedness
    /// off `GL_CULL_FACE`, the texture transform off the `texMat<unit>` uniform on the diffuse
    /// unit, the environment map's tint off `envMapColor`, and the ambient the game overrides off
    /// `sun.ambient`.
    ///
    /// A parent's `OVERRIDE` is honoured through `locks`, which a caller folding a chain carries
    /// from the root down and a caller reading one state set leaves at its default.
    ///
    /// @return whether the state set carried a material or a texture: what tells a surface from a
    ///         node that only sets a mode or a uniform on the way down.
    bool describeStateSet(const osg::StateSet& stateSet, SurfaceDescription& into, SurfaceLocks& locks);

    /// One state set on its own, under no lock.
    bool describeStateSet(const osg::StateSet& stateSet, SurfaceDescription& into);
}
