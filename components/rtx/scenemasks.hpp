#pragma once

#include <span>
#include <vector>

#include <osg/ref_ptr>

#include "scenedesc.hpp"
#include "texturedata.hpp"

namespace osg
{
    class Image;
}

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    /// One mesh an opacity micromap can be built for, and the material its mask comes from.
    struct MicromapCandidate
    {
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;
    };

    /// Which of `meshes` an opacity micromap can be built for.
    ///
    /// A mesh qualifies where it carries geometry, where every placement standing on it names the
    /// *same* material, and where that material is a cutout that is not translucent. A micromap
    /// belongs to the structure and so to the mesh, while a cutout belongs to the material — so a
    /// mesh two materials disagree about has no one answer to give, and the honest reply is none.
    ///
    /// **A translucent surface qualifies for none.** A micromap resolves a microtriangle as opaque
    /// from the same mask it is built out of, and an opaque microtriangle commits the hit — which is
    /// the end of the ray, and a pane of glass with it.
    ///
    /// **One answer, because two readers need it and two copies would drift.** `SceneMasks` opens
    /// the masks a build is about to read and the backend classifies against them; a filter written
    /// on each side is a mask opened for a mesh nobody classifies, or a mesh classified against a
    /// mask nobody opened — and the second of those is invisible, because an unclassified mesh
    /// renders correctly and only costs.
    ///
    /// @param materialOfMesh scratch the caller keeps across calls so a crossing allocates none.
    ///        Refilled here, and its contents are this function's working rather than an answer.
    void micromapCandidates(const SceneDesc& scene, std::span<const Index> meshes, std::vector<Index>& materialOfMesh,
        std::vector<MicromapCandidate>& into);

    /// The cutout masks a set of arriving meshes is classified against.
    ///
    /// **A mesh's micromap depends on the material it wears, and this is what stops it depending on
    /// the upload that carried it.** An extend describes the textures that arrived with it, because
    /// describing one costs a decode and a shading estimate and re-describing three hundred resident
    /// images was 5% of the game's CPU. But the masks an arriving mesh needs are the ones its
    /// material names, and Morrowind shares its images across cells — so a canopy arriving beside an
    /// image the renderer already holds had nothing to classify against, and went on asking
    /// `RTX_RESOLVE` for every candidate until the next reset. Flown across the Bitter Coast that
    /// left 18 of 566 cutout instances micromapped, against 3626 of 3626 on the same content staged
    /// in one go.
    ///
    /// **Described and never uploaded**, which is what makes it cheap: a mask taken from a resident
    /// slot is opened out of the image manager's cache and its levels tabulated, and neither the
    /// shading estimate nor the upload the description would otherwise pay for is done. So these
    /// carry no `TextureData::mShading` and must not reach a texture array.
    ///
    /// Non-copyable because the descriptions point into its own vectors. Moving is fine: a moved
    /// vector keeps the buffer they point at.
    class SceneMasks
    {
    public:
        /// @param meshes the meshes about to be built, which is what decides the masks needed.
        /// @param held descriptions the caller already has, taken as they stand rather than opened
        ///        a second time. They come out among the descriptions, so a classifier has one
        ///        place to look.
        SceneMasks(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> meshes,
            std::span<const TextureData> held);

        SceneMasks(const SceneMasks&) = delete;
        SceneMasks& operator=(const SceneMasks&) = delete;
        SceneMasks(SceneMasks&&) = default;
        SceneMasks& operator=(SceneMasks&&) = default;

        /// Every mask those meshes wear that could be read, each carrying its slot in
        /// `TextureData::mSlot`. A slot whose image could not be opened is absent rather than
        /// standing in: a mask that could not be decoded says nothing about the geometry wearing it,
        /// and deciding on its behalf is how a hole gets put through a wall.
        std::span<const TextureData> getDescriptions() const { return mDescriptions; }

        /// How many of those were opened here rather than taken from `held` — the masks the arrival
        /// rule used to leave behind, as a number.
        std::uint32_t getOpened() const { return mOpened; }

    private:
        std::vector<osg::ref_ptr<const osg::Image>> mImages;
        std::vector<MipLevel> mLevels;
        std::vector<TextureData> mDescriptions;
        std::uint32_t mOpened = 0;
    };
}
