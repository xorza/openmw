#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <osg/Node>
#include <osg/ref_ptr>

#include <components/surface/material.hpp>

#include "alphaimage.hpp"
#include "mirroridentity.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "walk.hpp"

namespace osg
{
    class Image;
    class NodeVisitor;
    class StateSet;
}

namespace SceneUtil
{
    class StateSetUpdater;
}

namespace Rtx
{
    struct AlphaScratch;
    struct Shading;

    /// What a chain of state sets says a surface is, read where the chain is and adopted where the
    /// scene is. Everything here points into the state sets it was read from, so a reading is good
    /// for as long as the model that carries them stands.
    struct MaterialReading
    {
        /// The state set the material is held under: the nearest one to the drawable. Null where
        /// the chain was empty, which is a drawable that wears nothing.
        const osg::StateSet* mKey = nullptr;

        /// What the content said, or nothing where nothing did.
        std::optional<Surface::Material> mDescribed;

        /// Whether the diffuse map's alpha ever reaches solid — decided by the reader for the one
        /// kind of surface the answer changes, a translucent one, and left unset for every other.
        /// The reader answers it because the walk over the texels is the reading's whole cost.
        std::optional<bool> mDiffuseSolid;
    };

    /// Turns what the content says a surface is into the scene's materials, and keeps the textures
    /// they name. Keyed on the state set, which OpenMW's optimizer makes a meaningful identity; a
    /// controller rewriting one is the exception, and `resolve` reads that one again on every
    /// frame. The animation is here because OpenMW animates shading with a state set that belongs
    /// to the traversal rather than to the graph, so a walk has to build it.
    class MaterialResolver
    {
    public:
        /// A material slot and the state set it is held under, because which state set of a chain
        /// names a material is this class's answer.
        struct Resolved
        {
            Index mIndex = sNoIndex;
            const osg::StateSet* mKey = nullptr;
        };

        /// @param pass the walk in progress: its sweep stamp and its counts, read at every call.
        ///        Borrowed, so that the mirror and everything resolving into it cannot come to hold
        ///        two answers.
        MaterialResolver(SceneDesc& scene, const MirrorPass& pass)
            : mScene(scene)
            , mPass(pass)
        {
        }

        /// The material slot for the chain of state sets in force at a drawable.
        Resolved resolve(std::span<const Shading> shading);

        /// Reads the chain of state sets in force at a drawable, for a thread that has no scene to
        /// resolve into. `resolve` is the same reading followed by `adopt`.
        ///
        /// @param scratch what a translucent diffuse map's texels are walked through.
        static MaterialReading read(std::span<const Shading> shading, AlphaScratch& scratch);

        /// The material slot for a reading, adding it where the mirror holds none under its key,
        /// with one hold taken on the entry — `MeshResolver::adopt` says why a hold. Standing only:
        /// a reading carries no controller. `sNoIndex` and no hold for a reading with no key.
        Index adopt(const MaterialReading& reading);

        /// Gives one `adopt` back, by the state set the reading named. Nothing for null.
        void release(const osg::StateSet* key);

        /// The sea's own, keyed on the state set it has not got because a node mask is what
        /// identifies it.
        Resolved resolveWater();

        /// Runs the state-set controller on `node`, if it carries one, and hands back what it wrote.
        /// Null where the node animates no shading, which is nearly every node in a cell.
        const osg::StateSet* animate(osg::Node& node, osg::NodeVisitor* visitor);

        /// Whether every material the map holds was met this epoch, the sea's included — see
        /// `Kept::whole`. What the mirror asks before it sweeps, because the survivor list this
        /// fills is read beside the mesh resolver's.
        bool whole() const { return mMaterials.whole(); }

        /// Drops every material neither this epoch nor a hold keeps, and collects the survivors
        /// into `live`.
        void retire(std::vector<Index>& live);

        /// Lets go of the images and the animated state sets this epoch did not meet. Asked
        /// whatever the materials did, because a cached material's images go stale on the frame
        /// after they arrived.
        void retireHolds();

        /// Reserves the identity maps once, so no frame rehashes them. `SceneExtractor` states the
        /// budgets.
        void reserve(std::size_t materials, std::size_t textures, std::size_t animated)
        {
            mMaterials.reserve(materials);
            mTextureOf.reserve(textures);
            mAnimated.reserve(animated);
        }

    private:
        /// Reads a whole material off the chain, which is what an arrival and a rewrite both want.
        Material readMaterial(std::span<const Shading> shading);

        /// The material a description comes to, with its images taken into the scene.
        ///
        /// @param diffuseSolid whether the diffuse map reaches solid, where a reader already
        ///        answered; asked of the image here otherwise, and only where it matters.
        Material describe(const Surface::Material* described, bool animated, std::optional<bool> diffuseSolid);

        /// The slot `key` already holds, stamped and counted as a reuse, or `sNoIndex`.
        Index reuse(const osg::StateSet* key);

        /// Adds `material` under `key`, counted as an arrival.
        using Entry = Identity<const osg::StateSet>::Entry;
        Entry adopt(const osg::StateSet* key, const Material& material);

        /// The scene's slot for one image, held for as long as this names it.
        Index takeTexture(const osg::Image* image);

        /// Whether `image`'s alpha ever reaches solid — `reachesSolid`, read at the first material
        /// that asks and kept. Asked only for a translucent material's own diffuse map, because it
        /// walks every texel of the finest level.
        bool diffuseReachesSolid(const osg::Image* image);

        /// What the scene knows one image as, and whether its alpha ever reaches solid — unset
        /// until something asks, because the walk over its texels is only worth doing for a
        /// material that has to tell a wisp from a mask.
        struct HeldTexture : Known
        {
            std::optional<bool> mSolid;
        };

        /// The state set a node's controllers write into, kept so that the address a material is
        /// keyed on is the same one next frame. See `animate`. An entry like any other, so the map
        /// sweeps it by the epoch every entry carries; its index names nothing.
        struct Animated : Known
        {
            osg::ref_ptr<osg::StateSet> mStateSet;

            /// The controller found on the node's callback chains, or null where there was none,
            /// and what the chains looked like when it was found.
            SceneUtil::StateSetUpdater* mUpdater = nullptr;
            std::uintptr_t mChains = 0;
        };

        SceneDesc& mScene;
        const MirrorPass& mPass;

        /// Which state set each material came from, and the sea under the one it has not got —
        /// `resolveWater`. Owning, so that a state set cannot go while the entry stands: see
        /// `ByAddress`.
        Identity<const osg::StateSet> mMaterials{ mPass };

        /// Which slot each image the walk has met stands in, so an animated material re-read every
        /// frame does not build four `VFS::Path::Normalized` strings a frame. The entry is a
        /// reference, because a slot whose last material stops naming it is handed out again at
        /// once.
        Identity<const osg::Image, HeldTexture> mTextureOf{ mPass };

        /// Owning for the same reason the identity maps are: a node freed and replaced at the same
        /// address would otherwise be handed the state set the first one's controllers were writing.
        Identity<const osg::Node, Animated> mAnimated{ mPass };

        /// What `diffuseReachesSolid` reads a texture's alpha in, refilled per image it is asked
        /// about — which is once per translucent diffuse map a cell arrives with.
        AlphaScratch mAlphaScratch;
    };
}
