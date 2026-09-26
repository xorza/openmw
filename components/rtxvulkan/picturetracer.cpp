#include "picturetracer.hpp"

#include <cassert>

#include <components/rtx/frameoptions.hpp>
#include <components/rtx/framesampling.hpp>
#include <components/rtx/shaders/counts.h>

#include "commands.hpp"
#include "device.hpp"
#include "devicescene.hpp"
#include "displaychain.hpp"
#include "guitextures.hpp"
#include "imageuse.hpp"
#include "presenttargets.hpp"
#include "timeline.hpp"
#include "tracemedia.hpp"
#include "tracerecording.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    PictureTracer::PictureTracer(const Device& device, const TracePasses& passes, const TraceMedia& media,
        DisplayChain& display, GuiTextures& textures)
        : mDevice(device)
        , mMedia(media)
        , mDisplay(display)
        , mTextures(textures)
        , mChain(device, passes, VK_IMAGE_USAGE_STORAGE_BIT, "view colour")
        , mCounts(Buffer::deviceLocal(
              device, sizeof(Shaders::FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "picture counts"))
    {
    }

    void PictureTracer::grow(const VkExtent2D extent, const RadianceWidth radiance)
    {
        mChain.grow(extent.width, extent.height, radiance);
        mTarget = Image(mDevice, mChain.getWidth(), mChain.getHeight(), PresentTargets::sFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "view target");
    }

    void PictureTracer::trace(const GuiSlot texture, const Shaders::VisibilityConstants& camera,
        const GuiTraceOptions& options, DeviceScene& traced, const RenderProfile& profile)
    {
        // The camera's own extent is how much of the texture the picture fills.
        const VkExtent2D extent{ camera.mCamera.mWidth, camera.mCamera.mHeight };
        assert(holds(extent) && "a picture larger than the chain was grown to");

        const VisibilityInputs inputs
            = mMedia.describe(traced, camera, mChain.getColour(), mCounts, mDisplay.getGlareCounts(), FrameSlot{});

        // Nothing reconstructs a picture, so nothing jitters it, and it has no frame before it.
        Shaders::VisibilityConstants sampled
            = sampleFrame(camera, FrameOptions{}, profile, Reconstruction{}, traced.getCounts(), nullptr);

        // The world's ripple field where the picture is of the world, which is the one place it
        // could have a wake in it; a subject of its own stands in no sea.
        if (options.mScene.isWorld())
            mMedia.placeRipples(sampled);

        Batch trace(mDevice.getPool());
        {
            const VkCommandBuffer commands = trace.getCommands();

            // A doll and a map tile are one frame with no frame before them, so every history says
            // so: the accumulator becomes a pass-through handing on the largest variance there is,
            // which is what tells the cascade to filter as widely as it can.
            const TraceResult picture = mChain.record(commands,
                TraceRecording{
                    .mInputs = inputs,
                    .mAsked = camera,
                    .mSampled = sampled,
                    .mTarget = &mTarget,
                });

            // The puffs over the picture — a torch's flame in a doll's hand is a sprite — and the
            // curve, and nothing else: a picture is measured off nothing, mapped with no share and
            // spread by no lens, because a map tile is a diagram and the same armour must be the
            // same brightness in two windows.
            picture.mColour.transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);
            mDisplay.record(commands,
                Display{
                    .mTrace = picture,
                    .mExtent = extent,
                    .mSampled = sampled,
                    .mTarget = mTarget,
                    .mExposure = Display::Picture{},
                });

            mTarget.transition(commands, Use::sComputeWrite, Use::sCopyRead);

            // Borrowed rather than transitioned. Where a GUI texture rests between writes is
            // `GuiTextures`' to say, and a caller that said it here had to keep a barrier's scope in
            // step with the commands below — which it did not.
            mTextures.writeWith(texture, commands, [&](const Image& into, VkImageLayout layout) {
                assert(extent.width <= into.getWidth() && extent.height <= into.getHeight());

                // Cleared whole and then covered in part, and only where the picture does not
                // cover it all: what the trace fills is as much of the texture as the widget is
                // currently wide, and the rest has to be the clear colour rather than what a wider
                // picture left there the last time this was drawn. Both are transfer writes to the
                // same image and nothing orders two of those, so the clear is left as what the copy
                // meets.
                if (extent.width < into.getWidth() || extent.height < into.getHeight())
                    into.clear(commands, Use::sTransferWrite,
                        VkClearColorValue{
                            .float32 = { options.mClear[0], options.mClear[1], options.mClear[2], options.mClear[3] } },
                        Use::sCopyWrite);

                assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    && "a texture lent in another layout than a copy takes");
                mTarget.copyTo(commands, into, layout, extent);
            });

            if (options.mReadBack)
                mTextures.readBackWith(texture, commands);
        }
        trace.defer();

        // The value the batch rides: the next submit this pool makes, whichever that is. The
        // tables it reads were named the same value as they were handed out above.
        traced.notePictureRide(traced.getSlot(), mDevice.getTimeline().getNext());
    }
}
