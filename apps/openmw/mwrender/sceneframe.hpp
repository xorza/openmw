#ifndef GAME_RENDER_SCENEFRAME_H
#define GAME_RENDER_SCENEFRAME_H

#include <optional>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/esm3/loadcell.hpp>

namespace osg
{
    class FrameStamp;
    class Node;
}

namespace Terrain
{
    class ObjectStorage;
    class World;
}

namespace MWRender
{
    class Precipitation;
    struct SkyState;

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

    /// What the game itself decides about the world this frame, read off where it keeps the
    /// value — the sun light, `FogManager`, the cell, the toggles — so it cannot disagree with
    /// what is drawn. What the weather settled is `SkyState`, the weather manager's own, which
    /// the frame carries beside this. In the world's own numbers, undecoded: every colour is a
    /// content file's three bytes over 255, and what that means is a question about a renderer's
    /// transport.
    struct WorldState
    {
        /// The sun light as `RenderingManager` keeps it, which `setSunDirection`, `setSunColour`,
        /// `setAmbientColour` and `configureAmbient` write: where it comes from, with
        /// `match sunlight to sun` already applied, and in a room the position Morrowind points a
        /// room's sun. The rasterizer lights through the light itself; the chain and the tracer
        /// read these.
        osg::Vec4f mSunLightPosition;
        osg::Vec4f mSunColour;

        /// Includes the night-eye effect and, in a room, the lift `configureAmbient` gives it.
        /// `mRoom` is the record.
        osg::Vec4f mAmbientColour;

        /// What `updateAmbient` added to the ambient for the Night-Eye effect, in the file's space.
        /// Read back rather than restated, so there is one number. Nought without the effect.
        osg::Vec4f mNightEye;

        /// `RenderingManager::setSunColour`'s third argument: the one number the light does not
        /// hold. Nought in a room.
        float mSunVisibility = 0.0f;

        /// Whether there is a sky to draw: outdoors, and `tsky` has not turned it off. What
        /// `RenderingManager::setSkyEnabled` was last told.
        bool mSkyShown = false;

        /// Whether a script has painted Secunda red, `Moons_Script_Color`.
        /// `RenderingManager::skySetMoonColour`.
        bool mMoonRed = false;

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

        /// The `AMBI` record of the room the player is standing in, or nothing anywhere else. The
        /// record and not `mAmbientColour`, because `configureAmbient` lifts an interior's ambient
        /// to `minimum interior brightness` for the rasterizer's own falloff and turns its sunlight
        /// into a directional light. Only in an `Interior`: a quasi-exterior's light is the
        /// weather's.
        std::optional<ESM::Cell::AMBIstruct> mRoom;

        float mGameHour = 0.0f;

        /// The `timescale` global: game seconds to each real one, which a renderer steps its own
        /// sky clock by (`Sky::SkyClock`).
        float mTimeScale = 0.0f;

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
        /// uniform leans its vegetation by. The gust, and not the base the weather records, which
        /// is `SkyState::mWeather`'s.
        float mWindSpeed = 0.0f;

        /// Whether the cell record calls this an interior, which is what the `isInterior` shader
        /// uniform has always meant. A quasi-exterior answers yes to this and to `isOutdoors` both.
        bool isInteriorCell() const { return mLocation != Location::Exterior; }

        /// Whether this counts as being outside — a sky overhead and weather in it: the condition
        /// `World::updateWeather` gates on, and what a technique marked `Disable_Exteriors` asks.
        bool isOutdoors() const { return mLocation != Location::Interior; }
    };

    /// Where the sun's disc is drawn, off the two records: the weather's orbit bent into the sky
    /// where the weather ran, and where it has not — a room — the point `configureAmbient` aimed
    /// the light, with no night. One rule, because the shader chain and the trace both place a
    /// disc, and upstream's `RenderingManager::update` told the chain the same two answers.
    osg::Vec4f sunDiscOf(const SkyState& sky, const WorldState& world);

    /// Where the frame is seen from, and how far it can see: the eye's, not the world's, because
    /// the same world is drawn through several — the frame's, a map tile's, a doll's. Where the
    /// eye stands is the camera's own view matrix, which the update traversal writes after this
    /// is described: a renderer reads it off the camera it adopted, at the moment it draws.
    struct EyeState
    {
        float mNearClip = 0.0f;
        float mViewDistance = 0.0f;
        osg::Matrixf mProjectionMatrix;

        /// The one the world settled on: the override wherever something asked for one, and the
        /// setting only where nothing did.
        float mFieldOfView = 0.0f;

        /// The one the player's own arms are drawn through, `first person field of view`, which
        /// `NpcAnimation` swaps the projection to under `Mask_FirstPerson`.
        float mArmsFieldOfView = 0.0f;

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

        /// Frame number and simulation time. The clock stops when the game is paused and so does
        /// everything the graph animates off it.
        const osg::FrameStamp& mWhen;

        /// What the weather settled, the weather manager's own.
        const SkyState& mSky;

        /// What the weather drops, as the game's particle systems: both renderers walk them, and
        /// each asks what it needs of them — the nodes, the occluder's box, how much rings the
        /// water.
        const Precipitation& mPrecipitation;

        const WorldState& mWorld;

        const EyeState& mEye;

        /// The world's terrain, for its storage, its worldspace and the active grid — not for its
        /// chunks, which a renderer that stands the ground itself is given none of.
        Terrain::World& mTerrain;

        /// What the content files say stands where: the ground's statics and the lights of the
        /// cells the paging leaves dark, which `Rtx::CellRing` reads out of here because the paging
        /// stands no `LIGH`.
        const Terrain::ObjectStorage& mObjectStorage;

        /// How long the frame stands for, in seconds, and whether the simulation stood still over
        /// it: what `RenderingManager::update` was handed, for the objects that step by it.
        float mDeltaTime = 0.0f;
        bool mPaused = false;
    };
}

#endif
