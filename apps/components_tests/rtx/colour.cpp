#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/colour.hpp>
#include <components/sceneutil/util.hpp>

namespace Rtx
{
    namespace
    {
        /// The byte overload is the float curve at that byte, and not an approximation of it.
        ///
        /// **Every one of the two hundred and fifty-six, bit for bit.** A texel arrives as a byte, so
        /// the only arguments the curve is ever given by a decoder are `k / 255` — which is what
        /// makes a table over `k` the same answer rather than a faster one. A `NEAR` here would say
        /// the pictures may differ by a rounding, and they may not.
        TEST(RtxSrgbTest, theByteCurveIsTheFloatCurveAtEveryStoredValue)
        {
            for (std::uint32_t stored = 0; stored < 256; ++stored)
            {
                const auto byte = static_cast<std::uint8_t>(stored);
                EXPECT_EQ(toLinear(byte), toLinear(static_cast<float>(stored) / 255.0f)) << "at " << stored;
            }
        }

        /// A value that is not one of the 256 is not answered as though it were.
        ///
        /// **What the float overload's recovery has to refuse.** It finds the byte a value would
        /// round to and divides it back before it believes the table, so a value one place off a
        /// stored byte — and a value halfway between two — must come out of the curve instead.
        /// Snapping either to the nearest byte would be a guess, and it would move the picture.
        TEST(RtxSrgbTest, aValueThatIsNotAStoredByteTakesTheCurve)
        {
            // One representable step above 127 / 255, which is as close to a stored byte as a value
            // can be without being one.
            const float justOver = std::nextafter(127.0f / 255.0f, 1.0f);
            EXPECT_NE(toLinear(justOver), toLinear(std::uint8_t{ 127 })) << "a value one place off was snapped";
            EXPECT_GT(toLinear(justOver), toLinear(std::uint8_t{ 127 })) << "the curve does not rise";

            // And halfway between two bytes, which lands strictly between their two answers.
            EXPECT_GT(toLinear(0.5f), toLinear(std::uint8_t{ 127 }));
            EXPECT_LT(toLinear(0.5f), toLinear(std::uint8_t{ 128 }));

            // A negative light's colour is `-k / 255`, which is none of the 256 and takes the
            // linear leg — the sign is what a refusal reads, and the curve is odd about nothing.
            EXPECT_EQ(toLinear(-10.0f / 255.0f), -10.0f / 255.0f / 12.92f);
        }

        /// The curve itself, anchored where it can be stated outright.
        ///
        /// **What the test above cannot say.** That one compares two spellings of the curve, so it
        /// passes whether or not either is right. These are the three points sRGB names.
        TEST(RtxSrgbTest, nothingStaysNothingAndFullStaysFull)
        {
            EXPECT_EQ(toLinear(std::uint8_t{ 0 }), 0.0f);
            EXPECT_EQ(toLinear(std::uint8_t{ 255 }), 1.0f);

            // The knee, which is the one place the curve is two rules: 10 / 255 = 0.0392 is under
            // `0.04045` and takes the linear leg, and 11 / 255 = 0.0431 is over it and takes the
            // power leg, which lands above what the linear one would have given.
            EXPECT_EQ(toLinear(std::uint8_t{ 10 }), 10.0f / 255.0f / 12.92f);
            EXPECT_GT(toLinear(std::uint8_t{ 11 }), 11.0f / 255.0f / 12.92f);
        }
    }

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
