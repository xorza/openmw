#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/fognoise.hpp>
#include <components/rtx/shaders/scene.h>

namespace Rtx
{
    namespace
    {
        constexpr int sSize = static_cast<int>(Shaders::FOG_FIELD_SIZE);
        constexpr int sLevels = static_cast<int>(Shaders::FOG_FIELD_LEVELS);

        int sizeAt(int level)
        {
            return std::max(sSize >> level, 1);
        }

        /// One texel of one level, as the two numbers the sampler hands back.
        osg::Vec2f texelAt(const FogNoise& noise, int level, int x, int y, int z)
        {
            const int size = sizeAt(level);
            const auto wrap = [size](int v) { return ((v % size) + size) % size; };

            const std::size_t at = noise.mOffsets[static_cast<std::size_t>(level)]
                + 2 * ((static_cast<std::size_t>(wrap(z)) * size + wrap(y)) * size + wrap(x));

            return osg::Vec2f(
                static_cast<float>(noise.mBytes[at]) / 255.0f, static_cast<float>(noise.mBytes[at + 1]) / 255.0f);
        }

        /// What `textureLod` gives at a wrapping coordinate: trilinear between the eight texels the
        /// point falls among, with the texel's own value sitting at its centre.
        osg::Vec2f sampleAt(const FogNoise& noise, int level, const osg::Vec3f& uvw)
        {
            const auto size = static_cast<float>(sizeAt(level));
            const osg::Vec3f grid(uvw.x() * size - 0.5f, uvw.y() * size - 0.5f, uvw.z() * size - 0.5f);

            const int x0 = static_cast<int>(std::floor(grid.x()));
            const int y0 = static_cast<int>(std::floor(grid.y()));
            const int z0 = static_cast<int>(std::floor(grid.z()));

            const osg::Vec3f f(grid.x() - static_cast<float>(x0), grid.y() - static_cast<float>(y0),
                grid.z() - static_cast<float>(z0));

            osg::Vec2f total(0.0f, 0.0f);
            for (int corner = 0; corner < 8; ++corner)
            {
                const int dx = corner & 1;
                const int dy = (corner >> 1) & 1;
                const int dz = (corner >> 2) & 1;

                const float weight = (dx != 0 ? f.x() : 1.0f - f.x()) * (dy != 0 ? f.y() : 1.0f - f.y())
                    * (dz != 0 ? f.z() : 1.0f - f.z());
                total += texelAt(noise, level, x0 + dx, y0 + dy, z0 + dz) * weight;
            }

            return total;
        }

        /// The three scales `fogShape` reads, combined the way it combines them, at a world position.
        ///
        /// **The shader's own arithmetic and not a description of it**, because what this measures is
        /// a property of the number the shader produces.
        ///
        /// @param level which level of the chain to read every scale at. The shader picks it from the
        ///        march's own stride; a test says it outright, so it can ask about a level the shader
        ///        would never choose.
        float shapeAt(const FogNoise& noise, const osg::Vec3f& position, int level = 0)
        {
            const auto read = [&](const osg::Vec3f& at, float tile) {
                return sampleAt(noise, level, osg::Vec3f(at.x() / tile, at.y() / tile, at.z() / tile));
            };

            const osg::Vec2f coarse = read(position, Shaders::FOG_TILE);

            const float drag = Shaders::FOG_WARP / Shaders::FOG_FIELD_SPREAD;
            const osg::Vec3f warped(
                position.x() + (coarse.x() - 0.5f) * drag, position.y() + (coarse.y() - 0.5f) * drag, position.z());

            float total = coarse.x() - 0.5f;
            float squares = 1.0f;
            float amplitude = 1.0f;
            float tile = Shaders::FOG_TILE;

            // The shader's `FOG_TURN`: the identity, then the 3-4-5 and the 5-12-13 triangles.
            constexpr std::array<std::array<float, 2>, 3> turns{ { { 1.0f, 0.0f }, { 0.8f, 0.6f },
                { 0.3846154f, 0.9230769f } } };

            for (std::uint32_t scale = 1; scale < Shaders::FOG_SCALES; ++scale)
            {
                amplitude *= 0.5f;
                tile /= Shaders::FOG_LACUNARITY;

                const auto [c, sn] = turns[scale];
                const osg::Vec3f turned(c * warped.x() - sn * warped.y(), sn * warped.x() + c * warped.y(), warped.z());

                total += amplitude * (read(turned, tile).x() - 0.5f);
                squares += amplitude * amplitude;
            }

            return 0.5f + total / std::sqrt(squares);
        }

        /// Where the `index`-th point of a Halton sequence in `base` falls in `[0, 1)`.
        double radicalInverse(std::uint32_t index, std::uint32_t base)
        {
            double inverse = 0.0;
            double fraction = 1.0 / static_cast<double>(base);

            while (index > 0)
            {
                inverse += static_cast<double>(index % base) * fraction;
                index /= base;
                fraction /= static_cast<double>(base);
            }

            return inverse;
        }

        /// Where to take the `index`-th sample of the field, over a box many tiles across.
        ///
        /// **Low-discrepancy and not a lattice.** The field repeats every `FOG_TILE` units, so a
        /// regular step lands on the same handful of places inside the tile however many samples are
        /// taken — measured, that put the field's own mean 0.09 of a spread off centre and carried
        /// the coverage with it. A Halton sequence covers the tile evenly, and converges faster than
        /// a random draw besides.
        ///
        /// A box and not a plane, because the field has a third axis now and a plane through one
        /// would sample a single slice of it.
        osg::Vec3f haltonAt(std::uint32_t index)
        {
            const float span = 40.0f * Shaders::FOG_TILE;

            return osg::Vec3f(static_cast<float>(radicalInverse(index, 2)) * span,
                static_cast<float>(radicalInverse(index, 3)) * span,
                static_cast<float>(radicalInverse(index, 5)) * span);
        }

        float smoothstep(float from, float to, float value)
        {
            const float t = std::clamp((value - from) / (to - from), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        /// One draw for the whole file. The bake is deterministic and takes long enough that nine
        /// tests each making their own would be the slowest thing in the suite.
        const FogNoise& baked()
        {
            static const FogNoise noise = bakeFogNoise();
            return noise;
        }

        /// Every level a march may read presents the same field to a sampler, and that is what one
        /// coverage band needs.
        ///
        /// **A level is the mean of the eight texels over it, so its own spread narrows going up.**
        /// A band cut against the full level's spread would clear almost nothing at the top of the
        /// chain, and the fog would then thin with distance for a reason nothing in the weather said.
        /// Every level is stretched back about its mean instead, so what a coarse step loses is the
        /// detail and never the amount of air — which is the argument `resolved` makes for a wave
        /// against a ray cone, made once here rather than at every step of every march.
        ///
        /// **Through the sampler and not off the texels**, because the two are not one field: a
        /// trilinear tap hands back values that cluster nearer the mean than the texels it blends,
        /// and the bake stretches for what the tap reads. So this reads the way `textureLod` does,
        /// at points no tap of the bake's own estimate landed on.
        TEST(RtxFogNoiseTest, everyLevelAMarchMayReadCarriesOneMeanAndOneSpread)
        {
            const FogNoise& noise = baked();

            ASSERT_EQ(noise.mOffsets.size(), static_cast<std::size_t>(sLevels));

            constexpr std::uint32_t count = 200000;
            const int cap = static_cast<int>(Shaders::FOG_FIELD_COARSEST);

            for (int level = 0; level <= cap; ++level)
            {
                double total = 0.0;
                double squares = 0.0;
                for (std::uint32_t index = 1; index <= count; ++index)
                {
                    const osg::Vec3f uvw(static_cast<float>(radicalInverse(index, 2)),
                        static_cast<float>(radicalInverse(index, 3)), static_cast<float>(radicalInverse(index, 5)));
                    const osg::Vec2f pair = sampleAt(noise, level, uvw);
                    for (const float value : { pair.x(), pair.y() })
                    {
                        total += double{ value };
                        squares += double{ value } * double{ value };
                    }
                }

                const double mean = total / (2.0 * count);
                const double spread = std::sqrt(squares / (2.0 * count) - mean * mean);

                // Eight bits over a spread of 0.12 is a quantiser step of about a fiftieth of one,
                // and the clipped tail below moves both by less again.
                EXPECT_NEAR(mean, 0.5, 0.002) << "level " << level;
                EXPECT_NEAR(spread, double{ Shaders::FOG_FIELD_SPREAD }, 0.004) << "level " << level;
            }
        }

        /// The tail the eight bits cannot hold is small enough to cost nothing.
        ///
        /// **A field normalised to a standard deviation runs past the range that stores it.** Half a
        /// unit either way is four spreads, and what falls outside is clamped — which moves the mean
        /// and the spread the test above asserts. This is what says by how much.
        TEST(RtxFogNoiseTest, theTailTheStorageCannotHoldIsNegligible)
        {
            const FogNoise& noise = baked();

            std::size_t clipped = 0;
            const auto count = std::size_t{ 2 } * sSize * sSize * sSize;
            for (std::size_t at = 0; at < count; ++at)
                if (noise.mBytes[at] == 0 || noise.mBytes[at] == 255)
                    ++clipped;

            EXPECT_LT(static_cast<double>(clipped) / static_cast<double>(count), 0.001)
                << clipped << " of " << count << " texels clamped";
        }

        /// The tile wraps, which is what lets one of it stand for a landscape.
        ///
        /// **A lattice that ran on past the last texel would seam.** The gradients are taken modulo
        /// the octave's own period, so the field crossing the far face is the field crossing the near
        /// one — and a sampler set to repeat then finds no edge at all.
        ///
        /// Measured as a step: across the wrap the neighbouring texels differ by no more than the
        /// pairs that straddle a lattice vertex anywhere else in the tile. A seam would show as a
        /// step far outside that spread.
        ///
        /// **Against the pairs that straddle a vertex, and not against every pair.** The smoothstep
        /// between two lattice values is flat at both of them, so two texels either side of a vertex
        /// differ by less than two in the middle of a cell — and the wrap sits on a vertex. Compared
        /// with every pair, a correct wrap reads as too smooth rather than as a seam.
        TEST(RtxFogNoiseTest, theTileWrapsWithNoSeamAcrossAnyFace)
        {
            const FogNoise& noise = baked();

            const int texelsPerCell = sSize / static_cast<int>(Shaders::FOG_FIELD_CELLS);

            double inside = 0.0;
            std::size_t insideCount = 0;
            for (int z = 0; z < sSize; ++z)
                for (int y = 0; y < sSize; ++y)
                    for (int x = texelsPerCell - 1; x + 1 < sSize; x += texelsPerCell)
                    {
                        inside
                            += double{ std::abs(texelAt(noise, 0, x, y, z).x() - texelAt(noise, 0, x + 1, y, z).x()) };
                        ++insideCount;
                    }

            const double typical = inside / static_cast<double>(insideCount);

            // The three faces, each against the slice that meets it on the other side.
            for (int axis = 0; axis < 3; ++axis)
            {
                double across = 0.0;
                for (int a = 0; a < sSize; ++a)
                    for (int b = 0; b < sSize; ++b)
                    {
                        const auto pick = [&](int along) {
                            if (axis == 0)
                                return texelAt(noise, 0, along, a, b);
                            if (axis == 1)
                                return texelAt(noise, 0, a, along, b);
                            return texelAt(noise, 0, a, b, along);
                        };

                        across += double{ std::abs(pick(sSize - 1).x() - pick(0).x()) };
                    }

                across /= static_cast<double>(std::size_t{ sSize } * sSize);

                // Half again on either side, which a seam would miss by orders rather than by a
                // fraction: two unrelated fields meeting would step by about the spread itself.
                EXPECT_GT(across, 0.5 * typical) << "axis " << axis;
                EXPECT_LT(across, 1.5 * typical) << "axis " << axis;
            }
        }

        /// The coverage band leaves `FOG_COVERAGE` of the air standing, which is what it is divided
        /// by.
        ///
        /// **So the noise redistributes the air rather than removing it.** The extinction the host
        /// derived is what a ray should cross on average — Morrowind's own view distance turned into a
        /// coefficient — and a band that clears two thirds of the ground would silently make the world
        /// three times clearer than the game says. Dividing by the band's own mean is what holds the
        /// average where it was.
        ///
        /// **Measured here rather than off a render, which is what makes the constant a measurement.**
        /// The frame-based test asserts the same thing to five per cent over nine viewpoints; this is
        /// the same number to four figures, over a lattice wide enough that no two samples share a
        /// tile.
        TEST(RtxFogNoiseTest, theCoverageBandLeavesTheShareTheDensityIsDividedBy)
        {
            const FogNoise& noise = baked();

            // A million points through a box forty tiles on a side. The band's own variance is about
            // an eighth, so a random draw of this many would carry a standard error of 0.0004 and a
            // Halton sequence carries less.
            constexpr std::uint32_t count = 1000000;

            double total = 0.0;
            for (std::uint32_t index = 1; index <= count; ++index)
                total
                    += double{ smoothstep(Shaders::FOG_CLEARING, Shaders::FOG_SOLID, shapeAt(noise, haltonAt(index))) };

            const double coverage = total / static_cast<double>(count);

            EXPECT_NEAR(coverage, double{ Shaders::FOG_COVERAGE }, 0.002) << "the band's own mean";
        }

        /// Every level a march may read clears about the same share of the air, and the first one
        /// it may not read is why there is a cap.
        ///
        /// **A level's texels carry one spread by construction, and a tap between them does not.**
        /// Trilinear filtering hands back values that cluster near the mean, so the band clears less
        /// and less as the chain runs out — and at the single texel that is the whole field's mean it
        /// clears half. Fog that thinned with distance for that reason is what a lamp then lit as an
        /// even glowing screen, with every tree in front of it a silhouette.
        ///
        /// **So `FOG_FIELD_COARSEST` is a measurement rather than a taste**: the last level whose
        /// coverage is still within a twentieth of the constant the density is divided by.
        TEST(RtxFogNoiseTest, everyLevelAMarchMayReadClearsTheShareTheDensityIsDividedBy)
        {
            const FogNoise& noise = baked();

            constexpr std::uint32_t count = 200000;
            const int cap = static_cast<int>(Shaders::FOG_FIELD_COARSEST);

            const auto coverageAt = [&](int level) {
                double total = 0.0;
                for (std::uint32_t index = 1; index <= count; ++index)
                    total += double{ smoothstep(
                        Shaders::FOG_CLEARING, Shaders::FOG_SOLID, shapeAt(noise, haltonAt(index), level)) };

                return total / count;
            };

            for (int level = 0; level <= cap; ++level)
                EXPECT_NEAR(coverageAt(level), double{ Shaders::FOG_COVERAGE }, 0.05 * double{ Shaders::FOG_COVERAGE })
                    << "level " << level;

            // The single texel is the whole field's own mean, and the band maps a mean to about half
            // what it leaves of the field itself. That is the collapse the cap stays in front of.
            EXPECT_LT(coverageAt(sLevels - 1), 0.6 * double{ Shaders::FOG_COVERAGE })
                << "the level the cap keeps a march off";
        }

        /// The three scales present the spread one of them has, which is what the band is cut against.
        ///
        /// **A weighted sum of independent draws carries the variance of the weights' squares.**
        /// Dividing by the plain sum would leave the stack narrower than one scale by a fifth, and
        /// the band would then clear a different share of the ground than it was measured against —
        /// silently, and only where more than one scale contributes.
        TEST(RtxFogNoiseTest, theStackOfScalesHasTheSpreadOneScaleHas)
        {
            const FogNoise& noise = baked();

            constexpr std::uint32_t count = 1000000;

            double total = 0.0;
            double squares = 0.0;
            for (std::uint32_t index = 1; index <= count; ++index)
            {
                const double shape = double{ shapeAt(noise, haltonAt(index)) };
                total += shape;
                squares += shape * shape;
            }

            const double mean = total / static_cast<double>(count);
            const double spread = std::sqrt(squares / static_cast<double>(count) - mean * mean);

            EXPECT_NEAR(mean, 0.5, 0.002);
            EXPECT_NEAR(spread, double{ Shaders::FOG_FIELD_SPREAD }, 0.004);
        }
    }
}
