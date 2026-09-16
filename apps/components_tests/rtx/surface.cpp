#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/CopyOp>
#include <osg/GL>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/rtx/surface.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/shader/removedalphafunc.hpp>

namespace Rtx
{
    namespace
    {
        /// Every role has a name and every name is its own role, in both directions.
        ///
        /// **The round trip is the point.** These names were fifty string literals spread over four
        /// files before there was one table, and a typo in any of them produced an untextured
        /// surface rather than a build error.
        TEST(RtxSurfaceTest, everyRoleRoundTripsThroughItsName)
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
        TEST(RtxSurfaceTest, aNameThatIsNotARoleIsRefused)
        {
            EXPECT_FALSE(textureRoleNamed("blendMap").has_value());
            EXPECT_FALSE(textureRoleNamed("diffusemap").has_value());
            EXPECT_FALSE(textureRoleNamed("").has_value());
        }

        /// A state set that sets only modes and uniforms describes no surface, and leaves the
        /// material it was folded into as it was.
        TEST(RtxSurfaceTest, aStateSetWithNoMaterialAndNoTextureSaysNothing)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            state->addUniform(new osg::Uniform("alpha", 0.5f));

            SurfaceDescription material;
            EXPECT_FALSE(describeStateSet(*state, material));

            // What it does say still lands: the walk folds a node that only sets a mode on the way
            // down, and a shape three nodes under it wears the mode.
            EXPECT_TRUE(material.mTwoSided);
            EXPECT_FLOAT_EQ(material.mOpacity, 0.5f);
        }

        /// A texture's role is the `TextureType` beside it or the sampler uniform naming its unit,
        /// and a unit nothing names is not the surface's: the rasterizer's own effects bind one
        /// that way, and a walk must not take the water's ripples for a surface.
        TEST(RtxSurfaceTest, aTextureIsReadByItsTypeOrItsSamplerAndAnUnnamedUnitIsNot)
        {
            osg::ref_ptr<osg::Image> diffuse = new osg::Image;
            osg::ref_ptr<osg::Image> glow = new osg::Image;
            osg::ref_ptr<osg::Image> normal = new osg::Image;
            osg::ref_ptr<osg::Image> blend = new osg::Image;

            osg::ref_ptr<osg::StateSet> unnamed = new osg::StateSet;
            unnamed->setTextureAttributeAndModes(0, new osg::Texture2D(diffuse));

            SurfaceDescription material;
            EXPECT_FALSE(describeStateSet(*unnamed, material));
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

            EXPECT_TRUE(describeStateSet(*state, material));
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
        TEST(RtxSurfaceTest, theColoursAreTheMaterialsAndTheOpacityFollowsTheAlphaUniform)
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

            SurfaceDescription material;
            EXPECT_TRUE(describeStateSet(*state, material));
            EXPECT_EQ(material.mDiffuseColour, (EncodedColour{ 0.25f, 0.5f, 0.75f }));
            EXPECT_FLOAT_EQ(material.mOpacity, 0.5f);
            EXPECT_EQ(material.mAmbientColour, (EncodedColour{ 0.1f, 0.2f, 0.3f }));
            EXPECT_EQ(material.mEmissiveColour, (EncodedColour{ 0.5f, 0.25f, 0.0f }));
            EXPECT_EQ(material.mSpecularColour, EncodedColour{});
            EXPECT_FLOAT_EQ(material.mGlossiness, 12.0f);
            EXPECT_FLOAT_EQ(material.mEmissiveMult, 2.0f);
            EXPECT_EQ(material.mVertexColour, VertexColour::Glow);

            // What `NifOsg::AlphaController` writes, on a state set of the traversal's own.
            osg::ref_ptr<osg::StateSet> animated = new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
            animated->addUniform(new osg::Uniform("alpha", 0.125f));
            describeStateSet(*animated, material);
            EXPECT_FLOAT_EQ(material.mOpacity, 0.125f);

            // What `MWRender::TransparencyUpdater` writes above a whole actor, which is a fade the
            // walk reads for itself and not a surface's opacity.
            osg::ref_ptr<osg::StateSet> faded = new osg::StateSet;
            faded->addUniform(new osg::Uniform("alpha", 0.75f));
            faded->addUniform(new osg::Uniform("actorFade", 0.5f));
            material.mOpacity = 1.0f;
            EXPECT_FALSE(describeStateSet(*faded, material));
            EXPECT_FLOAT_EQ(material.mOpacity, 1.0f);
        }

        /// A test is a cutout at its reference, a blend function wins over it, and the visitor's
        /// rewrite — a `RemovedAlphaFunc` at a default threshold beside an `alphaRef` uniform —
        /// reads the same as the attribute it replaced. `ALWAYS` is no test, which is what the
        /// scene root wears.
        TEST(RtxSurfaceTest, alphaTestingAndBlendingReadAsTheLoaderWroteThem)
        {
            osg::ref_ptr<osg::StateSet> tested = new osg::StateSet;
            tested->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 128.0f / 255.0f));

            SurfaceDescription material;
            describeStateSet(*tested, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 128.0f / 255.0f);

            osg::ref_ptr<osg::StateSet> visited = new osg::StateSet;
            visited->setAttribute(Shader::RemovedAlphaFunc::getInstance(osg::AlphaFunc::GREATER),
                osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
            visited->addUniform(new osg::Uniform("alphaRef", 64.0f / 255.0f));

            material = SurfaceDescription{};
            describeStateSet(*visited, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 64.0f / 255.0f);

            // Blending on top of the test: the mode says blend and the threshold survives.
            osg::ref_ptr<osg::StateSet> blended = new osg::StateSet;
            blended->setAttributeAndModes(new osg::BlendFunc);
            describeStateSet(*blended, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 64.0f / 255.0f);

            // And a test folded in after the blend keeps the blend.
            describeStateSet(*tested, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 128.0f / 255.0f);

            osg::ref_ptr<osg::StateSet> root = new osg::StateSet;
            root->setAttribute(Shader::RemovedAlphaFunc::getInstance(GL_ALWAYS));
            material = SurfaceDescription{};
            describeStateSet(*root, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Opaque);
            EXPECT_FLOAT_EQ(material.mAlphaRef, 0.0f);
        }

        /// A parent that set a texture `OVERRIDE` keeps it against a child that did not set its own
        /// `PROTECTED`, which is how OpenGL resolves the chain and how `MWRender::overrideTexture`
        /// puts the blood's texture on an effect's root. The lock is the fold's and not the
        /// description's: a fresh fold over the same leaf reads the leaf.
        TEST(RtxSurfaceTest, aParentsOverrideKeepsItsTextureAgainstAChildUnlessTheChildIsProtected)
        {
            osg::ref_ptr<osg::Image> blood = new osg::Image;
            osg::ref_ptr<osg::Image> own = new osg::Image;

            osg::ref_ptr<osg::StateSet> root = new osg::StateSet;
            root->setTextureAttribute(0, new osg::Texture2D(blood), osg::StateAttribute::OVERRIDE);
            root->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"), osg::StateAttribute::OVERRIDE);

            osg::ref_ptr<osg::StateSet> leaf = new osg::StateSet;
            leaf->setTextureAttributeAndModes(0, new osg::Texture2D(own));
            leaf->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));

            SurfaceLocks locks;
            SurfaceDescription material;
            EXPECT_TRUE(describeStateSet(*root, material, locks));
            EXPECT_TRUE(describeStateSet(*leaf, material, locks));
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), blood.get());

            // A protected leaf wins back what the root claimed.
            osg::ref_ptr<osg::StateSet> protectedLeaf = new osg::StateSet;
            protectedLeaf->setTextureAttribute(0, new osg::Texture2D(own), osg::StateAttribute::PROTECTED);
            protectedLeaf->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));

            locks = SurfaceLocks{};
            material = SurfaceDescription{};
            describeStateSet(*root, material, locks);
            describeStateSet(*protectedLeaf, material, locks);
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), own.get());

            // The lock is per role: the root's diffuse claims nothing about the leaf's glow.
            osg::ref_ptr<osg::StateSet> glowing = new osg::StateSet;
            glowing->setTextureAttributeAndModes(1, new osg::Texture2D(own));
            glowing->setTextureAttribute(1, new SceneUtil::TextureType("emissiveMap"));

            locks = SurfaceLocks{};
            material = SurfaceDescription{};
            describeStateSet(*root, material, locks);
            describeStateSet(*glowing, material, locks);
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), blood.get());
            EXPECT_EQ(material.getTexture(TextureRole::Emissive), own.get());

            // And the one-state-set fold carries no lock, so the leaf on its own reads the leaf.
            material = SurfaceDescription{};
            describeStateSet(*leaf, material);
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), own.get());
        }

        /// The other things a parent can claim: the material's colours, the alpha uniform, the
        /// modes. Each under its own lock, so a `GL_CULL_FACE` a root overrides leaves a leaf's
        /// blend alone.
        TEST(RtxSurfaceTest, aParentsOverrideLocksEachThingItSetsAndNothingElse)
        {
            osg::ref_ptr<SceneUtil::Material> rootColours = new SceneUtil::Material;
            rootColours->setDiffuse(osg::Vec4f(0.25f, 0.25f, 0.25f, 1.0f));
            osg::ref_ptr<SceneUtil::Material> leafColours = new SceneUtil::Material;
            leafColours->setDiffuse(osg::Vec4f(0.75f, 0.75f, 0.75f, 1.0f));

            osg::ref_ptr<osg::StateSet> root = new osg::StateSet;
            root->setAttribute(rootColours, osg::StateAttribute::OVERRIDE);
            root->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            root->addUniform(new osg::Uniform("alpha", 0.5f), osg::StateAttribute::OVERRIDE);

            osg::ref_ptr<osg::StateSet> leaf = new osg::StateSet;
            leaf->setAttribute(leafColours);
            leaf->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
            leaf->setAttributeAndModes(new osg::BlendFunc);
            leaf->addUniform(new osg::Uniform("alpha", 0.125f));

            SurfaceLocks locks;
            SurfaceDescription material;
            describeStateSet(*root, material, locks);
            describeStateSet(*leaf, material, locks);

            EXPECT_EQ(material.mDiffuseColour, (EncodedColour{ 0.25f, 0.25f, 0.25f }));
            EXPECT_TRUE(material.mTwoSided);
            EXPECT_FLOAT_EQ(material.mOpacity, 0.5f);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend) << "the blend was claimed by nobody";
        }

        /// The wrap comes off the texture, the one piece of sampler state the content decides per
        /// texture: every banner and every torch flame in the game clamps, and a description that
        /// dropped it repeated their edges.
        TEST(RtxSurfaceTest, theWrapComesOffTheTextureAndAnImageAloneRepeats)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;

            osg::ref_ptr<osg::Texture2D> clamped = new osg::Texture2D(image);
            clamped->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            clamped->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP);

            osg::ref_ptr<osg::Texture2D> alongT = new osg::Texture2D(image);
            alongT->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            alongT->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);

            osg::ref_ptr<osg::Texture2D> mirrored = new osg::Texture2D(image);
            mirrored->setWrap(osg::Texture::WRAP_S, osg::Texture::MIRROR);
            mirrored->setWrap(osg::Texture::WRAP_T, osg::Texture::MIRROR);

            SurfaceDescription material;
            material.setTexture(TextureRole::Diffuse, clamped.get());
            EXPECT_EQ(material.getTextureUse(TextureRole::Diffuse).mWrap, TextureWrap::Clamp);
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), image.get());

            material.setTexture(TextureRole::Diffuse, alongT.get());
            EXPECT_EQ(material.getTextureUse(TextureRole::Diffuse).mWrap, TextureWrap::ClampT);

            material.setTexture(TextureRole::Diffuse, mirrored.get());
            EXPECT_EQ(material.getTextureUse(TextureRole::Diffuse).mWrap, TextureWrap::Repeat);

            material.setTexture(TextureRole::Diffuse, image.get());
            EXPECT_EQ(material.getTextureUse(TextureRole::Diffuse).mWrap, TextureWrap::Repeat);

            material.setTexture(TextureRole::Diffuse, static_cast<const osg::Texture*>(nullptr));
            EXPECT_EQ(material.getTexture(TextureRole::Diffuse), nullptr);

            EXPECT_EQ(textureWrapOf(true, false), TextureWrap::ClampS);
            EXPECT_TRUE(clampsS(TextureWrap::ClampS));
            EXPECT_FALSE(clampsT(TextureWrap::ClampS));
            EXPECT_TRUE(clampsT(TextureWrap::Clamp));
        }

        /// How a blend composites comes off the function's factors: a destination of `ONE` adds,
        /// and so does `DST_ALPHA` because the frame's alpha is one; a source of `ONE` adds whole.
        /// A `GL_BLEND` mode on its own says only that the surface blends over.
        TEST(RtxSurfaceTest, theBlendKindComesOffTheFunctionsFactors)
        {
            const auto kindOf = [](GLenum source, GLenum destination) {
                osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
                state->setAttributeAndModes(new osg::BlendFunc(source, destination));
                SurfaceDescription material;
                describeStateSet(*state, material);
                EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
                return material.mBlend;
            };

            EXPECT_EQ(kindOf(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), BlendKind::Over);
            EXPECT_EQ(kindOf(GL_SRC_ALPHA, GL_ONE), BlendKind::Add);
            EXPECT_EQ(kindOf(GL_SRC_ALPHA, GL_DST_ALPHA), BlendKind::Add);
            EXPECT_EQ(kindOf(GL_ONE, GL_ONE), BlendKind::AddWhole);

            osg::ref_ptr<osg::StateSet> mode = new osg::StateSet;
            mode->setMode(GL_BLEND, osg::StateAttribute::ON);
            SurfaceDescription material;
            describeStateSet(*mode, material);
            EXPECT_EQ(material.mAlphaMode, AlphaMode::Blend);
            EXPECT_EQ(material.mBlend, BlendKind::Over);
        }

        /// The environment map's tint, the ambient the game overrides for a magic effect, and the
        /// unit a dark map is bound at are all read where the loader and the game put them.
        TEST(RtxSurfaceTest, theEnvironmentTintTheAmbientOverrideAndTheDarkUnitAreRead)
        {
            osg::ref_ptr<osg::Image> sheet = new osg::Image;
            osg::ref_ptr<osg::Image> dark = new osg::Image;

            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setTextureAttributeAndModes(1, new osg::Texture2D(dark));
            state->setTextureAttribute(1, new SceneUtil::TextureType("darkMap"));
            state->setTextureAttributeAndModes(2, new osg::Texture2D(sheet));
            state->setTextureAttribute(2, new SceneUtil::TextureType("envMap"));
            state->addUniform(new osg::Uniform("envMapColor", osg::Vec4f(0.25f, 0.5f, 1.0f, 1.0f)));
            state->addUniform(new osg::Uniform("sun.ambient", osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

            SurfaceDescription material;
            EXPECT_TRUE(describeStateSet(*state, material));
            EXPECT_EQ(material.getTexture(TextureRole::Environment), sheet.get());
            EXPECT_EQ(material.mEnvironmentColour, (EncodedColour{ 0.25f, 0.5f, 1.0f }));
            EXPECT_EQ(material.getTexture(TextureRole::Dark), dark.get());
            EXPECT_EQ(material.mDarkUnit, 1);
            ASSERT_TRUE(material.mAmbientOverride.has_value());
            EXPECT_EQ(*material.mAmbientOverride, (EncodedColour{ 1.0f, 1.0f, 1.0f }));

            // A state set with neither leaves both where they were, as everything else does.
            osg::ref_ptr<osg::StateSet> plain = new osg::StateSet;
            SurfaceDescription untouched;
            describeStateSet(*plain, untouched);
            EXPECT_EQ(untouched.mEnvironmentColour, (EncodedColour{ 1.0f, 1.0f, 1.0f }));
            EXPECT_FALSE(untouched.mAmbientOverride.has_value());
        }

        /// The texture transform is the scale and offset `NifOsg::UVController` built its matrix
        /// from, undone: scaled about the middle of the texture, then offset.
        TEST(RtxSurfaceTest, theTextureTransformIsUndoneToWhatTheControllerBuiltItFrom)
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

            SurfaceDescription material;
            describeStateSet(*state, material);
            EXPECT_EQ(material.mTextureScale, osg::Vec2f(2.0f, 4.0f));
            EXPECT_EQ(material.mTextureOffset, osg::Vec2f(-0.25f, 0.5f));
        }
    }
}
