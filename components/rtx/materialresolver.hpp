#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <osg/Node>
#include <osg/ref_ptr>

#include "alphaimage.hpp"
#include "index.hpp"
#include "mirroridentity.hpp"
#include "mirrorpass.hpp"
#include "scenedesc.hpp"

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

namespace Surface
{
    struct Material;
}

namespace Rtx
{
    struct AlphaScratch;
    struct Shading;

    /// What a chain of state sets says a surface is, read where the chain is and adopted where the
    /// scene is.
    ///
    /// **The half of a material's arrival that reads, apart from the half that inserts.** Everything
    /// here points into the state sets it was read from — the key and the description are theirs —
    /// so a reading is good for as long as the model that carries them stands, and a ring preparing
    /// cells ahead of the eye holds the model for exactly that.
    struct MaterialReading
    {
        /// The state set the material is held under: the nearest one to the drawable. Null where
        /// the chain was empty, which is a drawable that wears nothing.
        const osg::StateSet* mKey = nullptr;

        /// What the content said, or null where nothing did.
        const Surface::Material* mDescribed = nullptr;

        /// Whether the diffuse map's alpha ever reaches solid — decided by the reader for the one
        /// kind of surface the answer changes, a translucent one, and left unset for every other.
        /// The reader answers it because the walk over the texels is the reading's whole cost.
        std::optional<bool> mDiffuseSolid;
    };

    /// Turns what the content says a surface is into the scene's materials, and keeps the textures
    /// they name.
    ///
    /// **Keyed on the state set, which OpenMW makes a meaningful identity.** Its optimizer collapses
    /// equivalent state sets into one, so a material met again under another reference is the
    /// material already uploaded. A controller rewriting one is the exception, and `resolve` reads
    /// that one again on every frame it is met.
    ///
    /// **The animation is here because it is what a material is read from.** OpenMW animates shading
    /// by handing a `SceneUtil::StateSetUpdater` a state set that belongs to the traversal rather
    /// than to the graph, so the state set in force at a drawable is something a walk builds — and
    /// this is what builds it.
    class MaterialResolver
    {
    public:
        /// A material slot and the state set it is held under.
        ///
        /// **The key travels with the slot** because which state set of a chain names a material is
        /// this class's answer, and a caller that picked one for itself would be a second answer to
        /// that.
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
        ///
        /// Null where the node animates no shading, which is nearly every node in a cell.
        ///
        /// @param visitor what the controller is applied under, which is the walk itself.
        const osg::StateSet* animate(osg::Node& node, osg::NodeVisitor* visitor);

        /// Whether every material the map holds was met this epoch, the sea's included — see
        /// `Kept::whole`. What the mirror asks before it sweeps, because the survivor list this
        /// fills is read beside the mesh resolver's.
        bool whole() const { return mMaterials.whole(); }

        /// Drops every material neither this epoch nor a hold keeps, and collects the survivors
        /// into `live`.
        void retire(std::vector<Index>& live);

        /// Lets go of the images and the animated state sets this epoch did not meet.
        ///
        /// **Asked whatever the materials did.** A material a controller does not rewrite is
        /// resolved from its cached entry and never read again, so the images behind it go stale on
        /// the frame after they arrived — on a frame where nothing died at all. Each map skips its
        /// own walk where the epoch reached all of it.
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
        ///
        /// **One statement for the two resolvers**, so that what is left in each is only what it
        /// does differently: `resolve` re-reads an animated material, and `resolveWater` builds
        /// nothing at all.
        Index reuse(const osg::StateSet* key);

        /// Adds `material` under `key`, counted as an arrival.
        using Entry = Identity<const osg::StateSet>::Entry;
        Entry adopt(const osg::StateSet* key, const Material& material);

        /// The scene's slot for one image, held for as long as this names it.
        Index takeTexture(const osg::Image* image);

        /// Whether `image`'s alpha ever reaches solid — `reachesSolid`, measured at the first
        /// material that asks and kept for every later one.
        ///
        /// **Asked only where the answer changes something**, which is a translucent material's own
        /// diffuse map: it walks every texel of the finest level, and a cell holds hundreds of
        /// textures no medium is ever made of.
        bool diffuseReachesSolid(const osg::Image* image);

        /// What the scene knows one image as, and whether its alpha ever reaches solid.
        ///
        /// **Unset until something asks**, because the walk over its texels is only worth doing for
        /// a material that has to tell a wisp from a mask.
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

        /// Which slot each image the walk has met stands in.
        ///
        /// **What stops a texture's name being built again every frame.** A material a controller
        /// rewrites is read again on every frame it is met, and reading one asks for up to four
        /// textures. Asking by path builds a `VFS::Path::Normalized` that dies at the end of the
        /// call, because `SceneDesc::addTexture` takes a view: four strings off the heap per
        /// animated material per frame.
        ///
        /// **This entry is a reference, like the emitter resolver's holds.** A slot whose last
        /// material stops naming it drops to nought and is handed out again at once, so an entry
        /// that only remembered the number would answer with a slot another texture had taken over.
        Identity<const osg::Image, HeldTexture> mTextureOf{ mPass };

        /// Owning for the same reason the identity maps are: a node freed and replaced at the same
        /// address would otherwise be handed the state set the first one's controllers were writing.
        Identity<const osg::Node, Animated> mAnimated{ mPass };

        /// What `diffuseReachesSolid` reads a texture's alpha in, refilled per image it is asked
        /// about — which is once per translucent diffuse map a cell arrives with.
        AlphaScratch mAlphaScratch;
    };
}
