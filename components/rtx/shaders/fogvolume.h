// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_FOGVOLUME_H
#define OPENMW_COMPONENTS_RTX_SHADERS_FOGVOLUME_H

#include "portable.h"

// What the fog volume's images are made of and where each one is bound, said once for both sides
// that have to agree. `gbuffer.h` says why a format is a macro rather than a constant, and what a
// channel costs when the two statements of it drift.
//
// **Three formats and not one**, because two of these images hold a single channel: the sun's
// transport is a product of transmittances and carries no colour, so the accumulated and the
// per-slice copies of it are half floats one wide. The column depth is a world distance and is the
// one thing here a half float cannot hold.

#ifdef RTX_HOST

#define FOG_VOLUME_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define FOG_SUNWARD_FORMAT VK_FORMAT_R16_SFLOAT
#define FOG_DEPTH_FORMAT VK_FORMAT_R32_SFLOAT

#else

#define FOG_VOLUME_FORMAT rgba16f
#define FOG_SUNWARD_FORMAT r16f
#define FOG_DEPTH_FORMAT r32f

#endif

// Which binding of set three each image is, for the shaders that declare them and the owner that
// writes them.
//
// **Ten images and seventeen bindings, seven of them named twice**, because Vulkan has no
// descriptor a shader may both sample and store through — and every one but a pair's history is
// written by one pass and read by the next. The column depth is named once: both passes reach it
// through the one storage binding.
//
// **Sampled first and storage after**, so `FOG_SAMPLED_COUNT` is a bound rather than a table, and
// the layout and the writes cannot disagree about which kind a binding is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// What the air scatters at a point and the three answers a ray each gave there, as the
    /// previous frame left them. These are the quantities that reproject, so these are the ones a
    /// frame averages against.
    const uint BIND_FOG_WAS_SCATTER = 0;
    const uint BIND_FOG_WAS_SUNWARD = 1;

    /// The same two as this frame's scatter pass wrote them, which its integrate pass reads — and
    /// what a puff of smoke reads at a point rather than as a column's integral.
    const uint BIND_FOG_SCATTER = 2;
    const uint BIND_FOG_SUNWARD = 3;

    /// What every lamp puts into a froxel, per steradian and with nothing standing in the way.
    const uint BIND_FOG_LAMPS = 4;

    /// Both accumulated front to back, which is what a pixel reads.
    const uint BIND_FOG_AIR = 5;
    const uint BIND_FOG_AIR_SUNWARD = 6;

    /// What each slice holds once everything that lights it is applied, which a pixel steps through
    /// from the last edge it passed to where its surface stands.
    const uint BIND_FOG_SLICE = 7;
    const uint BIND_FOG_SLICE_SUNWARD = 8;

    /// The same seven, as the pass that fills each one writes it.
    const uint BIND_FOG_SCATTER_TARGET = 9;
    const uint BIND_FOG_SUNWARD_TARGET = 10;
    const uint BIND_FOG_LAMPS_TARGET = 11;
    const uint BIND_FOG_AIR_TARGET = 12;
    const uint BIND_FOG_AIR_SUNWARD_TARGET = 13;
    const uint BIND_FOG_SLICE_TARGET = 14;
    const uint BIND_FOG_SLICE_SUNWARD_TARGET = 15;

    /// How far each column's ray runs before it meets a surface, which every reader reaches through
    /// this one storage binding because none of them samples it.
    const uint BIND_FOG_COLUMN_DEPTH = 16;

    /// Where the sampled bindings end and the storage ones begin, and how many the set declares.
    const uint FOG_SAMPLED_COUNT = BIND_FOG_SCATTER_TARGET;
    const uint FOG_BINDING_COUNT = BIND_FOG_COLUMN_DEPTH + 1;

#ifdef RTX_HOST
}
#endif

#endif
