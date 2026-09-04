#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <osg/Node>
#include <osg/ref_ptr>

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

namespace Terrain
{
    class TerrainDrawable;
}

namespace Rtx
{
    struct Shading;

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
        /// @param pass the walk in progress: its sweep stamp and its counts, read at every call.
        ///        Borrowed, so that the mirror and everything resolving into it cannot come to hold
        ///        two answers.
        MaterialResolver(SceneDesc& scene, const MirrorPass& pass)
            : mScene(scene)
            , mPass(pass)
        {
        }

        /// The material slot for the chain of state sets in force at a drawable.
        Index resolve(std::span<const Shading> shading);

        /// The same for a terrain chunk, whose material is on the drawable rather than on the graph.
        Index resolveTerrain(const Terrain::TerrainDrawable& terrain);

        /// The sea's own, which is keyed on nothing because a node mask is what identifies it.
        Index resolveWater();

        /// Runs the state-set controller on `node`, if it carries one, and hands back what it wrote.
        ///
        /// Null where the node animates no shading, which is nearly every node in a cell.
        ///
        /// @param visitor what the controller is applied under, which is the walk itself.
        const osg::StateSet* animate(osg::Node& node, osg::NodeVisitor* visitor);

        /// Drops every material this epoch did not meet, and collects the survivors into `live`.
        ///
        /// @return how many were dropped.
        std::uint32_t retire(std::vector<Index>& live);

    private:
        /// Reads a whole material off the chain, which is what an arrival and a rewrite both want.
        Material readMaterial(std::span<const Shading> shading);

        /// The scene's slot for one image, held for as long as this names it.
        Index takeTexture(const osg::Image* image);

        /// Whether `image`'s alpha ever reaches solid — `reachesSolid`, measured at the first
        /// material that asks and kept for every later one.
        ///
        /// **Asked only where the answer changes something**, which is a translucent material's own
        /// diffuse map: it walks every texel of the finest level, and a cell holds hundreds of
        /// textures no medium is ever made of.
        bool diffuseReachesSolid(const osg::Image* image);

        /// A node's controllers and the state set they write into, kept so the address is the same
        /// one next frame. See `animate`.
        /// What the scene knows one image as, and whether its alpha ever reaches solid.
        ///
        /// **Unset until something asks**, because the walk over its texels is only worth doing for
        /// a material that has to tell a wisp from a mask.
        struct HeldTexture : Known
        {
            std::optional<bool> mSolid;
        };

        struct Animated
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            std::uint64_t mEpoch = 0;
        };

        SceneDesc& mScene;
        const MirrorPass& mPass;

        /// Which state set each material came from. Owning, so that a state set cannot go while the
        /// entry stands: see `ByAddress`.
        Identity<const osg::StateSet> mMaterials;

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
        Identity<const osg::Image, HeldTexture> mTextureOf;

        /// Owning for the same reason the identity maps are: a node freed and replaced at the same
        /// address would otherwise be handed the state set the first one's controllers were writing.
        std::unordered_map<osg::ref_ptr<const osg::Node>, Animated, ByAddress<const osg::Node>,
            ByAddress<const osg::Node>>
            mAnimated;

        /// The sea's material and when it was last met. Not in `mMaterials`, because what identifies
        /// it is the node mask rather than any state set — see `resolveWater`.
        Index mWater = sNoIndex;
        std::uint64_t mWaterEpoch = 0;

        /// One blend map's weights as floats, refilled per terrain layer that carries one.
        std::vector<float> mMaskScratch;

        /// One terrain material's layers, refilled per chunk: a run is allocated by length and the
        /// length is only known once the passes with no texture on them have been passed over.
        std::vector<MaterialLayer> mLayerScratch;
    };
}
