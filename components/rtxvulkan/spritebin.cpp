#include "spritebin.hpp"

#include <cassert>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/spritebin.h>
#include <components/rtx/shaders/spriteshade.h>

#include "bufferusage.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "imageuse.hpp"
#include "spritepasses.hpp"
#include "timeline.hpp"

namespace Rtx
{
    SpriteBin::SpriteBin(const Device& device)
        : mDevice(device)
        , mSprites(Buffer::hostWritten(device, 0, sTableFilledUsage, "binned sprites"))
        , mEmitterFrames(Buffer::hostWritten(device, 0, sTableUsage, "emitter frames"))
        , mOrder(Buffer::hostWritten(device, 0, sTableUsage, "sprite order"))
        , mRects(Buffer::hostWritten(device, 0, sTableUsage, "sprite rects"))
        , mTileList(Buffer::hostWritten(device, 0, sTableFilledUsage, "sprite tile list"))
        , mPresence(Buffer::hostWritten(device, 0, sTableFilledUsage, "sprite presence"))
        , mReport(Buffer::readBack(device, sizeof(std::uint32_t), sTableUsage, "sprite report"))
    {
        // Every table exists from here, whether or not anything is binned: a frame carries the
        // address of the sprites and the list, and a scene with no sprites bins none.
        mReport.writable<std::uint32_t>(0, 1).front() = 0;
    }

    void SpriteBin::take(const SpriteSource& source, const Shaders::Camera& camera, const VkCommandBuffer commands)
    {
        const Timeline& timeline = mDevice.getTimeline();
        const std::uint32_t count = source.mSpriteCount;
        const VkDeviceSize bytes = VkDeviceSize{ count } * sizeof(Shaders::GpuSprite);

        // At twice the high-water mark past it, as every table a frame writes: a storm thickens by a
        // few sprites a frame, and a table sized to each count would be made again on every one.
        outgrow(mSprites, mDevice, BufferKind::HostWritten, bytes, sTableFilledUsage, "binned sprites");
        outgrow(mOrder, mDevice, BufferKind::HostWritten,
            VkDeviceSize{ count } * Shaders::SPRITE_SHADE_LIGHTS * sizeof(std::uint64_t), sTableUsage, "sprite order");
        outgrow(mRects, mDevice, BufferKind::HostWritten, VkDeviceSize{ count } * sizeof(std::uint64_t), sTableUsage,
            "sprite rects");
        outgrow(mEmitterFrames, mDevice, BufferKind::HostWritten,
            VkDeviceSize{ source.mEmitterCount } * sizeof(Shaders::GpuEmitterFrame), sTableUsage, "emitter frames");
        outgrow(mPresence, mDevice, BufferKind::HostWritten,
            VkDeviceSize{ Shaders::spriteTilesIn(camera.mWidth, camera.mHeight) } * sizeof(std::uint32_t),
            sTableFilledUsage, "sprite presence");

        // Sized from what the last bin here said it needed, with room over it, because the need is
        // only known once the tiles are counted and that happens on the device. Read where the
        // host has waited past the submit the report rode — the frame ring's wait, a frame or two
        // on, and never a question to the device, so which frame first reads a report is a
        // function of the frames and not of the clock; a bin whose report is not yet waited for
        // keeps the capacity it has, which `SpriteListSize` never shrinks anyway. Here with the
        // rest, because the frame block carries this table's address too.
        const std::uint32_t reported
            = timeline.hasFinished(mReport.getNamedUntil()) ? *static_cast<const std::uint32_t*>(mReport.map()) : 0;
        mListSize.sizeFor(Shaders::spriteTilesIn(camera.mWidth, camera.mHeight), count, reported);

        // Past the need as the tables above are; the pass is told the capacity the rule gave, which
        // the buffer holds.
        outgrow(
            mTileList, mDevice, BufferKind::HostWritten, mListSize.getBytes(), sTableFilledUsage, "sprite tile list");

        // The placement's table, whole, because the shade writes over what it reads: the copy is
        // what lets a second trace against the same placement — a picture, or the frame after a
        // picture — start from sprites nothing has shaded. On the queue and not from the host,
        // so nothing a submit in flight reads is written under it. Whoever read this bin's tables
        // last is behind on the queue, and the barrier `CommandPool::begin` records at the head
        // of these commands is what orders the copy after it. Handed to the launch and the
        // dispatch both, because the shelter launch writes it before the shade does.
        if (bytes > 0)
            source.mSprites->copyTo(commands, mSprites, bytes);
        mSprites.transition(commands, Use::sBufferCopyWrite, Use::sBufferShaderReadWrite);
    }

    void SpriteBin::record(VkCommandBuffer commands, const Binning& what)
    {
        const SpriteShadePass& shading = what.mShading;
        const SpriteBinPass& pass = what.mPass;
        const SpriteSource& source = what.mSource;
        const osg::Vec3f& origin = what.mOrigin;
        const Shaders::Camera& camera = what.mCamera;
        const osg::Vec3f& toSun = what.mToSun;
        GpuTimer* const timer = what.mTimer;

        const std::uint32_t count = source.mSpriteCount;
        assert(mSprites.getSize() >= VkDeviceSize{ source.mSpriteCount } * sizeof(Shaders::GpuSprite)
            && "a bin recorded over sprites it never took");

        shading.record(commands,
            Shaders::SpriteShadeConstants{
                .mSprites = mSprites.addressFor(),
                .mEmitters = source.mEmitters,
                .mOrder = mOrder.addressFor(),
                .mToSun = toSun,
                .mEmitterCount = source.mEmitterCount,
                .mCount = count,
            },
            timer);

        pass.record(commands,
            Shaders::SpriteBinConstants{
                .mSprites = mSprites.addressFor(),
                .mEmitters = source.mEmitters,
                .mRects = mRects.addressFor(),
                .mList = mTileList.addressFor(),
                .mReport = mReport.addressFor(),
                .mPresences = source.mPresences,
                .mPresence = mPresence.addressFor(),
                .mOrigin = origin,
                .mCamera = camera,
                .mCount = count,
                .mCapacity = mListSize.getCapacity(),
                .mPresenceCount = source.mPresenceCount,
            },
            mTileList, mPresence, timer);

        // What the next bin here sizes its list from. A fence's access scope is the device's, so
        // without this the figure is whatever the caches held.
        mReport.orderForHostRead(commands);
    }

    VkDeviceSize SpriteBin::getBytes() const
    {
        return mSprites.getSize() + mEmitterFrames.getSize() + mOrder.getSize() + mRects.getSize() + mTileList.getSize()
            + mPresence.getSize() + mReport.getSize();
    }
}
