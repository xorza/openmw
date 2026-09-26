#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/refusal.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "support/device/harness.hpp"
#include "support/device/heldsubmit.hpp"
#include "support/device/memorylimits.hpp"
#include "support/device/texturepasses.hpp"
#include "support/testtexture.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxTextureArrayTest : Testing::DeviceTest
        {
        };

        /// Writing a set's descriptors waits for the submit that bound the set, and for nothing
        /// else.
        ///
        /// **The one per-copy resource that is not a buffer.** A set is bound by handle, so no
        /// address names it, and its bindings allow an update after a bind — but not of a
        /// descriptor a pending trace samples, and a trace samples whichever slots its materials
        /// name. `getSet` is the hand-out and stamps the set the way `Buffer::addressFor` stamps a
        /// buffer; `finishReads` waits for that stamp. Held on the queue while this thread waits,
        /// so the wait cannot return before the hold opens and lasts at least the hold's length.
        TEST_F(RtxTextureArrayTest, syncingASetWaitsForTheSubmitThatBoundIt)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            const SetLayout layout = TextureArray::describeLayout(device);
            const Testing::TexturePassSet passes(device);
            Batch setup(pool);
            TextureArray textures(device, setup, layout, passes.mPasses, 1);
            setup.flush();

            // An arrival, owed to every set: what `sync` has to write once the set is free. Ahead
            // of the hold, because a flush behind a held submit waits for the hold.
            const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const TextureData arrived = Testing::describeTexel(white, 0);
            Batch arrival(pool);
            std::vector<Refusal> refused;
            textures.write(arrival, std::span(&arrived, 1), refused);
            EXPECT_TRUE(refused.empty());
            arrival.flush();

            Testing::HeldSubmit hold(device);
            const VkCommandBuffer binder = pool.allocate(1).front();
            pool.begin(binder);
            EXPECT_NE(textures.getSet(FrameSlot{ 0 }), VK_NULL_HANDLE);
            hold.submit(binder);

            // Long enough that a wait which returned at once is told from one that waited, under three
            // shards of this binary sharing the device.
            constexpr std::chrono::milliseconds held{ 200 };
            const auto asked = std::chrono::steady_clock::now();
            hold.releaseAfter(held);

            textures.finishReads(FrameSlot{ 0 });
            EXPECT_GE(std::chrono::steady_clock::now() - asked, held) << "the sync did not wait for the set's binder";

            // A wait that returned early writes this into a set a submit the hold still keeps on
            // the queue has bound, which is the write the assert fires on.
            textures.sync(FrameSlot{ 0 });

            // And the other set, which nothing bound, waits for nothing.
            const auto other = std::chrono::steady_clock::now();
            textures.finishReads(FrameSlot{ 1 });
            EXPECT_LT(std::chrono::steady_clock::now() - other, held);
            textures.sync(FrameSlot{ 1 });
        }

        /// An arrival is held to the largest side it fits the room at, the whole of it to the one
        /// side: a file from its first level within the side, a bake as large as its source is held
        /// to, and a composite, whose side is the renderer's own, at what it costs. Where the
        /// arrival fits at no side, its files are held to the side they fit at without the
        /// composite, which gives way.
        ///
        /// **Hand-computed.** The ladder is 64, 32 and 16 texels square, four bytes a texel: 16384,
        /// 4096 and 1024 bytes, 21504 from the top, 5120 from the second level and 1024 from the
        /// third, and every texture brings a shading map of 32 by 32 halves, 2048 bytes. The
        /// ladder alone is 23552 bytes whole, 7168 held to 32 and 3072 held to 16, and no smaller
        /// for anything below 16, where it has no level. A bake of it is four bytes a texel over the
        /// levels the ladder stands with — 5376 texels from 64 and 1280 from 32, which with its map
        /// is what the ladder itself comes to. A composite is 512 square with its chain, 349525 texels, 1400148 bytes
        /// with its map whatever the side — so a byte short of it and the ladder's least, the
        /// arrival fits nowhere and the ladder is held as though the composite were not there.
        TEST_F(RtxTextureArrayTest, anArrivalIsHeldToTheLargestSideItFitsTheRoomAt)
        {
            Device& device = getDevice();
            const SetLayout layout = TextureArray::describeLayout(device);
            const Testing::TexturePassSet passes(device);
            Batch setup(getPool());
            const TextureArray textures(device, setup, layout, passes.mPasses, 3);
            setup.flush();

            const std::uint32_t limit = textures.getSideLimit();

            Testing::TestTexture ladder;
            Testing::paintLevels(ladder, 64, 64, 3, "ladder");
            const TextureData composite{ .mSlot = 1, .mSource = TextureSource::GroundComposite, .mFrom = 0 };
            const TextureData bake{ .mSlot = 2, .mSource = TextureSource::SpriteBake, .mFrom = 0 };

            const std::array alone{ ladder.mData };
            const std::array ground{ ladder.mData, composite };
            const std::array baked{ ladder.mData, bake };

            struct Case
            {
                std::span<const TextureData> mArrived;
                VkDeviceSize mRoom;
                std::uint32_t mSide;
            };
            const std::array cases{
                Case{ alone, 23552, limit },
                Case{ alone, 23551, 32 },
                Case{ alone, 7168, 32 },
                Case{ alone, 7167, 16 },
                Case{ alone, 3072, 16 },
                Case{ alone, 3071, 1 },
                Case{ alone, 0, 1 },
                Case{ ground, 1400148 + 23552, limit },
                Case{ ground, 1400148 + 23551, 32 },
                Case{ ground, 1400148 + 3072, 16 },
                Case{ ground, 1400147 + 3072, limit },
                Case{ ground, 23551, 32 },
                Case{ baked, 23552 + 23552, limit },
                Case{ baked, 23552 + 23551, 32 },
                Case{ baked, 7168 + 7168, 32 },
                Case{ baked, 7168 + 7167, 16 },
            };

            for (const Case& one : cases)
                EXPECT_EQ(textures.chooseSide(one.mArrived, one.mRoom), one.mSide)
                    << one.mArrived.size() << " textures in " << one.mRoom << " bytes";
        }

        /// A texture past the side the device takes stands from its first level within the side,
        /// and one with no level within it draws the stand-in, refused and saying why.
        ///
        /// **One texel wider than the device takes**, so the case is the device's own and no
        /// larger than a row: the second level is half that, within the side, and is what stands —
        /// four bytes a texel and the map's 2048.
        ///
        /// **The slot's texel word says which stands**: the level's count, and the stand-in's four by
        /// four with `TEXTURE_STANDS_IN` over it — as it does for a slot described as the stand-in,
        /// which is how the builder describes a file that does not read.
        TEST_F(RtxTextureArrayTest, aTexturePastTheDevicesSideBeginsAtItsFirstLevelWithinIt)
        {
            Device& device = getDevice();
            const SetLayout layout = TextureArray::describeLayout(device);
            const Testing::TexturePassSet passes(device);
            Batch setup(getPool());
            TextureArray textures(device, setup, layout, passes.mPasses, 2);
            setup.flush();

            const std::uint32_t limit = textures.getSideLimit();

            Testing::TestTexture wide;
            Testing::paintLevels(wide, limit + 1, 1, 2, "wide");
            Testing::TestTexture single;
            Testing::paintLevels(single, limit + 1, 1, 1, "single");
            single.mData.mSlot = 1;
            Testing::TestTexture unread;
            Testing::paintLevels(unread, 4, 4, 1, "unread");
            unread.mData.mSlot = 2;
            unread.mData.mSource = TextureSource::StandIn;

            const std::array arrived{ wide.mData, single.mData, unread.mData };
            std::vector<Refusal> refused;
            Batch arrival(getPool());
            textures.write(arrival, arrived, refused);
            arrival.flush();

            ASSERT_EQ(refused.size(), 1u);
            EXPECT_EQ(refused[0].mKind, Refused::Texture);
            EXPECT_EQ(refused[0].mName, "single");
            EXPECT_EQ(refused[0].mWhy,
                "its smallest level is " + std::to_string(limit + 1) + " by 1 texels, and the device takes "
                    + std::to_string(limit) + " a side");

            const TexturesHeld held = textures.getHeld();
            EXPECT_EQ(held.mCount, 1u) << "a texture the device takes at no level stood";
            EXPECT_EQ(held.mBytes, VkDeviceSize{ 4 } * ((limit + 1) >> 1) + 2048);
            EXPECT_EQ(held.mReduced, 1u) << "a texture standing from its second level was not counted as smaller";
            EXPECT_EQ(textures.getSide(), limit) << "the device's side is not the room's";

            EXPECT_EQ(textures.getTexels(0), (limit + 1) >> 1) << "the level that stands";
            EXPECT_EQ(textures.getTexels(1), Shaders::TEXTURE_STANDS_IN | 16u) << "past the side";
            EXPECT_EQ(textures.getTexels(2), Shaders::TEXTURE_STANDS_IN | 16u) << "described as the stand-in";
        }

        /// A texture the device has no room for comes down a level at a time, and one it has room
        /// for at no level draws the stand-in, refused and saying why.
        ///
        /// **Room for one block, and every gap filled.** The ladder's first level, 3072 square at
        /// four bytes, is 36 MiB, past half a block and so an allocation of its own; its map comes
        /// first and opens the block, which leaves a megabyte under the ceiling, and the level is
        /// refused although the side chosen for the arrival counted the block as room for it. Its
        /// second level, 1536 square, is 9 MiB and fits the block: 9437184 bytes and the map's 2048.
        /// The single level of the same 36 MiB has nothing further down. The slot's texel word says
        /// the stand-in while it stands in, and is the file's own count once it stands.
        TEST_F(RtxTextureArrayTest, aTextureTheDeviceHasNoRoomForComesDownALevelOrDrawsTheStandIn)
        {
            Device& device = getDevice();
            MemoryAllocator& memory = device.getMemory();
            const SetLayout layout = TextureArray::describeLayout(device);
            const Testing::TexturePassSet passes(device);
            Batch setup(getPool());
            TextureArray textures(device, setup, layout, passes.mPasses, 2);
            setup.flush();

            Testing::TestTexture ladder;
            Testing::paintLevels(ladder, 3072, 3072, 2, "ladder");
            Testing::TestTexture lone;
            Testing::paintLevels(lone, 3072, 3072, 1, "lone");
            lone.mData.mSlot = 1;

            const Testing::NoRoomForContent full(device);

            const Testing::BudgetLimit oneBlock(
                memory, Testing::budgetAbove(memory, MemoryUse::Texture, VkDeviceSize{ 65 } << 20));

            std::vector<Refusal> refused;
            {
                Batch arrival(getPool());
                textures.write(arrival, std::span(&ladder.mData, 1), refused);
                arrival.flush();
            }
            EXPECT_TRUE(refused.empty()) << "a texture with a level the device had room for was refused";
            EXPECT_EQ(textures.getSide(), textures.getSideLimit()) << "the room chose the level, and not the device";
            EXPECT_EQ(textures.getHeld().mBytes, VkDeviceSize{ 1536 } * 1536 * 4 + 2048);
            EXPECT_EQ(textures.getHeld().mReduced, 1u);

            {
                Batch arrival(getPool());
                textures.write(arrival, std::span(&lone.mData, 1), refused);
                arrival.flush();
            }
            ASSERT_EQ(refused.size(), 1u);
            EXPECT_EQ(refused[0].mKind, Refused::Texture);
            EXPECT_EQ(refused[0].mName, "lone");
            EXPECT_EQ(refused[0].mWhy, "no device memory is left for it");
            EXPECT_EQ(textures.getHeld().mCount, 1u) << "a texture with no room stood";
            EXPECT_EQ(textures.getTexels(0), 1536u * 1536u);
            EXPECT_EQ(textures.getTexels(1), Shaders::TEXTURE_STANDS_IN | 16u) << "no room";

            refused.clear();
            {
                const Testing::BudgetLimit ample(memory, ~VkDeviceSize{ 0 });
                Batch arrival(getPool());
                textures.write(arrival, std::span(&lone.mData, 1), refused);
                arrival.flush();
            }
            EXPECT_TRUE(refused.empty());
            EXPECT_EQ(textures.getHeld().mCount, 2u);
            EXPECT_EQ(textures.getHeld().mReduced, 1u) << "a texture standing as its file was counted as smaller";
            EXPECT_EQ(textures.getTexels(1), 3072u * 3072u) << "a slot that stands at last still says the stand-in";

            // Before the array goes: what the writes replaced is buried, and the fillers give their
            // room back after it.
            device.waitIdle();
            device.collectIdle();
        }
    }
}
