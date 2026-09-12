#include "runrecord.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

#include <components/files/conversion.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/shadingmap.hpp>
#include <components/rtx/texels.hpp>
#include <components/rtx/texturedata.hpp>

namespace Rtx
{
    namespace
    {
        /// How large one thumbnail is drawn, and how far apart the pairs stand.
        constexpr std::uint32_t sThumbnail = 128;
        constexpr std::uint32_t sGap = 6;

        /// How many pairs stand across the sheet.
        constexpr std::uint32_t sColumns = 6;
    }

    std::uint32_t ContactSheet::getStride()
    {
        return sThumbnail + sGap;
    }

    std::uint32_t ContactSheet::getThumbnail()
    {
        return sThumbnail;
    }

    std::uint32_t ContactSheet::getLeftOf(std::uint32_t index) const
    {
        return sGap + index % sColumns * (sThumbnail * 2 + sGap + sGap);
    }

    std::uint32_t ContactSheet::getTopOf(std::uint32_t index) const
    {
        return sGap + index / sColumns * (sThumbnail + sGap);
    }

    ContactSheet drawContactSheet(std::span<const TextureData> textures, float strength)
    {
        if (textures.empty())
            return ContactSheet{};

        const auto count = static_cast<std::uint32_t>(textures.size());
        const std::uint32_t rows = (count + sColumns - 1) / sColumns;
        const std::uint32_t pairWidth = sThumbnail * 2 + sGap;
        const std::uint32_t width = sColumns * pairWidth + (sColumns + 1) * sGap;
        const std::uint32_t height = rows * (sThumbnail + sGap) + sGap;

        // Mid grey behind them, so a texture that is black and one that is missing do not look the
        // same as the paper they are printed on.
        std::vector<std::uint8_t> sheet(std::size_t{ width } * height * 4, std::uint8_t{ 64 });
        for (std::size_t at = 3; at < sheet.size(); at += 4)
            sheet[at] = 255;

        ContactSheet drawn{ std::move(sheet), width, height, count };
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const TextureData& texture = textures[index];
            const std::uint32_t left = drawn.getLeftOf(index);
            const std::uint32_t top = drawn.getTopOf(index);

            for (std::uint32_t y = 0; y < sThumbnail; ++y)
                for (std::uint32_t x = 0; x < sThumbnail; ++x)
                {
                    // Nearest neighbour, and the centre of the texel a thumbnail pixel covers.
                    const float u = (x + 0.5f) / sThumbnail;
                    const float v = (y + 0.5f) / sThumbnail;
                    const auto texelX = std::min(
                        static_cast<std::uint32_t>(u * static_cast<float>(texture.mWidth)), texture.mWidth - 1);
                    const auto texelY = std::min(
                        static_cast<std::uint32_t>(v * static_cast<float>(texture.mHeight)), texture.mHeight - 1);

                    const osg::Vec3f stored = Rtx::texelAt(texture, texture.mLevels.front(), texelX, texelY);
                    osg::Vec3f corrected = stored;
                    if (!texture.mShading.empty() && strength > 0.0f)
                    {
                        // **In linear, because that is where the shader divides.** A texture's
                        // bytes are display-encoded and the sampler hands the frame linear values,
                        // so a sheet that divided the bytes would be showing a correction the
                        // renderer never applies — half again too strong in the darks.
                        const float factor = std::lerp(1.0f, Rtx::paintedLight(texture.mShading, u, v), strength);
                        const bool srgb = Rtx::isSrgb(texture.mFormat);
                        for (int channel = 0; channel < 3; ++channel)
                        {
                            const float linear = srgb ? Rtx::toLinear(stored[channel]) : stored[channel];
                            corrected[channel] = srgb ? Rtx::toEncoded(linear / factor) : linear / factor;
                        }
                    }

                    const auto place = [&](std::uint32_t offset, const osg::Vec3f& colour) {
                        const std::size_t at = (std::size_t{ top + y } * width + left + offset + x) * 4;
                        for (int channel = 0; channel < 3; ++channel)
                            drawn.mPixels[at + channel] = static_cast<std::uint8_t>(
                                std::lround(std::clamp(colour[channel], 0.0f, 1.0f) * 255.0f));
                    };

                    place(0, stored);
                    place(ContactSheet::getStride(), corrected);
                }
        }

        return drawn;
    }

    ContactSheet writeContactSheet(
        std::span<const TextureData> textures, const std::filesystem::path& out, float strength)
    {
        ContactSheet sheet = drawContactSheet(textures, strength);
        if (sheet.mCount > 0)
            writePng(out, sheet.mWidth, sheet.mHeight, sheet.mPixels);

        return sheet;
    }

    void RunRecord::add(BenchPlace place)
    {
        mPlaces.push_back(std::move(place));
        mReport += describePlace(mPlaces.back());
    }

    void RunRecord::checked(const bool held)
    {
        ++mChecked;
        mFailed += held ? 0u : 1u;
        if (!held)
            fail();
    }

    SessionResult RunRecord::describe(const Stop* const left) const
    {
        SessionResult result;
        result.mExitStatus = mExitStatus;
        result.mPlaces = mPlaces;
        result.mReport = mReport;
        if (left != nullptr)
            result.mLeft = *left;

        return result;
    }

    void RunRecord::finish(const SessionRequest& request)
    {
        mReport += describeTotal(mPlaces, false);

        // **What a `check` run came to, in one line.** A suite asks every check at each of several
        // places, so the verdict is otherwise something a reader counts by hand.
        if (mChecked > 0)
            mReport += std::format("\n{} checks asked, {} failed\n", mChecked, mFailed);

        if (!request.mHashes.empty())
        {
            mHashes.write(request.mHashes);
            mReport += std::format(
                "\nwrote {} frame hashes to {}\n", mHashes.frameCount(), Files::pathToUnicodeString(request.mHashes));
        }

        if (!request.mAgainst.empty())
        {
            mReport += std::format("\nagainst {}\n", Files::pathToUnicodeString(request.mAgainst));
            for (const FrameHashes::ViewDifference& difference : mHashes.against(mReference))
            {
                mReport += std::format("  {:<28} {}\n", difference.mView, describeDifference(difference));
                if (!difference.same())
                    fail();
            }
        }

        if (!request.mJson.empty())
        {
            mHeader.mSuite = request.mSuite;
            writeJson(request.mJson, mHeader, mPlaces);
            mReport += "wrote " + Files::pathToUnicodeString(request.mJson) + '\n';
        }
    }
}
