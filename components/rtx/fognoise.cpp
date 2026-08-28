#include "fognoise.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <osg/Vec3f>

#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        constexpr int sSize = static_cast<int>(Shaders::FOG_FIELD_SIZE);
        constexpr int sLevels = static_cast<int>(Shaders::FOG_FIELD_LEVELS);

        constexpr int sCells = static_cast<int>(Shaders::FOG_FIELD_CELLS);

        /// How wide `level` is, which is the tile halved that many times and never below one texel.
        int sizeAt(int level)
        {
            return std::max(sSize >> level, 1);
        }

        /// `v` taken modulo `period`, never negative.
        int wrapped(int v, int period)
        {
            return ((v % period) + period) % period;
        }

        /// The eight corners around a point blended by its fractions, each corner's value asked of
        /// `at(dx, dy, dz)`.
        template <class Corner>
        float trilinear(const osg::Vec3f& fraction, const Corner& at)
        {
            float total = 0.0f;
            for (int corner = 0; corner < 8; ++corner)
            {
                const int dx = corner & 1;
                const int dy = (corner >> 1) & 1;
                const int dz = (corner >> 2) & 1;

                const float weight = (dx != 0 ? fraction.x() : 1.0f - fraction.x())
                    * (dy != 0 ? fraction.y() : 1.0f - fraction.y()) * (dz != 0 ? fraction.z() : 1.0f - fraction.z());
                total += at(dx, dy, dz) * weight;
            }

            return total;
        }

        /// A repeatable value in `[0, 1]` for a lattice point, with the point taken modulo `period`.
        ///
        /// **The modulo is the whole of what makes the tile wrap.** A lattice that ran on past the
        /// last texel would give the far face a different value from the near one, and a field laid
        /// down every tile would then show a seam at every tile boundary. All three axes, so the
        /// volume wraps upwards as well as sideways.
        float valueAt(int x, int y, int z, int period, std::uint32_t seed)
        {
            const auto wrap = [period](int v) { return static_cast<std::uint32_t>(wrapped(v, period)); };

            std::uint32_t h = wrap(x) * 1664525u + wrap(y) * 1013904223u + wrap(z) * 2654435761u + seed * 374761393u;
            h ^= h >> 15u;
            h *= 0x2c1b3c6du;
            h ^= h >> 12u;
            h *= 0x297a2d39u;
            h ^= h >> 15u;

            // Sixteen bits is more than a haze needs, and taking the high ones is what keeps the
            // low bits of a weak avalanche out of the field.
            return static_cast<float>(h >> 16u) / 65535.0f;
        }

        /// Trilinear value noise at `at`, on a lattice that repeats every `period` cells along every
        /// axis.
        ///
        /// **Value noise and not gradient noise, because this is the field the renderer this is
        /// ported from draws its fog with.** Its `fog_noise` is exactly this — a hashed value at each
        /// corner, blended with a smoothstep so the lattice does not show as a grid of creases — and
        /// what the fog looks like is mostly what this looks like.
        float noiseAt(const osg::Vec3f& at, int period, std::uint32_t seed)
        {
            const int x0 = static_cast<int>(std::floor(at.x()));
            const int y0 = static_cast<int>(std::floor(at.y()));
            const int z0 = static_cast<int>(std::floor(at.z()));

            const auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
            const osg::Vec3f f(smooth(at.x() - static_cast<float>(x0)), smooth(at.y() - static_cast<float>(y0)),
                smooth(at.z() - static_cast<float>(z0)));

            return trilinear(
                f, [&](int dx, int dy, int dz) { return valueAt(x0 + dx, y0 + dy, z0 + dz, period, seed); });
        }

        /// What a trilinear sampler hands back at `at`, in texels, over a wrapping level of `size`
        /// texels a side with each texel's own value at its centre.
        float sampledAt(const std::vector<float>& field, int size, const osg::Vec3f& at)
        {
            const osg::Vec3f grid(at.x() - 0.5f, at.y() - 0.5f, at.z() - 0.5f);

            const int x0 = static_cast<int>(std::floor(grid.x()));
            const int y0 = static_cast<int>(std::floor(grid.y()));
            const int z0 = static_cast<int>(std::floor(grid.z()));

            const osg::Vec3f f(grid.x() - static_cast<float>(x0), grid.y() - static_cast<float>(y0),
                grid.z() - static_cast<float>(z0));

            return trilinear(f, [&](int dx, int dy, int dz) {
                const std::size_t index
                    = (static_cast<std::size_t>(wrapped(z0 + dz, size)) * size + wrapped(y0 + dy, size)) * size
                    + wrapped(x0 + dx, size);
                return field[index];
            });
        }

        /// Stretches `field` about its own mean until what a sampler reads from it has the spread
        /// every level shares.
        ///
        /// **Measured through the sampler and not off the texels, because the two are not the same
        /// field.** A trilinear tap between eight texels hands back values that cluster nearer the
        /// mean than any of the eight, so a level whose *texels* carried one spread presented a
        /// narrower one to every march — narrower the fewer texels it had, until a coverage band cut
        /// for the full level cleared half as much at the top of the chain. Sixty-four taps a texel
        /// stand for the continuous field here, and since the tap is linear, stretching the texels
        /// stretches what it reads by exactly the same factor.
        ///
        /// Clamped, because eight bits hold `[0, 1]` and a field normalised to a standard deviation
        /// has a tail past four of them. `theTailTheStorageCannotHoldIsNegligible` is what says the
        /// tail is small enough for that to cost nothing.
        void normalise(std::vector<float>& field, int size)
        {
            constexpr std::array<float, 4> taps{ 0.125f, 0.375f, 0.625f, 0.875f };

            double total = 0.0;
            double squares = 0.0;
            std::size_t count = 0;
            for (int z = 0; z < size; ++z)
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                        for (const float w : taps)
                            for (const float v : taps)
                                for (const float u : taps)
                                {
                                    const double read = double{ sampledAt(field, size,
                                        osg::Vec3f(static_cast<float>(x) + u, static_cast<float>(y) + v,
                                            static_cast<float>(z) + w)) };
                                    total += read;
                                    squares += read * read;
                                    ++count;
                                }

            const double mean = total / static_cast<double>(count);
            const double spread = std::sqrt(std::max(squares / static_cast<double>(count) - mean * mean, 0.0));
            const double scale = spread > 0.0 ? double{ Shaders::FOG_FIELD_SPREAD } / spread : 0.0;

            for (float& value : field)
                value = std::clamp(static_cast<float>(0.5 + (double{ value } - mean) * scale), 0.0f, 1.0f);
        }

        /// The eight texels over each texel of the level below, averaged.
        std::vector<float> halved(const std::vector<float>& field, int level)
        {
            const int size = sizeAt(level);
            const int half = sizeAt(level + 1);
            std::vector<float> out(static_cast<std::size_t>(half) * half * half, 0.0f);

            for (int z = 0; z < half; ++z)
                for (int y = 0; y < half; ++y)
                    for (int x = 0; x < half; ++x)
                    {
                        float total = 0.0f;
                        for (int corner = 0; corner < 8; ++corner)
                        {
                            const std::size_t at = (static_cast<std::size_t>(2 * z + ((corner >> 2) & 1)) * size + 2 * y
                                                       + ((corner >> 1) & 1))
                                    * size
                                + 2 * x + (corner & 1);
                            total += field[at];
                        }

                        out[(static_cast<std::size_t>(z) * half + y) * half + x] = total * 0.125f;
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
            channels[channel].resize(static_cast<std::size_t>(sSize) * sSize * sSize);

            for (int z = 0; z < sSize; ++z)
                for (int y = 0; y < sSize; ++y)
                    for (int x = 0; x < sSize; ++x)
                    {
                        // The texel's centre, which is where a sampler puts the texel's own value.
                        const osg::Vec3f unit((static_cast<float>(x) + 0.5f) / static_cast<float>(sSize),
                            (static_cast<float>(y) + 0.5f) / static_cast<float>(sSize),
                            (static_cast<float>(z) + 0.5f) / static_cast<float>(sSize));

                        // **Four texels a cell and one octave.** A lattice finer than that aliases
                        // into the level it is drawn at, and the chain then averages a mistake rather
                        // than the field; a second octave is what `fogShape`'s scales are for.
                        channels[channel][(static_cast<std::size_t>(z) * sSize + y) * sSize + x]
                            = noiseAt(unit * static_cast<float>(sCells), sCells, 0x9e3779b9u * (channel + 1u));
                    }
        }

        FogNoise noise;
        noise.mOffsets.reserve(sLevels);

        std::size_t texels = 0;
        for (int level = 0; level < sLevels; ++level)
            texels += static_cast<std::size_t>(sizeAt(level)) * sizeAt(level) * sizeAt(level);
        noise.mBytes.reserve(texels * channels.size());

        for (int level = 0; level < sLevels; ++level)
        {
            noise.mOffsets.push_back(noise.mBytes.size());

            for (auto& channel : channels)
                normalise(channel, sizeAt(level));

            const std::size_t count = static_cast<std::size_t>(sizeAt(level)) * sizeAt(level) * sizeAt(level);
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
