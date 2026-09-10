#pragma once

#include <cstdint>

#include <osg/Drawable>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>

#include "index.hpp"
#include "materialresolver.hpp"
#include "meshreader.hpp"
#include "mirroridentity.hpp"

namespace Terrain
{
    class ObjectStorage;
}

namespace Rtx
{
    /// What one residency's walk came to.
    ///
    /// **Reported and not written**, so the walk's counters stay the walk's. Every field is what
    /// this residency alone stood or dropped, and the walk is what adds them into the pass.
    struct ResidencyCount
    {
        /// Placements stood this walk, among the instances and on their own.
        std::uint32_t mDistantStatics = 0;
        std::uint32_t mGroundCells = 0;

        /// Rows this residency owns outright that arrived on this walk.
        std::uint32_t mMeshesAdded = 0;
        std::uint32_t mMaterialsAdded = 0;

        /// Rows it owned and has let go of since it last reported.
        ///
        /// **Carried by the residency and not by the walk**, so nothing has to reach the extractor
        /// between two walks: a world that is detached drops everything outside any walk, and it
        /// says so on the next `collect` rather than reaching for a counter that is not there.
        std::uint32_t mMeshesDisowned = 0;
        std::uint32_t mMaterialsDisowned = 0;
    };

    /// What a residency may do inside the walk that asks it.
    ///
    /// **One object, because the walk is what a residency is inside.** The rows a residency stands
    /// are adopted through the mirror's own resolvers, stamped by the epoch the walk carries and
    /// swept by the sweep that follows it. Every call here used to be a public method of
    /// `SceneExtractor` with one caller, which put eleven calls that only mean anything inside a
    /// walk in front of every other reader of that class.
    class Collector
    {
    public:
        virtual ~Collector() = default;

        Collector(const Collector&) = delete;
        Collector& operator=(const Collector&) = delete;

        /// Walks `node` as though the graph had parented it where the residency was asked.
        virtual void take(osg::Node& node) = 0;

        /// The material of a reading somebody else made, adopted under the state set it names.
        virtual MaterialResolver::Resolved adoptMaterial(const MaterialReading& reading) = 0;

        /// The same for a mesh, held under the identity the walk will find a clone's mesh under.
        ///
        /// @return the entry it is held under, which stays where it is for as long as it is
        ///         stamped — so a caller that stamps it every frame may keep it rather than look
        ///         the drawable up again.
        virtual Known& adoptMesh(const osg::Drawable& drawable, const MeshReading& reading, Index material) = 0;

        /// The entry `key` is held under, unstamped, or null where the mirror holds none.
        virtual Known* findMaterial(const osg::StateSet* key) = 0;

        /// Says the walk met one of those again, for a caller that kept what `adopt` handed it.
        virtual void keepMesh(Known& held) = 0;
        virtual void keepMaterial(Known& held) = 0;

        /// The rows a residency owns outright, which no drawable and no state set will ever name.
        ///
        /// **A cell's ground has no drawable and no state set**, so the identity maps have no key to
        /// hold it under and the sweep would release it on the first frame. The residency names its
        /// rows here on every walk instead, and they join the survivors the sweep hands the scene.
        virtual void keepOwnedMesh(Index mesh) = 0;
        virtual void keepOwnedMaterial(Index material) = 0;

    protected:
        Collector() = default;
    };

    /// Where the eye stands and how much world there is around it.
    ///
    /// **A value and not a set of setters.** Both residencies read all six of these, and both were
    /// told them one call at a time at one call site — so the reach was measured twice on every
    /// frame and a fact added to the pair had to be remembered twice.
    struct WorldAround
    {
        /// What the content files say stands where, or null for a world with none.
        const Terrain::ObjectStorage* mStorage = nullptr;
        ESM::RefId mWorldspace;

        /// Where the eye is, which decides every ring.
        osg::Vec3f mEye;

        /// How far out anything is stood, in units — `distantLandReach`.
        ///
        /// **Told rather than asked, so this library reads no settings.** Nought stands nothing.
        float mReach = 0.0f;

        /// The cells the game has stood for itself, as `Terrain::World` states them: minimum
        /// inclusive, maximum exclusive.
        osg::Vec4i mActiveGrid;

        /// Whether there is a distant world to stand in. False in an interior, where the eye's
        /// coordinates belong to another space.
        bool mOutdoors = true;
    };

    /// What a walk of the scene graph cannot reach, offered to the walk that asks for it.
    ///
    /// **What this renderer stands for itself, and the content files state.** A walk of the graph
    /// finds what the game stood: the active cells' objects and actors. The distance is nobody's
    /// node — `CellRing` stands its ground off the land records and its statics as instances of
    /// their templates, and `DistantLights` stands the lamps of cells the paging leaves dark, which
    /// have no node in either renderer because `LIGH` is not a paged type. Each is asked, inside the
    /// walk, for what it stands; the abstraction is here because the extractor may be handed none,
    /// which is every scene that is not a world.
    class Residency
    {
    public:
        virtual ~Residency() = default;

        Residency(const Residency&) = delete;
        Residency& operator=(const Residency&) = delete;

        /// Where the world is now. Told once a frame, before the walk.
        virtual void follow(const WorldAround& around) = 0;

        /// Hands `into` everything held that the graph does not parent.
        ///
        /// **What it stood is returned and not pushed**, so a residency that reports nothing is one
        /// the compiler names rather than one whose rows the sweep silently keeps.
        virtual ResidencyCount collect(Collector& into) = 0;

    protected:
        Residency() = default;
    };
}
