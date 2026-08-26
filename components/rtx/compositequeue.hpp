#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include "scenedesc.hpp"
#include "shadingmap.hpp"
#include "terraincomposite.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    /// How many rows of a composite one drain may bake.
    ///
    /// **A budget in the only unit that is honest about it.** A 512-square composite costs 28.5 ms,
    /// so a row is about 55 microseconds and sixteen of them is under a millisecond — which puts a
    /// whole composite about thirty frames out. That is a distant hillside shading from its layer
    /// stack for half a second after it arrives, which is a cost per hit and not a hitch, against a
    /// quarter of a second of frozen picture for the eight of them a cell boundary brings.
    inline constexpr std::uint32_t sBakeRowsPerDrain = 16;

    /// Every distant chunk waiting for its ground to be flattened, and the work drained into them.
    ///
    /// **The bake cannot happen on the frame that asks for it.** One costs 28.5 ms and a cell
    /// boundary wants several, so a walk that met a wide chunk and flattened it there and then would
    /// drop a quarter of a second of frames — which is what a spike is, however good the average.
    ///
    /// **Nothing is wrong while it waits.** A chunk asks by setting `Material::mFlatten` and its
    /// `mDiffuse` stays unset, which is the branch the shader already takes for every near chunk: it
    /// sums the layer stack at the hit. So the picture is right from the first frame and what the
    /// bake buys is the cost of that hit, not the sight of the ground.
    ///
    /// **A finished composite is held only until it is uploaded.** The bytes are a megabyte and a
    /// half apiece, and a region's worth is the same fifty megabytes the texture array already holds
    /// — keeping a second copy of that on the host would be paying twice for one picture.
    class CompositeQueue
    {
    public:
        /// Takes on every chunk that wants flattening and is not already waiting for it.
        ///
        /// **Off the shading revision, because that is what moves when a material is written.** A
        /// frame in which no material changed cannot have grown a chunk that wants a composite, and
        /// scanning the table for one would be a pass over every material in the world per frame.
        void gather(const SceneDesc& scene, Resource::ImageManager& images);

        /// Bakes at most `rows` more rows, oldest chunk first, and hands over what that finished.
        ///
        /// A finished composite takes a texture slot — which puts it among the scene's arrivals, so
        /// the upload that follows carries it — and goes onto the material that asked. One whose
        /// chunk left the world while it baked is dropped instead.
        ///
        /// @return how many composites finished.
        std::size_t drain(SceneDesc& scene, std::uint32_t rows);

        /// The finished composite in `slot`, or null where nothing here baked one.
        const TerrainComposite* find(Index slot) const;

        /// Lets go of everything `drain` finished. **After the upload and not before**: what is held
        /// between those two calls is the only copy of the bytes a backend has to read.
        void releaseFinished() { mFinished.clear(); }

        /// How many chunks are still waiting, which is what says whether a drain has anything to do.
        std::size_t getWaitingCount() const { return mWaiting.size(); }

    private:
        /// One chunk mid-bake, and enough of what it asked with to tell it apart from a chunk that
        /// has since taken over its slot.
        struct Waiting
        {
            Index mMaterial = sNoIndex;
            Index mLayerOffset = 0;
            Index mLayerCount = 0;
            TerrainComposite mComposite;
        };

        /// Oldest first, so a drain finishes one chunk rather than advancing every one of them.
        std::vector<Waiting> mWaiting;

        /// Finished this drain, by the slot they were given. Emptied by `releaseFinished`.
        std::unordered_map<Index, TerrainComposite> mFinished;

        /// The revision `gather` last read, so an unchanged table is not scanned again.
        ///
        /// **Nought is safe to start from** because `addMaterial` moves the revision: a table with
        /// anything in it to find is already past it.
        std::uint64_t mScanned = 0;

        /// The reset `gather` last saw, so a cleared scene takes what was waiting with it.
        std::uint64_t mReset = 0;

        /// Refilled per chunk rather than built afresh: a cell arriving is the frame that can least
        /// afford a string, a stack and a level table apiece.
        std::string mKey;
        std::vector<CompositeLayer> mStack;
        std::vector<MipLevel> mLevels;
        std::vector<osg::ref_ptr<const osg::Image>> mSources;

        /// Each ground texture's painted light, estimated once for as long as a gather runs.
        ///
        /// **Node-based and not a vector, because the stack spans these.** Estimating one reads
        /// every texel of a texture's largest level — the 5% of the game's CPU `texturebuilder.hpp`
        /// names — and neighbouring chunks are made of the same handful of ground textures, so this
        /// is a cache as much as it is storage. It has to outlive the layers pointing into it, which
        /// is until the composite that copies them is built.
        std::unordered_map<Index, ShadingMap> mPainted;
    };
}
