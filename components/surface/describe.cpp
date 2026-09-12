#include "describe.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <osg/AlphaFunc>
#include <osg/Material>
#include <osg/Matrixf>
#include <osg/StateSet>
#include <osg/Texture>
#include <osg/Uniform>

#include <components/sceneutil/material.hpp>
#include <components/sceneutil/util.hpp>

#include "material.hpp"

namespace Surface
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

        Colour stated(const osg::Vec4f& colour)
        {
            return Colour{ colour.r(), colour.g(), colour.b() };
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

                if (const std::optional<TextureRole> role = textureRoleNamed(name))
                    return role;
            }

            return std::nullopt;
        }

        /// A uniform by name, without building a `std::string` where the list is empty — which it
        /// is on nearly every state set a walk meets.
        const osg::Uniform* uniformNamed(const osg::StateSet& stateSet, const std::string& name)
        {
            if (stateSet.getUniformList().empty())
                return nullptr;

            return stateSet.getUniform(name);
        }

        void readColours(const osg::StateAttribute& attribute, Material& material)
        {
            if (const auto* own = dynamic_cast<const SceneUtil::Material*>(&attribute))
            {
                const osg::Vec4f diffuse = own->getDiffuse();
                material.mDiffuseColour = stated(diffuse);
                material.mOpacity = diffuse.a();
                material.mAmbientColour = stated(own->getAmbient());
                material.mEmissiveColour = stated(own->getEmission());
                material.mSpecularColour = stated(own->getSpecular());
                material.mGlossiness = own->getShininess();
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
                material.mSpecularColour = stated(plain->getSpecular(osg::Material::FRONT));
                material.mGlossiness = plain->getShininess(osg::Material::FRONT);
            }
        }

        void readTransform(const osg::StateSet& stateSet, const unsigned int unit, Material& material)
        {
            const osg::Uniform* uniform = uniformNamed(stateSet, "texMat" + std::to_string(unit));
            if (uniform == nullptr)
                return;

            osg::Matrixf transform;
            if (!uniform->get(transform))
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

    bool describe(const osg::StateSet& stateSet, Material& material)
    {
        bool said = false;

        if (const osg::StateAttribute* colours = stateSet.getAttribute(osg::StateAttribute::MATERIAL))
        {
            readColours(*colours, material);
            said = true;
        }

        std::optional<unsigned int> diffuseUnit;
        const osg::StateSet::TextureAttributeList& units = stateSet.getTextureAttributeList();
        for (unsigned int unit = 0; unit < units.size(); ++unit)
        {
            const osg::StateAttribute* attribute = stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
            const osg::Texture* texture = attribute != nullptr ? attribute->asTexture() : nullptr;
            if (texture == nullptr)
                continue;

            // **A unit nothing names is not a surface's**, whatever the shader visitor makes of unit
            // nought. Every loader names what it binds — `NifOsg` with a `TextureType`, the terrain
            // with a sampler uniform — and the hand-built state sets that do not are the
            // rasterizer's own effects: the water's ripple particles, the sky's dome. The one such
            // picture both renderers draw, the rain, is named where it is built.
            std::optional<TextureRole> role = textureRoleNamed(SceneUtil::getTextureType(stateSet, *texture, unit));
            if (!role.has_value())
                role = roleBySampler(stateSet, unit);
            if (!role.has_value())
                continue;

            material.setTexture(*role, texture);
            said = true;
            if (*role == TextureRole::Diffuse)
                diffuseUnit = unit;
        }

        // **The alpha test's reference, from wherever the visitor left it.** `Shader::ShaderVisitor`
        // replaces the attribute with a `RemovedAlphaFunc` at a default threshold and carries the
        // real one in a uniform, so the uniform is asked first and the attribute answers where no
        // visitor has run. `ALWAYS` is no test at all, which is what the scene root wears.
        if (const auto* alpha
            = static_cast<const osg::AlphaFunc*>(stateSet.getAttribute(osg::StateAttribute::ALPHAFUNC)))
        {
            float reference = alpha->getReferenceValue();
            if (const osg::Uniform* carried = uniformNamed(stateSet, "alphaRef"))
                carried->get(reference);

            material.mAlphaRef = alpha->getFunction() != osg::AlphaFunc::ALWAYS ? reference : 0.0f;
            if (material.mAlphaMode != AlphaMode::Blend)
                material.mAlphaMode = material.mAlphaRef > 0.0f ? AlphaMode::Cutout : AlphaMode::Opaque;
        }

        // Blending wins over testing, and the threshold survives for a renderer that would rather cut.
        if (stateSet.getAttribute(osg::StateAttribute::BLENDFUNC) != nullptr)
            material.mAlphaMode = AlphaMode::Blend;
        else if (const osg::StateAttribute::GLModeValue blend = stateSet.getMode(GL_BLEND);
                 blend != osg::StateAttribute::INHERIT)
            material.mAlphaMode = (blend & osg::StateAttribute::ON) ? AlphaMode::Blend
                : material.mAlphaRef > 0.0f                         ? AlphaMode::Cutout
                                                                    : AlphaMode::Opaque;

        // Only ever off by the content: a stencil property drawing both faces, or a material file's
        // two-sided flag. The scene root turns it on for everything under it.
        if (const osg::StateAttribute::GLModeValue cull = stateSet.getMode(GL_CULL_FACE);
            cull != osg::StateAttribute::INHERIT)
            material.mTwoSided = (cull & osg::StateAttribute::ON) == 0;

        // What `NifOsg::AlphaController` animates. `MWRender::TransparencyUpdater` writes the same
        // name beside `actorFade` to fade a whole actor, which is not the surface's own opacity and
        // is read by the walk as a fade instead.
        if (const osg::Uniform* animated = uniformNamed(stateSet, "alpha"))
            if (uniformNamed(stateSet, "actorFade") == nullptr)
                animated->get(material.mOpacity);

        readTransform(stateSet, diffuseUnit.value_or(0), material);

        return said;
    }
}
