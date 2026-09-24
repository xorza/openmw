#include <array>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/tangent.hpp>

namespace Rtx
{
    namespace
    {
        /// The six axes come back as they went, either way round, because an odd count of steps puts
        /// nought and one on a step.
        ///
        /// Along x is the square's `(1, 0)`: steps `16383 + 16383 = 32766 = 0x7FFE` and
        /// `0 + 16383 = 0x3FFF`, the second fifteen bits up, which is `0x1FFF8000`, and the present
        /// bit over them — the flipped bit as well for the other handedness.
        TEST(RtxTangentTest, theAxesComeBackExactlyWithTheirHandedness)
        {
            EXPECT_EQ(packTangent(osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f)), 0x9FFFFFFEu);
            EXPECT_EQ(packTangent(osg::Vec4f(1.0f, 0.0f, 0.0f, -1.0f)), 0xDFFFFFFEu);

            const std::array axes{
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(-1.0f, 0.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, -1.0f, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, -1.0f),
            };
            for (const osg::Vec3f& axis : axes)
                for (const float handedness : { 1.0f, -1.0f })
                {
                    const osg::Vec4f tangent(axis, handedness);
                    EXPECT_EQ(unpackTangent(packTangent(tangent)), tangent)
                        << axis.x() << ' ' << axis.y() << ' ' << axis.z() << ' ' << handedness;
                }
        }

        /// A tangent of no length is none, whatever its handedness, and so is one the generator
        /// left undefined. None reads back as nought.
        TEST(RtxTangentTest, noLengthIsNoTangent)
        {
            EXPECT_EQ(packTangent(osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f)), 0u);
            EXPECT_EQ(packTangent(osg::Vec4f(0.0f, 0.0f, 0.0f, -1.0f)), 0u);
            EXPECT_EQ(packTangent(osg::Vec4f(std::nanf(""), 0.0f, 0.0f, 1.0f)), 0u);
            EXPECT_EQ(unpackTangent(0u), osg::Vec4f());
        }

        /// Every direction with whole coordinates from minus three to three, above and below the
        /// equator and on it, comes back within the packing's bound, with its handedness, and the
        /// length it went in with is not stored.
        ///
        /// **The bound.** A stored coordinate is at most half a step, `1 / 32766`, from the one
        /// packed, which moves the point on the octahedron by at most
        /// `sqrt(du² + dv² + (|du| + |dv|)²) = sqrt(1.5) / 16383` on either side of the fold. The
        /// octahedron is nowhere nearer the centre than `1 / sqrt(3)`, so the projection onto the
        /// sphere stretches that by at most `sqrt(3)`: `sqrt(4.5) / 16383`, about `1.2948e-4`. The
        /// `1e-6` over it is the float arithmetic's, on unit values, either side of the steps.
        TEST(RtxTangentTest, everyDirectionComesBackWithinTheBoundOfTheSteps)
        {
            const float bound = std::sqrt(4.5f) / 16383.0f + 1e-6f;

            for (int x = -3; x <= 3; ++x)
                for (int y = -3; y <= 3; ++y)
                    for (int z = -3; z <= 3; ++z)
                    {
                        if (x == 0 && y == 0 && z == 0)
                            continue;

                        const osg::Vec3f direction(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                        for (const float handedness : { 1.0f, -1.0f })
                        {
                            const osg::Vec4f tangent(direction, handedness);
                            const std::uint32_t packed = packTangent(tangent);
                            const osg::Vec4f unpacked = unpackTangent(packed);

                            osg::Vec3f unit = direction;
                            unit.normalize();
                            EXPECT_LE((osg::Vec3f(unpacked.x(), unpacked.y(), unpacked.z()) - unit).length(), bound)
                                << x << ' ' << y << ' ' << z;
                            EXPECT_EQ(unpacked.w(), handedness) << x << ' ' << y << ' ' << z;
                            EXPECT_EQ(packTangent(tangent * 2.0f), packed)
                                << "the length was stored: " << x << ' ' << y << ' ' << z;
                        }
                    }
        }
    }
}
