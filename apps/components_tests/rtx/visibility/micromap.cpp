#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// A red mask sixteen texels square in four quadrants, the first and the last opaque and
        /// the other two holes, at one level — so the cone's mip and the micromap's level nought
        /// are one level, and the two ways of reading the mask can be asked to agree exactly.
        struct CheckerTexture
        {
            static constexpr std::uint32_t sExtent = 16;

            std::vector<std::uint8_t> mBytes;
            MipLevel mLevel{ 0, sExtent, sExtent };
            TextureData mData;

            CheckerTexture()
                : mBytes(std::size_t{ sExtent } * sExtent * 4, 0)
            {
                for (std::uint32_t y = 0; y < sExtent; ++y)
                    for (std::uint32_t x = 0; x < sExtent; ++x)
                    {
                        std::uint8_t* const texel = &mBytes[(std::size_t{ y } * sExtent + x) * 4];
                        texel[0] = 255;
                        texel[3] = (x < sExtent / 2) == (y < sExtent / 2) ? 255 : 0;
                    }

                mData = TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = sExtent,
                    .mHeight = sExtent,
                    .mBytes = std::as_bytes(std::span(mBytes)),
                    .mLevels = std::span(&mLevel, 1),
                    .mName = "checker",
                };
            }
        };

        /// The card of the cutout test: a hundred units from the eye, exactly filling a
        /// sixty-degree frame, so that each quadrant of the mask is a quadrant of the picture and
        /// the seams fall between pixel columns and rows rather than on them.
        constexpr float sHalfExtent = 57.735027f;
        const std::array<osg::Vec3f, 4> sCard{
            osg::Vec3f(-sHalfExtent, -50.0f, -sHalfExtent),
            osg::Vec3f(sHalfExtent, -50.0f, -sHalfExtent),
            osg::Vec3f(sHalfExtent, -50.0f, sHalfExtent),
            osg::Vec3f(-sHalfExtent, -50.0f, sHalfExtent),
        };

        constexpr std::uint32_t sSize = 64;

        /// What one frame of the card came to, read every way a test wants to compare two.
        struct Picture
        {
            std::vector<float> mRadiance;
            std::vector<float> mDepth;
            std::uint32_t mHits = 0;
            SceneStats mStats;
        };

        class RtxMicromapPictureTest : public RtxVisibilityTest
        {
        protected:
            /// The card's material: the checker as a cutout at the half, animated where asked —
            /// which is what refuses it a micromap and sends every candidate to the any-hit.
            static Index addCutout(SceneDesc& scene, bool animated)
            {
                return scene.addMaterial(Material{
                    .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("checker.dds")),
                    .mAlphaRef = 0.5f,
                    .mAlphaMode = AlphaMode::Cutout,
                    .mAnimated = animated,
                });
            }

            /// The eye a hundred and fifty units off the wall, looking at it through the card,
            /// under a sun from the upper left of the eye's own side — so a leaf's shadow lands on
            /// the wall under a hole, where the eye can see it — and a black sky, so that nothing a
            /// sky ray answers is multiplied by anything.
            static Shaders::VisibilityConstants lookAtTheCard()
            {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -150.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, sSize, sSize, 10000.0f);

                osg::Vec3f sun(-0.5f, -1.0f, 0.5f);
                sun.normalize();
                camera.mSunPosition = sun;
                camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();

                return camera;
            }

            /// Renders `scene` once and reads back everything two pictures are compared on.
            Picture take(const SceneDesc& scene)
            {
                Picture picture;

                std::vector<std::uint8_t> pixels;
                picture.mHits = countHits(scene, std::span(&mChecker.mData, 1), lookAtTheCard(), sSize, pixels);
                mRenderer->readChannel(Channel::Radiance, picture.mRadiance);
                mRenderer->readChannel(Channel::Depth, picture.mDepth);
                picture.mStats = mRenderer->getSceneStats();

                return picture;
            }

            /// The distance from the eye at pixel `(x, y)`, which is the depth channel's second
            /// value.
            static float distanceAt(const Picture& picture, std::uint32_t x, std::uint32_t y)
            {
                return picture.mDepth[(std::size_t{ y } * sSize + x) * 2 + 1];
            }

            CheckerTexture mChecker;
        };

        /// A card with a checker mask in front of a wall, traced over its micromap, comes out
        /// exactly as it does through the any-hit.
        ///
        /// **The whole picture, and not a pixel of it.** Every microtriangle the bake decided is
        /// resolved before any shader runs, and `micromap.h` says the decision is exact against the
        /// finest level — which this mask has one of — so the eye's ray, the bounce and every shadow
        /// ray must land where they landed when the any-hit was asked about every candidate. The
        /// animated variant is the same scene with the bake refused, and the two frames have to be
        /// the same bytes.
        TEST_F(RtxMicromapPictureTest, aMicromappedCardIsTracedExactlyAsTheAnyHitTracesIt)
        {
            const auto makeScene = [](bool animated) {
                SceneDesc scene = makeWall();
                const Index cutout = addCutout(scene, animated);
                const Index card = scene.addMesh(sCard, {}, sQuadUv, sQuadIndices, {}, Deform::None, sNoIndex, cutout);
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = card, .mMaterial = cutout });
                return scene;
            };

            const Picture baked = take(makeScene(false));
            const Picture asked = take(makeScene(true));

            // Something is behind every hole, so every ray lands on one surface or the other.
            EXPECT_EQ(baked.mHits, sSize * sSize);
            EXPECT_EQ(asked.mHits, sSize * sSize);

            EXPECT_EQ(baked.mStats.mCutoutInstances, 1u);
            EXPECT_EQ(baked.mStats.mMicromappedInstances, 1u);
            EXPECT_GT(baked.mStats.mMicromapBytes, 0u);
            EXPECT_EQ(baked.mStats.mMicromapsUntextured, 0u);
            EXPECT_EQ(asked.mStats.mCutoutInstances, 1u);
            EXPECT_EQ(asked.mStats.mMicromappedInstances, 0u) << "an animated mask cannot be baked";

            EXPECT_EQ(baked.mDepth, asked.mDepth);
            EXPECT_EQ(baked.mRadiance, asked.mRadiance);

            // And the picture is the checker: the quadrants on one diagonal stop the ray at the
            // card and the other two let it through to the wall, which is half again as far. The
            // pixels compared sit at the same angle off the axis — the centre of pixel sixteen is
            // fifteen and a half pixels off it, as pixel forty-seven's is — so the secant is the
            // same and the distances are in the ratio of the depths.
            const float first = distanceAt(baked, 16, 16);
            const float second = distanceAt(baked, 47, 16);
            EXPECT_NEAR(distanceAt(baked, 47, 47), first, 1e-3f) << "the diagonal quadrants match";
            EXPECT_NEAR(distanceAt(baked, 16, 47), second, 1e-3f);
            EXPECT_NEAR(std::max(first, second) / std::min(first, second), 1.5f, 1e-3f)
                << "one quadrant is the card at a hundred and the other the wall at a hundred and fifty";
        }

        /// A placement the game is fading keeps its micromap, reads its mask through the any-hit
        /// for as long as it fades, and is traced over the micromap again when the fade ends.
        ///
        /// A faded leaf dims a shadow ray and lets the eye see the wall through it, neither of
        /// which an opaque microtriangle committing without an any-hit would do — so the row
        /// switches the micromap off, and the frame has to be the bytes the any-hit gives. Nothing
        /// is rebuilt: the same structure counts as micromapped again once the fade is gone.
        TEST_F(RtxMicromapPictureTest, aFadingPlacementReadsItsMaskThroughTheAnyHitAndKeepsItsMicromap)
        {
            const auto makeScene = [](bool animated, float fade) {
                SceneDesc scene = makeWall();
                const Index cutout = addCutout(scene, animated);
                const Index card = scene.addMesh(sCard, {}, sQuadUv, sQuadIndices, {}, Deform::None, sNoIndex, cutout);
                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::identity(), .mMesh = card, .mMaterial = cutout, .mOpacity = fade });
                return scene;
            };

            const Picture whole = take(makeScene(false, 1.0f));
            const Picture askedFaded = take(makeScene(true, 0.5f));

            SceneDesc fading = makeScene(false, 0.5f);
            const Picture bakedFaded = take(fading);

            EXPECT_EQ(bakedFaded.mRadiance, askedFaded.mRadiance);
            EXPECT_NE(bakedFaded.mRadiance, whole.mRadiance) << "a fade changes the picture";

            // A fading row is translucent and not a cutout, and its micromap is off — but held.
            EXPECT_EQ(bakedFaded.mStats.mCutoutInstances, 0u);
            EXPECT_EQ(bakedFaded.mStats.mMicromappedInstances, 0u);
            EXPECT_EQ(bakedFaded.mStats.mMicromapBytes, whole.mStats.mMicromapBytes);

            // The fade ends, as it does in the game: the same placement, its row rewritten. The card
            // is the last placement; the wall is the first.
            fading.clearPlacement();
            fading.fadeInstance(static_cast<Index>(fading.getInstances().size() - 1), 1.0f);
            mRenderer->placeScene(Rtx::sWorld, fading, SeaState{});

            EXPECT_EQ(mRenderer->getSceneStats().mMicromappedInstances, 1u) << "the micromap was kept";
            EXPECT_EQ(mRenderer->getSceneStats().mCutoutInstances, 1u);

            mRenderer->renderFrame(lookAtTheCard(), FrameOptions{ .mExposure = 1.0f });
            EXPECT_EQ(mRenderer->finishFrame().value().mHits, sSize * sSize);

            std::vector<float> radiance;
            mRenderer->readChannel(Channel::Radiance, radiance);
            EXPECT_EQ(radiance, whole.mRadiance) << "the frame after the fade is the frame before it";
        }

        /// A mask whose finest level is opaque and whose every coarser level is a hole, at a
        /// distance the eye's cone reads two levels down.
        ///
        /// **What tells a micromap that is read from one that is ignored.** The checker above comes
        /// out the same either way, which is the promise; this comes out differently, which is the
        /// proof. The any-hit reads the level the cone can resolve, and finds a hole there. The
        /// micromap is a level-nought answer and finds the leaf — and its answer stands over the row's
        /// forced non-opaque bit, or nothing here would have changed at all. `micromap.h` says why
        /// the level-nought answer is the truer one under accumulation.
        struct HoleyChainTexture
        {
            static constexpr std::uint32_t sExtent = 256;

            std::vector<std::uint8_t> mBytes;
            std::vector<MipLevel> mLevels;
            TextureData mData;

            HoleyChainTexture()
            {
                for (std::uint32_t side = sExtent, level = 0; side >= 1; side /= 2, ++level)
                {
                    mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(mBytes.size()), side, side });
                    for (std::uint32_t texel = 0; texel < side * side; ++texel)
                    {
                        mBytes.push_back(255);
                        mBytes.push_back(0);
                        mBytes.push_back(0);
                        mBytes.push_back(level == 0 ? 255 : 0);
                    }
                }

                mData = TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = sExtent,
                    .mHeight = sExtent,
                    .mBytes = std::as_bytes(std::span(mBytes)),
                    .mLevels = mLevels,
                    .mName = "holey chain",
                };
            }
        };

        TEST_F(RtxMicromapPictureTest, aMicromapAnswersAtTheFinestLevelWhereTheConeWouldReadACoarserOne)
        {
            const HoleyChainTexture chain;

            const auto makeScene = [](bool animated) {
                SceneDesc scene = makeWall();
                const Index cutout = addCutout(scene, animated);
                const Index card = scene.addMesh(sCard, {}, sQuadUv, sQuadIndices, {}, Deform::None, sNoIndex, cutout);
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = card, .mMaterial = cutout });
                return scene;
            };

            // Four texels a pixel, so the cone reads level two: a hole everywhere for the any-hit.
            const auto distanceAtTheCentre = [&](bool animated) {
                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(makeScene(animated), std::span(&chain.mData, 1), lookAtTheCard(), sSize, pixels),
                    sSize * sSize);

                std::vector<float> depth;
                mRenderer->readChannel(Channel::Depth, depth);
                return depth[(std::size_t{ sSize / 2 } * sSize + sSize / 2) * 2 + 1];
            };

            const float throughTheAnyHit = distanceAtTheCentre(true);
            const float overTheMicromap = distanceAtTheCentre(false);

            EXPECT_NEAR(throughTheAnyHit / overTheMicromap, 1.5f, 1e-2f)
                << "the any-hit found the wall through a hole at level two, and the micromap the card at level "
                   "nought: "
                << throughTheAnyHit << " against " << overTheMicromap;
        }

        /// A rigged card with a mask is refitted over the micromap it was built with, and the
        /// picture follows its bone.
        ///
        /// **An update must describe what the build did, micromap included**, and the layers say
        /// so where a refit forgets it; what the picture says is that the structure it produced
        /// still cuts the holes out where the bone moved them. With nothing behind the card, the
        /// hits are the opaque quadrants and nothing else: the two of them fill half the frame where
        /// the card was built, and an eighth of it once the bone has walked it twice as far.
        TEST_F(RtxMicromapPictureTest, aRiggedCardIsRefittedOverItsMicromap)
        {
            SceneDesc scene;
            const Index cutout = addCutout(scene, false);
            const Index card
                = scene.addMesh(sCard, {}, sQuadUv, sQuadIndices, {}, Deform::Rig, addOneBoneRig(scene, 4), cutout);
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = card, .mMaterial = cutout });
            poseByOneBone(scene, card, osg::Matrixf::identity());

            const Picture built = take(scene);
            EXPECT_EQ(built.mHits, sSize * sSize / 2) << "two opaque quadrants of a card filling the frame";
            EXPECT_EQ(built.mStats.mMicromappedInstances, 1u);

            // Pixels sixteen and forty-seven sit at the same angle off the axis, and both are inside
            // the card after the move as well: the quadrant that stops the ray is whichever is nearer.
            const float near = distanceAt(built, 16, 16);
            const float far = distanceAt(built, 47, 16);
            const std::uint32_t opaqueX = near < far ? 16 : 47;

            // The bone walks the card a hundred units further off, and the placement is handed
            // back as a frame of the game hands it: cleared, re-walked, placed.
            scene.clearPlacement();
            poseByOneBone(scene, card, osg::Matrixf::translate(0.0f, 100.0f, 0.0f));
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = card, .mMaterial = cutout });
            mRenderer->placeScene(Rtx::sWorld, scene, SeaState{});

            mRenderer->renderFrame(lookAtTheCard(), FrameOptions{ .mExposure = 1.0f });
            EXPECT_EQ(mRenderer->finishFrame().value().mHits, sSize * sSize / 8)
                << "the same two quadrants at twice the distance cover a quarter of the width and the height";

            std::vector<float> depth;
            mRenderer->readChannel(Channel::Depth, depth);

            // The same pixel, the same quadrant of the mask, twice the distance along the same ray.
            const float moved = depth[(std::size_t{ 16 } * sSize + opaqueX) * 2 + 1];
            EXPECT_NEAR(moved / std::min(near, far), 2.0f, 1e-3f) << "the structure followed its vertices";
        }
    }
}
