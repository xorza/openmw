#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec4f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/pinning.h>
#include <components/rtxvulkan/barriers.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/dispatch.hpp>
#include <components/rtxvulkan/imageuse.hpp>

#include "support/device/harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        constexpr std::uint32_t sCount = 4096;

        using Vec3 = std::array<float, 3>;

        /// **The order `spirvpin.hpp` states, one rounding a step.** A plain operator here is one
        /// rounding: the tree compiles as ISO C++, which leaves `-ffp-contract` off, and targets no
        /// processor with a fused multiply-add for a compiler to contract into. Each fusion is
        /// `std::fma`, which rounds once.
        struct Pinned
        {
            static float dot(const Vec3& a, const Vec3& b)
            {
                return std::fma(a[2], b[2], std::fma(a[1], b[1], a[0] * b[0]));
            }

            static float dot4(const osg::Vec4f& a, const osg::Vec4f& b)
            {
                return std::fma(a[3], b[3], std::fma(a[2], b[2], std::fma(a[1], b[1], a[0] * b[0])));
            }

            static Vec3 cross(const Vec3& a, const Vec3& b)
            {
                return { std::fma(-a[2], b[1], a[1] * b[2]), std::fma(-a[0], b[2], a[2] * b[0]),
                    std::fma(-a[1], b[0], a[0] * b[1]) };
            }

            static Vec3 mix(const Vec3& x, const Vec3& y, float a)
            {
                Vec3 mixed{};
                for (std::size_t at = 0; at < 3; ++at)
                    mixed[at] = std::fma(y[at], a, x[at] * (1.0f - a));
                return mixed;
            }

            /// The matrix whose columns are `a`, `b` and `c`, times `v`.
            static Vec3 turn(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& v)
            {
                Vec3 turned{};
                for (std::size_t row = 0; row < 3; ++row)
                    turned[row] = std::fma(c[row], v[2], std::fma(b[row], v[1], a[row] * v[0]));
                return turned;
            }

            static Vec3 reflect(const Vec3& incident, const Vec3& normal)
            {
                const float twice = 2.0f * dot(normal, incident);
                Vec3 reflected{};
                for (std::size_t at = 0; at < 3; ++at)
                    reflected[at] = std::fma(-twice, normal[at], incident[at]);
                return reflected;
            }

            /// `v` scaled by the device's own inverse square root of `dot(v, v)`.
            static Vec3 normalize(const Vec3& v, float inverseLength)
            {
                return { v[0] * inverseLength, v[1] * inverseLength, v[2] * inverseLength };
            }

            /// From the device's own `(x - e0) / (e1 - e0)`.
            static float smoothstep(float along)
            {
                const float t = std::clamp(along, 0.0f, 1.0f);
                return (t * t) * std::fma(-2.0f, t, 3.0f);
            }

            /// With the device's own square root of what `k` comes to, held at nought.
            static Vec3 refract(const Vec3& incident, const Vec3& normal, float eta, float root)
            {
                const float cosine = dot(normal, incident);
                const float k = std::fma(-(eta * eta), std::fma(-cosine, cosine, 1.0f), 1.0f);
                if (k < 0.0f)
                    return { 0.0f, 0.0f, 0.0f };
                const float bend = std::fma(eta, cosine, root);
                Vec3 refracted{};
                for (std::size_t at = 0; at < 3; ++at)
                    refracted[at] = std::fma(-bend, normal[at], eta * incident[at]);
                return refracted;
            }

            /// From the device's own `floor(x / y)`.
            static float mod(float x, float y, float whole) { return std::fma(-y, whole, x); }

            static Vec3 faceforward(const Vec3& normal, const Vec3& incident, const Vec3& reference)
            {
                const bool facing = dot(reference, incident) < 0.0f;
                return facing ? normal : Vec3{ -normal[0], -normal[1], -normal[2] };
            }
        };

        Vec3 xyz(const osg::Vec4f& v)
        {
            return { v.x(), v.y(), v.z() };
        }

        /// What the probe has to write for `one`, in `pinning.comp`'s order, around the cores the
        /// device wrote in `device`: those four are the device's own and are taken as they are.
        std::array<float, Shaders::PINNING_RESULTS> expected(
            const Shaders::PinningCase& one, std::span<const float, Shaders::PINNING_RESULTS> device)
        {
            const Vec3 a = xyz(one.mA);
            const Vec3 b = xyz(one.mB);
            const Vec3 c = xyz(one.mC);

            std::array<float, Shaders::PINNING_RESULTS> results{};
            results[0] = Pinned::dot(a, b);
            results[1] = Pinned::dot4(one.mA, one.mB);
            const Vec3 crossed = Pinned::cross(a, b);
            const Vec3 mixed = Pinned::mix(a, b, one.mC.w());
            const Vec3 turned = Pinned::turn(a, b, c, { one.mA.w(), one.mB.w(), one.mC.w() });
            const Vec3 reflected = Pinned::reflect(a, b);
            const Vec3 facing = Pinned::faceforward(a, b, c);
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                results[2 + axis] = crossed[axis];
                results[5 + axis] = mixed[axis];
                results[8 + axis] = turned[axis];
                results[11 + axis] = reflected[axis];
                results[15 + axis] = facing[axis];
            }
            results[14] = std::fma(one.mA.x(), one.mB.x(), one.mC.x());

            const Vec3 normalized = Pinned::normalize(a, device[21]);
            const Vec3 refracted = Pinned::refract(a, b, one.mC.w(), device[27]);
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                results[18 + axis] = normalized[axis];
                results[24 + axis] = refracted[axis];
            }
            results[21] = device[21];
            results[22] = Pinned::smoothstep(device[23]);
            results[23] = device[23];
            results[27] = device[27];
            results[28] = Pinned::mod(one.mA.y(), one.mB.y(), device[29]);
            results[29] = device[29];
            return results;
        }

        /// Cases whose every component is a whole number of 2^-18 below 4 in magnitude: exact as a
        /// float, so both sides start from the same bits, and with 21 significant bits, so a
        /// product has 42 and every order and fusion rounds differently somewhere. Never a denormal,
        /// which the device may flush and the host keeps. Drawn off a fixed linear congruential
        /// sequence rather than a distribution, whose algorithm is the standard library's own. A case
        /// whose `smoothstep` edges or `mod` divisor come out equal to each other or to nought is
        /// drawn again, because it divides by nothing.
        std::vector<Shaders::PinningCase> makeCases()
        {
            std::uint64_t state = 0x9E3779B97F4A7C15ull;
            const auto draw = [&] {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                const auto whole = static_cast<std::int32_t>(state >> 43) - (1 << 20);
                return static_cast<float>(whole) / static_cast<float>(1 << 18);
            };
            const auto vector = [&] { return osg::Vec4f(draw(), draw(), draw(), draw()); };

            std::vector<Shaders::PinningCase> cases;
            cases.reserve(sCount);
            while (cases.size() < sCount)
            {
                const Shaders::PinningCase one{ .mA = vector(), .mB = vector(), .mC = vector() };
                if (one.mA.x() != one.mB.x() && one.mB.y() != 0.0f)
                    cases.push_back(one);
            }
            return cases;
        }

        struct RtxPinningTest : Testing::DeviceTest
        {
        };

        /// **What the build pins, the device computes to the bit**: every result of every case the
        /// same as the host's, which keeps the stated order with one rounding a step and one per
        /// fusion — around the device's own inverse square root, quotient, root and floor where an
        /// operation has one (`pinning.h`). A compile that reordered a sum, fused a step the build
        /// left apart or rounded a fusion twice would differ somewhere in the 106,496 results.
        ///
        /// **And the cases can tell those apart.** Summed from the far end, or with each product
        /// rounded before it is added, the dot product comes out otherwise on some cases — so an
        /// agreement is the order and the fusions, and not cases too easy to disagree on.
        TEST_F(RtxPinningTest, theDeviceComputesThePinnedOrderToTheBit)
        {
            const Device& device = getDevice();
            const ComputePipeline pipeline(device, sBindings, sizeof(Shaders::PinningConstants), {},
                Testing::getShaderDirectory() / "pinning.comp.spv", "pinning");
            CommandPool& pool = getPool();

            const std::vector<Shaders::PinningCase> cases = makeCases();
            Buffer source = Buffer::hostWritten(
                device, cases.size() * sizeof(Shaders::PinningCase), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "test");
            source.write(std::span<const Shaders::PinningCase>(cases));
            const Buffer written = Buffer::readBack(
                device, sizeof(float) * sCount * Shaders::PINNING_RESULTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "test");

            const VkDescriptorBufferInfo from{ source.getHandle(), 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo into{ written.getHandle(), 0, VK_WHOLE_SIZE };
            const std::array<VkWriteDescriptorSet, 2> writes{
                VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &from },
                VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &into },
            };

            pool.submitAndWait([&](VkCommandBuffer commands) {
                dispatch(commands, pipeline, writes, Shaders::PinningConstants{ .mCount = sCount },
                    groupsFor(sCount, Shaders::PINNING_WORKGROUP));
                Barriers done(commands);
                done.add(written.describeBarrier(Use::sBufferComputeWrite, Use::sBufferHostRead));
                done.flush();
            });

            std::vector<float> results(static_cast<std::size_t>(sCount) * Shaders::PINNING_RESULTS);
            std::memcpy(results.data(), written.map(), results.size() * sizeof(float));

            constexpr std::array<const char*, Shaders::PINNING_RESULTS> sNames{ "dot", "dot of four", "cross.x",
                "cross.y", "cross.z", "mix.x", "mix.y", "mix.z", "matrix.x", "matrix.y", "matrix.z", "reflect.x",
                "reflect.y", "reflect.z", "a product and a sum", "faceforward.x", "faceforward.y", "faceforward.z",
                "normalize.x", "normalize.y", "normalize.z", "the device's inverse length", "smoothstep",
                "the device's quotient", "refract.x", "refract.y", "refract.z", "the device's root", "mod",
                "the device's floor" };

            // Counted, with the first few named: a device that differs at all differs on thousands.
            constexpr std::size_t sNamed = 8;
            std::size_t differing = 0;
            std::size_t reversed = 0;
            std::size_t unfused = 0;
            std::size_t bent = 0;
            for (std::uint32_t at = 0; at < sCount; ++at)
            {
                const std::array<float, Shaders::PINNING_RESULTS> want = expected(cases[at],
                    std::span<const float, Shaders::PINNING_RESULTS>(
                        results.data() + static_cast<std::size_t>(at) * Shaders::PINNING_RESULTS,
                        Shaders::PINNING_RESULTS));
                for (std::uint32_t result = 0; result < Shaders::PINNING_RESULTS; ++result)
                {
                    const float got = results[at * Shaders::PINNING_RESULTS + result];
                    if (std::bit_cast<std::uint32_t>(got) == std::bit_cast<std::uint32_t>(want[result]))
                        continue;
                    if (++differing <= sNamed)
                        ADD_FAILURE() << sNames[result] << " of case " << at << ": the device wrote " << got
                                      << " and the pinned order " << want[result];
                }

                const Vec3 a = xyz(cases[at].mA);
                const Vec3 b = xyz(cases[at].mB);
                if (std::fma(a[0], b[0], std::fma(a[1], b[1], a[2] * b[2])) != want[0])
                    ++reversed;
                const float products[3] = { a[0] * b[0], a[1] * b[1], a[2] * b[2] };
                if (products[0] + products[1] + products[2] != want[0])
                    ++unfused;
                if (results[static_cast<std::size_t>(at) * Shaders::PINNING_RESULTS + 27] > 0.0f)
                    ++bent;
            }

            EXPECT_EQ(differing, 0u) << "results the device computed otherwise than the pinned order";
            EXPECT_GT(reversed, 0u) << "no case tells the far end's order from the pinned one";
            EXPECT_GT(unfused, 0u) << "no case tells rounded products from fused ones";
            EXPECT_GT(bent, 0u) << "no case refracts";
            EXPECT_LT(bent, static_cast<std::size_t>(sCount)) << "no case reflects wholly, which refracts nothing";
        }
    }
}
