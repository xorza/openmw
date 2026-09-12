#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <components/rtx/material.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/shader/removedalphafunc.hpp>
#include <components/surface/describe.hpp>
#include <components/surface/material.hpp>

namespace Surface
{
    namespace
    {
        /// Every role has a name and every name is its own role, in both directions.
        ///
        /// **The round trip is the point.** These names were fifty string literals spread over four
        /// files before there was one table, and a typo in any of them produced an untextured
        /// surface rather than a build error.
        TEST(SurfaceMaterialTest, everyRoleRoundTripsThroughItsName)
        {
            for (std::size_t i = 0; i < sTextureRoleCount; ++i)
            {
                const auto role = static_cast<TextureRole>(i);
                const std::string_view name = textureRoleName(role);

                EXPECT_FALSE(name.empty());
                EXPECT_EQ(textureRoleNamed(name), role) << name;
            }

            EXPECT_EQ(textureRoleName(TextureRole::Diffuse), "diffuseMap");
            EXPECT_EQ(textureRoleName(TextureRole::NormalHeight), "normalHeightMap");
        }

        /// A name that is not a role is not one. `blendMap` is bound the same way and is not what a
        /// surface is made of; `diffusemap` is a typo.
        TEST(SurfaceMaterialTest, aNameThatIsNotARoleIsRefused)
        {
            EXPECT_FALSE(textureRoleNamed("blendMap").has_value());
            EXPECT_FALSE(textureRoleNamed("diffusemap").has_value());
            EXPECT_FALSE(textureRoleNamed("").has_value());
        }

        /// A state set that sets only modes and uniforms describes no surface, and leaves the
        /// material it was folded into as it was.
        TEST(SurfaceMaterialTest, aStateSetWithNoMaterialAndNoTextureSaysNothing)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            state->addUniform(new osg::Uniform("alpha", 0.5f));

            Material material;
            EXPECT_FALSE(describe(*state, material));

            // What it does say still lands: the walk folds a node that only sets a mode on the way
            // down, and a shape three nodes under it wears the mode.
            EXPECT_TRUE(material.mTwoSided);
            EXPECT_FLOAT_EQ(material.mOpacity, 0.5f);
        }

        /// A texture's role is the `TextureType` beside it or the sampler uniform naming its unit,
        /// and a unit nothing names is not the surface's: the rasterizer's own effects bind one
        /// that way, and a walk must not take the water's ripples for a surface.
        TEST(SurfaceMaterialTest, aTextureIsReadByItsTypeOrItsSamplerAndAnUnnamedUnitIsNot)
        {
            osg::ref_ptr<osg::Image> diffuse = new osg::Image;
            osg::ref_ptr<osg::Image> glow = new osg::Image;
            osg::ref_ptr<osg::Image> normal = new osg::Image;
            osg::ref_ptr<osg::Image> blend = new osg::Image;

            osg::ref_ptr<osg::StateSet> unnamed = new osg::StateSet;
            unnamed->setTextureAttributeAndModes(0, new osg::Texture2D(diffuse));

            Material material;
            EXPECT_FALSE(describe(*unnamed, material));
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), nullptr);

            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setTextureAttributeAndModes(0, new osg::Texture2D(diffuse));
            state->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));
            state->setTextureAttributeAndModes(1, new osg::Texture2D(glow));
            state->setTextureAttribute(1, new SceneUtil::TextureType("emissiveMap"));
            state->setTextureAttributeAndModes(2, new osg::Texture2D(normal));
            state->addUniform(new osg::Uniform("normalMap", 2));
            // A blend map is bound the same way and is not what the surface is made of.
            state->setTextureAttributeAndModes(3, new osg::Texture2D(blend));
            state->addUniform(new osg::Uniform("blendMap", 3));

            EXPECT_TRUE(describe(*state, material));
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), diffuse.get());
            EXPECT_EQ(material.getTexture(TextureRole::Emissive), glow.get());
            EXPECT_EQ(material.getTexture(TextureRole::Normal), normal.get());
            for (const TextureRole other :
                { TextureRole::NormalHeight, TextureRole::Specular, TextureRole::Dark, TextureRole::Detail,
                    TextureRole::Decal, TextureRole::Gloss, TextureRole::Bump, TextureRole::Environment })
                EXPECT_EQ(material.getTexture(other), nullptr) << textureRoleName(other);
        }

        /// The colours come off the material attribute, and the opacity off it too until an
        /// `alpha` uniform animates it — unless `actorFade` stands beside that, which is the game
        /// fading an actor and not the surface's own.
        TEST(SurfaceMaterialTest, theColoursAreTheMaterialsAndTheOpacityFollowsTheAlphaUniform)
        {
            osg::ref_ptr<SceneUtil::Material> colours = new SceneUtil::Material;
            colours->setDiffuse(osg::Vec4f(0.25f, 0.5f, 0.75f, 0.5f));
            colours->setAmbient(osg::Vec4f(0.1f, 0.2f, 0.3f, 1.0f));
            colours->setEmission(osg::Vec4f(0.5f, 0.25f, 0.0f, 1.0f));
            colours->setSpecular(osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f));
            colours->setShininess(12.0f);
            colours->setEmissiveMultiplier(2.0f);
            colours->setVertexColorMode(SceneUtil::VertexColorModes::Emission);

            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setAttribute(colours);

            Material material;
            EXPECT_TRUE(describe(*state, material));
            EXPECT_EQ(material.mDiffuseColour, (Colour{ 0.25f, 0.5f, 0.75f }));
            EXPECT_FLOAT_EQ(material.mOpacity, 0.5f);
            EXPECT_EQ(material.mAmbientColour, (Colour{ 0.1f, 0.2f, 0.3f }));
            EXPECT_EQ(material.mEmissiveColour, (Colour{ 0.5f, 0.25f, 0.0f }));
            EXPECT_EQ(material.mSpecularColour, Colour{});
            EXPECT_FLOAT_EQ(material.mGlossiness, 12.0f);
            EXPECT_FLOAT_EQ(material.mEmissiveMult, 2.0f);
            EXPECT_EQ(material.mVertexColour, VertexColour::Glow);

            // What `NifOsg::AlphaController` writes, on a state set of the traversal's own.
            osg::ref_ptr<osg::StateSet> animated = new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
            animated->addUniform(new osg::Uniform("alpha", 0.125f));
            describe(*animated, material);
            EXPECT_FLOAT_EQ(material.mOpacity, 0.125f);

            // What `MWRender::TransparencyUpdater` writes above a whole actor, which is a fade the
            // walk reads for itself and not a surface's opacity.
            osg::ref_ptr<osg::StateSet> faded = new osg::StateSet;
            faded->addUniform(new osg::Uniform("alpha", 0.75f));
            faded->addUniform(new osg::Uniform("actorFade", 0.5f));
            material.mOpacity = 1.0f;
            EXPECT_FALSE(describe(*faded, material));
            EXPECT_FLOAT_EQ(material.mOpacity, 1.0f);
        }

        /// A test is a cutout at its reference, a blend function wins over it, and the visitor's
        /// rewrite — a `RemovedAlphaFunc` at a default threshold beside an `alphaRef` uniform —
        /// reads the same as the attribute it replaced. `ALWAYS` is no test, which is what the
        /// scene root wears.
        TEST(SurfaceMaterialTest, alphaTestingAndBlendingReadAsTheLoaderWroteThem)
        {
            osg::ref_ptr<osg::StateSet> tested = new osg::StateSet;
            tested->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 128.0f / 255.0f));

            Material material;
            describe(*tested, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 128.0f / 255.0f);

            osg::ref_ptr<osg::StateSet> visited = new osg::StateSet;
            visited->setAttribute(Shader::RemovedAlphaFunc::getInstance(osg::AlphaFunc::GREATER),
                osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
            visited->addUniform(new osg::Uniform("alphaRef", 64.0f / 255.0f));

            material = Material{};
            describe(*visited, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 64.0f / 255.0f);

            // Blending on top of the test: the mode says blend and the threshold survives.
            osg::ref_ptr<osg::StateSet> blended = new osg::StateSet;
            blended->setAttributeAndModes(new osg::BlendFunc);
            describe(*blended, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 64.0f / 255.0f);

            // And a test folded in after the blend keeps the blend.
            describe(*tested, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 128.0f / 255.0f);

            osg::ref_ptr<osg::StateSet> root = new osg::StateSet;
            root->setAttribute(Shader::RemovedAlphaFunc::getInstance(GL_ALWAYS));
            material = Material{};
            describe(*root, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Opaque);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 0.0f);
        }

        /// The texture transform is the scale and offset `NifOsg::UVController` built its matrix
        /// from, undone: scaled about the middle of the texture, then offset.
        TEST(SurfaceMaterialTest, theTextureTransformIsUndoneToWhatTheControllerBuiltItFrom)
        {
            const osg::Vec3f origin(0.5f, 0.5f, 0.0f);
            osg::Matrixf transform = osg::Matrixf::translate(origin);
            transform.preMultScale(osg::Vec3f(2.0f, 4.0f, 1.0f));
            transform.preMultTranslate(-origin);
            transform.setTrans(transform.getTrans() + osg::Vec3f(-0.25f, 0.5f, 0.0f));

            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setTextureAttributeAndModes(0, new osg::Texture2D(new osg::Image));
            state->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));
            SceneUtil::setupTexMatForStateSet(*state, 0, transform);

            Material material;
            describe(*state, material);
            EXPECT_EQ(material.mTextureScale, osg::Vec2f(2.0f, 4.0f));
            EXPECT_EQ(material.mTextureOffset, osg::Vec2f(-0.25f, 0.5f));
        }
    }
}
