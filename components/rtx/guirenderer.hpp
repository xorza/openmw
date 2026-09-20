#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

#include "frameextents.hpp"
#include "shaders/visibility.h"
#include "slot.hpp"

namespace Rtx
{
    /// One vertex of the GUI, in MyGUI's own layout: a position already in clip space, a colour
    /// packed a byte a channel, and a texture coordinate. MyGUI fills these by the thousand a frame.
    struct GuiVertex
    {
        float mX;
        float mY;
        float mZ;

        /// Red in the low byte, alpha in the high one — MyGUI's `ColourABGR`.
        std::uint32_t mColour;

        float mU;
        float mV;
    };

    /// A frame of GUI is copied out of the buffer MyGUI filled rather than walked.
    static_assert(sizeof(GuiVertex) == 24, "a GUI vertex is what MyGUI writes, and the buffer is read as its own");
    static_assert(std::is_trivial_v<GuiVertex>);

    /// How a run of GUI reaches what is already on the screen.
    enum class GuiBlend : std::uint32_t
    {
        /// Source alpha over the destination, which is every widget there is.
        Over,

        /// Added to the destination. One layer asks for this — the flash when the player is hit —
        /// and over it the same red reads as a tint on the world rather than light in front of it.
        Additive,
    };

    /// One run of vertices drawn with one texture. A run and not an index range, because MyGUI
    /// hands over triangle lists and no indices.
    struct GuiBatch
    {
        /// A slot from `addGuiTexture`.
        GuiSlot mTexture;
        std::uint32_t mFirstVertex = 0;
        std::uint32_t mVertexCount = 0;
        GuiBlend mBlend = GuiBlend::Over;
    };

    /// Which part of a GUI texture a write covers, with the origin at the top left.
    struct GuiRegion
    {
        std::uint32_t mX = 0;
        std::uint32_t mY = 0;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

    /// What a picture inside the interface is asked for, beyond where its camera stands. How much
    /// of the texture the picture fills, from its top-left corner, is the camera's own extent; the
    /// rest is left at `mClear`. The inventory doll's window resizes and the texture behind it
    /// does not.
    struct GuiTraceOptions
    {
        /// What the rest of the texture holds, red first: transparent black for a picture the GUI
        /// composites over what is behind it.
        std::array<float, 4> mClear{};

        /// What to trace against: a slot `Renderer::addViewScene` gave out, or the world's for the
        /// one the frame is drawn from. A map tile is a picture of the world; a doll is not.
        SceneSlot mScene = SceneSlot::world();

        /// Whether to leave a copy of the whole texture where `takeGuiCopy` can hand it to the host,
        /// which is the one time a picture inside the interface comes back to main memory.
        bool mReadBack = false;
    };

    /// What the interface is drawn on and with: its textures, its draw, and the pictures traced
    /// into it. The part of `Renderer` the GUI's backend and a view inside the interface consume,
    /// as an interface of its own, so neither depends on the scene and the frame halves.
    class GuiRenderer
    {
    public:
        virtual ~GuiRenderer() = default;

        GuiRenderer(const GuiRenderer&) = delete;
        GuiRenderer& operator=(const GuiRenderer&) = delete;

        /// What the last `Renderer::resize` settled on: the interface is laid out over the output
        /// extent, and a camera has to be built for the render extent.
        virtual FrameExtents getExtents() const = 0;

        /// A texture the GUI draws with, sized once and written whenever it changes, in a table of
        /// its own because a font atlas outlives every scene.
        virtual GuiSlot addGuiTexture(std::uint32_t width, std::uint32_t height) = 0;

        /// Bytes for a rectangle of a texture, four a pixel, tightly packed, row zero first, to be
        /// filled and handed back with `sendGuiTexture`: MyGUI's `lock` and `unlock`. The rectangle
        /// must lie inside the texture and only one may be lent at a time, both asserts. Write the
        /// span and do not read it back: a backend may lend memory the device reads directly.
        virtual std::span<std::uint8_t> lendGuiTexture(GuiSlot texture, const GuiRegion& region) = 0;

        /// Sends what `lendGuiTexture` handed out. The span stops being writable here.
        virtual void sendGuiTexture(GuiSlot texture) = 0;

        virtual void dropGuiTexture(GuiSlot texture) = 0;

        /// Everything the GUI asked to draw, over the finished picture, after the tone curve because
        /// the GUI's colours are display-referred. Vertices are in clip space with +Y up, as MyGUI
        /// produces them.
        virtual void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) = 0;

        /// Traces the scene from `camera` into a GUI texture rather than into the frame: a map
        /// tile, the inventory doll. Not the frame's chain — nothing upscales or averages and the
        /// exposure is one, because a still has no previous frame. Recorded and not run: the picture
        /// rides the next submit, reads the copy of the scene its last placement wrote, and the next
        /// placement of that scene waits for the frame it rode.
        virtual void traceGuiTexture(
            GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
            = 0;

        /// The copy the last `traceGuiTexture` with `mReadBack` left of `texture`, four bytes a
        /// pixel, tightly packed, row zero first, into `into` as far as it reaches. False until the
        /// copy arrived, which is two frames on, and never a wait.
        virtual bool takeGuiCopy(GuiSlot texture, std::span<std::uint8_t> into) = 0;

        /// Submits every picture recorded and not yet carried and waits for them, for a harness or a
        /// test standing outside any frame. A game never calls it.
        virtual void finishGuiTraces() = 0;

    protected:
        GuiRenderer() = default;
    };
}
