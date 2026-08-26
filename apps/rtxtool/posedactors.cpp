#include "posedactors.hpp"

#include <osg/MatrixTransform>

#include <algorithm>
#include <cmath>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadnpc.hpp>

#include "actor.hpp"
#include "cellscene.hpp"
#include "npc.hpp"
#include "placement.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        /// How far into their own idle somebody standing at `where` is.
        ///
        /// **Derived from where they stand rather than drawn at random**, so a shot of a town is the
        /// same shot twice. Any offset would do — all that matters is that no two people share one,
        /// and a position is the one thing about a resident that already differs.
        float phaseAt(const osg::Matrixf& where)
        {
            const osg::Vec3f at = where.getTrans();
            const float mixed = at.x() * 0.31718f + at.y() * 0.17321f + at.z() * 0.07734f;
            return std::abs(std::fmod(mixed, 1.0f));
        }
    }

    PosedActors::PosedActors(World& world, Rtx::SceneDesc& scene, Rtx::SceneExtractor& extractor, osg::Group& root,
        const ActorRequest& request)
        : mWorld(world)
        , mScene(scene)
        , mExtractor(extractor)
        , mRoot(root)
        , mSeconds(request.mSeconds)
        , mClothes(request.mClothes)
        , mLastSeconds(request.mSeconds)
    {
    }

    // Out of line because `Actor` is only forward declared in the header.
    PosedActors::~PosedActors() = default;

    void PosedActors::add(ActorModel model, const osg::Matrixf& transform, osg::Group* cell, bool prop)
    {
        auto actor = std::make_unique<Actor>(mWorld, std::move(model), transform);

        // **Under the cell that placed it, the way its references go in.** From here the walk that
        // mirrors the world mirrors this too, and posing is all this class still does — and the node
        // leaves when the cell does, which is what unloading a cell means. An actor nobody's cell
        // placed hangs on the run's own root, because there is nothing for it to leave with.
        osg::ref_ptr<osg::MatrixTransform> where = new osg::MatrixTransform(osg::Matrixd(transform));
        where->addChild(&actor->getRoot());
        (cell != nullptr ? *cell : mRoot).addChild(where);

        mResidents.push_back(Resident{
            .mActor = std::move(actor),

            // Nudged per actor as well as per position, so a row in front of a camera — spread along
            // one axis, which walks the offset in step — comes out scattered rather than in a wave.
            .mPhase = phaseAt(transform) + static_cast<float>(mResidents.size() + 1) * 0.137f,
            .mProp = prop,
            .mCell = cell,
        });
    }

    void PosedActors::forgetDeparted()
    {
        std::erase_if(mResidents, [](const Resident& resident) {
            // A group that has left the root is a cell that has been unloaded. Null is an actor no
            // cell placed, which stays for as long as the run does.
            return resident.mCell != nullptr && resident.mCell->getNumParents() == 0;
        });
    }

    std::size_t PosedActors::getPropCount() const
    {
        return static_cast<std::size_t>(std::count_if(
            mResidents.begin(), mResidents.end(), [](const Resident& resident) { return resident.mProp; }));
    }

    void PosedActors::addRow(const ActorRequest& request, const Placement& placement)
    {
        const std::size_t wanted = request.size();

        // **Counted from where the row starts, not from how many actors there are.** The region's own
        // residents were added first and stand where the cell put them; a row indexed off the total
        // would begin as far off to one side as there are people in the town.
        //
        // Counted off what has actually been added, so somebody nobody has a record for leaves no
        // gap in the row.
        const std::size_t first = mResidents.size();
        const auto next
            = [&] { return placeActor(placement.mOrigin, placement.mTarget, mResidents.size() - first, wanted); };

        for (const std::string& model : request.mCreatures)
            add(loadCreature(mWorld, VFS::Path::Normalized(model)), next(), nullptr);

        for (const std::string& id : request.mPeople)
        {
            const ESM::NPC* who = findNpc(mWorld, id);
            if (who == nullptr)
            {
                Log(Debug::Warning) << "No such person as " << id;
                continue;
            }

            add(buildNpc(mWorld, *who, mClothes), next(), nullptr);
        }
    }

    void PosedActors::addResidents(std::span<const CellPerson> people)
    {
        for (const CellPerson& person : people)
            if (person.mRecord != nullptr)
                add(buildNpc(mWorld, *person.mRecord, mClothes), person.mTransform, person.mParent.get());
    }

    void PosedActors::addProps(std::span<const CellProp> props)
    {
        for (const CellProp& prop : props)
        {
            try
            {
                add(loadProp(mWorld, prop.mModel), prop.mTransform, prop.mParent.get(), /*prop=*/true);
            }
            catch (const std::exception& failed)
            {
                Log(Debug::Warning) << "Cannot instance " << prop.mModel << ": " << failed.what();
                continue;
            }
        }
    }

    const Rtx::ExtractionStats& PosedActors::settle()
    {
        mPlaced = place(mSeconds);
        return mPlaced;
    }

    bool PosedActors::advanceTo(float seconds)
    {
        if (mResidents.empty())
            return false;

        place(seconds);
        mExtractor.advance();

        // **After the walk and after the history, which is where the game has it.** The sweep is
        // sound here for the first time: the walk above was the whole graph, so anything it did not
        // meet has genuinely gone. What it costs — the mark, the sweep and the compaction that
        // follows one — is cost the game pays every frame and this could not see until now.
        mExtractor.retire();
        return true;
    }

    Rtx::ExtractionStats PosedActors::mirror()
    {
        // The game's frame, and now this one: empty the lists a walk refills, then walk the whole
        // thing. The lights are among what it refills — they are `LightSource` nodes in the graph,
        // exactly as the game has them.
        // **Before the walk, because the walk is what would still find them.** A cell unloaded
        // since the last frame took its group off the root; anyone who was standing in it goes now.
        forgetDeparted();

        mScene.clearPlacement();

        // **The world walk and not a subtree's**, because this is the same root `StagedWorld` walks
        // and the sweep after it is global: a walk that missed what the graph does not parent
        // retires it.
        return mExtractor.extractWorld(mRoot, osg::Matrixf::identity(), 0);
    }

    void PosedActors::unplace()
    {
        // **The placements are not among what this empties.** They are addressed by slot and the
        // scene keeps them: a world that stands still stands still, and an actor walked in again
        // finds the slot it had. What `clearPlacement` empties is the per-frame lists, and the walk
        // that comes next is what fills them again.
        mScene.clearPlacement();
    }

    bool PosedActors::step(std::uint32_t frame)
    {
        return advanceTo(mSeconds + static_cast<float>(frame) * sFrameSeconds);
    }

    void PosedActors::posedAt(float seconds, float elapsed)
    {
        for (const Resident& resident : mResidents)
            resident.mActor->pose(seconds + resident.mPhase * resident.mActor->getDuration(), elapsed);
    }

    Rtx::ExtractionStats PosedActors::place(float seconds)
    {
        posedAt(seconds, seconds - mLastSeconds);
        mExtractor.advanceEmitters(seconds - mLastSeconds);
        mLastSeconds = seconds;

        // **Posed, and not placed.** They are in the graph, so the walk that mirrors everything else
        // mirrors them where they now stand. What this returns is what that walk found.
        return mirror();
    }
}
