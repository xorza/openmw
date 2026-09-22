#pragma once

#include <string>

#include <osg/Image>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/vfs/pathutil.hpp>

#include "result.hpp"

namespace Terrain
{
    class ObjectStorage;
    class Storage;
}

namespace Rtx
{
    /// Where a cell's content is read from, by path: a model's template, and an image. An
    /// interface, so that a ring can be handed a model by a test that has no loader. The game
    /// answers out of `Resource::SceneManager`, whose template is the one node every clone is
    /// copied from and whose image cache hands one object to a template and to whoever asks for the
    /// path.
    class ContentSource
    {
    public:
        virtual ~ContentSource() = default;

        /// The template at `path`, or null where nothing stands for it. Safe to call from any
        /// thread, which is what the game's loader promises of its own.
        virtual osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) = 0;

        /// The image at `path`, or why none reads there — `Rtx::openImage`'s answer. Safe from any
        /// thread, as the template is.
        virtual Result<osg::ref_ptr<const osg::Image>, std::string> getImage(VFS::Path::NormalizedView path) = 0;
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

    /// What the mirror is handed of the settings, and never reads for itself: the two knobs the
    /// paging read for the distance's statics, which this renderer stands itself, and how far out
    /// the world is built. Handed once, because a frame reads what it was handed: the reach is one
    /// number for the ground, the air, the distant lights and the checks, and a host that asked
    /// the registry per frame could answer it differently in each. A run's, in `RunSetup`, so the
    /// harness and the played game fill it the one way; the statics need a restart, and the reach
    /// follows the menu through `WorldMirror::setReach`.
    struct MirrorKnobs
    {
        /// `distantLandReach`, in units.
        float mReach = 0.0f;

        /// `object paging`: whether the distance's statics stand at all.
        bool mDistantStatics = true;

        /// `object paging min size`: the size rule's constant.
        float mMinSize = 0.0f;
    };

    /// Where the eye stands and how much world there is around it, as one value the ring reads.
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

        /// Whether the eye stands in the exterior worldspace, which is where a distant world is.
        /// False in an interior cell, whose coordinates belong to another space — a quasi-exterior
        /// included, which has a sky (`WorldReading::mOutdoors`) and no distance.
        bool mExterior = true;

        /// The world's clock, in seconds: what a lamp the ring stands is animated by, as the walk
        /// animates the graph's by its frame stamp.
        double mSimulationTime = 0.0;
    };
}
