#pragma once

#include <vulkan/vulkan_core.h>

#include <components/rtx/guirenderer.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>

#include "buffer.hpp"
#include "image.hpp"
#include "tracechain.hpp"

namespace Rtx
{
    class Device;
    class DeviceScene;
    class DisplayChain;
    class GuiTextures;
    class TraceMedia;

    /// A picture inside the interface — a map tile, the inventory doll, the race preview — traced
    /// into a GUI texture. Its own chain and not the frame's, because borrowing the frame's images
    /// would mean resizing them away from the frame and back between two of them.
    class PictureTracer
    {
    public:
        /// @param passes what the chain traces with, which outlives it.
        PictureTracer(const Device& device, const TracePasses& passes, const TraceMedia& media, DisplayChain& display,
            GuiTextures& textures);

        /// Whether a picture this big fits what is built, which `grow` would leave alone.
        bool holds(VkExtent2D extent) const { return mChain.holds(extent.width, extent.height); }

        /// Makes the chain at least this big, and the byte image the texture is copied out of with
        /// it. What it replaces is destroyed and not buried, so the caller has waited the device
        /// idle.
        void grow(VkExtent2D extent, RadianceWidth radiance);

        /// Records a picture of `traced` under `camera` into `texture`, which `holds` the camera's
        /// extent, on a batch that rides the next submit and that nobody here waits for: the next
        /// placement of the scene waits for what its tables say, and what the picture writes
        /// nothing else reads. Not counted and not timed, because the hit count and the report are
        /// the frame's.
        void trace(GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options,
            DeviceScene& traced, const RenderProfile& profile);

    private:
        const Device& mDevice;
        const TraceMedia& mMedia;
        DisplayChain& mDisplay;
        GuiTextures& mTextures;

        TraceChain mChain;

        /// The picture as bytes, which is what the texture is copied out of. Empty until something
        /// asks for a picture, and grown with the chain.
        Image mTarget;

        /// What the picture sums its census into, which nothing reads: the hit count and the
        /// crossings are the frame's, and a picture traced beside it must not add to them.
        Buffer mCounts;
    };
}
