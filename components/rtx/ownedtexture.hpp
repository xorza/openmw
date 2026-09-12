#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "texturedata.hpp"

namespace Rtx
{
    /// A four-channel texture this process built and owns, which describes itself the way a file
    /// does. The description spans this object's storage, so it is valid only while this is, and
    /// only until the next `open`.
    class OwnedTexture
    {
    public:
        static constexpr std::size_t sStride = 4;

        /// Lays out a chain from `width` by `height` down to one texel, and clears its bytes and
        /// its name — a name is the source's and does not survive being opened over.
        void openChain(std::uint32_t width, std::uint32_t height, TextureFormat format);

        /// The same, keeping the extents `shape` states rather than halving to one texel.
        void openLike(std::span<const MipLevel> shape, TextureFormat format);

        void reuse();

        void setName(std::string_view name) { mName = name; }

        bool isEmpty() const { return mShape.isEmpty(); }

        const MipPyramid& getShape() const { return mShape; }

        std::uint32_t getWidth() const { return mShape.getWidth(); }
        std::uint32_t getHeight() const { return mShape.getHeight(); }

        /// The four bytes of one texel, to read or to write.
        std::span<std::byte, sStride> at(std::uint32_t level, std::uint32_t x, std::uint32_t y);
        std::span<const std::byte, sStride> at(std::uint32_t level, std::uint32_t x, std::uint32_t y) const;

        /// One whole level, for a walk that indexes it itself.
        std::span<std::byte> level(std::uint32_t which);

        TextureData describe() const;

    private:
        MipPyramid mShape;
        std::vector<std::byte> mBytes;
        TextureFormat mFormat = TextureFormat::Rgba8Unorm;
        std::string_view mName;
    };
}
