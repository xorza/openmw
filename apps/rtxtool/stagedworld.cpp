#include "stagedworld.hpp"

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <components/esm/position.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/rng.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/settings/values.hpp>
#include <components/weather/downpour.hpp>

#include "cellscene.hpp"
#include "content.hpp"
#include "world.hpp"

namespace RtxTool
{
    StagedWorld::StagedWorld(
        World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors)
        : mExtractor(mScene)
        , mWorld(&world)
        , mActors(actors)
    {
        seedDraws();

        // **Where a scene begins, and the counter a light's id comes from restarts with it.** A
        // `SceneUtil::LightSource` takes its id from one that runs for the whole process, and
        // `Rtx::lightPhase` derives a flame's phase from it — so a cell staged a second time stood
        // every flame somewhere else in its own cycle, and half of a lamp-lit room moved. Holding
        // the phase at a constant made two stagings of one cell render identically, which is how it
        // was found. Once, because the read below is what builds every light a region has:
        // restarting the counter after it would hand two lights one id, and with it one phase.
        //
        // `World::beginStaging` says what else shares that lifetime.
        world.beginStaging();

        const RegionLoad arrived = loadRegion(RegionRequest{ world, cell, *mRoot, mLoaded, actors.mProps }, mScene,
            mExtractor, SkyMoment{ request.mWeather, request.mDay, request.mHour });

        mLighting = arrived.mLighting;
        mLighting.mSeconds = request.mSeaSeconds;

        // **After the region, because an interior's sheet sits over whatever the room holds.** The
        // level it answers is what "how deep is this point" is asked against, so it goes into the
        // lighting the frame is described from.
        mWater.emplace(*mRoot);
        mLighting.mWaterLevel = mWater->follow(cell);

        // **The same particle systems the game builds, from the same component.** Under a group of
        // this harness's own rather than the sky's camera-relative transform, because there is no
        // sky manager here; neither carries a translation, and `Rtx::mirrorPrecipitation` is what
        // stands the drops in the world.
        //
        // **Both of the weather's constants, read once and where the other one is.** The first is a
        // game setting and comes from the store; the second is a fallback and comes from the ini,
        // which is the only reason they are fetched differently.
        mStormWindSpeed = world.getContent().findGameSetting("fStromWindSpeed", 50.0f);
        mRainGravity = Fallback::Map::getFloat("Weather_Precip_Gravity");

        mWeatherNode = new osg::Group;
        mWeatherNode->setName("Precipitation");
        mPrecipitation
            = std::make_unique<Weather::Precipitation>(mWeatherNode, *world.getResourceSystem().getSceneManager(), ~0u);

        // **The moons' portraits, into the same table the trace reads.** Held rather than named by a
        // material: a moon is drawn by a ray that reached nothing, so no material can speak for its
        // texture and the sweep would take the slot on the first frame a cell died.
        mLighting.mFaces = Rtx::addMoonFaces(mScene);
        mLighting.mSky = Rtx::addSkyContent(mScene, *world.getResourceSystem().getSceneManager(),
            Rtx::SkyMeshes{ .mClouds = Settings::models().mSkyclouds,
                .mStars = Settings::models().mSkynight02,
                .mStarsFallback = Settings::models().mSkynight01 });
        mReport = std::move(arrived.mReport);

        // **Before the first walk, because the walk runs the animators.** The graph's own
        // controllers — a brazier's flipbook, a lava flow — read the clock off the traversal, and a
        // shot is only repeatable if it is told which second it is showing rather than measuring
        // one of its own.
        setSeconds(actors.mSeconds);

        // **Without this the sea is shaded as ordinary geometry.** `isWater` answers no for every
        // mask until it is told which one names the water, and the game tells its own extractor the
        // same thing at `rtxrenderer.cpp:172`.
        mExtractor.setWaterMask(SceneUtil::Mask_Water);

        mRegion = cell.mRegion;

        // Absent for an interior, and that is what `moveTo` reads as "this never streams".
        if (cell.isExterior())
            mStanding = CellSquare{ .mX = cell.getGridX(), .mY = cell.getGridY() };

        // **Before the first walk, because a paged world resolves its chunks during one.** The
        // camera below is placed from the scene's own bounds and the scene does not exist yet, so
        // the detail is anchored on where the run was told to stand — or, where it was told nothing,
        // on the middle of the cell it is centred on. Anchoring it on the origin instead put Seyda
        // Neen's ground seventy thousand units away from the eye that asked for it, and the coarsest
        // chunks in the tree are what came back.
        const float cellSize = static_cast<float>(ESM::getCellSize(ESM::Cell::sDefaultWorldspaceId));
        const osg::Vec3f middle((cell.getGridX() + 0.5f) * cellSize, (cell.getGridY() + 0.5f) * cellSize, 0.0f);
        mWorld->setTerrainViewPoint(request.mOrigin.value_or(middle));

        // **After the terrain exists and before the first walk**, because a paged world only has a
        // residency once it has been built. Every world walk from here asks it, whoever makes the
        // walk.
        mExtractor.follow(mWorld->getResidencies());

        // The first walk. Everything after this is the same walk again, once a frame.
        mStaged = mirror(0);

        // **Before anyone goes in.** A row of actors stands relative to where the camera ends up, so
        // the camera cannot be derived from bounds that already contain them.
        //
        // **An interior is entered rather than framed.** Where something in the world teleports here,
        // the camera stands and faces exactly as the game would stand and face a character who had
        // just walked in — see `Content::findArrival`. Framing is what is left for a cell nothing leads
        // to, and for every exterior.
        const std::optional<ESM::Position> arrival = mWorld->getContent().findArrival(cell);
        mPlacement
            = arrival.has_value() ? placeOnArrival(*arrival, request.mOrigin, request.mTarget) : frame(cell, request);

        // **After the camera is placed and before the first walk.** The box is finite and centred on
        // the eye, so a shot whose weather arrived while the box still sat at the origin is a shot
        // of a rainstorm happening somewhere else.
        setFalling(request.mWeather);
        driveWeather(mPlacement.mOrigin);

        const std::span<const CellPerson> residents
            = actors.mResidents ? std::span<const CellPerson>(mReport.mPeople) : std::span<const CellPerson>();

        const std::span<const CellProp> props
            = actors.mProps ? std::span<const CellProp>(mReport.mProps) : std::span<const CellProp>();

        if (!actors.empty() || !residents.empty() || !props.empty())
        {
            mPosed = std::make_unique<PosedActors>(world, mScene, mExtractor, *mRoot, mPrecipitation.get(), actors);
            mPosed->addResidents(residents);
            mPosed->addProps(props);
            mPosed->addRow(actors, mPlacement);
        }

        // **Whoever is standing here, and whether anybody is.** An empty cell has no plumes to run
        // up, but the weather hangs its emitters off this same graph.
        warmEmitters();

        // **And walked afterwards, because a warm-up is not a walk.** The first mirror above ran
        // before the weather had built anything and `stepEmitters` deliberately mirrors nothing, so
        // without a second one the scene handed to a renderer holds the cell and no rain at all.
        if (mPosed != nullptr)
            mSettled = mPosed->settle();
        else
            mStaged = mirror(0);
    }

    StagedWorld::~StagedWorld()
    {
        // **A staged world gives its ground back**, which is `dropCellsOutside`'s rule applied to
        // the whole region rather than to the cells behind a moving camera. What it costs the next
        // staging not to have is at `World::clearTerrain`.
        mWorld->clearTerrain();
    }

    void StagedWorld::seedDraws()
    {
        // **Two generators and not one, which is how long this went half done.** A particle's
        // direction, speed and lifetime, and a flickering lamp's phase, are drawn from `Misc::Rng`.
        // Where each of the weather's drops falls in its box is drawn from the C library's
        // `std::rand`, which is what `osgParticle::BoxPlacer` and every other `osgParticle::range`
        // are written against and which nothing else this binary runs touches. Each is one sequence for
        // the whole process, so a staging that began wherever the last one ended drew a different
        // world — with only the first reset, a `verify` of one view rendered a storm that a `verify`
        // of every view did not, and the two disagreed on an eighth of the frame.
        Misc::Rng::init(sSeed);
        std::srand(sSeed);
    }

    void StagedWorld::warmEmitters()
    {
        // Taken here as well as at the top of a staging, and `seedDraws` says why.
        seedDraws();

        // **A frame's worth at a time, because that is the step the emitters were authored
        // against**: a birth rate is particles per second, a collider bounces per step, and a
        // lifetime quantised to one long stride would put every particle at the same age. The
        // animation clock is held where it is throughout — a warm-up is about the emitters and
        // nothing else, and turning it would leave every actor two seconds into its idle.
        for (float at = PosedActors::sFrameSeconds; at <= sWarmSeconds; at += PosedActors::sFrameSeconds)
        {
            // **The emitters are stepped by the walk that mirrors, and there has not been one yet.**
            // So they are stepped on their own here, over the same graph and the same clock a mirror
            // would have used, which is the whole of what a warm-up is. An actor has to be standing
            // where it will stand while that happens, because its own plume hangs off it.
            if (mPosed != nullptr)
                mPosed->poseFor(PosedActors::sFrameSeconds);

            mExtractor.advanceEmitters(PosedActors::sFrameSeconds);
            mExtractor.stepEmitters(*mRoot);

            // **And the weather, which is not under that root.** It is walked as a second one, so
            // it is stepped as a second one — see `Rtx::mirrorPrecipitation`.
            mExtractor.stepEmitters(*mPrecipitation->getNode());
        }
    }

    Rtx::ExtractionStats StagedWorld::mirror(std::size_t frame)
    {
        // **Emptied before it is filled, which is what `RtxRenderer::renderFrame` does too.** The
        // lists a walk refills — the lights, the sprites, the emitters, the deforming set — are
        // appended to rather than replaced, so a second walk without this counted every light in the
        // region twice. It did not show while the lights were read out of records and placed once;
        // it showed the moment they became `LightSource` nodes the walk meets.
        mScene.clearPlacement();

        // **The epoch of the walk before, bumped here and not when that walk ended.** What moved
        // becomes what settled, and a placement reads both — so an advance taken between a walk and
        // the hand-over that follows it empties `getMoved` before anything has written those rows.
        // `RtxRenderer::renderFrame` advances after its trace, which is the same instant as this.
        mExtractor.advance();

        // What the weather drops, which is a second root to this walk exactly as it is to the
        // game's.
        Rtx::mirrorPrecipitation(mExtractor, mPrecipitation.get(), frame);

        // **The world walk, so the chunks a paged world keeps out of the graph are dated, counted
        // and swept with everything the graph does hold.** What it follows was set when the terrain
        // was built; it is not passed here, because the actors' own stepper walks this same root and
        // an argument only one of the two remembered is what left a town standing on open sea.
        const Rtx::ExtractionStats found = mExtractor.extractWorld(*mRoot, osg::Matrixf::identity(), 0, frame);

        // **After the walk and on every one of them, which is the cadence `RtxRenderer::renderFrame`
        // runs at.** The walk above was the whole world, which is the precondition the sweep names,
        // so this is sound wherever a mirror is — and a harness that swept only at a crossing
        // under-costed every frame between two of them.
        //
        // `PosedActors::advanceTo` carries the same call for the frames it walks instead of this
        // one, so a frame is swept once whichever of the two stepped it. `settle` is not a frame and
        // is the reason that one sits there rather than beside its own walk.
        mExtractor.retire();

        return found;
    }

    Placement StagedWorld::frame(const ESM::Cell& cell, const StagingRequest& request) const
    {
        if (!cell.isExterior())
            return placeCamera(mScene.getBounds(), request.mFieldOfView, request.mOrigin, request.mTarget);

        // **The square that was staged, and not everything the scene reaches.** Framing the whole of
        // it put the eye a million and a half units out and photographed the sea — the sheet is a
        // hundred and fifty cells across — and leaving only the sea out still framed the four cells
        // of distant ground. A view names a place, so the camera is placed from that place. The
        // height is left open because how high the ground stands there is what is being asked.
        //
        // A far plane still wants everything there is; see `SceneDesc::getBounds`.
        const auto side = static_cast<float>(ESM::getCellSize(ESM::Cell::sDefaultWorldspaceId));
        const auto reach = static_cast<float>(Constants::CellGridRadius);
        const osg::BoundingBoxf staged((static_cast<float>(cell.getGridX()) - reach) * side,
            (static_cast<float>(cell.getGridY()) - reach) * side, -std::numeric_limits<float>::max(),
            (static_cast<float>(cell.getGridX()) + reach + 1.0f) * side,
            (static_cast<float>(cell.getGridY()) + reach + 1.0f) * side, std::numeric_limits<float>::max());

        return placeCamera(
            mScene.getContentBoundsWithin(staged), request.mFieldOfView, request.mOrigin, request.mTarget);
    }

    void StagedWorld::driveWeather(const osg::Vec3f& eye)
    {
        // The same question the game asks of the water it owns, asked here of the level this cell
        // reported. A cell with no water reports minus infinity, so nothing is ever under it.
        const bool underwater = eye.z() < mLighting.mWaterLevel;

        mPrecipitation->update(Weather::Conditions{
            .mEye = eye,

            // **Aimed at the camera, because that is the body standing in this weather.** The game
            // aims an ash storm at the player off Red Mountain; the rule is the same one and the
            // observer is whoever is looking.
            .mStormDirection = Weather::stormDirection(mStormEffect, eye),

            .mUnderwater = underwater,
        });
    }

    void StagedWorld::setFalling(std::string_view weather)
    {
        const Weather::Downpour falling = Weather::downpourAt(weather, mStormWindSpeed, mRainGravity);
        mStormEffect = falling.mParticleEffect;
        mPrecipitation->setWeather(falling);
        mLighting.mRainOnWater = Rtx::rainOnWater(mPrecipitation.get());
    }

    Crossing StagedWorld::moveTo(const osg::Vec3f& where)
    {
        driveWeather(where);

        // **Every frame and not only on a crossing**, because the detail a paged world builds at is
        // a distance from the eye rather than a property of the cell: a camera flying across one
        // cell changes what the chunks under it should be without changing which cell it is in.
        mWorld->setTerrainViewPoint(where);

        if (!mStanding.has_value())
            return {};

        // **Two integers compared, and nothing spelled out.** This runs every frame of a streaming
        // run and answers no on all but a handful of them; naming the square to find that out would
        // be two allocations a frame for the privilege.
        const CellSquare square = squareAt(where);
        if (square == *mStanding)
            return {};

        mStanding = square;

        // **Open sea, and the answer is to keep what is already loaded.** Every point has a square;
        // not every square has a cell record, and a camera over the water is standing in one of the
        // ones that does not. The game holds its last grid there too.
        const ESM::Cell* cell = mWorld->getContent().findCell(cellAt(square));
        if (cell == nullptr)
            return {};

        mRegion = cell->mRegion;

        // **The sheet moves with the camera's cell, exactly as `Water::changeCell` moves the
        // game's.** It is finite — a hundred and fifty cells across — so a camera that flew far
        // enough from where it started would otherwise reach the edge of the sea.
        mLighting.mWaterLevel = mWater->follow(*cell);

        // **The actors come out first.** The new cells are walked into whatever the scene holds, so
        // a snapshot retaken with everyone still in it would place a second copy of them on the very
        // next frame.
        if (mPosed != nullptr)
            mPosed->unplace();

        const CellReport arrived = readRegion(RegionRequest{ *mWorld, *cell, *mRoot, mLoaded, mActors.mProps });

        // **The ring that arrived and the ones that left.** The working set is a square that follows
        // the camera, not everything ever visited; without the second half this grows for as long as
        // the run lasts and stops resembling the game after the first crossing.
        const Crossing crossed{ .mArrived = arrived.mCells,
            .mDeparted = dropCellsOutside(*mWorld, *cell, *mRoot, mLoaded) };

        // Built, then walked, which is the split the game has too. The walk is also what tells its
        // own sweep that the departed cells are no longer met.
        mirror(0);

        if (mPosed == nullptr)
            return crossed;

        // The people who arrived with the ring. A resident belongs to the half of the scene that is
        // walked in again per frame, which is why they go in after the cells rather than with them.
        if (mActors.mResidents)
            mPosed->addResidents(arrived.mPeople);
        if (mActors.mProps)
            mPosed->addProps(arrived.mProps);

        mSettled = mPosed->settle();
        return crossed;
    }

    bool StagedWorld::advanceTo(float seconds)
    {
        const float elapsed = seconds - mSeconds;
        setSeconds(seconds);
        if (mPosed != nullptr && mPosed->advanceTo(seconds))
            return true;

        // **Nothing moved, and the walk happens anyway.** Actors already walk when they step, so
        // this is only ever the still world — which is exactly the case a snapshot was hiding.
        //
        // The emitters are carried by that step where there is one, so this is the only path that
        // owes them the gap itself.
        mExtractor.advanceEmitters(static_cast<double>(elapsed));
        mirror(0);
        return true;
    }

    Motion* StagedWorld::getMotion()
    {
        // **The actors' own stepping first**, because it already walks the whole graph when it runs
        // and asking for both would walk it twice. Where there are none, the walk is still the
        // game's — a still world is walked every frame here exactly as it is there.
        return mPosed != nullptr ? static_cast<Motion*>(mPosed.get()) : &mEveryFrame;
    }

    bool StagedWorld::EveryFrame::step(std::uint32_t frame)
    {
        mStaged.mirror(frame);

        // **Always true, because the frame after a walk has to be handed over.** A walk that found
        // everything where it was still emptied and refilled the per-frame lists, and the backend's
        // copy of those is what a hand-over rewrites.
        return true;
    }

    void StagedWorld::setSeconds(float seconds)
    {
        mSeconds = seconds;
        mExtractor.setSimulationTime(mSeconds);
    }

    std::size_t StagedWorld::getActorCount() const
    {
        return mPosed == nullptr ? 0 : mPosed->getCount() - mPosed->getPropCount();
    }

    std::size_t StagedWorld::getPropCount() const
    {
        return mPosed == nullptr ? 0 : mPosed->getPropCount();
    }
}
