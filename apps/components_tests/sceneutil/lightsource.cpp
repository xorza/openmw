#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include <gtest/gtest.h>

#include <osg/Vec4f>

#include <algorithm>
#include <cmath>

namespace
{
    /// The colour of the flame a lamp burns with.
    const osg::Vec4f sFlame(1.0f, 0.5f, 0.25f, 1.0f);

    /// The white one a lamp carried in a pack adds, so that it lights its bearer.
    const osg::Vec4f sWhite(1.0f, 1.0f, 1.0f, 1.0f);

    /// What a Light spell puts on an actor: `Animation::setLightEffect` writes 1.5 into the ambient
    /// and leaves the diffuse at nothing.
    const osg::Vec4f sGlow(1.5f, 1.5f, 1.5f, 1.0f);

    /// A light, and the frame-by-frame write that is the subject here.
    ///
    /// **Both kinds of colour at once.** The game puts a light's output in the diffuse or in the
    /// ambient depending on what put the light there, and one lamp carrying both answers for the
    /// two — and shows that the write keeps them apart.
    ///
    /// **The write is called rather than reached through a traversal.** A `LightSource` carries an
    /// update callback that walks up for a `LightManager` and throws when there is none, so a bare
    /// one cannot be handed to an update visitor — and what that callback calls first is `update`.
    struct Lamp
    {
        osg::ref_ptr<SceneUtil::LightSource> mSource = new SceneUtil::LightSource;

        Lamp(SceneUtil::LightController::LightType type, float fade, const osg::Vec4f& ambient)
        {
            osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
            light->setDiffuse(sFlame);
            light->setSpecular(sWhite);
            light->setAmbient(ambient);

            mSource->setLight(light);
            mSource->setActorFade(fade);
            mSource->getController().setType(type);
        }

        Lamp(SceneUtil::LightController::LightType type, float fade)
            : Lamp(type, fade, sGlow)
        {
        }

        const SceneUtil::LightSource& source() const { return *mSource; }

        /// Runs one frame and gives back what the light now radiates.
        const SceneUtil::Light& lit(double seconds, size_t frame)
        {
            mSource->update(frame, seconds);

            return *mSource->getLight(frame);
        }
    };

    /// A light is dimmed by its owner, whatever it radiates with and whatever it is doing.
    ///
    /// **Two things kept the fade out**, and this guards both. `LT_Normal` returned early, so a
    /// steady lantern an actor carries kept full brightness until that actor's node mask cut it off
    /// at `actors processing range` — a light going out in one frame at the distance the game spends
    /// the last tenth of that range fading the actor away over. Twenty of the carriable lights the
    /// game ships are steady, and every one is a lantern. And the fade was applied by the animation,
    /// which writes the diffuse and the specular and never the ambient — so a Light spell's glow,
    /// whose whole output is ambient, snapped off at that same cut.
    TEST(SceneUtilLightSourceTest, aLightIsDimmedByItsOwnersFade)
    {
        Lamp full(SceneUtil::LightController::LT_Normal, /*fade=*/1.0f);
        const SceneUtil::Light& burning = full.lit(1.0, 1);
        EXPECT_EQ(burning.getDiffuse(), sFlame) << "an actor nothing is hiding";
        EXPECT_EQ(burning.getAmbient(), sGlow);

        Lamp half(SceneUtil::LightController::LT_Normal, /*fade=*/0.5f);
        const SceneUtil::Light& dimmed = half.lit(1.0, 1);
        EXPECT_EQ(dimmed.getDiffuse(), osg::Vec4f(0.5f, 0.25f, 0.125f, 0.5f)) << "half faded is half lit";
        EXPECT_EQ(dimmed.getSpecular(), osg::Vec4f(0.5f, 0.5f, 0.5f, 0.5f));
        EXPECT_EQ(dimmed.getAmbient(), osg::Vec4f(0.75f, 0.75f, 0.75f, 0.5f));

        // The same two numbers, kept apart from the colours for a renderer that works in linear
        // light: scaling a display-encoded colour is not scaling the light it stands for.
        EXPECT_EQ(half.source().getDiffuseScale(), 0.5f);
        EXPECT_EQ(half.source().getAmbientScale(), 0.5f);

        // What the distance fade reaches exactly at `actors processing range`, which is the frame
        // before the node mask takes the whole actor out of the picture.
        Lamp gone(SceneUtil::LightController::LT_Normal, /*fade=*/0.0f);
        const SceneUtil::Light& out = gone.lit(1.0, 1);
        EXPECT_EQ(out.getDiffuse(), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f))
            << "the light was still burning when the actor was cut";
        EXPECT_EQ(out.getAmbient(), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f))
            << "the glow was still burning when the actor was cut";
    }

    /// And an animated one, which is where the fade already applied, still does.
    ///
    /// **Only the zero is checked.** A flicker's brightness comes off a clock and a random phase, so
    /// what a fade of a half would leave is not a number this can predict — but nothing times nought
    /// is nought whatever the phase, and nought is the value the cut happens at.
    TEST(SceneUtilLightSourceTest, aFlickeringLightGoesOutWithItsOwner)
    {
        Lamp gone(SceneUtil::LightController::LT_Flicker, /*fade=*/0.0f);
        EXPECT_EQ(gone.lit(1.0, 1).getDiffuse(), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f));

        // The same light with nothing hiding its owner is burning, so the zero above is the fade and
        // not the flicker having settled on nothing.
        Lamp burning(SceneUtil::LightController::LT_Flicker, /*fade=*/1.0f);
        EXPECT_GT(burning.lit(1.0, 1).getDiffuse().x(), 0.0f);
    }

    /// The animation reaches the diffuse and stops there.
    ///
    /// **Because the ambient is not a flame.** The white one `ActorAnimation::addHiddenItemLight`
    /// adds is what a lamp in a pack lights its bearer with, and it has no flame of its own to
    /// flicker: a lantern the actor is not holding would otherwise pulse against a body it is
    /// nowhere near.
    TEST(SceneUtilLightSourceTest, anAnimationReachesTheDiffuseAndNotTheAmbient)
    {
        // A pulse, because it is one sine and so what it must reach is a number rather than a hope:
        // eight samples over its three-second turn put one of them within an eighth of a turn of the
        // peak, so the deepest of them is at least `0.35 * cos(pi / 8)` from rest.
        Lamp lamp(SceneUtil::LightController::LT_PulseSlow, /*fade=*/1.0f, sWhite);

        float deepest = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            const SceneUtil::Light& lit = lamp.lit(static_cast<double>(i) * 0.375, static_cast<size_t>(i));

            EXPECT_EQ(lit.getAmbient(), sWhite) << "the animation reached the ambient, at sample " << i;
            EXPECT_EQ(lamp.source().getAmbientScale(), 1.0f);

            deepest = std::max(deepest, std::abs(lit.getDiffuse().x() - sFlame.x()));
        }

        EXPECT_GT(deepest, 0.32f) << "the animation never ran";
    }

    /// A frame writes what the light is, not what the frame before it left.
    ///
    /// `LightManager::getLightsInViewSpace` dims a light that is nearly out of range by scaling the
    /// colour it finds, and counts on the next frame to put the colour back. A light that wrote
    /// nothing per frame — which every light with no animation used to be — instead kept the scaled
    /// colour and had it scaled again, so a lamp standing still faded away over a few seconds.
    TEST(SceneUtilLightSourceTest, aFrameWritesTheBaseColourRatherThanWhatTheLastOneLeft)
    {
        Lamp lamp(SceneUtil::LightController::LT_Normal, /*fade=*/1.0f);
        EXPECT_EQ(lamp.lit(1.0, 1).getDiffuse(), sFlame);

        SceneUtil::Light& dimmed = *lamp.mSource->getLight(1);
        dimmed.setDiffuse(dimmed.getDiffuse() * 0.25f);

        // Two frames on, since that is the next one to write this same buffer.
        EXPECT_EQ(lamp.lit(2.0, 3).getDiffuse(), sFlame) << "the distance fade compounded frame after frame";
    }
}
