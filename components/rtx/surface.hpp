#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/ref_ptr>

namespace osg
{
    class StateSet;
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
        /// carries the file name a renderer identifies a texture by. Sampler state stays on the
        /// texture.
        std::array<osg::ref_ptr<const osg::Image>, sTextureRoleCount> mTextures;

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

        void setTexture(TextureRole role, const osg::Image* image)
        {
            mTextures[static_cast<std::size_t>(role)] = image;
        }

        /// The same, taking whatever the texture was bound as. Null and imageless textures clear the
        /// role, which is what a placeholder a flip controller has not filled in yet amounts to.
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
    /// `BlendFunc` or `GL_BLEND`, two-sidedness off `GL_CULL_FACE`, and the texture transform off
    /// the `texMat<unit>` uniform on the diffuse unit.
    ///
    /// @return whether the state set carried a material or a texture: what tells a surface from a
    ///         node that only sets a mode or a uniform on the way down.
    bool describeStateSet(const osg::StateSet& stateSet, SurfaceDescription& into);
}
