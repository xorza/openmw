#pragma once

#include <osg/Vec3f>

#include <components/sky/moonstate.hpp>
#include <components/sky/timeofday.hpp>

#include "skyutil.hpp"

namespace MWRender
{
    /// What the weather manager settled about the sky, in the world's own numbers, undecoded:
    /// every colour is a content file's three bytes over 255, and what that means is a question
    /// about a renderer's transport. `MWWorld::WeatherManager` owns one and writes it where it
    /// decides each part; both renderers reach it through `MWBase::World::getSkyState` and derive
    /// what they draw from it when they draw. Nothing here is recorded a second time by a setter.
    /// What the game itself decides — the toggles, the water, the sun light — is `WorldState`'s.
    struct SkyState
    {
        /// The weather the world settled on, whole: `WeatherManager::mResult` is this record.
        WeatherResult mWeather;

        /// The hours the day is divided into, which the sun's disc and the stars ramp by.
        Sky::TimeOfDaySettings mTimes;

        /// Whether the weather ran: outdoors, where everything below is written every update.
        /// Indoors the weather manager stops at once, and the rest holds whatever it last held.
        bool mOutdoors = false;

        /// The orbit's direction, as `WeatherManager::update` runs the sun east to west. Where the
        /// disc is drawn is `Sky::sunDiscPosition` of it, and where the light comes from is each
        /// renderer's own answer (`match sunlight to sun`).
        osg::Vec3f mSunDirection;
        bool mNight = false;

        /// Whether the disc is drawn at this hour, `Sky::sunUp`: the weather manager hides it
        /// through the night.
        bool mSunUp = true;

        /// How far the glare has come up since sunrise, nought to one over the day.
        float mGlareFade = 1.0f;

        /// Masser and Secunda. An alpha of nothing is a moon that is not drawn.
        Sky::MoonState mMoons[2] = {};

        /// What drives the particle effect: off Red Mountain at the player in an ash or blight
        /// storm, and due north otherwise. Not the deck's direction, which is the weather's own.
        /// Read only under a storm, which the same update that writes this declares.
        osg::Vec3f mStormParticleDirection;
    };
}
