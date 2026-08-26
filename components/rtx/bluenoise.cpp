#include "bluenoise.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <random>

namespace Rtx
{
    namespace
    {
        constexpr std::size_t sExtent = Shaders::BLUE_NOISE_EXTENT;
        constexpr std::size_t sCount = sExtent * sExtent;

        /// Width of the Gaussian the cluster and void measure is taken with, in pixels.
        ///
        /// Ulichney's figure. It sets what "near" means: too tight and a position stops seeing past
        /// its immediate neighbours, which leaves clumps at the scale it cannot see; too wide and
        /// every position looks alike and the search has nothing left to choose by.
        constexpr float sSigma = 1.5f;

        /// How far the Gaussian is carried before it is dropped, in pixels.
        ///
        /// Three and a third sigma, where it is worth a part in 250 of its peak. It must stay inside
        /// the tile: a splat wide enough to wrap onto itself would stop measuring distance.
        constexpr int sRadius = 5;
        constexpr int sSide = 2 * sRadius + 1;
        static_assert(sSide < static_cast<int>(sExtent));

        /// The share of the tile the first arrangement fills.
        ///
        /// A tenth, which is sparse enough to leave room for rearranging into something even and
        /// dense enough to say where the rest should go.
        constexpr std::size_t sInitialOnes = sCount / 10;

        /// The pattern being ranked and the field saying where its ones are crowded.
        ///
        /// **The field is carried rather than recomputed.** Every step moves one pixel and then asks
        /// the whole tile which position is now the most crowded; rebuilding the field each time
        /// would make this cubic in the pixel count, where updating it over the Gaussian's own
        /// support and scanning once leaves it quadratic.
        struct Field
        {
            std::vector<float> mKernel;
            std::vector<std::uint8_t> mOnes;
            std::vector<float> mEnergy;

            Field()
                : mKernel(static_cast<std::size_t>(sSide) * sSide)
                , mOnes(sCount, 0)
                , mEnergy(sCount, 0.0f)
            {
                for (int dy = -sRadius; dy <= sRadius; ++dy)
                    for (int dx = -sRadius; dx <= sRadius; ++dx)
                    {
                        const float squared = static_cast<float>(dx * dx + dy * dy);
                        mKernel[static_cast<std::size_t>((dy + sRadius) * sSide + (dx + sRadius))]
                            = std::exp(-squared / (2.0f * sSigma * sSigma));
                    }
            }

            void set(std::size_t at, bool one)
            {
                assert((mOnes[at] != 0) != one);
                mOnes[at] = one ? std::uint8_t{ 1 } : std::uint8_t{ 0 };

                const float sign = one ? 1.0f : -1.0f;
                const int cx = static_cast<int>(at % sExtent);
                const int cy = static_cast<int>(at / sExtent);
                const int extent = static_cast<int>(sExtent);

                for (int dy = -sRadius; dy <= sRadius; ++dy)
                {
                    const std::size_t row = static_cast<std::size_t>((cy + dy + extent) % extent) * sExtent;
                    for (int dx = -sRadius; dx <= sRadius; ++dx)
                        mEnergy[row + static_cast<std::size_t>((cx + dx + extent) % extent)]
                            += sign * mKernel[static_cast<std::size_t>((dy + sRadius) * sSide + (dx + sRadius))];
                }
            }

            /// The one with the most company: the sample to take away.
            std::size_t tightestCluster() const
            {
                std::size_t found = 0;
                float most = -std::numeric_limits<float>::infinity();
                for (std::size_t i = 0; i < sCount; ++i)
                    if (mOnes[i] != 0 && mEnergy[i] > most)
                    {
                        most = mEnergy[i];
                        found = i;
                    }

                return found;
            }

            /// Put the pattern and its field back to a state they were both in at once.
            ///
            /// The two have to agree or every search after them is answering about a tile that is
            /// not there. `set` is what normally keeps them in step; this is the one place that
            /// rewinds, and it takes them together so that it cannot rewind one of them.
            void restore(const std::vector<std::uint8_t>& ones, const std::vector<float>& energy)
            {
                mOnes = ones;
                mEnergy = energy;
            }

            /// The hole with the least: where the next sample goes.
            ///
            /// **The same rule serves past half full**, where the zeros are the minority and it is
            /// their tightest cluster that wants breaking up. Every position on a torus sees the same
            /// total Gaussian, so the zeros' own field is that constant minus this one — and its
            /// largest value is exactly this one's smallest.
            std::size_t largestVoid() const
            {
                std::size_t found = 0;
                float least = std::numeric_limits<float>::infinity();
                for (std::size_t i = 0; i < sCount; ++i)
                    if (mOnes[i] == 0 && mEnergy[i] < least)
                    {
                        least = mEnergy[i];
                        found = i;
                    }

                return found;
            }
        };

        /// Void-and-cluster: one mask, as the order in which its pixels would be filled in.
        ///
        /// Ulichney 1993. The rank of a pixel is how many others come before it, so thresholding the
        /// tile anywhere leaves a pattern with no clumps and no holes at that level — which is the
        /// property that makes it useful as an offset per pixel rather than merely as a texture.
        ///
        /// Three passes over one starting arrangement: settle it, rank its members downward by
        /// pulling the tightest cluster apart, then rank everything else upward by filling the
        /// largest void. `seed` is what makes the channels differ.
        std::vector<std::uint32_t> rankMatrix(std::uint32_t seed)
        {
            Field field;

            std::vector<std::size_t> order(sCount);
            std::iota(order.begin(), order.end(), std::size_t{ 0 });
            std::shuffle(order.begin(), order.end(), std::mt19937(seed));
            for (std::size_t i = 0; i < sInitialOnes; ++i)
                field.set(order[i], true);

            // Settle: move the most crowded sample into the emptiest hole until the two are the same
            // place, which is the definition of nothing left to improve. Ulichney proves it stops;
            // the bound is here so that a mistake in the measure shows as an assertion rather than as
            // a hang at startup.
            std::size_t settling = 0;
            for (; settling < sCount; ++settling)
            {
                const std::size_t from = field.tightestCluster();
                field.set(from, false);

                const std::size_t to = field.largestVoid();
                if (to == from)
                {
                    field.set(from, true);
                    break;
                }

                field.set(to, true);
            }
            assert(settling < sCount);

            const std::vector<std::uint8_t> settled = field.mOnes;
            const std::vector<float> settledEnergy = field.mEnergy;

            // Ranking down: the last sample placed is the one whose removal leaves the evenest
            // pattern, so removing them in that order and numbering backwards puts the most isolated
            // one first.
            std::vector<std::uint32_t> rank(sCount, 0);
            for (std::uint32_t r = static_cast<std::uint32_t>(sInitialOnes); r-- > 0;)
            {
                const std::size_t at = field.tightestCluster();
                field.set(at, false);
                rank[at] = r;
            }

            // And up, from the settled arrangement again, all the way to full.
            field.restore(settled, settledEnergy);
            for (std::uint32_t r = static_cast<std::uint32_t>(sInitialOnes); r < sCount; ++r)
            {
                const std::size_t at = field.largestVoid();
                field.set(at, true);
                rank[at] = r;
            }

            return rank;
        }
    }

    BlueNoise::BlueNoise()
        : mValues(sCount * Shaders::RANDOM_STREAMS)
    {
        for (std::uint32_t channel = 0; channel < Shaders::RANDOM_STREAMS; ++channel)
        {
            // A different arrangement to start from is the whole of what makes a channel its own.
            // The seeds are fixed, so the tile is the same on every machine and on every run — which
            // is what lets a test name what it should contain.
            const std::vector<std::uint32_t> rank = rankMatrix(0x9e3779b9u + channel * 0x85ebca6bu);

            // Cell centres. The ranks are a permutation of `[0, count)`, so the values are the
            // stratified sequence `(i + 0.5) / count` dealt out over the tile: uniform by
            // construction, and never exactly zero or one.
            for (std::size_t i = 0; i < sCount; ++i)
                mValues[i * Shaders::RANDOM_STREAMS + channel]
                    = (static_cast<float>(rank[i]) + 0.5f) / static_cast<float>(sCount);
        }
    }

    const BlueNoise& BlueNoise::shared()
    {
        static const BlueNoise tile;
        return tile;
    }
}
