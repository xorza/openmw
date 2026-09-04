#include "fixture.hpp"

#include <cstddef>
#include <cstdint>

namespace Rtx::Testing
{
    namespace
    {
        /// A surface nothing described is a canary rather than a guess.
        ///
        /// Every state set the content pipeline produces carries a description; one that does not
        /// was built somewhere else, or was rebuilt by something that copied the pipeline state and
        /// dropped the description with it. The extractor says so and does not try to recover it.
        TEST(RtxSceneExtractorTest, anUndescribedSurfaceIsCountedRatherThanGuessedAt)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*quad, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mUndescribedMaterials, 1u);
            EXPECT_EQ(stats.mInstances, 1u) << "the geometry is still placed; only its shading is unknown";
            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mDiffuse, Rtx::sNoIndex);
        }

        /// A texture arrives under the format it was decoded in, and its mip chain is counted beside
        /// it.
        ///
        /// The count is what says whether the content is what the uploader was written for, so a
        /// walk that met a format nobody expected reports it rather than leaving it to a throw.
        TEST(RtxSceneExtractorTest, texturesAreCountedByFormatAndByWhetherTheyBroughtMips)
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

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            const FormatCount& blocks = stats.mTextureFormats[static_cast<std::size_t>(ImageFormat::Bc1)];
            EXPECT_EQ(blocks.mMet, 2u);
            EXPECT_EQ(blocks.mMipped, 1u) << "one of the two brought a chain";
            EXPECT_EQ(stats.mTextureFormats[static_cast<std::size_t>(ImageFormat::Unnamed)].mMet, 0u);
        }

        /// Every count of a walk, each a different number, so a sum short of one is short by an
        /// amount the assertions below name.
        ExtractionStats counted(std::uint32_t from)
        {
            ExtractionStats stats;
            stats.mMeshesAdded = from + 1;
            stats.mMaterialsAdded = from + 2;
            stats.mSheets = from + 3;
            stats.mComposites = from + 4;
            stats.mMeshesReused = from + 5;
            stats.mMaterialsReused = from + 6;
            stats.mInstances = from + 7;
            stats.mDeformed = from + 8;
            stats.mEmitters = from + 9;
            stats.mSprites = from + 10;
            stats.mSkippedUnknown = from + 11;
            stats.mUndescribedMaterials = from + 12;
            stats.mSkippedEmpty = from + 13;
            stats.mLights = from + 14;
            stats.mTextureFormats[static_cast<std::size_t>(ImageFormat::Bc3)]
                = FormatCount{ .mMet = from + 15, .mMipped = from + 16 };
            stats.mUnskinned = from + 17;
            return stats;
        }

        /// Two walks add up count by count, and the report adds the settled walk to the staged one.
        ///
        /// **A count the sum passes over is a number that is quietly short**, which is what a report
        /// of an incremental mirror is least able to survive: it dropped the sheets and the flattened
        /// ground, and both read as zero however much of either a cell held.
        TEST(RtxSceneExtractorTest, twoWalksAddUpCountByCount)
        {
            ExtractionStats sum = counted(0);
            sum += counted(100);

            EXPECT_EQ(sum.mMeshesAdded, 102u);
            EXPECT_EQ(sum.mMaterialsAdded, 104u);
            EXPECT_EQ(sum.mSheets, 106u);
            EXPECT_EQ(sum.mComposites, 108u);
            EXPECT_EQ(sum.mMeshesReused, 110u);
            EXPECT_EQ(sum.mMaterialsReused, 112u);
            EXPECT_EQ(sum.mInstances, 114u);
            EXPECT_EQ(sum.mDeformed, 116u);
            EXPECT_EQ(sum.mEmitters, 118u);
            EXPECT_EQ(sum.mSprites, 120u);
            EXPECT_EQ(sum.mSkippedUnknown, 122u);
            EXPECT_EQ(sum.mUndescribedMaterials, 124u);
            EXPECT_EQ(sum.mSkippedEmpty, 126u);
            EXPECT_EQ(sum.mLights, 128u);
            EXPECT_EQ(sum.mUnskinned, 134u);

            const FormatCount& blocks = sum.mTextureFormats[static_cast<std::size_t>(ImageFormat::Bc3)];
            EXPECT_EQ(blocks.mMet, 130u);
            EXPECT_EQ(blocks.mMipped, 132u);
            EXPECT_EQ(sum.mTextureFormats[static_cast<std::size_t>(ImageFormat::Bc1)].mMet, 0u);
        }

        /// The format an unnamed count stood for survives the sum, since a report that says how many
        /// there were and not which they were sends the reader nowhere.
        TEST(RtxSceneExtractorTest, theUnnamedFormatSurvivesASumWithAWalkThatMetNone)
        {
            ExtractionStats met;
            met.mUnnamedFormat = GL_ALPHA;

            ExtractionStats sum;
            sum += met;
            EXPECT_EQ(sum.mUnnamedFormat, GL_ALPHA);

            sum += ExtractionStats{};
            EXPECT_EQ(sum.mUnnamedFormat, GL_ALPHA) << "a walk that met none says nothing about it";
        }
    }
}
