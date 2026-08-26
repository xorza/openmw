#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <osg/Group>
#include <osg/PositionAttitudeTransform>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/weather/precipitation.hpp>

#include "cellscene.hpp"
#include "lighting.hpp"
#include "placement.hpp"
#include "posedactors.hpp"
#include "waterplane.hpp"

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class World;

    /// What a step of the camera cost the graph, or all zeroes where it stayed in its square.
    struct Crossing
    {
        /// How many cells the ring brought in. Fewer than three at a coastline, where the sea has
        /// no cell record to read.
        std::uint32_t mArrived = 0;

        /// How many were taken off the graph behind it.
        std::uint32_t mDeparted = 0;

        /// Whether anything happened at all, which is what says the scene has to be handed over
        /// rather than placed.
        bool happened() const { return mArrived > 0 || mDeparted > 0; }
    };

    /// What has to be decided before a region can be read, beyond which cell it is.
    struct StagingRequest
    {
        /// When and in what weather, for the exterior that has a sky.
        std::string mWeather = "Clear";
        float mHour = 12.0f;

        /// Which day, counted from the one a new game begins on — 16 Last Seed. Only the moons read
        /// it: their phase runs on a three-day cycle and their rise hour on a twenty-four day one.
        int mDay = 0;

        float mFieldOfView = 60.0f;

        /// Where to stand and what to look at. Either left out is derived from the cell's own
        /// bounds, which is a fine establishing shot for a town and a poor one for a room.
        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;
    };

    /// A region of the world read into a scene, with its people and props posed into it.
    ///
    /// **Everything between "which cell" and "trace it", and it is the same everywhere.** A shot
    /// and a profiling run differ in what they do with the frames and in nothing before them, and
    /// the order here is not arbitrary: the camera is pinned from the world's own bounds *before*
    /// anyone stands in it, because a row of actors is placed relative to where the camera ends up
    /// and bounds that already contain them would put it somewhere else.
    ///
    /// **It streams, so a camera that goes somewhere is not a second implementation of all this.**
    /// `moveTo` brings the ring the camera has walked into and takes the cells behind it off the
    /// graph; the window and a route-following bench both cross cells through it, which is what
    /// stops the two drifting apart again.
    class StagedWorld
    {
    public:
        StagedWorld(World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors);
        ~StagedWorld();

        StagedWorld(const StagedWorld&) = delete;
        StagedWorld& operator=(const StagedWorld&) = delete;

        /// True where the region placed no geometry at all, which nothing can be traced from.
        bool empty() const { return mScene.getPlacedCount() == 0; }

        const Rtx::SceneDesc& getScene() const { return mScene; }

        /// Mutable for whoever hands it to a backend, which consumes the arrivals it holds.
        Rtx::SceneDesc& getScene() { return mScene; }
        const CellLighting& getLighting() const { return mLighting; }

        /// The region the camera's own cell names, or an empty id where it names none.
        ///
        /// **Followed across crossings**, because it is what decides which weathers are on offer and
        /// a run that flies from the Bitter Coast to the Ashlands leaves one region for another.
        const ESM::RefId& getRegion() const { return mRegion; }

        /// One frame's worth of driving the weather, from where the eye now is.
        ///
        /// **The same one call `RenderingManager::update` makes**, which is the point: this used to
        /// set two of `Weather::Precipitation`'s four inputs and call neither of the others, so a
        /// shot of a storm showed drops that never froze under water and an ash cloud that never
        /// turned to face the wind.
        /// Where a camera goes when nothing leads into the cell: a view of the place from outside it.
        Placement frame(const ESM::Cell& cell, const StagingRequest& request) const;

        void driveWeather(const osg::Vec3f& eye);

        /// Moves the sky to another moment, without reading the region again.
        ///
        /// **What the window's clock and weather keys turn.** The cells, their lamps and their
        /// water are the same either side of it; what changes is where the sun and the moons stand
        /// and what the air is doing, which `relight` works out from the settings alone.
        void setSky(std::string_view weather, int day, float hour)
        {
            relight(mLighting, weather, day, hour);
            setFalling(weather);
        }

        /// The same, partway between two weathers.
        void setSky(std::string_view from, std::string_view to, float blend, int day, float hour)
        {
            relight(mLighting, from, to, blend, day, hour);

            // **What it is turning into, and not a blend of the two.** A transition's precipitation
            // fades in rather than crossing, and a harness that has to pick one instant is better
            // showing the weather that is arriving than half of each.
            setFalling(blend < 0.5f ? from : to);
        }
        const Placement& getPlacement() const { return mPlacement; }

        /// What advances the scene between frames by frame index, or null where nothing in it
        /// moves. Borrowed: it does not outlive this.
        ///
        /// **By index and not by the clock**, which is what makes a run of frames reproducible: a
        /// world stepped by how long the last frame took renders a different sequence on every
        /// machine. A window wants the other one — see `advanceTo`.
        Motion* getMotion();

        /// Advances the world to `seconds` and walks whatever moved back in. False where nothing
        /// did, which is what spares the frame a hand-over it does not need.
        ///
        /// **The window's clock.** Frames it dropped would otherwise animate in slow motion; a run
        /// being measured wants `getMotion` instead, for the reason above.
        bool advanceTo(float seconds);

        /// Brings the region around `where` in and takes the cells that left off the graph.
        ///
        /// **The camera's own cell is what triggers it, not a distance**, which is the game's rule
        /// and so this one: a step that stays inside the square costs one string comparison, and a
        /// step that leaves it pays for the whole ring at once. Interiors have no neighbours and
        /// never cross.
        ///
        /// Everything a crossing implies happens here — the actors come out of the scene before the
        /// walk that would place them twice, the new cells' residents go in, the emitters are warmed
        /// and the sweep runs — so a caller whose crossing `happened` has a scene that may have
        /// grown or been renumbered and must hand it over rather than place it.
        Crossing moveTo(const osg::Vec3f& where);

        /// What the actors and props came to once they were walked in. All zero where there are
        /// none.
        const Rtx::ExtractionStats& getSettled() const { return mSettled; }

        /// What the staging walk found: the still world, before anyone was posed into it.
        const Rtx::ExtractionStats& getStaged() const { return mStaged; }

        /// What reading the region found, including everything it could not place. Kept because a
        /// report of what the renderer is handed has to come off the same load the renderer got.
        const CellReport& getReport() const { return mReport; }

        /// Walks the same graph again and says what that added.
        ///
        /// **A diagnostic and not a frame.** Nothing changes between the two, so every count it
        /// returns for new geometry should be zero — which is the property the incremental mirror
        /// rests on, and the only way to ask it is to ask twice.
        Rtx::ExtractionStats mirrorAgain() { return mirror(0); }

        /// People and creatures, which is everyone posed less the props.
        std::size_t getActorCount() const;
        std::size_t getPropCount() const;

    private:
        /// Walks the graph into the scene, the way `Tracer` does every frame.
        ///
        /// **Nothing outside calls this any more.** Staging walks once and streaming walks again per
        /// ring; the per-frame walk belongs to `PosedActors`, which owns the pose that made it
        /// necessary.
        Rtx::ExtractionStats mirror(std::size_t frame);

        /// Moves the world's clock, which is what everything the graph animates is driven by.
        void setSeconds(float seconds);

        /// Tells the precipitation what this weather drops, read straight off the content files.
        void setFalling(std::string_view weather);

        /// Runs every emitter in the staged graph up to a steady state, before the first frame.
        ///
        /// **The world's job and not the actors'.** A particle system loads holding the seed the
        /// file authored — a handful of specks a fifth of a unit across — and only reaches the
        /// flame or the rainstorm it is meant to be once its emitter has run for a lifetime or two.
        /// A single frame stepped from nothing integrates nothing, so a shot of a lit room showed
        /// fifty-five candles with no flames on them.
        ///
        /// It used to sit inside `PosedActors::settle`, which made it something only a cell with
        /// people in it got — and the weather hangs its emitters off this same graph, so a view
        /// with nobody standing in it rendered one frame's worth of rain and the game rendered a
        /// storm.
        void warmEmitters();

        /// How long the emitters are run before the first frame is drawn.
        ///
        /// **Two seconds, which is longer than anything the game emits lives.** The median lifetime
        /// across the shipped emitters is under a second, so by here every seed particle the file
        /// authored has died and been replaced by the emitter's own — which is the steady state a
        /// frame is supposed to show.
        static constexpr float sWarmSeconds = 2.0f;

        /// What this weather drives past the eye, kept because `driveWeather` aims it every frame:
        /// an ash storm blows off Red Mountain at whoever is watching, so where the camera stands
        /// is half the answer.
        std::string mStormEffect;

        /// The graph the mirror walks, assembled the way the game assembles its own: a group per
        /// cell, a reference under a transform inside it, the terrain hung alongside.
        ///
        /// **Held for as long as the scene is, because the mirror re-walks it every frame.** That
        /// is the whole of what makes this harness measure what the game measures — the walk, the
        /// sweep, and a cell arriving all cost here what they cost there.
        osg::ref_ptr<osg::Group> mRoot = new osg::Group;

        Rtx::SceneDesc mScene;
        Rtx::SceneExtractor mExtractor;

        /// Which cells are in the graph, and the group each hangs under.
        LoadedCells mLoaded;

        /// Walks the whole graph every frame whether or not anything in it moved.
        ///
        /// **What the game does, and this does it too.** Nothing in a still world changes between
        /// two of these, so what it buys is not motion: it is that the per-frame lists are emptied
        /// and refilled on the same cadence the game empties and refills them, so a sweep that takes
        /// something the next walk was supposed to bring back cannot hide.
        ///
        /// **It was a switch and it is not one any more.** Measured, a `shot` costs 1.85 seconds
        /// with it and 1.84 without, and a still frame comes out byte-identical either way — so the
        /// only thing an option bought was a harness that normally ran at a cadence the game never
        /// does, which is the opposite of what this tool is for.
        class EveryFrame : public Motion
        {
        public:
            explicit EveryFrame(StagedWorld& staged)
                : mStaged(staged)
            {
            }

            bool step(std::uint32_t frame) override;

        private:
            StagedWorld& mStaged;
        };

        CellLighting mLighting;
        EveryFrame mEveryFrame{ *this };

        /// The world's water, one sheet the way the game has it. Declared after the root it hangs
        /// under and before anything that walks it.
        std::optional<WaterPlane> mWater;
        Placement mPlacement;

        /// Where the weather's particles hang, moved to the eye each time it moves.
        ///
        /// **The game hangs them under the sky's `CameraRelativeTransform`, which is the same thing
        /// said in the rasterizer's vocabulary**: a finite box of drops is a rainstorm only because
        /// it travels with the player. There is no such transform here, so the box is a node this
        /// puts where the eye is.
        osg::ref_ptr<osg::PositionAttitudeTransform> mWeatherNode;

        /// The rain and the storm the weather drives, built exactly as the game builds them.
        ///
        /// **The reason this exists is that it did not.** Precipitation lived under `apps/openmw/`
        /// and so was the one part of a frame this harness could not render — and so the one part
        /// where a wrong traversal mask, a gate read off a cull traversal and an emitter nothing
        /// ever stepped all survived until somebody opened the game and stood in the rain.
        std::unique_ptr<Weather::Precipitation> mPrecipitation;

        /// What the staging load and the staging walk came to, kept for whoever reports on them.
        CellReport mReport;
        Rtx::ExtractionStats mStaged;

        /// Held rather than borrowed: the extractor keys its meshes on node pointers, so actors
        /// freed while the scene still names them is a dangling identity.
        std::unique_ptr<PosedActors> mPosed;
        Rtx::ExtractionStats mSettled;

        /// What `moveTo` needs and the constructor already had. Borrowed: the world outlives this.
        World* mWorld = nullptr;
        ActorRequest mActors;

        /// Which square the camera stood in when the region was last brought in. Absent for an
        /// interior, which is the same test as "this never streams".
        std::optional<CellSquare> mStanding;
        ESM::RefId mRegion;

        /// Where the world's clock stands, in seconds. One clock for the whole staged world: an
        /// actor's idle and the flipbook on the brazier beside them are the same second of it.
        float mSeconds = 0.0f;

        /// `fStromWindSpeed`, above which a weather counts as a storm and turns its effect to face
        /// the wind, and `Weather_Precip_Gravity`, which every weather's rain falls at. Read once,
        /// because neither the store nor the ini changes while this runs.
        float mStormWindSpeed = 0.0f;
        float mRainGravity = 0.0f;
    };
}
