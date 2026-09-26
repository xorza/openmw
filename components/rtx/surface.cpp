#include "surface.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/GL>
#include <osg/Material>
#include <osg/Matrixf>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Texture>
#include <osg/Uniform>
#include <osg/Vec4f>

#include <components/sceneutil/material.hpp>
#include <components/sceneutil/util.hpp>

#include "texels.hpp"

namespace Rtx
{
    namespace
    {
        /// The three modes a NIF can state map one for one. The other three are `SceneUtil::Material`'s
        /// alone — nothing here writes them — and ambient or diffuse on its own still tints the one
        /// albedo, while a specular the renderer has not got is nothing.
        VertexColour vertexColourOf(SceneUtil::VertexColorModes mode)
        {
            switch (mode)
            {
                case SceneUtil::VertexColorModes::Emission:
                    return VertexColour::Glow;
                case SceneUtil::VertexColorModes::AmbientAndDiffuse:
                case SceneUtil::VertexColorModes::Ambient:
                case SceneUtil::VertexColorModes::Diffuse:
                    return VertexColour::Tint;
                case SceneUtil::VertexColorModes::None:
                case SceneUtil::VertexColorModes::Specular:
                    break;
            }

            return VertexColour::None;
        }

        EncodedColour stated(const osg::Vec4f& colour)
        {
            return EncodedColour{ colour.r(), colour.g(), colour.b() };
        }

        /// The role a sampler uniform gives `unit`, or nothing where none names it. The terrain
        /// binds its layers this way — `diffuseMap = 0`, `normalMap = 2` — with no `TextureType`
        /// beside them.
        std::optional<TextureRole> roleBySampler(const osg::StateSet& stateSet, const unsigned int unit)
        {
            for (const auto& [name, uniform] : stateSet.getUniformList())
            {
                const osg::Uniform* candidate = uniform.first.get();
                if (candidate == nullptr || candidate->getType() != osg::Uniform::INT)
                    continue;

                int bound = -1;
                if (!candidate->get(bound) || bound != static_cast<int>(unit))
                    continue;

                if (const std::optional<TextureRole> role = sTextureRoleNames.named(name))
                    return role;
            }

            return std::nullopt;
        }

        /// A uniform by name and the flags it was set with, without building a `std::string` where
        /// the list is empty — which it is on nearly every state set a walk meets.
        const osg::StateSet::RefUniformPair* uniformNamed(const osg::StateSet& stateSet, const std::string& name)
        {
            const osg::StateSet::UniformList& list = stateSet.getUniformList();
            if (list.empty())
                return nullptr;

            const auto found = list.find(name);
            return found != list.end() ? &found->second : nullptr;
        }

        /// Whether a texture's wrap mode clamps. `CLAMP`, `CLAMP_TO_EDGE` and `CLAMP_TO_BORDER` are
        /// three spellings of one edge; `MIRROR` does not occur in the content and repeats.
        bool clamps(const osg::Texture::WrapMode mode)
        {
            return mode == osg::Texture::CLAMP || mode == osg::Texture::CLAMP_TO_EDGE
                || mode == osg::Texture::CLAMP_TO_BORDER;
        }

        /// How a `BlendFunc` composites. A destination of `ONE` adds, and so does `DST_ALPHA`,
        /// because the frame's alpha is one; a source of `ONE` adds the colour whole.
        BlendKind blendKindOf(const osg::BlendFunc& blend)
        {
            const bool adds
                = blend.getDestination() == osg::BlendFunc::ONE || blend.getDestination() == osg::BlendFunc::DST_ALPHA;
            if (!adds)
                return BlendKind::Over;

            return blend.getSource() == osg::BlendFunc::ONE ? BlendKind::AddWhole : BlendKind::Add;
        }

        void readColours(const osg::StateAttribute& attribute, SurfaceDescription& material)
        {
            if (const auto* own = dynamic_cast<const SceneUtil::Material*>(&attribute))
            {
                const osg::Vec4f diffuse = own->getDiffuse();
                material.mDiffuseColour = stated(diffuse);
                material.mOpacity = diffuse.a();
                material.mAmbientColour = stated(own->getAmbient());
                material.mEmissiveColour = stated(own->getEmission());
                material.mEmissiveMult = own->getEmissiveMultiplier();
                material.mVertexColour = vertexColourOf(own->getVertexColorMode());
                return;
            }

            // A state set assembled by hand rather than loaded — the character doll's default
            // material — carries OpenSceneGraph's own, which has no emissive multiplier and no
            // vertex-colour mode of the content's kind.
            if (const auto* plain = dynamic_cast<const osg::Material*>(&attribute))
            {
                const osg::Vec4f diffuse = plain->getDiffuse(osg::Material::FRONT);
                material.mDiffuseColour = stated(diffuse);
                material.mOpacity = diffuse.a();
                material.mAmbientColour = stated(plain->getAmbient(osg::Material::FRONT));
                material.mEmissiveColour = stated(plain->getEmission(osg::Material::FRONT));
            }
        }

        void readTransform(
            const osg::StateSet& stateSet, const unsigned int unit, SurfaceDescription& material, SurfaceLocks& locks)
        {
            const osg::StateSet::RefUniformPair* uniform = uniformNamed(stateSet, "texMat" + std::to_string(unit));
            if (uniform == nullptr || !SurfaceLocks::takes(locks.mTextureMatrix, uniform->second))
                return;

            osg::Matrixf transform;
            if (!uniform->first->get(transform))
                return;

            // The matrix `NifOsg::UVController` builds: scaled about the middle of the texture and
            // then offset, `(p - o) * s + o + t` with `o` at a half. Undone here to the two numbers
            // it was built from, which are what a renderer that samples rather than binds wants.
            const float scaleU = transform(0, 0);
            const float scaleV = transform(1, 1);
            material.mTextureScale = osg::Vec2f(scaleU, scaleV);
            material.mTextureOffset
                = osg::Vec2f(transform(3, 0) - 0.5f * (1.0f - scaleU), transform(3, 1) - 0.5f * (1.0f - scaleV));
        }
    }

    bool SurfaceLocks::takesTexture(const TextureRole role, const osg::StateAttribute::OverrideValue flags)
    {
        const std::uint32_t bit = 1u << static_cast<std::uint32_t>(role);
        bool lock = (mTextures & bit) != 0;
        const bool taken = takes(lock, flags);
        if (lock)
            mTextures |= bit;
        return taken;
    }

    void SurfaceDescription::setTexture(SurfaceMap map, const osg::Texture* texture)
    {
        TextureUse& use = mTextures[static_cast<std::size_t>(map)];
        if (texture == nullptr)
        {
            use = TextureUse{};
            return;
        }

        use.mImage = texture->getImage(0);
        use.mWrap = textureWrapOf(
            clamps(texture->getWrap(osg::Texture::WRAP_S)), clamps(texture->getWrap(osg::Texture::WRAP_T)));
    }

    bool describeStateSet(const osg::StateSet& stateSet, SurfaceDescription& material)
    {
        SurfaceLocks locks;
        return describeStateSet(stateSet, material, locks);
    }

    bool describeStateSet(const osg::StateSet& stateSet, SurfaceDescription& material, SurfaceLocks& locks)
    {
        bool said = false;

        if (const osg::StateSet::RefAttributePair* colours = stateSet.getAttributePair(osg::StateAttribute::MATERIAL))
        {
            if (SurfaceLocks::takes(locks.mMaterial, colours->second))
                readColours(*colours->first, material);
            said = true;
        }

        std::optional<unsigned int> diffuseUnit;
        const osg::StateSet::TextureAttributeList& units = stateSet.getTextureAttributeList();
        for (unsigned int unit = 0; unit < units.size(); ++unit)
        {
            const osg::StateSet::RefAttributePair* pair
                = stateSet.getTextureAttributePair(unit, osg::StateAttribute::TEXTURE);
            const osg::Texture* texture = pair != nullptr ? pair->first->asTexture() : nullptr;
            if (texture == nullptr)
                continue;

            // **A unit nothing names is not a surface's**, whatever the shader visitor makes of unit
            // nought. Every loader names what it binds — `NifOsg` with a `TextureType`, the terrain
            // with a sampler uniform — and the hand-built state sets that do not are the
            // rasterizer's own effects: the water's ripple particles, the sky's dome. The one such
            // picture both renderers draw, the rain, is named where it is built.
            std::optional<TextureRole> role
                = sTextureRoleNames.named(SceneUtil::getTextureType(stateSet, *texture, unit));
            if (!role.has_value())
                role = roleBySampler(stateSet, unit);
            if (!role.has_value())
                continue;

            said = true;

            // A role the trace declines is still a role — it said the surface is one — and is
            // kept nowhere, so no lock is worth taking for it.
            const std::optional<SurfaceMap> map = mapOf(*role);
            if (!map.has_value() || !locks.takesTexture(*role, pair->second))
                continue;

            material.setTexture(*map, texture);
            if (*map == SurfaceMap::Diffuse)
                diffuseUnit = unit;
            if (*map == SurfaceMap::Normal)
                material.mNormalHeight = *role == TextureRole::NormalHeight && texture->getImage(0) != nullptr
                    && carriesHeight(*texture->getImage(0));
            if (*map == SurfaceMap::Dark)
                material.mDarkUnit = static_cast<std::uint8_t>(unit);
        }

        // **The alpha test's reference, from wherever the visitor left it.** `Shader::ShaderVisitor`
        // replaces the attribute with a `RemovedAlphaFunc` at a default threshold and carries the
        // real one in a uniform, so the uniform is asked first and the attribute answers where no
        // visitor has run. `ALWAYS` is no test at all, which is what the scene root wears.
        if (const osg::StateSet::RefAttributePair* tested = stateSet.getAttributePair(osg::StateAttribute::ALPHAFUNC);
            tested != nullptr && SurfaceLocks::takes(locks.mAlphaFunc, tested->second))
        {
            const auto* alpha = static_cast<const osg::AlphaFunc*>(tested->first.get());
            float reference = alpha->getReferenceValue();
            if (const osg::StateSet::RefUniformPair* carried = uniformNamed(stateSet, "alphaRef"))
                carried->first->get(reference);

            material.mAlphaRef = alpha->getFunction() != osg::AlphaFunc::ALWAYS ? reference : 0.0f;
            if (material.mAlphaMode != AlphaMode::Blend)
                material.mAlphaMode = material.mAlphaRef > 0.0f ? AlphaMode::Cutout : AlphaMode::Opaque;
        }

        // Blending wins over testing, and the threshold survives for a renderer that would rather cut.
        // The function says how the blend composites; a mode alone says only that it does.
        if (const osg::StateSet::RefAttributePair* blended = stateSet.getAttributePair(osg::StateAttribute::BLENDFUNC))
        {
            if (SurfaceLocks::takes(locks.mBlend, blended->second))
            {
                material.mAlphaMode = AlphaMode::Blend;
                material.mBlend = blendKindOf(*static_cast<const osg::BlendFunc*>(blended->first.get()));
            }
        }
        else if (const osg::StateAttribute::GLModeValue blend = stateSet.getMode(GL_BLEND);
                 blend != osg::StateAttribute::INHERIT && SurfaceLocks::takes(locks.mBlend, blend))
            material.mAlphaMode = (blend & osg::StateAttribute::ON) ? AlphaMode::Blend
                : material.mAlphaRef > 0.0f                         ? AlphaMode::Cutout
                                                                    : AlphaMode::Opaque;

        // Only ever off by the content: a stencil property drawing both faces, or a material file's
        // two-sided flag. The scene root turns it on for everything under it.
        if (const osg::StateAttribute::GLModeValue cull = stateSet.getMode(GL_CULL_FACE);
            cull != osg::StateAttribute::INHERIT && SurfaceLocks::takes(locks.mCull, cull))
            material.mTwoSided = (cull & osg::StateAttribute::ON) == 0;

        // What `NifOsg::AlphaController` animates. `MWRender::TransparencyUpdater` writes the same
        // name beside `actorFade` to fade a whole actor, which is not the surface's own opacity and
        // is read by the walk as a fade instead.
        if (const osg::StateSet::RefUniformPair* animated = uniformNamed(stateSet, "alpha"))
            if (uniformNamed(stateSet, "actorFade") == nullptr && SurfaceLocks::takes(locks.mAlpha, animated->second))
                animated->first->get(material.mOpacity);

        // What `NifOsg` sets white under a `NiTextureEffect` and `SceneUtil::GlowUpdater` sets to
        // the enchantment's colour. Read on its own lock rather than the texture's, because the
        // glow updater rewrites the texture every frame and the colour once.
        if (const osg::StateSet::RefUniformPair* tint = uniformNamed(stateSet, "envMapColor"))
            if (SurfaceLocks::takes(locks.mEnvironmentColour, tint->second))
            {
                osg::Vec4f colour;
                if (tint->first->get(colour))
                    material.mEnvironmentColour = stated(colour);
            }

        // The ambient the game overrides for a magic effect — `SceneUtil::configureSunAmbientOverride`.
        if (const osg::StateSet::RefUniformPair* ambient = uniformNamed(stateSet, "sun.ambient"))
            if (SurfaceLocks::takes(locks.mAmbientOverride, ambient->second))
            {
                osg::Vec4f colour;
                if (ambient->first->get(colour))
                    material.mAmbientOverride = stated(colour);
            }

        readTransform(stateSet, diffuseUnit.value_or(0), material, locks);

        return said;
    }
}
