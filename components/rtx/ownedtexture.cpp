#include "ownedtexture.hpp"

namespace Rtx
{
    void OwnedTexture::openChain(const std::uint32_t width, const std::uint32_t height, const TextureFormat format)
    {
        mFormat = format;
        mName = {};
        mBytes.assign(mShape.layOutTo1x1(width, height, sStride), std::byte{});
    }

    void OwnedTexture::openLike(const std::span<const MipLevel> shape, const TextureFormat format)
    {
        mFormat = format;
        mName = {};
        mBytes.assign(mShape.layOutLike(shape, sStride), std::byte{});
    }

    void OwnedTexture::reuse()
    {
        mShape.reuse();
        mBytes.clear();
        mName = {};
    }

    std::span<std::byte, OwnedTexture::sStride> OwnedTexture::at(
        const std::uint32_t level, const std::uint32_t x, const std::uint32_t y)
    {
        return std::span<std::byte, sStride>(mBytes.data() + mShape.offsetOf(level, x, y, sStride), sStride);
    }

    std::span<const std::byte, OwnedTexture::sStride> OwnedTexture::at(
        const std::uint32_t level, const std::uint32_t x, const std::uint32_t y) const
    {
        return std::span<const std::byte, sStride>(mBytes.data() + mShape.offsetOf(level, x, y, sStride), sStride);
    }

    std::span<std::byte> OwnedTexture::level(const std::uint32_t which)
    {
        const MipLevel& shape = mShape.getLevel(which);
        return std::span<std::byte>(mBytes).subspan(
            shape.mOffset, std::size_t{ shape.mWidth } * shape.mHeight * sStride);
    }

    TextureData OwnedTexture::describe() const
    {
        return TextureData{
            .mFormat = mFormat,
            .mWidth = mShape.getWidth(),
            .mHeight = mShape.getHeight(),
            .mBytes = mBytes,
            .mLevels = mShape.mLevels,
            .mName = mName,
        };
    }
}
