#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec4f>

#include "alphabounds.hpp"

namespace Rtx
{
    /// Where one triangle's states sit in a micromap's data, and how finely it was cut.
    ///
    /// The two fields a `VkMicromapTriangleEXT` carries besides its format, which is fixed for a
    /// whole micromap here. Offsets are byte-aligned and nothing more, which is what the
    /// specification asks of them, so the states are packed end to end.
    struct MicromapTriangle
    {
        std::uint32_t mDataOffset = 0;
        std::uint16_t mSubdivisionLevel = 0;
    };

    /// How many triangles were cut at one level, which is what a build has to be told up front.
    struct MicromapUsage
    {
        std::uint32_t mCount = 0;
        std::uint32_t mSubdivisionLevel = 0;
    };

    /// How much of a mesh's area came out each way, in triangles.
    ///
    /// **Area and not microtriangles**, because the two are not comparable across levels: a triangle
    /// resolved outright carries one state and a subdivided one carries a thousand, and counting
    /// states would say the second mattered a thousand times more. These three sum to the mesh's
    /// triangle count, so each reads directly as the share of the surface that stopped asking.
    struct MicromapTally
    {
        double mOpaque = 0.0;
        double mTransparent = 0.0;
        double mUnknown = 0.0;
    };

    /// One mesh's opacity micromap: what traversal may settle about a cutout without shading it.
    ///
    /// **This removes work and must change no pixel.** Every microtriangle it calls opaque or
    /// transparent is one the cutout shader would have decided the same way at any level its cone
    /// could pick, which is what `AlphaBounds` is for; everything the mask straddles stays unknown
    /// and goes on asking. So a frame rendered with these built and one rendered without them are
    /// the same frame, and a byte comparison of foliage is what says the subdivision, the ordering
    /// and the bound are all right at once.
    class Micromap
    {
    public:
        /// How many of the mask's finest texels one microtriangle is aimed at covering.
        ///
        /// The whole trade is here: a finer cut resolves more of the boundary and costs four times
        /// the memory and four times the classification a level lower would. Sixteen puts a
        /// microtriangle at about four texels on a side, which is the scale a cutout's edge is drawn
        /// at in Morrowind's masks — finer than that subdivides inside the gradient the compressor
        /// left, which resolves nothing.
        static constexpr float sTexelsPerMicrotriangle = 16.0f;

        /// The finest cut this makes, whatever the device would allow.
        ///
        /// Ada reports twelve, which is sixteen million microtriangles for one triangle and four
        /// megabytes of states to describe a leaf. Five is a thousand microtriangles and 256 bytes,
        /// and a triangle that needs more than that is one whose texture is bigger than the geometry
        /// under it.
        static constexpr std::uint32_t sSubdivisionCeiling = 5;

        /// Classifies every triangle of one mesh against one material's mask.
        ///
        /// Leaves the micromap empty — which a caller must read as *build none* rather than as *all
        /// transparent* — where there is nothing to classify against: a mesh with no texture
        /// coordinates, or a mask that could not be decoded. Geometry whose cutout cannot be
        /// answered for goes on asking, exactly as it does now.
        ///
        /// @param texCoords the mesh's own vertices' texture coordinates.
        /// @param indices the mesh's triangle list, addressing `texCoords`.
        /// @param transform mesh texture coordinates to the mask's, as `uv * xy + zw`.
        /// @param bounds the mask, already bound against this material's cutoff.
        /// @param maxLevel the finest subdivision the device will build, which
        ///        `sSubdivisionCeiling` is then taken against.
        Micromap(std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices,
            const osg::Vec4f& transform, const AlphaBounds& bounds, std::uint32_t maxLevel);

        bool isEmpty() const { return mTriangles.empty(); }

        std::span<const std::byte> getData() const { return mData; }
        std::span<const MicromapTriangle> getTriangles() const { return mTriangles; }
        std::span<const MicromapUsage> getUsage() const { return mUsage; }

        const MicromapTally& getTally() const { return mTally; }

        /// The state at one microtriangle of one triangle, unpacked from the two bits holding it.
        ///
        /// For a reader checking what was built — the packing is the interface the hardware reads
        /// by, so nothing in the renderer goes through this.
        Opacity at(std::uint32_t triangle, std::uint32_t index) const;

    private:
        std::vector<std::byte> mData;
        std::vector<MicromapTriangle> mTriangles;
        std::vector<MicromapUsage> mUsage;
        MicromapTally mTally;
    };
}
