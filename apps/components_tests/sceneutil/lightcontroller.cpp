#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/NodeVisitor>

namespace
{
    using namespace testing;

    /// The visitor a light controller reads its clock and its buffer index off.
    struct Tick : osg::NodeVisitor
    {
        osg::ref_ptr<osg::FrameStamp> mStamp = new osg::FrameStamp;

        explicit Tick(double seconds, unsigned int traversal)
            : osg::NodeVisitor(UPDATE_VISITOR, TRAVERSE_ALL_CHILDREN)
        {
            mStamp->setSimulationTime(seconds);
            setFrameStamp(mStamp);
            setTraversalNumber(static_cast<int>(traversal));
        }
    };

    /// A light and the controller that drives it, held apart.
    ///
    /// **The controller is called rather than installed.** A `LightSource` carries a collect callback
    /// of its own that walks up for a `LightManager` and throws when there is none, so a bare one
    /// cannot be handed to an update traversal — and the subject here is the controller, not the
    /// manager it would report to.
    struct Lamp
    {
        osg::ref_ptr<SceneUtil::LightSource> mSource = new SceneUtil::LightSource;
        osg::ref_ptr<SceneUtil::LightController> mController = new SceneUtil::LightController;

        Lamp(SceneUtil::LightController::LightType type, float fade)
        {
            mSource->setLight(new SceneUtil::Light);
            mSource->setActorFade(fade);

            mController->setType(type);
            mController->setDiffuse(osg::Vec4f(1.0f, 0.5f, 0.25f, 1.0f));
            mController->setSpecular(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
        }

        /// Runs one update and gives back what the light now radiates.
        osg::Vec4f lit(double seconds, unsigned int traversal)
        {
            Tick tick(seconds, traversal);
            (*mController)(mSource.get(), &tick);

            return mSource->getLight(traversal)->getDiffuse();
        }
    };

    /// A steady light is dimmed by its owner, exactly as an animated one is.
    ///
    /// **The type is what decided whether the fade applied at all**, and that is what this guards:
    /// `LT_Normal` returns early, so a lantern an actor carries would keep full brightness until
    /// that actor's node mask cut it off at `actors processing range` — a light going out in one
    /// frame at the distance the game spends the last tenth of that range fading the actor away
    /// over. Twenty of the carriable lights the game ships are steady, and every one is a lantern.
    TEST(SceneUtilLightControllerTest, aSteadyLightIsDimmedByItsOwnersFade)
    {
        Lamp full(SceneUtil::LightController::LT_Normal, /*fade=*/1.0f);
        EXPECT_EQ(full.lit(1.0, 1), osg::Vec4f(1.0f, 0.5f, 0.25f, 1.0f)) << "an actor nothing is hiding";

        Lamp half(SceneUtil::LightController::LT_Normal, /*fade=*/0.5f);
        EXPECT_EQ(half.lit(1.0, 1), osg::Vec4f(0.5f, 0.25f, 0.125f, 0.5f)) << "half faded is half lit";

        // What the distance fade reaches exactly at `actors processing range`, which is the frame
        // before the node mask takes the whole actor out of the picture.
        Lamp gone(SceneUtil::LightController::LT_Normal, /*fade=*/0.0f);
        EXPECT_EQ(gone.lit(1.0, 1), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f))
            << "the light was still burning when the actor was cut";
    }

    /// And an animated one, which is where the fade already applied, still does.
    ///
    /// **Only the zero is checked.** A flicker's brightness comes off a clock and a random phase, so
    /// what a fade of a half would leave is not a number this can predict — but nothing times nought
    /// is nought whatever the phase, and nought is the value the cut happens at.
    TEST(SceneUtilLightControllerTest, aFlickeringLightGoesOutWithItsOwner)
    {
        Lamp gone(SceneUtil::LightController::LT_Flicker, /*fade=*/0.0f);
        EXPECT_EQ(gone.lit(1.0, 1), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f));

        // The same light with nothing hiding its owner is burning, so the zero above is the fade and
        // not the flicker having settled on nothing.
        Lamp burning(SceneUtil::LightController::LT_Flicker, /*fade=*/1.0f);
        EXPECT_GT(burning.lit(1.0, 1).x(), 0.0f);
    }
}
