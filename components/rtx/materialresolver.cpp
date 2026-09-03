#include "materialresolver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <string>

#include <osg/Image>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <components/sceneutil/statesetupdater.hpp>
#include <components/surface/material.hpp>
// `terraindrawable.hpp` holds `osg::ref_ptr`s to composite-map types it only forward-declares, so it
// does not compile on its own. This is what completes them.
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>
#include <components/vfs/pathutil.hpp>

#include "extractionstats.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "terraincomposite.hpp"

namespace Rtx
{
    namespace
    {
        const osg::Texture2D* getTexture(const osg::StateSet& stateSet, unsigned int unit)
        {
            return dynamic_cast<const osg::Texture2D*>(
                stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
        }

        /// A pass's texture matrix for `unit`, as the `uv * xy + zw` the shader wants.
        ///
        /// OpenSceneGraph hands the matrix to GLSL transposed — it stores rows where GLSL reads
        /// columns — so what a shader multiplies its coordinate by is the transpose of what is here,
        /// and the translation it picks up is this matrix's last row.
        osg::Vec4f getTextureTransform(const osg::StateSet& stateSet, unsigned int unit)
        {
            // Terrain binds two units and no more, so the names are spelled rather than built —
            // and named once for the process, because `getUniform` asks for a `std::string` and a
            // ground material is read for every chunk that arrives.
            static const std::array<std::string, 2> sNames{ "texMat0", "texMat1" };
            assert(unit < sNames.size());

            const osg::Uniform* uniform = stateSet.getUniform(sNames[unit]);
            osg::Matrixf matrix;
            if (uniform == nullptr || !uniform->get(matrix))
                return osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f);

            return osg::Vec4f(matrix(0, 0), matrix(1, 1), matrix(3, 0), matrix(3, 1));
        }

        /// The weights of one blend map, as floats in row order.
        ///
        /// `ESMTerrain` builds these as one byte per texel in `GL_ALPHA`, which is a hundred bytes
        /// for a chunk; widening them costs a few kilobytes a cell and saves requiring 8-bit storage
        /// of the device for the sake of it.
        ///
        /// **That one format is read along the row, and everything else asks `getColor`.**
        /// `getColor` decides on the pixel format and the data type per texel and builds a `Vec4` to
        /// hand back one component of it, which is 0.44% of a crossing — spent on the frame a chunk
        /// arrives, which is the frame with the least room. A blend map in any other format is a
        /// mod's or a test's, and the slow path is both what serves it and what the fast path is
        /// checked against.
        void readMask(const osg::Image& image, std::vector<float>& weights)
        {
            weights.clear();
            weights.reserve(static_cast<std::size_t>(image.s()) * image.t());

            if (image.getPixelFormat() == GL_ALPHA && image.getDataType() == GL_UNSIGNED_BYTE)
            {
                // **The reciprocal and not a divide, because that is `getColor`'s own arithmetic.**
                // The two disagree in the last place for 126 of the 256 byte values, and a weight is
                // what a chunk's ground is blended by and what its composite is baked from — so a
                // divide here would move the picture by a bit and the scene digests with it.
                constexpr float perByte = 1.0f / 255.0f;

                for (int row = 0; row < image.t(); ++row)
                {
                    const unsigned char* along = image.data(0, row);
                    for (int column = 0; column < image.s(); ++column)
                        weights.push_back(along[column] * perByte);
                }
                return;
            }

            for (int row = 0; row < image.t(); ++row)
                for (int column = 0; column < image.s(); ++column)
                    weights.push_back(image.getColor(column, row).a());
        }

        /// The state-set controller on `node`, from whichever callback chain carries it.
        ///
        /// **Both chains, because `NifOsg` picks between them by a flag on the content.** Anything
        /// marked `AnimFlag_AutoPlay` is hung from a cull callback and everything else from an
        /// update callback; what they animate — a flipbook, a scrolling UV, an alpha, a material
        /// colour — is the same either way.
        SceneUtil::StateSetUpdater* findUpdater(osg::Node& node)
        {
            for (osg::Callback* chain : { node.getCullCallback(), node.getUpdateCallback() })
                for (osg::Callback* callback = chain; callback != nullptr; callback = callback->getNestedCallback())
                    if (auto* updater = dynamic_cast<SceneUtil::StateSetUpdater*>(callback))
                        return updater;

            return nullptr;
        }
    }

    const osg::StateSet* MaterialResolver::animate(osg::Node& node, osg::NodeVisitor* visitor)
    {
        // Asked of every node in the graph every frame, and nearly all of a cell hangs off no
        // callback at all.
        if (node.getCullCallback() == nullptr && node.getUpdateCallback() == nullptr)
            return nullptr;

        SceneUtil::StateSetUpdater* updater = findUpdater(node);
        if (updater == nullptr)
            return nullptr;

        auto [entry, arrived] = mAnimated.try_emplace(&node);
        if (arrived)
        {
            // **A copy of what the node already wears, and a shallow one.** `applyCull` starts from
            // an empty state set and lets the rasterizer's state stack supply everything it does
            // not itself write — which a mirror reading one state set per surface cannot do, so a
            // fire would lose its material along with its animation. Shallow because an updater
            // that means to write an attribute makes itself a private copy in `setDefaults`, which
            // is the contract `applyUpdate` already rests on.
            //
            // The node's own is read rather than created: `getOrCreateStateSet` would leave an
            // empty one behind on a node that had none, and the walk above would then push it over
            // the material a parent was contributing.
            const osg::StateSet* base = node.getStateSet();
            entry->second.mStateSet
                = base != nullptr ? new osg::StateSet(*base, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
            updater->setDefaults(entry->second.mStateSet);
        }

        entry->second.mEpoch = mEpoch;
        updater->apply(entry->second.mStateSet, visitor);
        return entry->second.mStateSet;
    }

    Index MaterialResolver::resolveTerrain(const Terrain::TerrainDrawable& terrain, ExtractionStats& stats)
    {
        const Terrain::TerrainDrawable::PassVector& passes = terrain.getPasses();
        if (passes.empty())
            return sNoIndex;

        // The first pass is as good an identity as the chunk itself and is already a state set, so
        // terrain shares the material map with everything else.
        const osg::StateSet* identity = passes.front().get();
        const auto known = mMaterials.find(identity);
        if (known != mMaterials.end())
        {
            ++stats.mMaterialsReused;
            known->second.mEpoch = mEpoch;
            return known->second.mIndex;
        }

        Material material;
        material.mKind = MaterialKind::Terrain;

        mLayerScratch.clear();

        for (const osg::ref_ptr<osg::StateSet>& pass : passes)
        {
            const Surface::Material* described = Surface::getMaterial(*pass);
            if (described == nullptr)
            {
                ++stats.mUndescribedMaterials;
                continue;
            }

            MaterialLayer layer;
            layer.mDiffuse = takeTexture(described->getTexture(Surface::TextureRole::Diffuse), stats);
            if (layer.mDiffuse == sNoIndex)
                continue;

            layer.mDiffuseTransform = getTextureTransform(*pass, 0);

            // A chunk of a single ground type is given no blend map at all, and stays at full weight.
            const osg::Texture2D* mask = getTexture(*pass, 1);
            if (mask != nullptr && mask->getImage(0) != nullptr)
            {
                const osg::Image& image = *mask->getImage(0);
                readMask(image, mMaskScratch);

                // The two sides are what `SceneDesc::release` reconstructs the run's length from,
                // so a mask that is not as long as its own grid leaks the difference.
                assert(mMaskScratch.size() == static_cast<std::size_t>(image.s()) * image.t());

                layer.mMaskOffset = mScene.addMask(mMaskScratch);
                layer.mMaskWidth = static_cast<std::uint16_t>(image.s());
                layer.mMaskHeight = static_cast<std::uint16_t>(image.t());
                layer.mMaskTransform = getTextureTransform(*pass, 1);
            }

            mLayerScratch.push_back(layer);
        }

        if (mLayerScratch.empty())
            return sNoIndex;

        const Span run = mScene.addLayers(mLayerScratch);
        material.mLayerOffset = run.mOffset;
        material.mLayerCount = run.mCount;

        // **A chunk this wide is a shading question and not only a texturing one.** It covers whole
        // cells and carries every ground type in them, so shading it live costs a mask lookup and a
        // texture fetch per layer at every hit — and once there is distance to look at, distant hits
        // are most of the pixels. Past a cell the stack is flattened into one texture and a hit
        // takes a single fetch; the layers stay, because they are the recipe the bake reads.
        //
        // **A single layer is already a single fetch**, and flattening one would do nothing but
        // resample a tiling ground texture into something coarser than the file it came from.
        const osg::BoundingBox& bounds = terrain.getBoundingBox();
        const float across = std::max(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin());

        if (mLayerScratch.size() > 1 && across >= sCompositeFrom)
        {
            material.mFlatten = true;
            ++stats.mComposites;
        }

        const Index index = mScene.addMaterial(material);
        mMaterials.emplace(identity, Known{ .mIndex = index, .mEpoch = mEpoch });
        ++stats.mMaterialsAdded;
        return index;
    }

    Index MaterialResolver::resolveWater(ExtractionStats& stats)
    {
        // **One material for the sea, and it is keyed on nothing.** Water has no albedo — what it
        // looks like is what is behind and above it, worked out from the world position — so there
        // is nothing on a state set worth reading, and reading one is actively wrong twice over.
        //
        // `MWRender::Water` animates its surface with a `SceneUtil::StateSetUpdater`, which swaps
        // the node's state set between two copies of its own every frame: keyed on the address, the
        // mirror saw a new material each frame and swept the one before it, for a surface that had
        // not changed. And with `water shader = true` there is no state set on the node at all,
        // because that one is pushed from a cull callback the mirror runs outside of.
        if (mWater != sNoIndex)
        {
            ++stats.mMaterialsReused;
            mWaterEpoch = mEpoch;
            return mWater;
        }

        mWater = mScene.addMaterial(Material{ .mKind = MaterialKind::Water });
        mWaterEpoch = mEpoch;
        ++stats.mMaterialsAdded;
        return mWater;
    }

    Index MaterialResolver::resolve(std::span<const Shading> shading, ExtractionStats& stats)
    {
        if (shading.empty())
            return sNoIndex;

        // The material's identity is the state set nearest the drawable. Two drawables that share
        // it share their shading: OpenMW's optimizer collapses equivalent state sets into one
        // object, so sharing the pointer means sharing the values, and what the parents above
        // contribute in this graph is light and render-bin state rather than material.
        const Shading& own = shading.back();

        const auto known = mMaterials.find(own.mStateSet);
        if (known != mMaterials.end())
        {
            ++stats.mMaterialsReused;
            known->second.mEpoch = mEpoch;

            // **Read again, because a controller rewrote it since the last frame.** The state set
            // is the same object — that is what lets the material keep its slot and every placement
            // standing on it stay where it is — and everything inside it is this frame's.
            if (own.mAnimated)
                mScene.setMaterial(known->second.mIndex, readMaterial(shading, stats));

            return known->second.mIndex;
        }

        const Index index = mScene.addMaterial(readMaterial(shading, stats));
        mMaterials.emplace(own.mStateSet, Known{ .mIndex = index, .mEpoch = mEpoch });
        ++stats.mMaterialsAdded;
        return index;
    }

    Index MaterialResolver::takeTexture(const osg::Image* image, ExtractionStats& stats)
    {
        if (image == nullptr || image->getFileName().empty())
            return sNoIndex;

        // **Outside the cache, because what this counts is what the walk met and not what it
        // added.** `openmw-rtxtool scene --twice` reads these off a second walk of one graph, and a
        // count that only rose on an arrival would report nothing there.
        countFormat(*image, stats);

        if (const auto known = mTextureOf.find(image); known != mTextureOf.end())
        {
            known->second.mEpoch = mEpoch;
            return known->second.mIndex;
        }

        const Index index = mScene.addTexture(VFS::Path::Normalized(image->getFileName()));

        // **Held, because this entry is the reference.** `mTextureOf` says why a slot the map names
        // has to be one nothing else can hand out.
        mScene.holdTexture(index);
        mTextureOf.emplace(image, Known{ .mIndex = index, .mEpoch = mEpoch });

        return index;
    }

    Material MaterialResolver::readMaterial(std::span<const Shading> shading, ExtractionStats& stats)
    {
        Material material;

        // Before the description, because a surface nothing described is still one a controller
        // rewrites: what the flag states is a fact about the state set and not about what is in it.
        material.mAnimated = !shading.empty() && shading.back().mAnimated;

        const Surface::Material* described = findDescription(shading);
        if (described == nullptr)
        {
            ++stats.mUndescribedMaterials;
            return material;
        }

        material.mDiffuse = takeTexture(described->getTexture(Surface::TextureRole::Diffuse), stats);
        material.mEmissive = takeTexture(described->getTexture(Surface::TextureRole::Emissive), stats);

        // The two normal roles differ in what the alpha channel holds, and parallax is a rasterizer
        // feature this renderer does not have: to a ray tracer they are the same texture.
        material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::Normal), stats);
        if (material.mNormal == sNoIndex)
            material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::NormalHeight), stats);

        material.mAlphaRef = described->mAlphaRef;
        switch (described->mAlphaMode)
        {
            case Surface::AlphaMode::Blend:
                material.mAlphaMode = AlphaMode::Blend;
                break;
            case Surface::AlphaMode::Cutout:
                material.mAlphaMode = AlphaMode::Cutout;
                break;
            case Surface::AlphaMode::Opaque:
                break;
        }

        material.mTwoSided = described->mTwoSided;
        material.mDiffuseColour = described->mDiffuseColour;

        // Folded together because the game's own shader only ever uses their product.
        material.mEmissiveColour = described->mEmissiveColour * described->mEmissiveMult;

        // **Scaled about the middle of the texture, then offset**, which is what `NifOsg` builds its
        // texture matrix from — so `(uv - 0.5) * scale + 0.5 + offset`, resolved here into the
        // `uv * xy + zw` the sampler takes. Doing the arithmetic once on the host keeps two
        // multiplies and an add out of every texture fetch in the frame.
        const osg::Vec2f scale = described->mTextureScale;
        const osg::Vec2f offset = described->mTextureOffset;
        material.mTextureTransform = osg::Vec4f(
            scale.x(), scale.y(), 0.5f * (1.0f - scale.x()) + offset.x(), 0.5f * (1.0f - scale.y()) + offset.y());

        return material;
    }

    std::uint32_t MaterialResolver::retire(std::vector<Index>& live)
    {
        std::uint32_t went = sweep(mMaterials, mEpoch, live);

        // The sea's own, which is in no identity map because it is keyed on nothing. It survives a
        // frame that met water and goes with the last cell that had any.
        if (mWater != sNoIndex)
        {
            if (mWaterEpoch == mEpoch)
                live.push_back(mWater);
            else
            {
                mWater = sNoIndex;
                ++went;
            }
        }

        // The walk's own hold on every image a material is read from, given back the same way.
        //
        // **Most of these are met once.** A material a controller does not rewrite is resolved from
        // its cached entry and never read again, so the images behind it go stale on the frame after
        // they arrived — and the material's own reference is what keeps their slots. What settles
        // here is the animated materials, which are the ones the map exists for.
        std::erase_if(mTextureOf, [this](const auto& entry) {
            if (entry.second.mEpoch == mEpoch)
                return false;

            mScene.dropTexture(entry.second.mIndex);
            return true;
        });

        // What `animate` keeps. Swept with everything else because it is keyed on a node the graph
        // can drop, and because a state set held past its node holds the textures in it alive too.
        std::erase_if(mAnimated, [this](const auto& entry) { return entry.second.mEpoch != mEpoch; });

        return went;
    }
}
