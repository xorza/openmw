#pragma once

#include <cstdint>

#include <osg/Drawable>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include "cellworld.hpp"
#include "extractionstats.hpp"
#include "index.hpp"
#include "materialresolver.hpp"
#include "meshreader.hpp"

namespace Rtx
{
    /// What a residency may do inside the walk that asks it.
    ///
    /// **One object, because the walk is what a residency is inside.** The rows a residency stands
    /// are adopted through the mirror's own resolvers, under the identity the walk would find a
    /// clone's mesh under, so that a mesh both stand is one mesh. As public methods of
    /// `SceneExtractor` these would be calls that only mean anything inside a walk in front of every
    /// other reader of that class.
    ///
    /// **An adoption is a hold, and a release gives it back.** A count on the entry says once what
    /// re-stamping every adopted entry on every walk would say each frame: the sweep keeps a held
    /// entry whatever its stamp, and a residency keeps the drawable and the state set it adopted
    /// under — which it has anyway — to release by.
    class SceneAdopter
    {
    public:
        virtual ~SceneAdopter() = default;

        SceneAdopter(const SceneAdopter&) = delete;
        SceneAdopter& operator=(const SceneAdopter&) = delete;

        /// Walks `node` as though the graph had parented it where the residency was asked.
        virtual void take(osg::Node& node) = 0;

        /// The material of a reading somebody else made, adopted under the state set it names, with
        /// one hold taken on it. `sNoIndex` and no hold where the reading names no state set.
        virtual Index adoptMaterial(const MaterialReading& reading) = 0;

        /// The same for a mesh, held under the identity the walk will find a clone's mesh under.
        virtual Index adoptMesh(const osg::Drawable& drawable, const MeshReading& reading, Index material) = 0;

        /// Gives one hold back on what `adoptMesh` held under `drawable`.
        virtual void releaseMesh(const osg::Drawable& drawable) = 0;

        /// The same for `adoptMaterial`, by the state set the reading named. Nothing for null.
        virtual void releaseMaterial(const osg::StateSet* key) = 0;

    protected:
        SceneAdopter() = default;
    };

    /// Where the eye stands and how much world there is around it.
    ///
    /// **A value and not a set of setters.** Both residencies read all of these, and told them one
    /// call at a time the reach is measured twice on every frame and a fact added to the pair has
    /// to be remembered twice. The world itself is one value inside it, for the same reason.
    struct WorldAround
    {
        /// What is read and where from. A world with no storage is a world with none.
        CellWorld mWorld;

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

        /// Hands `into` everything held that the graph does not parent, and adds what it stood to
        /// `stats` — the walk's own, because a residency is stood inside the walk.
        virtual void collect(SceneAdopter& into, ExtractionStats& stats) = 0;

    protected:
        Residency() = default;
    };
}
