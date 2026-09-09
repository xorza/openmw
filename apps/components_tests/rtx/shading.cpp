#include <gtest/gtest.h>

#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/ref_ptr>

#include <components/rtx/shading.hpp>

namespace Rtx
{
    namespace
    {
        /// A state set carrying no uniform at all inherits what the chain already had.
        ///
        /// **Nearly every state set in the world is this one**, which is why the empty answer is
        /// asked for before the two names are. `fadeThrough` runs at every node and every drawable a
        /// walk enters, and `osg::StateSet::getUniform` searches a map keyed on `std::string`.
        TEST(RtxShadingTest, aStateSetWithNoUniformInheritsWhatTheChainHad)
        {
            const osg::ref_ptr<osg::StateSet> bare = new osg::StateSet;

            EXPECT_FLOAT_EQ(fadeThrough(*bare, 1.0f), 1.0f);
            EXPECT_FLOAT_EQ(fadeThrough(*bare, 0.25f), 0.25f) << "the chain's own fade was not carried through";
        }

        /// A state set carrying a uniform that is not the fade inherits too.
        ///
        /// **The guard is on the list and not on the answer**, so a state set with something else on
        /// it still has to reach the two lookups and come back with what it was given.
        TEST(RtxShadingTest, aStateSetWithAnotherUniformInheritsAsWell)
        {
            const osg::ref_ptr<osg::StateSet> other = new osg::StateSet;
            other->addUniform(new osg::Uniform("useNormalAsColor", 1));

            EXPECT_FLOAT_EQ(fadeThrough(*other, 0.5f), 0.5f);
        }

        /// The fade the game writes is read, and the alpha beside it is multiplied into it.
        ///
        /// `MWRender::TransparencyUpdater` writes `actorFade`, and Invisibility and Chameleon ride
        /// `alpha` on the same state set. Three quarters of a half is three eighths, and the
        /// inherited fade is replaced rather than multiplied — the nearest state set that carries
        /// the pair is the whole answer.
        TEST(RtxShadingTest, theFadeAndTheAlphaAreMultipliedAndReplaceWhatWasInherited)
        {
            const osg::ref_ptr<osg::StateSet> fading = new osg::StateSet;
            fading->addUniform(new osg::Uniform("actorFade", 0.75f));

            EXPECT_FLOAT_EQ(fadeThrough(*fading, 1.0f), 0.75f);
            EXPECT_FLOAT_EQ(fadeThrough(*fading, 0.1f), 0.75f)
                << "an inherited fade was multiplied rather than replaced";

            fading->addUniform(new osg::Uniform("alpha", 0.5f));
            EXPECT_FLOAT_EQ(fadeThrough(*fading, 1.0f), 0.375f);
        }
    }
}
