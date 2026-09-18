#include "materialresolver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <osg/Callback>
#include <osg/CopyOp>
#include <osg/Image>
#include <osg/StateSet>
#include <osg/Vec2f>
#include <osg/Vec4f>

#include <components/sceneutil/statesetupdater.hpp>
#include <components/vfs/pathutil.hpp>

#include "alphaimage.hpp"
#include "colour.hpp"
#include "contract.hpp"
#include "extractionstats.hpp"
#include "material.hpp"
#include "scenedesc.hpp"
#include "shaders/look.h"
#include "shading.hpp"
#include "surface.hpp"
#include "texels.hpp"

namespace Rtx
{
    namespace
    {
        /// What the sea's material is keyed on: the state set it has not got (`resolveWater`).
        /// Nothing else in the world can key as null, because a shading chain's entries come from
        /// `MirrorTraversal::pushShading`, which takes a reference.
        constexpr const osg::StateSet* sSea = nullptr;

        /// What one texel of a sheet adds on average under `blend`: weighted by its own alpha
        /// where the blend reads one, and whole where `AddWhole` reads none.
        osg::Vec3f meanUnder(const MeanTexel& mean, const BlendKind blend)
        {
            return blend == BlendKind::AddWhole ? mean.mWhole : mean.mColour;
        }

        /// The state-set controller on `node`, from whichever callback chain carries it: `NifOsg`
        /// hangs anything marked `AnimFlag_AutoPlay` from a cull callback and everything else from
        /// an update callback.
        SceneUtil::StateSetUpdater* findUpdater(osg::Node& node)
        {
            for (osg::Callback* chain : { node.getCullCallback(), node.getUpdateCallback() })
                for (osg::Callback* callback = chain; callback != nullptr; callback = callback->getNestedCallback())
                    if (auto* updater = dynamic_cast<SceneUtil::StateSetUpdater*>(callback))
                        return updater;

            return nullptr;
        }

        /// What hangs on `node`'s two chains, as one number: a callback added, removed or swapped
        /// anywhere on either changes it. Pointer arithmetic down chains of one or two, against the
        /// casts `findUpdater` takes.
        std::uintptr_t chainSignature(const osg::Node& node)
        {
            std::uintptr_t signature = 0;
            for (const osg::Callback* chain : { node.getCullCallback(), node.getUpdateCallback() })
                for (const osg::Callback* callback = chain; callback != nullptr;
                     callback = callback->getNestedCallback())
                    signature = (signature * 31u) ^ reinterpret_cast<std::uintptr_t>(callback);

            return signature;
        }
    }

    const osg::StateSet* MaterialResolver::animate(osg::Node& node, osg::NodeVisitor* visitor)
    {
        // Asked of every node in the graph every frame, and nearly all of a cell hangs off no
        // callback at all.
        if (node.getCullCallback() == nullptr && node.getUpdateCallback() == nullptr)
            return nullptr;

        // The casts are taken when the chains change, not per frame. The entry remembers what it
        // found and what the chains looked like when it found it; a controller swapped, appended or
        // removed under the walk changes the signature, and a node whose chains carry no updater
        // keeps a null one.
        const auto [entry, arrived] = mAnimated.reach(&node);
        Animated& held = entry->second;
        const std::uintptr_t chains = chainSignature(node);
        if (arrived || chains != held.mChains)
        {
            held.mChains = chains;
            held.mUpdater = findUpdater(node);
        }

        SceneUtil::StateSetUpdater* updater = held.mUpdater;
        if (updater == nullptr)
            return nullptr;

        if (held.mStateSet == nullptr)
        {
            // A shallow copy of what the node already wears: `applyCull` starts from an empty state
            // set and lets the rasterizer's state stack supply the rest, so a fire would lose its
            // material along with its animation. Read rather than created, because
            // `getOrCreateStateSet` would leave an empty one on a node that had none, pushed over
            // the material a parent was contributing.
            const osg::StateSet* base = node.getStateSet();
            held.mStateSet = base != nullptr ? new osg::StateSet(*base, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
            updater->setDefaults(held.mStateSet);
        }

        updater->apply(held.mStateSet, visitor);
        return held.mStateSet;
    }

    MaterialResolver::Entry MaterialResolver::reuse(const osg::StateSet* const key)
    {
        const Entry known = mMaterials.find(key);
        if (known == mMaterials.end())
            return known;

        ++mPass.getStats().mMaterialsReused;
        mMaterials.stamp(known);

        return known;
    }

    MaterialResolver::Entry MaterialResolver::adopt(const osg::StateSet* const key, const Material& material)
    {
        const Index index = mScene.addMaterial(material);
        ++mPass.getStats().mMaterialsAdded;

        return mMaterials.add(key, HeldMaterial{ { .mIndex = index } });
    }

    void MaterialResolver::releaseWorn(const Worn& worn)
    {
        for (std::size_t at = 0; at < worn.mCount; ++at)
        {
            const auto held = mTextureOf.find(worn.mImages[at]);
            contract(held != mTextureOf.end(), "a worn image the mirror does not hold");
            mTextureOf.drop(held);
        }
    }

    MaterialResolver::Resolved MaterialResolver::resolveWater()
    {
        // One material for the sea, identified by the state set it has not got: water has no
        // albedo, `MWRender::Water` swaps its node's state set between two copies every frame, and
        // with `water shader = true` there is no state set on the node at all. In the map under
        // `sSea` rather than beside it, so that one sweep and one count answer for every material.
        if (const Entry held = reuse(sSea); held != mMaterials.end())
            return Resolved{ .mIndex = held->second.mIndex, .mKey = sSea };

        return Resolved{ .mIndex = adopt(sSea, Material{ .mKind = MaterialKind::Water })->second.mIndex, .mKey = sSea };
    }

    MaterialReading MaterialResolver::read(std::span<const Shading> shading, AlphaScratch& scratch, MeanTexels& means)
    {
        if (shading.empty())
            return MaterialReading{};

        MaterialReading reading{ .mKey = shading.back().mStateSet };
        if (!describeSurface(shading, reading.mDescribed.emplace()))
        {
            reading.mDescribed.reset();
            return reading;
        }

        // Off the description the material is copied from, so the reader walks the texels of
        // exactly the images `describe` would.
        const SurfaceDescription& described = *reading.mDescribed;
        const bool translucent = translucentSurface(described.mAlphaMode, described.mOpacity, described.mBlend);
        const bool additive = additiveSurface(described.mAlphaMode, described.mBlend);
        const osg::Image* const diffuse = described.getTexture(TextureRole::Diffuse);

        if (diffuse != nullptr && !diffuse->getFileName().empty())
        {
            if (translucent)
                reading.mDiffuseSolid = reachesSolid(*diffuse, scratch);
            if (additive)
                reading.mDiffuseMean = meanUnder(means.of(*diffuse), described.mBlend);
        }

        return reading;
    }

    Index MaterialResolver::adopt(const MaterialReading& reading)
    {
        if (reading.mKey == nullptr)
            return sNoIndex;

        Entry known = reuse(reading.mKey);
        if (known == mMaterials.end())
            known = adopt(reading.mKey, describe(reading, false, nullptr));

        mMaterials.hold(known);
        return known->second.mIndex;
    }

    void MaterialResolver::release(const osg::StateSet* const key)
    {
        if (key == nullptr)
            return;

        const auto known = mMaterials.find(key);
        contract(known != mMaterials.end(), "a material released that the mirror does not hold");
        mMaterials.drop(known);
    }

    MaterialResolver::Resolved MaterialResolver::resolve(std::span<const Shading> shading)
    {
        if (shading.empty())
            return Resolved{};

        // The material's identity is the state set nearest the drawable. Two drawables that share
        // it share their shading: OpenMW's optimizer collapses equivalent state sets into one
        // object, so sharing the pointer means sharing the values, and what the parents above
        // contribute in this graph is light and render-bin state rather than material.
        const Shading& own = shading.back();

        if (const Entry known = reuse(own.mStateSet); known != mMaterials.end())
        {
            // Read again, because a controller rewrote it since the last frame. The state set
            // is the same object — that is what lets the material keep its slot and every placement
            // standing on it stay where it is — and everything inside it is this frame's. What it
            // wears is kept across the frames, for the reason `Worn` gives.
            if (own.mAnimated)
            {
                HeldMaterial& held = known->second;
                if (!held.mWorn.has_value())
                    held.mWorn.emplace();
                mScene.setMaterial(held.mIndex, readMaterial(shading, &*held.mWorn));
            }

            return Resolved{ .mIndex = known->second.mIndex, .mKey = own.mStateSet };
        }

        // An arrival under a controller starts wearing what it wears from its first frame.
        if (!own.mAnimated)
            return Resolved{ .mIndex = adopt(own.mStateSet, readMaterial(shading, nullptr))->second.mIndex,
                .mKey = own.mStateSet };

        Worn worn;
        const Material material = readMaterial(shading, &worn);
        const Entry added = adopt(own.mStateSet, material);
        added->second.mWorn = worn;

        return Resolved{ .mIndex = added->second.mIndex, .mKey = own.mStateSet };
    }

    Index MaterialResolver::takeTexture(const TextureUse& use, Worn* const worn)
    {
        ExtractionStats& stats = mPass.getStats();

        const osg::Image* const image = use.get();
        if (image == nullptr || image->getFileName().empty())
            return sNoIndex;

        // Outside the cache, because what this counts is what the walk met and not what it
        // added. `openmw-rtxtool scene` reads these off a second walk of one graph, and a
        // count that only rose on an arrival would report nothing there.
        stats.mFormats.count(*image);

        const auto wrap = static_cast<std::size_t>(use.mWrap);

        auto known = mTextureOf.find(image);
        if (known != mTextureOf.end())
            mTextureOf.stamp(known);
        else
            known = mTextureOf.add(image, HeldTexture{});

        Index& slot = known->second.mSlots[wrap];
        if (slot == sNoIndex)
        {
            slot = mScene.textures().add(VFS::Path::Normalized(image->getFileName()), use.mWrap);

            // Held, because this entry is the reference. `mTextureOf` says why a slot the map names
            // has to be one nothing else can hand out.
            mScene.textures().hold(slot);
        }

        if (worn != nullptr)
        {
            const auto first = worn->mImages.begin();
            const auto last = first + worn->mCount;
            if (std::find(first, last, image) == last)
            {
                if (worn->mCount == Worn::sMost)
                {
                    // The oldest goes, which is the one `mNext` stands on once the ring is full.
                    const auto oldest = mTextureOf.find(worn->mImages[worn->mNext]);
                    contract(oldest != mTextureOf.end(), "a worn image the mirror does not hold");
                    mTextureOf.drop(oldest);
                    ++stats.mWornBeyondKept;
                }
                else
                    ++worn->mCount;

                worn->mImages[worn->mNext] = image;
                worn->mNext = static_cast<std::uint8_t>((worn->mNext + 1) % Worn::sMost);
                mTextureOf.hold(known);
            }
        }

        return slot;
    }

    osg::Vec3f MaterialResolver::diffuseMeanOf(const osg::Image* const image, const BlendKind blend)
    {
        if (image == nullptr)
            return Shaders::NO_TEXTURE_ALBEDO;

        // Asked only of an image `takeTexture` already met, as `diffuseReachesSolid` is; anything
        // else is a sheet whose glow nothing can read, which is drawn as untextured and so lights
        // as untextured.
        const auto known = mTextureOf.find(image);
        if (known == mTextureOf.end())
            return Shaders::NO_TEXTURE_ALBEDO;

        // Kept by the slot for a file, so the frames after the first find it without the name;
        // an unnamed image is asked of the cache every time, which reads it every time.
        const MeanTexel*& mean = known->second.mMean;
        if (mean == nullptr)
        {
            const MeanTexel& read = mMeans.of(*image);
            if (image->getFileName().empty())
                return meanUnder(read, blend);
            mean = &read;
        }

        return meanUnder(*mean, blend);
    }

    bool MaterialResolver::diffuseReachesSolid(const osg::Image* const image)
    {
        if (image == nullptr)
            return true;

        // Asked only of an image `takeTexture` already met, which is the only way a material
        // can come to name one. Anything else is a texture this cannot answer for, and the answer
        // that leaves the surface traced exactly as it was is that it reaches solid.
        const auto known = mTextureOf.find(image);
        if (known == mTextureOf.end())
            return true;

        std::optional<bool>& solid = known->second.mSolid;
        if (!solid.has_value())
            solid = reachesSolid(*image, mAlphaScratch);

        return *solid;
    }

    Material MaterialResolver::readMaterial(std::span<const Shading> shading, Worn* const worn)
    {
        const bool animated = !shading.empty() && shading.back().mAnimated;

        // Nothing read ahead: the answers about the map are asked of the images here, where the
        // walk is, and kept against the next material that names them.
        MaterialReading reading;
        if (!describeSurface(shading, reading.mDescribed.emplace()))
            reading.mDescribed.reset();

        return describe(reading, animated, worn);
    }

    Material MaterialResolver::describe(const MaterialReading& reading, const bool animated, Worn* const worn)
    {
        ExtractionStats& stats = mPass.getStats();

        Material material;

        // Before the description, because a surface nothing described is still one a controller
        // rewrites: what the flag states is a fact about the state set and not about what is in it.
        material.mAnimated = animated;

        if (!reading.mDescribed.has_value())
        {
            ++stats.mUndescribedSurfaces;
            return material;
        }

        const SurfaceDescription* const described = &*reading.mDescribed;

        // Kept, because the medium test below asks about the same image and asking the description
        // twice for it is asking twice.
        const osg::Image* const diffuse = described->getTexture(TextureRole::Diffuse);

        material.mDiffuse = takeTexture(described->getTextureUse(TextureRole::Diffuse), worn);
        material.mEmissive = takeTexture(described->getTextureUse(TextureRole::Emissive), worn);
        material.mEnvironment = takeTexture(described->getTextureUse(TextureRole::Environment), worn);
        material.mEnvironmentColour = decodeColour(described->mEnvironmentColour);
        material.mDark = takeTexture(described->getTextureUse(TextureRole::Dark), worn);
        material.mDarkUnit = described->mDarkUnit;

        material.mAlphaRef = described->mAlphaRef;
        material.mAlphaMode = described->mAlphaMode;
        material.mBlend = described->mBlend;
        material.mVertexColour = described->mVertexColour;

        material.mTwoSided = described->mTwoSided;
        material.mOpacity = described->mOpacity;

        // Decoded here, because this is where the game's numbers enter the trace. A record's
        // colour is written in the space the artist saw and everything past this is light.
        material.mDiffuseColour = decodeColour(described->mDiffuseColour);

        // The multiplier is applied past the decode: it is a gain on the light and not a colour of
        // its own. Folded in because the game's own shader only ever uses their product.
        material.mEmissiveColour = decodeColour(described->mEmissiveColour) * described->mEmissiveMult;

        // The ambient the game overrides joins the glow, which is where the rasterizer's own sum
        // puts it: `objects.frag` adds `ambientColor * ambientLight` beside the emission and
        // multiplies the whole by the texture, and a white override makes that the material's
        // ambient colour whole. A lighting term and not a colour, so it takes the glow's scale.
        if (described->mAmbientOverride.has_value())
        {
            const osg::Vec3f overridden = decodeColour(*described->mAmbientOverride);
            const osg::Vec3f ambient = decodeColour(described->mAmbientColour);
            material.mEmissiveColour
                += osg::Vec3f(overridden.x() * ambient.x(), overridden.y() * ambient.y(), overridden.z() * ambient.z());
        }

        // Scaled about the middle of the texture, then offset, which is what `NifOsg` builds its
        // texture matrix from — so `(uv - 0.5) * scale + 0.5 + offset`, resolved here into the
        // `uv * xy + zw` the sampler takes. Doing the arithmetic once on the host keeps two
        // multiplies and an add out of every texture fetch in the frame.
        const osg::Vec2f scale = described->mTextureScale;
        const osg::Vec2f offset = described->mTextureOffset;
        material.mTextureTransform = osg::Vec4f(
            scale.x(), scale.y(), 0.5f * (1.0f - scale.x()) + offset.x(), 0.5f * (1.0f - scale.y()) + offset.y());

        // Last, and only for the surfaces the answer separates. Every field the tests read is
        // filled above, and the walk over a texture's texels is worth nothing to a material that is
        // opaque, masked, or has no diffuse map to read — `Material::isMedium` is the other half,
        // and a glow is asked of an additive sheet alone. The reading's answer where one was made,
        // and the walk over the texels only where none was: `value_or` would take the walk whatever
        // the reading said.
        if (material.isTranslucent() && material.mDiffuse != sNoIndex)
            material.mDiffuseNeverSolid
                = !(reading.mDiffuseSolid.has_value() ? *reading.mDiffuseSolid : diffuseReachesSolid(diffuse));

        if (material.isAdditive() && material.mDiffuse != sNoIndex)
            material.mDiffuseMean
                = reading.mDiffuseMean.has_value() ? *reading.mDiffuseMean : diffuseMeanOf(diffuse, material.mBlend);

        return material;
    }

    void MaterialResolver::retire(std::vector<Index>& live)
    {
        mMaterials.sweep(live, [this](const HeldMaterial& held) {
            if (held.mWorn.has_value())
                releaseWorn(*held.mWorn);
        });
    }

    void MaterialResolver::retireHolds()
    {
        // The walk's own hold on every image a material is read from, given back the same way.
        // Most are met once and go stale on the frame after they arrived; what settles here is the
        // animated materials.
        mTextureOf.retire([this](const HeldTexture& held) {
            for (const Index slot : held.mSlots)
                mScene.textures().drop(slot);
        });

        // What `animate` keeps. Swept beside everything else because it is keyed on a node the graph
        // can drop, and because a state set held past its node holds the textures in it alive too.
        mAnimated.retire();
    }
}
