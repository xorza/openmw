#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include <osg/Vec2f>

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
        osg::Vec2f texelAt(const FogNoise& noise, int level, int x, int y)
        {
            const int size = sizeAt(level);
            const auto wrap = [size](int v) { return ((v % size) + size) % size; };

            const std::size_t at = noise.mOffsets[static_cast<std::size_t>(level)]
                + 2 * (static_cast<std::size_t>(wrap(y)) * size + wrap(x));

            return osg::Vec2f(
                static_cast<float>(noise.mBytes[at]) / 255.0f, static_cast<float>(noise.mBytes[at + 1]) / 255.0f);
        }

        /// What `textureLod` gives at a wrapping coordinate: bilinear between the four texels the
        /// point falls among, with the texel's own value sitting at its centre.
        osg::Vec2f sampleAt(const FogNoise& noise, int level, const osg::Vec2f& uv)
        {
            const auto size = static_cast<float>(sizeAt(level));
            const osg::Vec2f grid(uv.x() * size - 0.5f, uv.y() * size - 0.5f);

            const int x0 = static_cast<int>(std::floor(grid.x()));
            const int y0 = static_cast<int>(std::floor(grid.y()));

            const osg::Vec2f f(grid.x() - static_cast<float>(x0), grid.y() - static_cast<float>(y0));

            osg::Vec2f total(0.0f, 0.0f);
            for (int corner = 0; corner < 4; ++corner)
            {
                const int dx = corner & 1;
                const int dy = (corner >> 1) & 1;

                const float weight = (dx != 0 ? f.x() : 1.0f - f.x()) * (dy != 0 ? f.y() : 1.0f - f.y());
                total += texelAt(noise, level, x0 + dx, y0 + dy) * weight;
            }

            return total;
        }

        /// The three scales `fogShape` reads, combined the way it combines them, at a world position.
        ///
        /// **The shader's own arithmetic and not a description of it**, because what this measures is
        /// a property of the number the shader produces. The finest level alone, since what a step
        /// far enough away to reach a coarser one gets is the same field by construction — which is
        /// the other thing this file asserts.
        float shapeAt(const FogNoise& noise, const osg::Vec2f& position, float spacing = 0.0f)
        {
            const auto read = [&](const osg::Vec2f& at, float tile) {
                const float texel = tile / static_cast<float>(Shaders::FOG_FIELD_SIZE);
                const int level = static_cast<int>(
                    std::clamp(std::log2(std::max(spacing / texel, 1.0f)), 0.0f, static_cast<float>(sLevels - 1)));
                return sampleAt(noise, level, osg::Vec2f(at.x() / tile, at.y() / tile));
            };

            const osg::Vec2f coarse = read(position, Shaders::FOG_TILE);

            const float drag = Shaders::FOG_WARP / Shaders::FOG_FIELD_SPREAD;
            const osg::Vec2f warped(
                position.x() + (coarse.x() - 0.5f) * drag, position.y() + (coarse.y() - 0.5f) * drag);

            float total = coarse.x() - 0.5f;
            float squares = 1.0f;
            float amplitude = 1.0f;
            float tile = Shaders::FOG_TILE;

            for (std::uint32_t scale = 1; scale < Shaders::FOG_SCALES; ++scale)
            {
                amplitude *= 0.5f;
                tile /= Shaders::FOG_LACUNARITY;

                total += amplitude * (read(warped, tile).x() - 0.5f);
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
        osg::Vec2f haltonAt(std::uint32_t index)
        {
            const float span = 40.0f * Shaders::FOG_TILE;

            return osg::Vec2f(static_cast<float>(radicalInverse(index, 2)) * span,
                static_cast<float>(radicalInverse(index, 3)) * span);
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

        /// Every level of the chain is the same field, and that is what one coverage band needs.
        ///
        /// **A level is the mean of the eight texels over it, so its own spread narrows going up.**
        /// A band cut against the full level's spread would clear almost nothing at the top of the
        /// chain, and the fog would then thin with distance for a reason nothing in the weather said.
        /// Every level is stretched back about its mean instead, so what a coarse step loses is the
        /// detail and never the amount of air — which is the argument `resolved` makes for a wave
        /// against a ray cone, made once here rather than at every step of every march.
        TEST(RtxFogNoiseTest, everyLevelOfTheChainCarriesOneMeanAndOneSpread)
        {
            const FogNoise& noise = baked();

            ASSERT_EQ(noise.mOffsets.size(), static_cast<std::size_t>(sLevels));

            // The coarsest level is one texel, which has no spread to speak of and is left out.
            for (int level = 0; level + 1 < sLevels; ++level)
            {
                const int size = sizeAt(level);
                const auto count = static_cast<double>(std::size_t{ 2 } * size * size);

                double total = 0.0;
                double squares = 0.0;
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                    {
                        const osg::Vec2f pair = texelAt(noise, level, x, y);
                        for (const float value : { pair.x(), pair.y() })
                        {
                            total += double{ value };
                            squares += double{ value } * double{ value };
                        }
                    }

                const double mean = total / count;
                const double spread = std::sqrt(squares / count - mean * mean);

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
            const auto count = std::size_t{ 2 } * sSize * sSize;
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
        /// Measured as a step: across the wrap the neighbouring texels differ by no more than they do
        /// anywhere else in the tile. A seam would show as a step far outside that spread.
        TEST(RtxFogNoiseTest, theTileWrapsWithNoSeamAcrossEitherEdge)
        {
            const FogNoise& noise = baked();

            double inside = 0.0;
            std::size_t insideCount = 0;
            for (int y = 0; y < sSize; ++y)
                for (int x = 0; x + 1 < sSize; ++x)
                {
                    inside += double{ std::abs(texelAt(noise, 0, x, y).x() - texelAt(noise, 0, x + 1, y).x()) };
                    ++insideCount;
                }

            const double typical = inside / static_cast<double>(insideCount);

            // The two edges, each against the row that meets it on the other side.
            for (int axis = 0; axis < 2; ++axis)
            {
                double across = 0.0;
                for (int a = 0; a < sSize; ++a)
                {
                    const auto pick = [&](int along) {
                        return axis == 0 ? texelAt(noise, 0, along, a) : texelAt(noise, 0, a, along);
                    };

                    across += double{ std::abs(pick(sSize - 1).x() - pick(0).x()) };
                }

                across /= static_cast<double>(sSize);

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

            // A million points over forty tiles a side. The band's own variance is about an eighth,
            // so a random draw of this many would carry a standard error of 0.0004 and a Halton
            // sequence carries less.
            constexpr std::uint32_t count = 1000000;

            double total = 0.0;
            for (std::uint32_t index = 1; index <= count; ++index)
                total
                    += double{ smoothstep(Shaders::FOG_CLEARING, Shaders::FOG_SOLID, shapeAt(noise, haltonAt(index))) };

            const double coverage = total / static_cast<double>(count);

            EXPECT_NEAR(coverage, double{ Shaders::FOG_COVERAGE }, 0.002) << "the band's own mean";
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
