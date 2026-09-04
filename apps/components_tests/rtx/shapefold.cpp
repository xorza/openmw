#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/shapefold.hpp>

namespace Rtx
{
    namespace
    {
        /// A unit quad, and the same four positions again as the vertices its back was modelled
        /// with — which is how the content spells a card: eight vertices, not four.
        const std::array<osg::Vec3f, 8> sCard{
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        };

        const std::vector<std::uint32_t> sFront{ 0, 1, 2, 0, 2, 3 };

        TEST(RtxShapeFoldTest, aCardDoubledForItsBackKeepsTheFrontAndIsASheet)
        {
            ShapeFold fold;

            // The back wound the other way, on the second set of vertices.
            std::vector<std::uint32_t> indices{ 0, 1, 2, 0, 2, 3, 6, 5, 4, 7, 6, 4 };
            EXPECT_TRUE(fold.fold(sCard, indices).mSheet);
            EXPECT_EQ(indices, sFront) << "the copy the file wrote first is the one kept";

            // Folded again there is nothing left to pair, so a sheet is not a sheet twice.
            EXPECT_FALSE(fold.fold(sCard, indices).mSheet);
            EXPECT_EQ(indices, sFront);
        }

        TEST(RtxShapeFoldTest, aTwinIsMatchedByItsCornersAndNotByWhereTheFileStartedIt)
        {
            ShapeFold fold;

            // (0, 1, 2) reversed is (0, 2, 1), which the file may as well spell (2, 1, 0) or
            // (1, 0, 2): every rotation of it is the same back.
            for (const std::array<std::uint32_t, 3> back : { std::array<std::uint32_t, 3>{ 4, 6, 5 },
                     std::array<std::uint32_t, 3>{ 6, 5, 4 }, std::array<std::uint32_t, 3>{ 5, 4, 6 } })
            {
                std::vector<std::uint32_t> indices{ 0, 1, 2, back[0], back[1], back[2] };
                EXPECT_TRUE(fold.fold(sCard, indices).mSheet);
                EXPECT_EQ(indices, (std::vector<std::uint32_t>{ 0, 1, 2 }));
            }

            // And the same triangle again with the same winding is a second front, not a back.
            std::vector<std::uint32_t> twice{ 0, 1, 2, 4, 5, 6 };
            EXPECT_FALSE(fold.fold(sCard, twice).mSheet);
            EXPECT_EQ(twice, (std::vector<std::uint32_t>{ 0, 1, 2, 4, 5, 6 }));
        }

        TEST(RtxShapeFoldTest, aSolidHasNoTwinsAndAMixedShapeLosesOnlyItsTwins)
        {
            ShapeFold fold;

            // A tetrahedron: four faces, no two over the same three corners.
            const std::array<osg::Vec3f, 4> tetra{
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };
            std::vector<std::uint32_t> solid{ 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3 };
            const std::vector<std::uint32_t> before = solid;
            EXPECT_FALSE(fold.fold(tetra, solid).mSheet);
            EXPECT_EQ(solid, before);

            // A doubled card with one lone triangle beside it: the twin goes, the lone one stays,
            // and the shape is not a sheet — a leaf's stem is not lit through.
            std::vector<std::uint32_t> mixed{ 0, 1, 2, 2, 1, 0, 1, 2, 3 };
            EXPECT_FALSE(fold.fold(sCard, mixed).mSheet);
            EXPECT_EQ(mixed, (std::vector<std::uint32_t>{ 0, 1, 2, 1, 2, 3 }));

            std::vector<std::uint32_t> none;
            EXPECT_FALSE(fold.fold(sCard, none).mSheet);
            EXPECT_TRUE(none.empty());
        }

        /// A shape is closed when every edge of what survives the fold carries a triangle each way.
        ///
        /// **The fact that says which of a surface's two normals is lying**, and every case here is
        /// one the content ships. A tetrahedron stands for the solids, and the same tetrahedron with
        /// a face taken off stands for what Morrowind actually models — a rock is a dome with no
        /// base, which is why so little of the game answers yes.
        ///
        /// **A card is open both before and after its twin goes.** Doubled, every edge carries two
        /// triangles the *same* way round the outline and none the other; folded, it is one quad
        /// with a boundary. Neither is a solid, and the difference matters: `mSheet` and `mClosed`
        /// are two facts and a shape may carry both, so neither can be read off the other.
        TEST(RtxShapeFoldTest, aShapeIsClosedWhenEveryEdgeCarriesATriangleEachWay)
        {
            ShapeFold fold;

            const std::array<osg::Vec3f, 4> tetra{
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };

            std::vector<std::uint32_t> solid{ 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3 };
            EXPECT_TRUE(fold.fold(tetra, solid).mClosed);

            // The same solid with one face off, which is the shape of every rock in the game.
            std::vector<std::uint32_t> dome{ 0, 1, 3, 1, 2, 3, 2, 0, 3 };
            EXPECT_FALSE(fold.fold(tetra, dome).mClosed);

            // A single quad: three of its four edges carry one triangle, and the shared diagonal
            // carries two — but both the same way.
            std::vector<std::uint32_t> quad = sFront;
            EXPECT_FALSE(fold.fold(sCard, quad).mClosed);

            // And the doubled card the fold reduces to that quad.
            std::vector<std::uint32_t> card{ 0, 1, 2, 0, 2, 3, 6, 5, 4, 7, 6, 4 };
            const FoldedShape folded = fold.fold(sCard, card);
            EXPECT_TRUE(folded.mSheet);
            EXPECT_FALSE(folded.mClosed);

            // A tetrahedron doubled inside out is both at once, which is what stops either fact
            // being read off the other. No shipped shape is, but two exteriors hold one each.
            std::vector<std::uint32_t> twinned{ 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3, 0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3,
                0 };
            const FoldedShape both = fold.fold(tetra, twinned);
            EXPECT_TRUE(both.mSheet);
            EXPECT_TRUE(both.mClosed);

            std::vector<std::uint32_t> none;
            EXPECT_FALSE(fold.fold(sCard, none).mClosed);
        }

        /// The rule, written again the slow obvious way, for the cross-check below.
        ///
        /// **Independent of the real one on purpose.** It compares every triangle against every
        /// other rather than looking a spelling up, and it rotates corners with a sort rather than
        /// with an index — so the two agree only where the rule they share is the rule, and not
        /// because they share a mistake.
        struct Reference
        {
            static std::array<osg::Vec3f, 3> spelling(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c)
            {
                const auto lower = [](const osg::Vec3f& l, const osg::Vec3f& r) {
                    return std::make_tuple(l.x(), l.y(), l.z()) < std::make_tuple(r.x(), r.y(), r.z());
                };

                std::array<osg::Vec3f, 3> rotated{ a, b, c };
                for (int turn = 0; turn < 2; ++turn)
                    if (lower(rotated[1], rotated[0]) || lower(rotated[2], rotated[0]))
                        rotated = { rotated[1], rotated[2], rotated[0] };

                return rotated;
            }

            /// The indices that survive, and whether every triangle was one of a pair.
            static std::pair<std::vector<std::uint32_t>, bool> fold(
                std::span<const osg::Vec3f> positions, const std::vector<std::uint32_t>& indices)
            {
                const std::size_t count = indices.size() / 3;

                const auto corners = [&](std::size_t t, bool reversed) {
                    return spelling(positions[indices[3 * t]], positions[indices[3 * t + (reversed ? 2 : 1)]],
                        positions[indices[3 * t + (reversed ? 1 : 2)]]);
                };

                enum class Fate
                {
                    Alone,
                    Kept,
                    Dropped
                };
                std::vector<Fate> fates(count, Fate::Alone);

                for (std::size_t t = 0; t < count; ++t)
                {
                    if (fates[t] != Fate::Alone)
                        continue;

                    for (std::size_t other = 0; other < count; ++other)
                    {
                        if (other == t || fates[other] != Fate::Alone || corners(other, false) != corners(t, true))
                            continue;

                        fates[t] = Fate::Kept;
                        fates[other] = Fate::Dropped;
                        break;
                    }
                }

                std::vector<std::uint32_t> kept;
                bool sheet = count > 0;
                for (std::size_t t = 0; t < count; ++t)
                {
                    if (fates[t] == Fate::Dropped)
                        continue;
                    if (fates[t] == Fate::Alone)
                        sheet = false;

                    kept.insert(kept.end(), indices.begin() + static_cast<std::ptrdiff_t>(3 * t),
                        indices.begin() + static_cast<std::ptrdiff_t>(3 * t + 3));
                }

                return { kept, sheet };
            }
        };

        /// Every shape the content can hand it, against the rule written the slow way.
        ///
        /// **What the three cases above cannot reach.** A doubled card is two triangles and the
        /// answer is obvious; a merged paging chunk is tens of thousands, with the same corner
        /// spelled by triangles far apart in the list, triangles doubled three and four times over,
        /// and degenerate ones whose reverse is their own spelling. Which copy survives depends on
        /// the order the pairing walks, and getting that wrong deletes geometry the player can see.
        ///
        /// A fixed seed, so a failure is a failure that can be run again.
        TEST(RtxShapeFoldTest, everyShapeFoldsTheWayTheRuleSaysItShould)
        {
            std::mt19937 random(20260830);
            ShapeFold fold;

            // A small pool of positions, so triangles collide often and the awkward cases happen
            // rather than being hoped for.
            for (const std::size_t corners : { std::size_t{ 3 }, std::size_t{ 5 }, std::size_t{ 12 } })
            {
                std::vector<osg::Vec3f> positions;
                for (std::size_t at = 0; at < corners; ++at)
                    positions.push_back(osg::Vec3f(static_cast<float>(at % 3), static_cast<float>(at / 3), 0.0f));

                for (int attempt = 0; attempt < 200; ++attempt)
                {
                    const std::size_t count = 1 + random() % 24;

                    std::vector<std::uint32_t> indices;
                    for (std::size_t t = 0; t < count; ++t)
                    {
                        const auto corner = [&] { return static_cast<std::uint32_t>(random() % positions.size()); };
                        const std::uint32_t a = corner();
                        const std::uint32_t b = corner();
                        const std::uint32_t c = corner();

                        indices.insert(indices.end(), { a, b, c });

                        // Half of them doubled the way the content doubles a card, so most meshes
                        // here have twins to find rather than being noise.
                        if (random() % 2 == 0)
                            indices.insert(indices.end(), { a, c, b });
                    }

                    const auto [expected, expectedSheet] = Reference::fold(positions, indices);

                    std::vector<std::uint32_t> folded = indices;
                    const bool sheet = fold.fold(positions, folded).mSheet;

                    EXPECT_EQ(folded, expected) << "corners " << corners << ", attempt " << attempt;
                    EXPECT_EQ(sheet, expectedSheet) << "corners " << corners << ", attempt " << attempt;
                }
            }
        }
    }
}
