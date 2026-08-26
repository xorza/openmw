#include <gtest/gtest.h>

#include <osg/StateSet>
#include <osg/Texture2D>

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

        TEST(SurfaceMaterialTest, aStateSetNobodyDescribedHasNoMaterial)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            EXPECT_EQ(getMaterial(*state), nullptr);
            EXPECT_EQ(getWritableMaterial(*state), nullptr);

            // Something else in the container is not a material either.
            state->getOrCreateUserDataContainer()->addUserObject(new osg::Texture2D);
            EXPECT_EQ(getMaterial(*state), nullptr);
        }

        TEST(SurfaceMaterialTest, whatIsSetIsWhatIsRead)
        {
            osg::ref_ptr<osg::Image> texture = new osg::Image;

            Material material;
            material.setTexture(TextureRole::Emissive, texture);
            material.mAlphaMode = AlphaMode::Cutout;
            material.mAlphaRef = 128.0f / 255.0f;
            material.mTwoSided = true;

            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            setMaterial(*state, material);

            const Material* read = getMaterial(*state);
            ASSERT_NE(read, nullptr);
            EXPECT_EQ(read->getTexture(TextureRole::Emissive), texture.get());
            EXPECT_EQ(read->getTexture(TextureRole::Diffuse), nullptr);
            EXPECT_EQ(read->mAlphaMode, AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(read->mAlphaRef, 128.0f / 255.0f);
            EXPECT_TRUE(read->mTwoSided);
            EXPECT_EQ(*read, material);
        }

        /// Setting twice replaces rather than accumulating, and does not add a second attachment.
        TEST(SurfaceMaterialTest, describingAgainReplacesWhatWasThere)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;

            Material first;
            first.mAlphaRef = 0.25f;
            setMaterial(*state, first);

            Material second;
            second.mAlphaRef = 0.75f;
            setMaterial(*state, second);

            ASSERT_NE(getMaterial(*state), nullptr);
            EXPECT_FLOAT_EQ(getMaterial(*state)->mAlphaRef, 0.75f);
            EXPECT_EQ(state->getUserDataContainer()->getNumUserObjects(), 1u);
        }

        /// A state set the only holder is edited in place; one a copy shares is duplicated first.
        ///
        /// **Both halves matter and for different reasons.** In place is what keeps an animated
        /// surface off the allocator — a controller rewrites its material every frame it is applied.
        /// Duplicating when shared is what stops that edit reaching every other drawable using the
        /// same surface, which is what a shallow-copied state set means: `SceneUtil::StateSetUpdater`
        /// starts its per-node scratch as exactly that.
        TEST(SurfaceMaterialTest, aWriteIsInPlaceWhenNothingElseCanSeeItAndACopyWhenItCan)
        {
            osg::ref_ptr<osg::StateSet> original = new osg::StateSet;

            Material material;
            material.mAlphaRef = 0.25f;
            setMaterial(*original, material);

            const Material* before = getMaterial(*original);
            ASSERT_EQ(getWritableMaterial(*original), before) << "nothing else holds it, so it is not moved";

            osg::ref_ptr<osg::StateSet> copy = new osg::StateSet(*original, osg::CopyOp::SHALLOW_COPY);
            ASSERT_EQ(getMaterial(*copy), before) << "a shallow copy is sharing the one description";

            getWritableMaterial(*copy)->mAlphaRef = 0.75f;

            EXPECT_NE(getMaterial(*copy), before) << "and writing to it moved the copy off the shared one";
            EXPECT_FLOAT_EQ(getMaterial(*copy)->mAlphaRef, 0.75f);
            EXPECT_FLOAT_EQ(getMaterial(*original)->mAlphaRef, 0.25f) << "the original is untouched";

            // And the copy is now the only holder of its own, so the next write stays in place.
            const Material* mine = getMaterial(*copy);
            EXPECT_EQ(getWritableMaterial(*copy), mine);
        }
    }
}
