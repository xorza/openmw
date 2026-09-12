#pragma once

#include <cstdint>

#include <osg/Drawable>
#include <osg/Image>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Vec3f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/vfs/pathutil.hpp>

#include "extractionstats.hpp"
#include "materialresolver.hpp"
#include "meshreader.hpp"
#include "runs.hpp"

namespace Terrain
{
    class ObjectStorage;
    class Storage;
}

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    /// Where a cell's content is read from, by path: a model's template, and an image. An
    /// interface, so that a ring can be handed a model by a test that has no loader. The game
    /// answers out of `Resource::SceneManager`, whose template is the one node every clone is copied
    /// from and whose image cache hands one object to a template and to whoever asks for the path.
    class ContentSource
    {
    public:
        virtual ~ContentSource() = default;

        /// The template at `path`, or null where nothing stands for it. Safe to call from any
        /// thread, which is what the game's loader promises of its own.
        virtual osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) = 0;

        /// The image at `path`, or null where nothing could be read there. Safe from any thread,
        /// as the template is.
        virtual osg::ref_ptr<const osg::Image> getImage(VFS::Path::NormalizedView path) = 0;
    };

    /// The game's content, out of its scene manager.
    class SceneContent final : public ContentSource
    {
    public:
        explicit SceneContent(Resource::SceneManager& scenes)
            : mScenes(scenes)
        {
        }

        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) override;
        osg::ref_ptr<const osg::Image> getImage(VFS::Path::NormalizedView path) override;

    private:
        Resource::SceneManager& mScenes;
    };

    /// Where the world's cells are read from: the content, and which worldspace of it — exactly
    /// `CellReader`'s arguments, compared as one because a change to any of them is a reader that
    /// has to be built again.
    struct CellWorld
    {
        /// What the content files say stands where, or null for a world with none.
        const Terrain::ObjectStorage* mStorage = nullptr;

        /// The land the heights and the blend maps are read off.
        Terrain::Storage* mGround = nullptr;

        /// The loader the models and the images come out of.
        ContentSource* mContent = nullptr;

        ESM::RefId mWorldspace;

        /// Which nodes a walk of a template may descend into — the frame walk's own.
        osg::Node::NodeMask mMask = ~0u;

        /// Whether there is enough here to read anything at all.
        bool isReadable() const { return mStorage != nullptr && mGround != nullptr && mContent != nullptr; }

        bool operator==(const CellWorld& other) const = default;
    };

    /// What a residency may do inside the walk that asks it: adopt rows through the mirror's own
    /// resolvers, under the identity the walk would find a clone's mesh under, so that a mesh both
    /// stand is one mesh. An adoption is a hold and a release gives it back, so the sweep keeps a
    /// held entry whatever its stamp.
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

    /// Where the eye stands and how much world there is around it, as one value both residencies
    /// read.
    struct WorldAround
    {
        /// What is read and where from. A world with no storage is a world with none.
        CellWorld mWorld;

        /// Where the eye is, which decides every ring.
        osg::Vec3f mEye;

        /// How far out anything is stood, in units — `distantLandReach`. Told rather than asked,
        /// so this library reads no settings. Nought stands nothing.
        float mReach = 0.0f;

        /// The cells the game has stood for itself, as `Terrain::World` states them: minimum
        /// inclusive, maximum exclusive.
        osg::Vec4i mActiveGrid;

        /// Whether there is a distant world to stand in. False in an interior, where the eye's
        /// coordinates belong to another space.
        bool mOutdoors = true;
    };

    /// What a walk of the scene graph cannot reach, offered to the walk that asks for it: the
    /// distance is nobody's node. `CellRing` stands its ground and statics, and `DistantLights`
    /// the lamps of cells the paging leaves dark, because `LIGH` is not a paged type.
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
