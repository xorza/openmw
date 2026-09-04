#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include <components/rtx/cloudshell.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/skybuilder.hpp>

namespace Rtx
{
    namespace
    {
        /// A layer to hang a deck on, standing in for what the cloud mesh is read for. Morrowind's
        /// own comes to 0.711 tiles up at a curvature of 0.057; the numbers here are only distinct.
        const Rtx::CloudShell sShell{
            .mTiles = osg::Vec2f(0.75f, -0.75f), .mCurvature = 0.06f, .mRings = osg::Vec3f(1.0f, 1.5f, 2.0f)
        };

        /// What a deck radiates, for the tests that are about everything else it carries. Two
        /// distinct colours, so a field written from the wrong one shows.
        const Rtx::DeckLight sLight{ .mLit = osg::Vec3f(0.5f, 0.6f, 0.7f), .mShadowed = osg::Vec3f(0.1f, 0.2f, 0.3f) };

        /// A blend that is not a number is not a sky.
        ///
        /// **The bug this is here for turned the game's sky black and left the harness's alone.**
        /// `Weather::cloudBlendFactor` divides the transition by `Clouds_Maximum_Percent`, and the
        /// shipped fallbacks record none for ash or blight — so a transition into either handed back
        /// a NaN. The rasterizer survives one, because a NaN opacity draws nothing and the sky it
        /// already had stays; a tracer mixes its whole sky by it and gets a NaN back, which is black.
        ///
        /// **And `std::clamp` does not catch it**, which is the part worth a test rather than a
        /// comment: it asks whether the value is *outside* the range, both comparisons are false for
        /// a NaN, and it hands the NaN straight back. The boundary has to ask the question the other
        /// way round.
        TEST(RtxSkyBuilderTest, aCloudBlendThatIsNotANumberComesOutAsNoBlendAtAll)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const auto deck = [&](float blend) {
                return describeClouds(Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN, blend, sLight, north,
                    north, 0.0f, textures);
            };

            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mBlend, 0.0f) << "a NaN is no crossing";
            EXPECT_EQ(deck(-1.0f).mBlend, 0.0f) << "and neither is anything under nought";
            EXPECT_EQ(deck(7.0f).mBlend, 1.0f) << "or over one";
            EXPECT_EQ(deck(0.25f).mBlend, 0.25f) << "while a real one is passed through untouched";

            // The rest of the deck still says what it says, so the guard is on the blend and not a
            // bail-out that would have taken the clouds with it.
            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mTexture, 3u);
            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mNext, 5u);
            EXPECT_GT(deck(std::numeric_limits<float>::quiet_NaN()).mOpacity, 0.0f);
        }

        /// A weather the content files give no cloud texture has no deck, rather than a grey one.
        ///
        /// Ash and blight name none in the shipped fallbacks, and Solstheim's two name files the
        /// archives do not hold — and an unreadable texture is drawn as the stand-in, which is an
        /// opaque mid grey and over a deck is the entire sky.
        TEST(RtxSkyBuilderTest, aWeatherWithNoCloudTextureGetsNoDeck)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mShell = sShell;

            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_CLEAR), 3u);
            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_ASHSTORM), Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_COUNT + 4u), Rtx::Shaders::NO_TEXTURE)
                << "and an index past the ten is not a lookup";

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const Rtx::Shaders::CloudDeck none = describeClouds(Rtx::Shaders::WEATHER_ASHSTORM,
                Rtx::Shaders::WEATHER_ASHSTORM, 0.0f, sLight, north, north, 0.0f, textures);

            EXPECT_EQ(none.mOpacity, 0.0f) << "nothing to draw, said the way an interior says it";
            EXPECT_EQ(none.mTexture, Rtx::Shaders::NO_TEXTURE);

            // And a sky whose mesh gave up no shape has nowhere to hang one, whatever sheet the
            // weather names.
            SkyContent unhung = textures;
            unhung.mShell = Rtx::CloudShell{};
            EXPECT_EQ(describeClouds(Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_CLEAR, 0.0f, sLight, north,
                          north, 0.0f, unhung)
                          .mOpacity,
                0.0f);
        }

        /// A deck is lit by what stands over it, and its own body is what keeps the sun off its base.
        ///
        /// **The engine paints its deck and this one lights it.** `Sky::cloudColour` adds an eighth
        /// to a display-encoded fog, which read as light is a third again over a clear day and eight
        /// times over the same weather's night — so a painted deck arrived at midnight eight times
        /// the sky it covers. Lit, it is `CLOUD_TRANSMISSION` of whatever reaches it, whatever hour
        /// that is.
        ///
        /// A sun of 8, 4, 2 straight overhead, a sky mean of 0.4, 0.8, 1.2, and a moon a third of
        /// the way up delivering 0.4, 0.8, 1.2 to a face square to it:
        ///
        ///     sky   0.4 * 0.25                    = 0.1
        ///     sun   8 * 1 * 0.25 / pi             = 0.636620
        ///     moon  0.4 * 0.5 * 0.25 / pi         = 0.015915
        ///
        /// so red comes to 0.752535 lit and 0.1 shadowed, and the other two follow their own terms.
        TEST(RtxSkyBuilderTest, aDeckIsLitByWhatStandsOverIt)
        {
            const osg::Vec3f skyMean(0.4f, 0.8f, 1.2f);

            Rtx::Sun sun;
            sun.mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
            sun.mIrradiance = osg::Vec3f(8.0f, 4.0f, 2.0f);

            std::array<Rtx::MoonPlacement, 2> moons{};
            moons[0].mDirection = osg::Vec3f(0.0f, std::sqrt(0.75f), 0.5f);
            moons[0].mIrradiance = osg::Vec3f(0.4f, 0.8f, 1.2f);

            const Rtx::DeckLight day = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_NEAR(day.mShadowed.x(), 0.1f, 1.0e-6f);
            EXPECT_NEAR(day.mShadowed.y(), 0.2f, 1.0e-6f);
            EXPECT_NEAR(day.mShadowed.z(), 0.3f, 1.0e-6f);

            EXPECT_NEAR(day.mLit.x(), 0.752535f, 1.0e-5f);
            EXPECT_NEAR(day.mLit.y(), 0.550141f, 1.0e-5f);
            EXPECT_NEAR(day.mLit.z(), 0.506901f, 1.0e-5f);

            // **At night the two differ by the moons alone**, which is the case the whole change is
            // for: no sun over the layer, and a deck that is a quarter of the sky it hides.
            sun.mIrradiance = osg::Vec3f();
            const Rtx::DeckLight night = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_EQ(night.mShadowed, day.mShadowed);
            EXPECT_NEAR(night.mLit.x() - night.mShadowed.x(), 0.015915f, 1.0e-5f);

            // A moon under the horizon delivers nothing to a layer over it, and needs no test of its
            // own to say so — the cosine does it.
            moons[0].mDirection = osg::Vec3f(0.0f, std::sqrt(0.75f), -0.5f);
            const Rtx::DeckLight down = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_EQ(down.mLit, down.mShadowed);
            EXPECT_EQ(down.mShadowed, day.mShadowed);
        }

        /// The shape the deck hangs on is the mesh's, and it is passed through untouched.
        TEST(RtxSkyBuilderTest, theLayersOwnShapeReachesTheShader)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const Rtx::Shaders::CloudDeck deck = describeClouds(
                Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_CLEAR, 0.0f, sLight, north, north, 0.0f, textures);

            EXPECT_EQ(deck.mCurvature, sShell.mCurvature);
            EXPECT_EQ(deck.mRings, sShell.mRings);

            // The mesh's height in tiles reaches the shader as a tile's own width, which is what a
            // chosen altitude turns it into — and it keeps the mesh's sign, because the sheet's `v`
            // runs the other way and dropping that mirrors every cloud.
            EXPECT_EQ(deck.mPerTile, sShell.mTiles / Rtx::sCloudAltitude);
            EXPECT_EQ(deck.mAltitude, Rtx::sCloudAltitude);
            EXPECT_LT(deck.mPerTile.y(), 0.0f);
        }

        /// Each sheet is turned by its own weather's storm, and the turn is the storm itself.
        ///
        /// **The engine turns each of its two cloud meshes separately**, so a transition into an
        /// ashstorm drives the sheet ahead off Red Mountain while the one overhead still runs due
        /// north. Turning a crossing back by that angle wants its cosine and its sine, and for a
        /// unit direction measured from north those are the direction's own two components,
        /// swapped — so no angle is taken and none is undone.
        TEST(RtxSkyBuilderTest, eachSheetIsTurnedByItsOwnWeathersStorm)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const osg::Vec3f east(1.0f, 0.0f, 0.0f);

            const Rtx::Shaders::CloudDeck deck = describeClouds(
                Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN, 0.5f, sLight, north, east, 0.0f, textures);

            EXPECT_EQ(deck.mBearing, osg::Vec2f(1.0f, 0.0f)) << "due north is no turn at all";
            EXPECT_EQ(deck.mNextBearing, osg::Vec2f(0.0f, 1.0f)) << "and due east is a quarter of one";

            // **A direction nobody stated is zero, and a bearing of zero collapses the whole sheet
            // onto one texel.** `WeatherResult` names the weather ahead's storm only while one is
            // arriving, and leaves the field where the last transition left it otherwise.
            const Rtx::Shaders::CloudDeck settled = describeClouds(Rtx::Shaders::WEATHER_CLEAR,
                Rtx::Shaders::WEATHER_RAIN, 0.5f, sLight, north, osg::Vec3f(), 0.0f, textures);

            EXPECT_EQ(settled.mNextBearing, osg::Vec2f(1.0f, 0.0f)) << "which reads as due north";
        }

        /// The level a sheet's texels are read against crosses with the sheet, and falls back with it.
        ///
        /// **The fall-back is the half worth a test.** The shader samples the weather ahead only
        /// where that weather names a sheet, and reads the near one twice where it does not — so a
        /// mean carried half way toward a weather that draws nothing would read every texel of the
        /// near sheet against a level no sheet has, and lift or drop the whole deck by it.
        TEST(RtxSkyBuilderTest, theLevelASheetIsReadAgainstCrossesWithTheSheet)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;
            textures.mCloudMean[Rtx::Shaders::WEATHER_CLEAR] = 0.4f;
            textures.mCloudMean[Rtx::Shaders::WEATHER_RAIN] = 0.2f;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const auto deck = [&](std::uint32_t next, float blend) {
                return describeClouds(Rtx::Shaders::WEATHER_CLEAR, next, blend, sLight, north, north, 0.0f, textures);
            };

            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_RAIN, 0.0f).mMean, 0.4f);
            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_RAIN, 1.0f).mMean, 0.2f);

            // A quarter of the way across: `0.75 * 0.4 + 0.25 * 0.2`.
            EXPECT_NEAR(deck(Rtx::Shaders::WEATHER_RAIN, 0.25f).mMean, 0.35f, 1.0e-6f);

            // Ash names no sheet in the shipped fallbacks, so the shader reads the clear one at both
            // ends of that crossing and this stays the clear one's whatever the blend says.
            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_ASHSTORM, 0.5f).mMean, 0.4f);
            EXPECT_EQ(textures.meanOf(Rtx::Shaders::WEATHER_COUNT + 4u), 0.0f)
                << "and an index past the ten is not a lookup";
        }

        /// The stars go out when the weather keeps them in, and the sheet is not even named then.
        TEST(RtxSkyBuilderTest, aWeatherThatHidesTheSunHidesTheStarsWithIt)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mNight.mField = 8;
            textures.mNight.mTile = 0.9f;
            textures.mNight.mHorizon = 0.4f;

            // Full night, clear weather: all of the sheet.
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mFade, 1.0f);
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mTexture, 8u);

            // A thunderstorm's `Glare_View` is nought, and under one there are no stars at all.
            EXPECT_EQ(describeStars(1.0f, 0.0f, 0.0f, textures).mFade, 0.0f);
            EXPECT_EQ(describeStars(1.0f, 0.0f, 0.0f, textures).mTexture, Rtx::Shaders::NO_TEXTURE)
                << "and a sheet nobody can see is one nothing has to sample";

            // Nor by day, whatever the weather is doing.
            EXPECT_EQ(describeStars(0.0f, 1.0f, 0.0f, textures).mFade, 0.0f);

            // Half out is half out, and the roll is carried whatever the fade came to.
            EXPECT_EQ(describeStars(0.5f, 1.0f, 2.5f, textures).mFade, 0.5f);
            EXPECT_EQ(describeStars(0.5f, 1.0f, 2.5f, textures).mTurn, 2.5f);

            // **The scale and the fade come off the mesh and are passed through**, which is the
            // whole reason they are fields and not constants: a replaced night sky changes them.
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mTile, 0.9f);
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mHorizon, 0.4f);

            // And a mesh that gave up no scale draws nothing, rather than dividing by it.
            SkyContent unread;
            unread.mClouds.fill(Rtx::sNoIndex);
            unread.mNight.mField = 8;
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, unread).mTexture, Rtx::Shaders::NO_TEXTURE);
        }
    }
}
