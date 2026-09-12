#include "materialresolver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <osg/Image>
#include <osg/StateSet>

#include <components/sceneutil/statesetupdater.hpp>
#include <components/surface/material.hpp>
#include <components/vfs/pathutil.hpp>

#include "alphaimage.hpp"
#include "decodecolour.hpp"
#include "extractionstats.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"

namespace Rtx
{
    namespace
    {
        /// What the sea's material is keyed on: the state set it has not got. See `resolveWater`.
        ///
        /// **Nothing else in the world can key as null.** A shading chain's entries come from
        /// `MirrorTraversal::pushShading`, which takes a reference.
        constexpr const osg::StateSet* sSea = nullptr;

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

        // **The casts are taken when the chains change, not per frame.** The entry remembers what it
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
            held.mStateSet = base != nullptr ? new osg::StateSet(*base, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
            updater->setDefaults(held.mStateSet);
        }

        updater->apply(held.mStateSet, visitor);
        return held.mStateSet;
    }

    Index MaterialResolver::reuse(const osg::StateSet* const key)
    {
        const auto known = mMaterials.find(key);
        if (known == mMaterials.end())
            return sNoIndex;

        ++mPass.getStats().mMaterialsReused;
        mMaterials.stamp(known);

        return known->second.mIndex;
    }

    MaterialResolver::Entry MaterialResolver::adopt(const osg::StateSet* const key, const Material& material)
    {
        const Index index = mScene.materials().add(material);
        ++mPass.getStats().mMaterialsAdded;

        return mMaterials.add(key, Known{ .mIndex = index });
    }

    MaterialResolver::Resolved MaterialResolver::resolveWater()
    {
        // **One material for the sea, and what identifies it is the state set it has not got.**
        // Water has no albedo — what it looks like is what is behind and above it, worked out from
        // the world position — so there is nothing on a state set worth reading, and reading one is
        // actively wrong twice over.
        //
        // `MWRender::Water` animates its surface with a `SceneUtil::StateSetUpdater`, which swaps
        // the node's state set between two copies of its own every frame: keyed on the address, the
        // mirror saw a new material each frame and swept the one before it, for a surface that had
        // not changed. And with `water shader = true` there is no state set on the node at all,
        // because that one is pushed from a cull callback the mirror runs outside of.
        //
        // **In the map under `sSea` rather than beside it**, so that one sweep and one count answer
        // for every material the walk met. A slot held outside them is a slot the survivor list has
        // to be told about by hand.
        if (const Index held = reuse(sSea); held != sNoIndex)
            return Resolved{ .mIndex = held, .mKey = sSea };

        return Resolved{ .mIndex = adopt(sSea, Material{ .mKind = MaterialKind::Water })->second.mIndex, .mKey = sSea };
    }

    MaterialReading MaterialResolver::read(std::span<const Shading> shading, AlphaScratch& scratch)
    {
        if (shading.empty())
            return MaterialReading{};

        MaterialReading reading{ .mKey = shading.back().mStateSet, .mDescribed = findDescription(shading) };
        if (reading.mDescribed == nullptr)
            return reading;

        // **The same two facts `Material::isTranslucent` reads**, off the description they are
        // copied from, so the reader walks the texels of exactly the images `describe` would.
        const Surface::Material& described = *reading.mDescribed;
        const bool translucent = described.mAlphaMode == Surface::AlphaMode::Blend && described.mOpacity < 1.0f;
        const osg::Image* const diffuse = described.getTexture(Surface::TextureRole::Diffuse);

        if (translucent && diffuse != nullptr && !diffuse->getFileName().empty())
            reading.mDiffuseSolid = reachesSolid(*diffuse, scratch);

        return reading;
    }

    Index MaterialResolver::adopt(const MaterialReading& reading)
    {
        if (reading.mKey == nullptr)
            return sNoIndex;

        auto known = mMaterials.find(reading.mKey);
        if (known != mMaterials.end())
        {
            ++mPass.getStats().mMaterialsReused;
            mMaterials.stamp(known);
        }
        else
            known = adopt(reading.mKey, describe(reading.mDescribed, false, reading.mDiffuseSolid));

        mMaterials.hold(known);
        return known->second.mIndex;
    }

    void MaterialResolver::release(const osg::StateSet* const key)
    {
        if (key == nullptr)
            return;

        const auto known = mMaterials.find(key);
        assert(known != mMaterials.end() && "a material released that the mirror does not hold");
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

        if (const Index held = reuse(own.mStateSet); held != sNoIndex)
        {
            // **Read again, because a controller rewrote it since the last frame.** The state set
            // is the same object — that is what lets the material keep its slot and every placement
            // standing on it stay where it is — and everything inside it is this frame's.
            if (own.mAnimated)
                mScene.setMaterial(held, readMaterial(shading));

            return Resolved{ .mIndex = held, .mKey = own.mStateSet };
        }

        return Resolved{ .mIndex = adopt(own.mStateSet, readMaterial(shading))->second.mIndex, .mKey = own.mStateSet };
    }

    Index MaterialResolver::takeTexture(const osg::Image* image)
    {
        ExtractionStats& stats = mPass.getStats();

        if (image == nullptr || image->getFileName().empty())
            return sNoIndex;

        // **Outside the cache, because what this counts is what the walk met and not what it
        // added.** `openmw-rtxtool scene --twice` reads these off a second walk of one graph, and a
        // count that only rose on an arrival would report nothing there.
        stats.mFormats.count(*image);

        if (const auto known = mTextureOf.find(image); known != mTextureOf.end())
        {
            mTextureOf.stamp(known);
            return known->second.mIndex;
        }

        const Index index = mScene.textures().add(VFS::Path::Normalized(image->getFileName()));

        // **Held, because this entry is the reference.** `mTextureOf` says why a slot the map names
        // has to be one nothing else can hand out.
        mScene.textures().hold(index);
        mTextureOf.add(image, HeldTexture{ { .mIndex = index }, std::nullopt });

        return index;
    }

    bool MaterialResolver::diffuseReachesSolid(const osg::Image* const image)
    {
        if (image == nullptr)
            return true;

        // **Asked only of an image `takeTexture` already met**, which is the only way a material
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

    Material MaterialResolver::readMaterial(std::span<const Shading> shading)
    {
        return describe(findDescription(shading), !shading.empty() && shading.back().mAnimated, std::nullopt);
    }

    Material MaterialResolver::describe(
        const Surface::Material* const described, const bool animated, const std::optional<bool> diffuseSolid)
    {
        ExtractionStats& stats = mPass.getStats();

        Material material;

        // Before the description, because a surface nothing described is still one a controller
        // rewrites: what the flag states is a fact about the state set and not about what is in it.
        material.mAnimated = animated;

        if (described == nullptr)
        {
            ++stats.mUndescribedSurfaces;
            return material;
        }

        // Kept, because the medium test below asks about the same image and asking the description
        // twice for it is asking twice.
        const osg::Image* const diffuse = described->getTexture(Surface::TextureRole::Diffuse);

        material.mDiffuse = takeTexture(diffuse);
        material.mEmissive = takeTexture(described->getTexture(Surface::TextureRole::Emissive));

        // The two normal roles differ in what the alpha channel holds, and parallax is a rasterizer
        // feature this renderer does not have: to a ray tracer they are the same texture.
        material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::Normal));
        if (material.mNormal == sNoIndex)
            material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::NormalHeight));

        material.mAlphaRef = described->mAlphaRef;
        material.mAlphaMode = described->mAlphaMode;
        material.mVertexColour = described->mVertexColour;

        material.mTwoSided = described->mTwoSided;
        material.mOpacity = described->mOpacity;

        // **Decoded here, because this is where the game's numbers enter the trace.** A record's
        // colour is written in the space the artist saw and everything past this is light.
        material.mDiffuseColour = decodeColour(described->mDiffuseColour);

        // The multiplier is applied past the decode: it is a gain on the light and not a colour of
        // its own. Folded in because the game's own shader only ever uses their product.
        material.mEmissiveColour = decodeColour(described->mEmissiveColour) * described->mEmissiveMult;

        // **Scaled about the middle of the texture, then offset**, which is what `NifOsg` builds its
        // texture matrix from — so `(uv - 0.5) * scale + 0.5 + offset`, resolved here into the
        // `uv * xy + zw` the sampler takes. Doing the arithmetic once on the host keeps two
        // multiplies and an add out of every texture fetch in the frame.
        const osg::Vec2f scale = described->mTextureScale;
        const osg::Vec2f offset = described->mTextureOffset;
        material.mTextureTransform = osg::Vec4f(
            scale.x(), scale.y(), 0.5f * (1.0f - scale.x()) + offset.x(), 0.5f * (1.0f - scale.y()) + offset.y());

        // **Last, and only for the surfaces the answer separates.** Every field the test reads is
        // filled above, and the walk over a texture's texels is worth nothing to a material that is
        // opaque, masked, or has no diffuse map to read — `Material::isMedium` is the other half.
        if (material.isTranslucent() && material.mDiffuse != sNoIndex)
            material.mDiffuseNeverSolid = !diffuseSolid.value_or(diffuseReachesSolid(diffuse));

        return material;
    }

    void MaterialResolver::retire(std::vector<Index>& live)
    {
        mMaterials.sweep(live);
    }

    void MaterialResolver::retireHolds()
    {
        // The walk's own hold on every image a material is read from, given back the same way.
        //
        // **Most of these are met once.** A material a controller does not rewrite is resolved from
        // its cached entry and never read again, so the images behind it go stale on the frame after
        // they arrived — and the material's own reference is what keeps their slots. What settles
        // here is the animated materials, which are the ones the map exists for.
        mTextureOf.retire([this](const HeldTexture& held) { mScene.textures().drop(held.mIndex); });

        // What `animate` keeps. Swept beside everything else because it is keyed on a node the graph
        // can drop, and because a state set held past its node holds the textures in it alive too.
        mAnimated.retire();
    }
}
