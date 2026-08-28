#include "fognoise.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <osg/Vec2f>

#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        constexpr int sSize = static_cast<int>(Shaders::FOG_FIELD_SIZE);
        constexpr int sLevels = static_cast<int>(Shaders::FOG_FIELD_LEVELS);

        /// How many octaves the tile holds, and how many cells across it the first of them has.
        ///
        /// **The finest octave is four texels wide and stops there.** An octave finer than that is a
        /// lattice the tile cannot carry: it aliases into the level it is drawn at, and the chain
        /// then averages a mistake rather than the field. Six octaves from two cells reaches
        /// sixty-four, which is four texels at the size `Shaders::FOG_FIELD_SIZE` names.
        constexpr int sOctaves = 6;
        constexpr int sCells = 2;

        /// How wide `level` is, which is the tile halved that many times and never below one texel.
        int sizeAt(int level)
        {
            return std::max(sSize >> level, 1);
        }

        /// The eight compass directions, which is Perlin's gradient set one axis down.
        ///
        /// **Eight directions and not a random vector.** A gradient drawn uniformly off the circle
        /// clusters, and the field then has patches where every corner pulls the same way. These are
        /// spread evenly by construction.
        const std::array<osg::Vec2f, 8> sEdges{ osg::Vec2f(1, 0), osg::Vec2f(-1, 0), osg::Vec2f(0, 1),
            osg::Vec2f(0, -1), osg::Vec2f(0.7071068f, 0.7071068f), osg::Vec2f(-0.7071068f, 0.7071068f),
            osg::Vec2f(0.7071068f, -0.7071068f), osg::Vec2f(-0.7071068f, -0.7071068f) };

        /// Which of them a lattice point carries, with the point taken modulo `period`.
        ///
        /// **The modulo is the whole of what makes the tile wrap.** A lattice that ran on past the
        /// last texel would give the far edge a different gradient from the near one, and a field
        /// laid down every tile would then show a seam at every tile boundary.
        const osg::Vec2f& gradientAt(int x, int y, int period, std::uint32_t seed)
        {
            const auto wrap = [period](int v) { return static_cast<std::uint32_t>(((v % period) + period) % period); };

            std::uint32_t h = wrap(x) * 1664525u + wrap(y) * 1013904223u + seed * 374761393u;
            h ^= h >> 15u;
            h *= 0x2c1b3c6du;
            h ^= h >> 12u;
            h *= 0x297a2d39u;
            h ^= h >> 15u;

            return sEdges[h % 8u];
        }

        float fade(float t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        /// Gradient noise at `at`, on a lattice that repeats every `period` cells.
        float perlinAt(const osg::Vec2f& at, int period, std::uint32_t seed)
        {
            const int x0 = static_cast<int>(std::floor(at.x()));
            const int y0 = static_cast<int>(std::floor(at.y()));

            const osg::Vec2f f(at.x() - static_cast<float>(x0), at.y() - static_cast<float>(y0));
            const osg::Vec2f smooth(fade(f.x()), fade(f.y()));

            float corners[4];
            for (int corner = 0; corner < 4; ++corner)
            {
                const int dx = corner & 1;
                const int dy = (corner >> 1) & 1;

                const osg::Vec2f offset(f.x() - static_cast<float>(dx), f.y() - static_cast<float>(dy));
                corners[corner] = gradientAt(x0 + dx, y0 + dy, period, seed) * offset;
            }

            const auto blend = [](float a, float b, float t) { return a + (b - a) * t; };

            return blend(
                blend(corners[0], corners[1], smooth.x()), blend(corners[2], corners[3], smooth.x()), smooth.y());
        }

        /// The octaves of that noise, over a position given as a fraction of the tile.
        float fractalAt(const osg::Vec2f& unit, std::uint32_t seed)
        {
            float total = 0.0f;
            float weight = 0.0f;
            float amplitude = 1.0f;
            int period = sCells;

            for (int octave = 0; octave < sOctaves; ++octave)
            {
                total += amplitude
                    * perlinAt(unit * static_cast<float>(period), period, seed + static_cast<std::uint32_t>(octave));
                weight += amplitude;
                amplitude *= 0.5f;
                period *= 2;
            }

            return total / weight;
        }

        /// Stretches `field` about its own mean until it has the spread every level shares.
        ///
        /// Clamped, because eight bits hold `[0, 1]` and a field normalised to a standard deviation
        /// has a tail past four of them. `theTailTheStorageCannotHoldIsNegligible` is what says the
        /// tail is small enough for that to cost nothing.
        void normalise(std::vector<float>& field)
        {
            double total = 0.0;
            for (const float value : field)
                total += double{ value };

            const double mean = total / static_cast<double>(field.size());

            double squares = 0.0;
            for (const float value : field)
                squares += (double{ value } - mean) * (double{ value } - mean);

            const double spread = std::sqrt(squares / static_cast<double>(field.size()));
            const double scale = spread > 0.0 ? double{ Shaders::FOG_FIELD_SPREAD } / spread : 0.0;

            for (float& value : field)
                value = std::clamp(static_cast<float>(0.5 + (double{ value } - mean) * scale), 0.0f, 1.0f);
        }

        /// The four texels over each texel of the level below, averaged.
        std::vector<float> halved(const std::vector<float>& field, int level)
        {
            const int size = sizeAt(level);
            const int half = sizeAt(level + 1);
            std::vector<float> out(static_cast<std::size_t>(half) * half, 0.0f);

            for (int y = 0; y < half; ++y)
                for (int x = 0; x < half; ++x)
                {
                    float total = 0.0f;
                    for (int corner = 0; corner < 4; ++corner)
                        total += field[static_cast<std::size_t>(2 * y + ((corner >> 1) & 1)) * size + 2 * x
                            + (corner & 1)];

                    out[static_cast<std::size_t>(y) * half + x] = total * 0.25f;
                }

            return out;
        }
    }

    FogNoise bakeFogNoise()
    {
        // **Two fields and not one, because the shape and the displacement are read together.** The
        // domain is warped by a noise of its own, and a warp is a vector: taking it from one channel
        // means a second fetch at a second place, where two channels of one fetch are already there.
        std::array<std::vector<float>, 2> channels;
        for (std::uint32_t channel = 0; channel < channels.size(); ++channel)
        {
            channels[channel].resize(static_cast<std::size_t>(sSize) * sSize);

            for (int y = 0; y < sSize; ++y)
                for (int x = 0; x < sSize; ++x)
                {
                    // The texel's centre, because a lattice sampled at its corners puts the field's
                    // own zero on the texel a level averages from.
                    const osg::Vec2f unit((static_cast<float>(x) + 0.5f) / static_cast<float>(sSize),
                        (static_cast<float>(y) + 0.5f) / static_cast<float>(sSize));

                    channels[channel][static_cast<std::size_t>(y) * sSize + x]
                        = fractalAt(unit, 0x9e3779b9u * (channel + 1u));
                }
        }

        FogNoise noise;
        noise.mOffsets.reserve(sLevels);

        std::size_t texels = 0;
        for (int level = 0; level < sLevels; ++level)
            texels += static_cast<std::size_t>(sizeAt(level)) * sizeAt(level);
        noise.mBytes.reserve(texels * channels.size());

        for (int level = 0; level < sLevels; ++level)
        {
            noise.mOffsets.push_back(noise.mBytes.size());

            for (auto& channel : channels)
                normalise(channel);

            const std::size_t count = static_cast<std::size_t>(sizeAt(level)) * sizeAt(level);
            for (std::size_t at = 0; at < count; ++at)
                for (const auto& channel : channels)
                    noise.mBytes.push_back(static_cast<std::uint8_t>(std::lround(channel[at] * 255.0f)));

            if (level + 1 < sLevels)
                for (auto& channel : channels)
                    channel = halved(channel, level);
        }

        return noise;
    }
}
