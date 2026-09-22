#include "dlssupscaler.hpp"

#include <cassert>
#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "dlsspass.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    DlssUpscaler::DlssUpscaler(const Device& device, VkInstance instance)
        : mDevice(device)
        , mNgx(device, instance)
    {
        if (!mNgx.isAvailable())
            throw Unsupported("DLSS Ray Reconstruction was asked for and " + mNgx.getObstacle());
    }

    DlssUpscaler::~DlssUpscaler() = default;

    VkExtent2D DlssUpscaler::renderSizeFor(const VkExtent2D output, const Upscale mode) const
    {
        return mNgx.getRenderSize(output, mode);
    }

    void DlssUpscaler::release()
    {
        mPass.reset();
        mOutput = Image();
    }

    void DlssUpscaler::resize(const VkExtent2D render, const VkExtent2D output, const Upscaling& how)
    {
        // Released before the next is built: the feature holds the network's weights for one pair
        // of resolutions, and the image it writes is sixteen bytes a pixel of the output, so neither
        // is left behind for a pair that may not come back.
        release();

        // Half floats whatever width the run gave the trace's own composite: this one is shown
        // and never summed. The peak linear radiance a frame of this game reaches is under nine,
        // measured over the view suite and a camera pointed at the noon sun, so a half carries
        // it with four orders of magnitude to spare at a step finer than the display's — which
        // is also `RadianceWidth::Shown`'s argument.
        mOutput = Image(mDevice, output.width, output.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "upscaled");

        // Building uploads the network's weights, which is once per resolution rather than once
        // per frame.
        mDevice.getPool().submitAndWait([&](VkCommandBuffer commands) {
            mPass = std::make_unique<DlssPass>(mNgx, commands, render, output, how.mMode, how.mPreset);
        });
    }

    const Image& DlssUpscaler::record(const VkCommandBuffer commands, const UpscaleInputs& inputs)
    {
        // `resize` makes the pass and its image together, so nothing here asks whether they are
        // there beyond this.
        assert(mPass != nullptr && !mOutput.isEmpty() && "a reconstruction before the first resize");

        mOutput.transition(commands, Use::sUndefined, Use::sAnyGeneralWrite);
        mPass->record(commands, inputs, mOutput);

        // What NGX recorded is its own; nothing here knows which stages it used.
        mOutput.transition(commands, Use::sAnyGeneralWrite, Use::sTraceReadWrite);

        return mOutput;
    }

    std::unique_ptr<Upscaler> makeUpscaler(const Device& device, const VkInstance instance)
    {
        return std::make_unique<DlssUpscaler>(device, instance);
    }

    std::string describeUpscaling(const Device& device, const VkInstance instance)
    {
        // An answer rather than a runtime, which is why reporting on a device cannot disturb one:
        // NGX keeps one runtime per process and its shutdown is unconditional, so a `Dlss` built
        // to ask with and let go would end this renderer's the moment it left scope.
        try
        {
            const DlssSupport support = Dlss::probe(device, instance);
            return support.mAvailable ? "available" : "unavailable, " + support.mObstacle;
        }
        catch (const Unsupported& obstacle)
        {
            return std::string("unavailable, ") + obstacle.what();
        }
        catch (const DeviceError& failed)
        {
            return std::string("unavailable, ") + failed.what();
        }
    }

    std::span<const char* const> upscalerInstanceExtensions()
    {
        return Dlss::getInstanceExtensions();
    }

    std::span<const char* const> upscalerDeviceExtensions()
    {
        return Dlss::getDeviceExtensions();
    }
}
