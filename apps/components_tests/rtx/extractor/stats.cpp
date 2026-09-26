#include <cstddef>
#include <cstdint>

#include <osg/CullFace>
#include <osg/GL>
#include <osg/StateAttribute>
#include <osg/ref_ptr>

#include <components/rtx/formatcensus.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/texturetable.hpp>

#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// A surface nothing described is a canary rather than a guess.
        ///
        /// Every state set the content pipeline produces carries a description; one that does not
        /// was built somewhere else, or was rebuilt by something that copied the pipeline state and
        /// dropped the description with it. The extractor says so and does not try to recover it.
        TEST_F(RtxSceneExtractorTest, anUndescribedSurfaceIsCountedRatherThanGuessedAt)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

            const ExtractionStats stats = walk(*quad);

            EXPECT_EQ(stats.mUndescribedSurfaces, 1u);
            EXPECT_EQ(stats.mInstances, 1u) << "the geometry is still placed; only its shading is unknown";
            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, Rtx::sNoIndex);
        }

        /// A texture arrives under the format it was decoded in, and its mip chain is counted beside
        /// it — and leaves with its slot.
        ///
        /// The count is what says whether the content is what the uploader was written for, so a
        /// scene that stands a format nobody expected reports it rather than leaving it to a throw.
        /// **Kept by the table and not by the walk**, so a second walk over the same graph reads
        /// the same census and a region walked away from is counted out of it.
        TEST_F(RtxSceneExtractorTest, texturesAreCountedByFormatAndByWhetherTheyBroughtMips)
        {
            osg::ref_ptr<osg::Image> chained = new osg::Image;
            chained->setFileName("textures/tx_chained.dds");
            chained->allocateImage(4, 4, 1, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE);

            // One offset is one level past the first, and eight bytes is where a second BC1 block
            // would begin. `getNumMipmapLevels` counts the offsets and reads no further.
            chained->setMipmapLevels(osg::Image::MipmapDataType{ 8 });

            osg::ref_ptr<osg::Image> flat = new osg::Image;
            flat->setFileName("textures/tx_flat.dds");
            flat->allocateImage(4, 4, 1, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            for (const osg::ref_ptr<osg::Image>& image : { chained, flat })
            {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                paint(*quad->getOrCreateStateSet(), *image);
                root->addChild(quad);
            }

            walk(*root);
            mExtractor.retire();

            const FormatCensus& census = mScene.textures().getFormats();
            const FormatCount& blocks = census.mMet[static_cast<std::size_t>(TextureFormat::Bc1RgbaSrgb)];
            EXPECT_EQ(blocks.mMet, 2u);
            EXPECT_EQ(blocks.mMipped, 1u) << "one of the two brought a chain";
            EXPECT_EQ(census.mMet[static_cast<std::size_t>(TextureFormat::Unnamed)].mMet, 0u);

            mScene.clearPlacement();
            walk(*root, 0, 1);
            mExtractor.retire();
            EXPECT_EQ(blocks.mMet, 2u) << "a second walk counted what the first already stood";

            root->removeChildren(0, root->getNumChildren());
            mScene.clearPlacement();
            walk(*root, 0, 2);
            mExtractor.retire();
            EXPECT_EQ(blocks.mMet, 0u) << "a slot freed kept its count";
            EXPECT_EQ(blocks.mMipped, 0u);
        }

        /// Every count of a walk, each a different number, so a sum short of one is short by an
        /// amount the assertions below name.
        ExtractionStats counted(std::uint32_t from)
        {
            ExtractionStats stats;
            stats.mMeshesAdded = from + 1;
            stats.mMaterialsAdded = from + 2;
            stats.mSheets = from + 3;
            stats.mDistantStatics = from + 4;
            stats.mMeshesReused = from + 5;
            stats.mMaterialsReused = from + 6;
            stats.mInstances = from + 7;
            stats.mDeformed = from + 8;
            stats.mEmitters = from + 9;
            stats.mSprites = from + 10;
            stats.mSkippedUnknown = from + 11;
            stats.mUndescribedSurfaces = from + 12;
            stats.mSkippedEmpty = from + 13;
            stats.mLights = from + 14;
            stats.mUnskinned = from + 17;
            stats.mGroundCells = from + 18;
            stats.mWornBeyondKept = from + 21;
            stats.mRestood = from + 22;
            return stats;
        }

        /// Two walks add up count by count, and the report adds the settled walk to the staged one.
        ///
        /// **A count the sum passes over is a number that is quietly short**, which is what a report
        /// of an incremental mirror is least able to survive: it dropped the sheets and the
        /// composites once, and both read as zero however much of either a cell held.
        TEST_F(RtxSceneExtractorTest, twoWalksAddUpCountByCount)
        {
            ExtractionStats sum = counted(0);
            sum += counted(100);

            EXPECT_EQ(sum.mMeshesAdded, 102u);
            EXPECT_EQ(sum.mMaterialsAdded, 104u);
            EXPECT_EQ(sum.mSheets, 106u);
            EXPECT_EQ(sum.mDistantStatics, 108u);
            EXPECT_EQ(sum.mMeshesReused, 110u);
            EXPECT_EQ(sum.mMaterialsReused, 112u);
            EXPECT_EQ(sum.mInstances, 114u);
            EXPECT_EQ(sum.mDeformed, 116u);
            EXPECT_EQ(sum.mEmitters, 118u);
            EXPECT_EQ(sum.mSprites, 120u);
            EXPECT_EQ(sum.mSkippedUnknown, 122u);
            EXPECT_EQ(sum.mUndescribedSurfaces, 124u);
            EXPECT_EQ(sum.mSkippedEmpty, 126u);
            EXPECT_EQ(sum.mLights, 128u);
            EXPECT_EQ(sum.mUnskinned, 134u);
            EXPECT_EQ(sum.mGroundCells, 136u);
            EXPECT_EQ(sum.mWornBeyondKept, 142u);
            EXPECT_EQ(sum.mRestood, 144u);
        }
    }
}
