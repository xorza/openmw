#ifndef GAME_RENDER_SCENEFRAME_H
#define GAME_RENDER_SCENEFRAME_H

#include <cstdint>
#include <optional>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/esm3/loadcell.hpp>
#include <components/sky/moonstate.hpp>

namespace osg
{
    class Camera;
    class FrameStamp;
    class Node;
}

namespace Resource
{
    class ImageManager;
}

namespace Terrain
{
    class ObjectStorage;
    class World;
}

namespace MWRender
{
    struct WeatherResult;

    /// What kind of place the player is standing in, as the cell record says. Three and not two,
    /// because a quasi-exterior — Vivec's cantons — is an interior cell with a sky and weather: the
    /// `isInterior` uniform counts it as inside, the shader chain's exterior mask as outside, and
    /// `MWRender::readWorld` as neither, because it stands in a weather's air with no ring of cut
    /// ground under it.
    enum class Location
    {
        Interior,
        QuasiExterior,
        Exterior,
    };

    /// A distance fog, as the game describes one: a colour and the linear ramp it fills.
    struct FogBand
    {
        osg::Vec4f mColour;
        float mStart = 0.0f;
        float mEnd = 0.0f;
    };

    /// What the world is doing this frame. Read off where it settled — `mSunLight`, `FogManager`,
    /// `Precipitation` — wherever something keeps the value, so it cannot disagree with what is
    /// drawn; and kept here by the setter that decided it where nothing else does: the drawn sun,
    /// the water switch, the weather and the moons. In the world's own numbers, undecoded:
    /// every colour is a content file's three bytes over 255, and what that means is a question
    /// about a renderer's transport.
    struct WorldState
    {
        /// Where the sun is drawn, which is not where its light comes from whenever
        /// `match sunlight to sun` is off.
        osg::Vec4f mSunPosition;

        /// The way the light travels: the rasterizer's, and a ray tracer takes `-mSunPosition`
        /// instead, or its shadows, glitter and haze each stand around a different sun.
        osg::Vec4f mSunVector;

        bool mSunAtNight = false;
        osg::Vec4f mSunColour;
        float mSunVisibility = 0.0f;

        /// What the disc is painted with, and its transparency in `w`, which sits at one all night
        /// with the disc hidden — `Rtx::sunShareAt` answers whether there is a sun. Not
        /// `mSunColour`, whose night value is the dome's blue and turns a disc blue through every dawn.
        osg::Vec4f mSunDiscColour{ 1.0f, 1.0f, 1.0f, 0.0f };

        /// How much of the sun this weather lets through, which dims a disc under an overcast and
        /// keeps the stars in behind one.
        float mSunGlare = 1.0f;

        /// How far the deck has crossed from this weather's cloud texture to the next one's. Not
        /// `mWeatherTransition`: each weather carries a `Transition_Delta` of its own, so the clouds
        /// cross on a curve while every colour crosses linearly.
        float mCloudBlend = 0.0f;

        /// How far out the stars have come: the engine's four-point `Stars` ramp at this hour,
        /// before the weather's glare is taken off it.
        float mNightFade = 0.0f;

        /// What the cloud deck is lit by, before `SkyManager::setWeather` lifts it by an eighth: the
        /// weather's own fog colour and not `mAir`'s, which knows about being underwater and about
        /// a room.
        osg::Vec4f mCloudFog;

        /// What the weather drops: the rain box and the driven effect, or null where there is none.
        /// Nodes rather than a description, because they are `osgParticle` systems the game's
        /// `Precipitation` builds and both renderers walk. Both are camera-relative and a walk
        /// stands them at the eye.
        osg::Node* mRain = nullptr;
        osg::Node* mWeatherEffect = nullptr;

        /// Whether what is falling is kept out from under roofs, and over what range about the eye:
        /// the state the precipitation says its occluder is in, for the renderer that has one.
        bool mPrecipitating = false;
        osg::Vec3f mPrecipitationRange;

        /// How much of what is falling rings the water, nought to one: the precipitation's alpha
        /// where its kind makes ripples. `Water::setRainIntensity` takes the same number.
        float mRainOnWater = 0.0f;

        /// How far the cloud deck has scrolled, in texture units, and how far the star sphere has
        /// rolled, in radians. Clocks the game advances while the sky is on, because the deck runs
        /// on the weather's speed and the stars come round once in four days; neither is a function
        /// of the hour.
        float mCloudScroll = 0.0f;
        float mStarRoll = 0.0f;

        /// The weather the world settled on, whole, for a renderer that draws a dome out of it the
        /// way `SkyManager::setWeather` does; null until the weather has run, which it does only
        /// outdoors. A pointer, because the record holds strings and the frame is built per frame.
        const WeatherResult* mWeather = nullptr;

        /// Whether there is a sky to draw: outdoors, and `tsky` has not turned it off. What
        /// `RenderingManager::setSkyEnabled` was last told.
        bool mSkyEnabled = false;

        /// Whether the disc is drawn at this hour — the weather manager hides it through the night
        /// — and how far the glare has come up since sunrise, nought to one over the day.
        bool mSunEnabled = true;
        float mGlareFade = 1.0f;

        /// Whether a script has painted Secunda red, `Moons_Script_Color`.
        bool mMoonRed = false;

        /// Includes the night-eye effect and, in a room, the lift `configureAmbient` gives it.
        /// `mRoom` is the record.
        osg::Vec4f mAmbientColour;

        /// Meaningless in an `Interior`, where the weather system stops writing it and it keeps
        /// whatever it held wherever the player was last outdoors.
        osg::Vec4f mSkyColour;

        /// Where the player is standing, as the cell record says and not as read off what is drawn:
        /// off the dome every quasi-exterior is an exterior and `tsky` has a say in it.
        Location mLocation = Location::Interior;

        bool mWaterEnabled = false;
        float mWaterHeight = 0.0f;
        bool mUnderwater = false;

        /// Fog above the water, which a renderer whose fog is a medium reads even with the eye
        /// submerged, because what it models down there is the water itself.
        FogBand mAir;

        /// Fog with the eye under the water: the water. Both bands every frame, because the
        /// rasterizer's shaders carry both and switch per fragment.
        FogBand mWaterFog;

        /// Where the player stands, which the rasterizer's vegetation bends away from.
        osg::Vec3f mPlayerPosition;

        /// What the content recorded, before `FogManager` made a ramp of it: a weather's blended
        /// `Land_Fog_Depth` outdoors, a cell's `AMBI` density indoors. The ramp's start and end
        /// exist to hide a far clip plane, and a renderer with no far clip reads this depth over the
        /// distance its picture reaches.
        float mFogDepth = 0.0f;

        /// The `AMBI` record of the room the player is standing in, or nothing anywhere else. The
        /// record and not `mAmbientColour`, because `configureAmbient` lifts an interior's ambient
        /// to `minimum interior brightness` for the rasterizer's own falloff and turns its sunlight
        /// into a directional light. Only in an `Interior`: a quasi-exterior's light is the
        /// weather's.
        std::optional<ESM::Cell::AMBIstruct> mRoom;

        /// What `updateAmbient` added to the ambient for the Night-Eye effect, in the file's space.
        /// Read back rather than restated, so there is one number. Nought without the effect.
        osg::Vec4f mNightEye;

        float mGameHour = 0.0f;

        /// Which weather the sky is under, as a script id: an index into the ten
        /// `MWWorld::WeatherManager` registers.
        int mWeatherId = 0;

        /// Which one it is turning into, and nothing while it is turning into none. The world says
        /// -1 there, and a default of zero would have said "a transition to Clear, just finished".
        /// The two fields below are read unconditionally — by the shader chain and by
        /// `Rtx::describeClouds` — and hold whatever the weather manager last left.
        std::optional<int> mNextWeatherId;

        /// How far that transition has left to run: one when it begins and zero when it ends, the
        /// weather manager's own mix being `1 - this`. Meaningless without `mNextWeatherId`.
        float mWeatherTransition = 0.0f;

        /// How hard the wind blows, as the game's own dial: what the rasterizer's `windSpeed`
        /// uniform leans its vegetation by.
        float mWindSpeed = 0.0f;

        /// What the weather itself records blowing at, before the gust the engine wanders about it.
        /// Not `mWindSpeed`, which `calculateWindSpeed` multiplies by eight and caps at seventy;
        /// this is the number the content files state, and both paths have to stand in one air.
        float mBaseWindSpeed = 0.0f;

        /// Masser and Secunda, as the weather system last settled them. The world's own numbers and
        /// not a placement, which keeps this header clear of the ray tracer's types. An alpha of
        /// nothing is a moon that is not drawn.
        Sky::MoonState mMoons[2] = {};

        /// Which way each of the two cloud decks is driven. Reported rather than derived, because
        /// an ash or blight storm blows off Red Mountain at the player. One each, because the
        /// rasterizer turns each of its two cloud meshes by its own weather's storm; the second is
        /// unit length only while a weather is arriving, and a renderer that draws one deck reads
        /// a zero as due north.
        osg::Vec3f mCloudDirection = osg::Vec3f(0.0f, 1.0f, 0.0f);
        osg::Vec3f mNextCloudDirection = osg::Vec3f(0.0f, 1.0f, 0.0f);

        /// Whether the cell record calls this an interior, which is what the `isInterior` shader
        /// uniform has always meant. A quasi-exterior answers yes to this and to `isOutdoors` both.
        bool isInteriorCell() const { return mLocation != Location::Exterior; }

        /// Whether this counts as being outside — a sky overhead and weather in it: the condition
        /// `World::updateWeather` gates on, and what a technique marked `Disable_Exteriors` asks.
        bool isOutdoors() const { return mLocation != Location::Interior; }
    };

    /// Where the frame is seen from, and how far it can see: the eye's, not the world's, because
    /// the same world is drawn through several — the frame's, a map tile's, a doll's.
    struct EyeState
    {
        float mNearClip = 0.0f;
        float mViewDistance = 0.0f;
        osg::Matrixf mProjectionMatrix;

        /// The one the world settled on: the override wherever something asked for one, and the
        /// setting only where nothing did.
        float mFieldOfView = 0.0f;

        /// Whether the eye is the player's, as against a camera a script or a harness parked
        /// somewhere: what decides whether the player's own body is in the picture.
        bool mPlayersEye = true;
    };

    /// What there is to draw, and what the world is doing while it is drawn. Handed down rather
    /// than reached up for, so a renderer knows only what a frame is; where there is no world —
    /// the main menu, a loading screen — `Renderer::renderGui` is called instead.
    struct SceneFrame
    {
        /// The whole world, from the top. Not the cull's results: rays go everywhere.
        osg::Node& mScene;

        const osg::Camera& mCamera;

        /// Frame number and simulation time. The clock stops when the game is paused and so does
        /// everything the graph animates off it.
        const osg::FrameStamp& mWhen;

        const WorldState& mWorld;

        const EyeState& mEye;

        /// Where a texture the mirror has not seen before is read from.
        Resource::ImageManager& mImages;

        /// The world's terrain, for its storage, its worldspace and the active grid — not for its
        /// chunks, which a renderer that stands the ground itself is given none of.
        Terrain::World& mTerrain;

        /// What the content files say stands where: the lights of the cells the paging leaves
        /// dark, which `Rtx::DistantLights` reads out of here because the paging stands no `LIGH`.
        const Terrain::ObjectStorage& mObjectStorage;

        /// How long the frame stands for, in seconds, and whether the simulation stood still over
        /// it: what `RenderingManager::update` was handed, for the objects that step by it.
        float mDeltaTime = 0.0f;
        bool mPaused = false;
    };
}

#endif
