#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/decodecolour.hpp>
#include <components/sceneutil/util.hpp>

namespace Rtx
{
    namespace
    {
        /// The packing is `0xAABBGGRR`: red in the low byte.
        ///
        /// Reading it the other way round turns every candle in the game blue, which is the kind of
        /// wrong that looks deliberate.
        TEST(RtxDecodeColourTest, aColourIsRedFirstAndDecodedOutOfDisplaySpace)
        {
            EXPECT_EQ(decodeColour(0x00FFFFFF), osg::Vec3f(1.0f, 1.0f, 1.0f));
            EXPECT_EQ(decodeColour(0), osg::Vec3f(0.0f, 0.0f, 0.0f));

            const osg::Vec3f candle = decodeColour(0x000080FF);
            EXPECT_FLOAT_EQ(candle.x(), 1.0f) << "red is the low byte";
            EXPECT_EQ(candle.z(), 0.0f) << "and blue the third";

            // Mid grey is where the two spaces diverge most, so it is where skipping the decode is
            // most visible: 128 of 255 is 0.50196 encoded and
            // ((0.50196 + 0.055) / 1.055)^2.4 = 0.21586 linear.
            EXPECT_NEAR(candle.y(), 0.21586f, 1e-5f);
            EXPECT_NEAR(decodeColour(0x00808080).x(), 0.21586f, 1e-5f);
        }

        /// The colour the game hands over is the same colour, and takes the same decode.
        ///
        /// **OpenMW's own comment calls its pipeline linear and it is not the numbers it is talking
        /// about.** `SceneUtil::colourFromRGB` divides a record's bytes by 255 and stops, so what
        /// settles on a light, a fog or the sky is display-encoded exactly as the record was — which
        /// is why the game path decodes rather than passing it through, and why the two must land on
        /// the same value for the same record or a screenshot and the game are two different worlds.
        TEST(RtxDecodeColourTest, aColourTheGameHasAlreadyUnpackedDecodesToTheSameLight)
        {
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
                EXPECT_EQ(decodeColour(packed), decodeColour(SceneUtil::colourFromRGB(packed))) << "packed " << packed;

            // The alpha is dropped rather than carried: nothing downstream of a light has a use for
            // one, and a fog colour arrives with its own.
            EXPECT_EQ(decodeColour(osg::Vec4f(1.0f, 1.0f, 1.0f, 0.25f)), osg::Vec3f(1.0f, 1.0f, 1.0f));

            // The same mid grey, reached the other way: 128 of 255 encoded is 0.21586 linear.
            EXPECT_NEAR(decodeColour(osg::Vec4f(128.0f / 255.0f, 0.0f, 0.0f, 1.0f)).x(), 0.21586f, 1e-5f);
        }
    }
}
