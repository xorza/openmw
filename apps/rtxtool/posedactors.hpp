#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <osg/Group>
#include <osg/Matrixf>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include "motion.hpp"

namespace RtxTool
{
    struct ActorModel;
    struct CellPerson;
    struct CellProp;
    struct Placement;
    class Actor;
    class World;

    /// Who to put in front of the camera.
    struct ActorRequest
    {
        /// Creature model paths, as a `CREA` record holds them.
        std::vector<std::string> mCreatures;

        /// `NPC_` record ids. Each is assembled out of their race's body parts.
        std::vector<std::string> mPeople;

        /// How many seconds into their animation they start.
        float mSeconds = 0.0f;

        /// Whether the region's own residents stand in it as well.
        bool mResidents = true;

        /// Whether the cell's live props — the candles, torches, braziers and fireplaces whose
        /// emitters have to be *run* to hold anything — are instanced and stepped.
        ///
        /// Off leaves them as the shared templates every other reference uses, which is cheaper and
        /// which draws each emitter's authored seed particles rather than a flame.
        bool mProps = true;

        /// Whether people wear what their record carries. Off leaves everyone in their skin.
        bool mClothes = true;

        bool empty() const { return mCreatures.empty() && mPeople.empty(); }
        std::size_t size() const { return mCreatures.size() + mPeople.size(); }
    };

    /// A row of actors, posed and walked into the scene once per frame.
    ///
    /// **The still world is restored from a snapshot rather than mirrored again.** The game re-walks
    /// its whole graph every frame and can, because it has one; the harness does not — a cell is a
    /// template fetched per reference and a terrain grid built cell by cell, and walking that again
    /// per frame is the load again per frame. What a frame actually needs is that the placements be
    /// there and the deforming meshes be re-read, and a list copied back gives the first exactly as
    /// a re-walk would.
    class PosedActors : public Motion
    {
    public:
        /// Nobody yet, and the world as it currently stands taken as the snapshot.
        ///
        /// @param request read here for what applies to everyone — how they are dressed and where in
        ///        their animation they start — because the residents go in before any row does.
        ///
        /// **The snapshot before anyone is added, and `settle` after everyone is.** Adding builds a
        /// body without placing it; a snapshot taken with one already in it would put a second copy
        /// of that person in the scene on the very next frame.
        PosedActors(World& world, Rtx::SceneDesc& scene, Rtx::SceneExtractor& extractor, osg::Group& root,
            const ActorRequest& request);
        ~PosedActors();

        PosedActors(const PosedActors&) = delete;
        PosedActors& operator=(const PosedActors&) = delete;

        /// Everyone `request` names, standing in a row in front of `placement` and facing it.
        void addRow(const ActorRequest& request, const Placement& placement);

        /// Everyone a cell placed, each where the cell put them.
        void addResidents(std::span<const CellPerson> people);

        /// The cell's live props, each as an instance of its own model.
        ///
        /// **An instance per reference, and it costs no geometry.** A template's particle systems
        /// are shared and never updated, so thirty candles would share one frozen flame; cloning
        /// deep-copies the nodes and the emitters and leaves the plain geometry shared, so the mesh
        /// cache still resolves all thirty candles to one mesh.
        void addProps(std::span<const CellProp> props);

        /// Walks everyone in and reports what they came to.
        ///
        /// **After `StagedWorld::warmEmitters`, which is what runs the emitters up.** Warming used
        /// to happen here, which quietly made it something only a cell with people in it got: the
        /// weather hangs its own emitters off the same graph, so a view with nobody standing in it
        /// rendered a rainstorm of one frame's worth of drops.
        const Rtx::ExtractionStats& settle();

        /// Poses everyone `elapsed` further on without walking them in, for a caller stepping the
        /// whole graph's emitters. An actor's own plume is stepped by that walk, so it has to be
        /// standing where it will stand while the walk happens.
        void poseFor(float elapsed) { posedAt(mSeconds, elapsed); }

        /// How far a repeated frame carries the animation. Sixty a second, because that is what the
        /// frame budget is written against and an actor should move the same amount per frame here
        /// as it does in the game.
        ///
        /// Public because `StagedWorld::warmEmitters` steps the whole graph on it.
        static constexpr float sFrameSeconds = 1.0f / 60.0f;

        /// Advances to `seconds` and walks everyone back in. False where there is nobody.
        bool advanceTo(float seconds);

        bool step(std::uint32_t frame) override;

        /// Empties the lists a frame refills, and puts the world's own lights back into them.
        ///
        /// Placements are not among those lists: the scene holds them in slots and an actor walked
        /// in again finds the one it had, so nothing has to be taken out to stop it being placed
        /// twice.
        void unplace();

        /// Empties the per-frame lists and walks the graph, which is what fills them again.
        Rtx::ExtractionStats mirror();

        /// What the actors added the first time they were walked in.
        const Rtx::ExtractionStats& getPlaced() const { return mPlaced; }

        /// Everything being posed, props included.
        std::size_t getCount() const { return mResidents.size(); }

        /// How many of those are props rather than people or creatures.
        std::size_t getPropCount() const;

    private:
        /// @param cell the group to hang it under, or null to hang it on the run's own root — which
        ///        is what an actor a request asked for gets, since no cell placed it.
        /// @param prop a candle or a fire rather than a person: posed identically, counted apart.
        void add(ActorModel model, const osg::Matrixf& transform, osg::Group* cell, bool prop = false);

        /// Drops everyone whose cell has left the graph.
        ///
        /// **Read off the graph rather than told.** Unloading a cell is taking its group off the
        /// root, so a group with no parent is a cell that has gone — the same signal
        /// `QuadTreeWorld::collect` reads to know its terrain is disabled. Nothing has to remember
        /// to call anything, which is what kept the residents standing after their town was swept.
        void forgetDeparted();
        Rtx::ExtractionStats place(float seconds);

        /// Poses everyone at `seconds`, having advanced the world by `elapsed`, without walking
        /// them into the scene.
        ///
        /// **Two clocks and not one.** A keyframe track is *sampled* at a time and wrapped to its
        /// own length; an emitter *integrates* and can only go forwards. The warm-up holds the first
        /// still and turns the second, and a window turns both — so the caller says which is which
        /// rather than one being derived from the other.
        void posedAt(float seconds, float elapsed);

        World& mWorld;
        Rtx::SceneDesc& mScene;
        Rtx::SceneExtractor& mExtractor;

        /// Where an actor is hung, so the one walk that mirrors the world mirrors them too.
        ///
        /// **They used to be extracted separately, and that was the harness's whole divergence.**
        /// A walk that covered only the movers is a walk that never pays for the world, so the
        /// benchmark could not see the cost the game pays every frame. In the graph they are just
        /// more of it.
        osg::Group& mRoot;

        /// One thing being posed, and what it belongs to.
        ///
        /// **One record rather than vectors side by side**, because every field here is indexed by
        /// the same actor and a resident that leaves takes all of them with it. Kept apart, a
        /// removal has to remember each one.
        struct Resident
        {
            std::unique_ptr<Actor> mActor;

            /// How far into its own animation it is, on top of the clock.
            ///
            /// **Without it a town breathes in unison**, which reads as a rank of automata rather
            /// than as people: everyone plays the same idle and nothing in the content offsets them.
            float mPhase = 0.0f;

            /// A prop rather than a person or a creature. Posed exactly the same way — the emitters
            /// hang off update callbacks, which is what the traversal is for — and only counted
            /// apart.
            bool mProp = false;

            /// The cell group this hangs under, or null for the actors a request asked for rather
            /// than a cell. **What says when it should go**: the group leaves the root when its cell
            /// is unloaded, and a resident under a group with no parent is standing in a street that
            /// is no longer there.
            ///
            /// **Owning, which is what makes that question askable.** `LoadedCells` lets go of the
            /// group when the cell leaves; without a reference here the node would be freed and the
            /// parent count read off memory nobody owns. It holds the departed cell's subtree until
            /// the next walk, which is one frame and the price of not being told.
            osg::ref_ptr<osg::Group> mCell;
        };

        std::vector<Resident> mResidents;

        Rtx::ExtractionStats mPlaced;
        float mSeconds = 0.0f;
        bool mClothes = true;

        /// The animation time the last frame ran at, so the next can say how much time has passed.
        float mLastSeconds = 0.0f;
    };
}
