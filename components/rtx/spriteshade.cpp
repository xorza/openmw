#include "spriteshade.hpp"

#include <algorithm>
#include <cmath>

namespace Rtx
{
    namespace
    {
        /// A unit vector square to `axis`: its cross with whichever world axis it lies least along.
        osg::Vec3f squareTo(const osg::Vec3f& axis)
        {
            const osg::Vec3f helper
                = std::abs(axis.z()) < 0.9f ? osg::Vec3f(0.0f, 0.0f, 1.0f) : osg::Vec3f(1.0f, 0.0f, 0.0f);
            osg::Vec3f result = axis ^ helper;
            result.normalize();
            return result;
        }

        std::uint32_t clampCell(float at)
        {
            return static_cast<std::uint32_t>(std::clamp(at, 0.0f, static_cast<float>(SpriteShade::sCells - 1)));
        }
    }

    void SpriteShade::shade(
        std::span<Shaders::GpuSprite> sprites, std::span<const Shaders::GpuEmitter> emitters, const osg::Vec3f& toSun)
    {
        // A run that is passed over keeps the nought its sprites were built with, and a run that is
        // shaded is written whole — so nothing is cleared first.
        for (const Shaders::GpuEmitter& emitter : emitters)
        {
            const bool oriented = emitter.mAcross.length2() > 0.0f && emitter.mUpward.length2() > 0.0f;
            if (emitter.mAdditive != 0u || oriented || emitter.mCount < 2)
                continue;

            const std::span<Shaders::GpuSprite> run = sprites.subspan(emitter.mFirst, emitter.mCount);
            shadeToward(run, emitter, toSun, &Shaders::GpuSprite::mSunLayers);
            shadeToward(run, emitter, osg::Vec3f(0.0f, 0.0f, 1.0f), &Shaders::GpuSprite::mSkyLayers);
        }
    }

    void SpriteShade::shadeToward(std::span<Shaders::GpuSprite> run, const Shaders::GpuEmitter& emitter,
        const osg::Vec3f& toward, float Shaders::GpuSprite::*into)
    {
        const osg::Vec3f across = squareTo(toward);
        const osg::Vec3f upward = toward ^ across;

        float largest = 0.0f;
        mProjected.clear();
        mOrder.clear();
        for (std::uint32_t at = 0; at < run.size(); ++at)
        {
            const osg::Vec3f local = run[at].mPosition - emitter.mCentre;
            mProjected.push_back(Projected{ local * toward, local * across, local * upward });
            mOrder.push_back(at);
            largest = std::max(largest, run[at].mRadius);
        }

        const float cell = std::max(2.0f * emitter.mReach / static_cast<float>(sCells), largest / sLargestInCells);

        if (!(cell > 0.0f))
            return;

        // Nearest the light first, so what a sprite reads is what the ones before it laid down. Stable,
        // so two at one depth are ordered by index and a frame cannot flicker between the two answers.
        std::stable_sort(mOrder.begin(), mOrder.end(),
            [this](std::uint32_t a, std::uint32_t b) { return mProjected[a].mDepth > mProjected[b].mDepth; });

        mGrid.assign(std::size_t{ sCells } * sCells, 0.0f);

        const float inverseCell = 1.0f / cell;
        for (const std::uint32_t at : mOrder)
        {
            // Cell centres sit at whole numbers, so the reach's edge is half a cell outside the first.
            const float x = (mProjected[at].mAcross + emitter.mReach) * inverseCell - 0.5f;
            const float y = (mProjected[at].mUpward + emitter.mReach) * inverseCell - 0.5f;

            run[at].*into = readAt(x, y);
            layDown(x, y, run[at].mRadius * inverseCell, run[at].mAlpha);
        }
    }

    float SpriteShade::readAt(float x, float y) const
    {
        const float fx = std::clamp(x, 0.0f, static_cast<float>(sCells - 1));
        const float fy = std::clamp(y, 0.0f, static_cast<float>(sCells - 1));
        const std::uint32_t x0 = static_cast<std::uint32_t>(fx);
        const std::uint32_t y0 = static_cast<std::uint32_t>(fy);
        const std::uint32_t x1 = std::min(x0 + 1, sCells - 1);
        const std::uint32_t y1 = std::min(y0 + 1, sCells - 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        const auto cell = [this](std::uint32_t cx, std::uint32_t cy) { return mGrid[std::size_t{ cy } * sCells + cx]; };
        const float top = std::lerp(cell(x0, y0), cell(x1, y0), tx);
        const float bottom = std::lerp(cell(x0, y1), cell(x1, y1), tx);

        return std::lerp(top, bottom, ty);
    }

    void SpriteShade::layDown(float x, float y, float radius, float weight)
    {
        // Too small to reach a cell's centre: its whole area, on the cell it is in.
        if (radius < 0.5f)
        {
            mGrid[std::size_t{ clampCell(std::round(y)) } * sCells + clampCell(std::round(x))]
                += weight * Shaders::PI * radius * radius;

            return;
        }

        // **A row at a time, and the inside of the disc without a square root.** The ramp is one cell
        // wide, so a cell more than half a cell within the rim is covered in full and a cell outside
        // it is not covered at all — exactly, by the clamp — and only the ring between those two
        // radii needs the distance itself. Of a disc eight cells across that ring is a seventh of the
        // cells, where the bounding box asked for the distance at every one of them and at the
        // corners it does not reach besides. Measured at Vivec, where `layDown` was 12.4% of the whole
        // frame's CPU and the emitters around it were a third of it.
        const float outer = radius + 0.5f;
        const float inner = radius - 0.5f;
        const float outside = outer * outer;
        const float within = inner * inner;

        const std::uint32_t fromY = clampCell(std::floor(y - outer));
        const std::uint32_t untilY = clampCell(std::ceil(y + outer));

        for (std::uint32_t cy = fromY; cy <= untilY; ++cy)
        {
            const float dy = static_cast<float>(cy) - y;

            // How far the disc reaches across this row, which is one square root for the row rather
            // than one for each of its cells. Negative where the row misses the disc — and where the
            // clamps above pulled a row onto the grid that the disc never touched.
            const float span = outside - dy * dy;
            if (!(span > 0.0f))
                continue;

            const float half = std::sqrt(span);
            const std::uint32_t fromX = clampCell(std::ceil(x - half));
            const std::uint32_t untilX = clampCell(std::floor(x + half));

            float* const row = mGrid.data() + std::size_t{ cy } * sCells;
            for (std::uint32_t cx = fromX; cx <= untilX; ++cx)
            {
                const float dx = static_cast<float>(cx) - x;
                const float away = dx * dx + dy * dy;
                if (away <= within)
                {
                    row[cx] += weight;
                    continue;
                }

                row[cx] += weight * std::clamp(radius - std::sqrt(away) + 0.5f, 0.0f, 1.0f);
            }
        }
    }
}
