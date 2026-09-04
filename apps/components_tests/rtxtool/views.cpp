#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/placement.hpp>
#include <apps/rtxtool/views.hpp>

namespace RtxTool
{
    namespace
    {
        /// Writes `text` to a scratch view file, reads it back, and removes the file.
        std::vector<View> readViews(std::string_view text)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-route-test.cfg";
            {
                std::ofstream out(file);
                out << text;
            }

            struct Remove
            {
                std::filesystem::path mFile;
                ~Remove() { std::filesystem::remove(mFile); }
            } removed{ file };

            return loadViews(file);
        }

        /// A route resolves to the coordinates of the view it names, and both halves are required.
        ///
        /// **The destination is copied at load and never looked up again**, so this is the one place
        /// that can get the pairing wrong — and getting it wrong flies the camera somewhere else,
        /// which a benchmark reports as a different number rather than as an error.
        TEST(RtxViewsTest, aRouteTakesItsDestinationFromTheViewItNames)
        {
            const std::vector<View> read = readViews(R"(
[start]
cell = -3,-2
pos = 100, 200, 300
look = 100, 300, 300
to = finish
speed = 1500

[finish]
cell = -1,-2
pos = 8292, 200, 700
look = 8292, 300, 700
)");

            ASSERT_EQ(read.size(), std::size_t{ 2 });
            const View* start = findView(read, "start");
            ASSERT_NE(start, nullptr);
            ASSERT_TRUE(start->mRoute.has_value());

            EXPECT_EQ(start->mRoute->mTo, "finish");
            EXPECT_EQ(start->mRoute->mOrigin, osg::Vec3f(8292.0f, 200.0f, 700.0f));
            EXPECT_EQ(start->mRoute->mTarget, osg::Vec3f(8292.0f, 300.0f, 700.0f));
            EXPECT_EQ(start->mRoute->mSpeed, 1500.0f);

            // The destination is an ordinary view and goes nowhere itself.
            const View* finish = findView(read, "finish");
            ASSERT_NE(finish, nullptr);
            EXPECT_FALSE(finish->mRoute.has_value());
        }

        /// Every way of half-writing a route is a refusal rather than a camera that stands still.
        TEST(RtxViewsTest, aHalfWrittenRouteIsRefused)
        {
            constexpr std::string_view sEnd = "\n[finish]\ncell = -1,-2\npos = 8292, 0, 0\nlook = 8292, 100, 0\n";

            EXPECT_THROW(
                readViews(std::string("[start]\ncell = -3,-2\nto = finish\n") + std::string(sEnd)), std::runtime_error)
                << "a destination with no speed";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nspeed = 1500\n"), std::runtime_error)
                << "a speed with nowhere to go";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nto = nowhere\nspeed = 1500\n"), std::runtime_error)
                << "a destination that is not a view";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nto = finish\nspeed = 1500\n\n[finish]\ncell = -1,-2\n"),
                std::runtime_error)
                << "a destination with no coordinates of its own to arrive at";

            EXPECT_THROW(readViews(std::string("[start]\ncell = -3,-2\nto = finish\nspeed = -1\n") + std::string(sEnd)),
                std::runtime_error)
                << "a speed that goes backwards";

            EXPECT_THROW(
                readViews(std::string("[start]\ncell = -3,-2\nto = finish\nspeed = quickly\n") + std::string(sEnd)),
                std::runtime_error)
                << "a speed that is not a number";
        }

        /// A place says what it is looked at under, and takes where it stands from another place.
        ///
        /// **The pair is the point.** A dawn row and a noon row of one camera mean something beside
        /// each other only where the camera is identical by construction — coordinates copied by
        /// hand drift the first time either is moved, and the pair then reads two cameras as a
        /// difference the hour made.
        TEST(RtxViewsTest, aPlaceFixesItsConditionsAndTakesItsCameraFromWhatItIsLike)
        {
            const std::vector<View> read = readViews(R"(
[ship]
cell = -2,-9
pos = 100, 200, 300
look = 100, 300, 300

[ship-dawn]
like = ship
hour = 6.5

[ship-overcast]
like = ship
weather = Overcast

[ship-dusk-from-the-mast]
like = ship
pos = 100, 200, 900
hour = 19.25
)");

            ASSERT_EQ(read.size(), std::size_t{ 4 });

            // A place that fixes nothing keeps both conditions absent, which is what lets a run name
            // them.
            const View* noon = findView(read, "ship");
            ASSERT_NE(noon, nullptr);
            EXPECT_FALSE(noon->mHour.has_value());
            EXPECT_FALSE(noon->mWeather.has_value());

            const View* dawn = findView(read, "ship-dawn");
            ASSERT_NE(dawn, nullptr);
            ASSERT_TRUE(dawn->mHour.has_value());
            EXPECT_EQ(*dawn->mHour, 6.5f);

            // **The two conditions are independent**: an hour fixed leaves the sky free and a sky
            // fixed leaves the hour free, so a place may name either alone.
            EXPECT_FALSE(dawn->mWeather.has_value());

            const View* overcast = findView(read, "ship-overcast");
            ASSERT_NE(overcast, nullptr);
            ASSERT_TRUE(overcast->mWeather.has_value());
            ASSERT_TRUE(overcast->mOrigin.has_value());
            EXPECT_EQ(*overcast->mWeather, "Overcast");
            EXPECT_FALSE(overcast->mHour.has_value());
            EXPECT_EQ(*overcast->mOrigin, osg::Vec3f(100.0f, 200.0f, 300.0f));

            // The whole camera, taken rather than restated.
            EXPECT_EQ(dawn->mCell, "-2,-9");
            ASSERT_TRUE(dawn->mOrigin.has_value());
            ASSERT_TRUE(dawn->mTarget.has_value());
            EXPECT_EQ(*dawn->mOrigin, osg::Vec3f(100.0f, 200.0f, 300.0f));
            EXPECT_EQ(*dawn->mTarget, osg::Vec3f(100.0f, 300.0f, 300.0f));

            // **What a borrower states itself is kept**, so a place may sit somewhere else under
            // the same cell and the same view of it.
            const View* mast = findView(read, "ship-dusk-from-the-mast");
            ASSERT_NE(mast, nullptr);
            ASSERT_TRUE(mast->mOrigin.has_value());
            EXPECT_EQ(*mast->mOrigin, osg::Vec3f(100.0f, 200.0f, 900.0f)) << "its own position was overwritten";
            EXPECT_EQ(*mast->mTarget, osg::Vec3f(100.0f, 300.0f, 300.0f)) << "the look it did not state";
            EXPECT_EQ(mast->mCell, "-2,-9");
        }

        /// Every way of writing a condition or a likeness wrong is a refusal.
        ///
        /// A view that quietly stood at another hour, under another sky, or in another place would
        /// report a number against a frame nobody asked for — which is the failure this whole file
        /// exists to stop.
        TEST(RtxViewsTest, aConditionOrALikenessThatCannotBeMeantIsRefused)
        {
            constexpr std::string_view sShip = "[ship]\ncell = -2,-9\npos = 1, 2, 3\nlook = 1, 9, 3\n";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = dawn\n"), std::runtime_error)
                << "an hour that is not a number";

            // **The whole of the field, so a number with a letter after it is a typo and not an
            // hour.** A view that stood at six because `6h` began with a six would report a figure
            // against a frame nobody asked for.
            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = 6h\n"), std::runtime_error)
                << "an hour with a letter after it";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = 24\n"), std::runtime_error)
                << "an hour off the end of the clock";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = -1\n"), std::runtime_error)
                << "an hour before the day began";

            // Midnight and a moment before the next one are both hours of the day.
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[dark]\nlike = ship\nhour = 0\n"));
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[late]\nlike = ship\nhour = 23.99\n"));

            // **A weather is one of the ten and spelled as the content files spell it.** Anything
            // else reaches the fallback map as a key it refuses, which is a throw at the frame
            // rather than at the file — and by then the run has staged a cell for it.
            EXPECT_THROW(readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = Drizzle\n"), std::runtime_error)
                << "a weather that is none of the ten";

            EXPECT_THROW(
                readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = overcast\n"), std::runtime_error)
                << "a weather spelled in the wrong case";

            EXPECT_NO_THROW(readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = Thunderstorm\n"));

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = nowhere\n"), std::runtime_error)
                << "like a view that is not there";

            EXPECT_THROW(readViews("[dawn]\nlike = dawn\n"), std::runtime_error) << "like itself";

            EXPECT_THROW(
                readViews(std::string(sShip) + "[dawn]\nlike = ship\n[later]\nlike = dawn\n"), std::runtime_error)
                << "a chain, which would make the order things are read in decide what a view is";
        }

        /// Which condition wins, which is the one rule the three commands that draw a view all read.
        TEST(RtxViewsTest, theConditionOnTheCommandLineBeatsTheOneAPlaceFixes)
        {
            // Neither says anything: noon under a clear sky, which is how a picture of a place is
            // taken.
            EXPECT_EQ(hourFor(std::nullopt, std::nullopt), sDefaultHour);
            EXPECT_EQ(weatherFor(std::nullopt, std::nullopt), sDefaultWeather);

            // Only the place: the place decides, which is what makes a view id one frame.
            EXPECT_EQ(hourFor(std::nullopt, 6.5f), 6.5f);
            EXPECT_EQ(weatherFor(std::nullopt, std::string("Overcast")), "Overcast");

            // The command line, over a place that fixes one and over a place that does not.
            EXPECT_EQ(hourFor(9.0f, 6.5f), 9.0f);
            EXPECT_EQ(hourFor(9.0f, std::nullopt), 9.0f);
            EXPECT_EQ(weatherFor(std::string("Rain"), std::string("Overcast")), "Rain");
            EXPECT_EQ(weatherFor(std::string("Rain"), std::nullopt), "Rain");

            // And the two disagree, or none of the above says anything.
            EXPECT_NE(hourFor(std::nullopt, 6.5f), hourFor(9.0f, 6.5f));
            EXPECT_NE(weatherFor(std::nullopt, std::string("Overcast")), weatherFor(std::string("Rain"), std::nullopt));
        }

        /// Where the camera stands part-way along, hand-computed and clamped at the far end.
        ///
        /// **A thousand units at a hundred a second is a tenth of the way after a second**, and the
        /// look point moves with it — the two endpoints below are each a hundred units ahead of
        /// their own position, so the camera faces the same way throughout and the frame at the
        /// halfway mark is the frame five hundred units in.
        TEST(RtxViewsTest, aRouteIsFlownAtItsSpeedAndStopsWhenItArrives)
        {
            const Placement start{ .mOrigin = { 0.0f, 0.0f, 0.0f }, .mTarget = { 0.0f, 100.0f, 0.0f } };
            const Route route{ .mTo = "finish",
                .mOrigin = { 0.0f, 1000.0f, 0.0f },
                .mTarget = { 0.0f, 1100.0f, 0.0f },
                .mSpeed = 100.0f };

            EXPECT_EQ(route.partAt(start, 0.0f), 0.0f);
            EXPECT_EQ(route.partAt(start, 1.0f), 0.1f);
            EXPECT_EQ(route.partAt(start, 5.0f), 0.5f);

            // Ten seconds is the thousand units exactly; anything past it stands at the far end.
            EXPECT_EQ(route.partAt(start, 10.0f), 1.0f);
            EXPECT_EQ(route.partAt(start, 40.0f), 1.0f);

            EXPECT_EQ(route.at(start, 0.5f).mOrigin, osg::Vec3f(0.0f, 500.0f, 0.0f));
            EXPECT_EQ(route.at(start, 0.5f).mTarget, osg::Vec3f(0.0f, 600.0f, 0.0f));
            EXPECT_EQ(route.at(start, 1.0f).mOrigin, route.mOrigin);
            EXPECT_EQ(route.at(start, 1.0f).mTarget, route.mTarget);

            // **Twice the speed is twice the distance, which is what says the speed is read at all.**
            const Route faster{
                .mTo = route.mTo, .mOrigin = route.mOrigin, .mTarget = route.mTarget, .mSpeed = 200.0f
            };
            EXPECT_EQ(faster.partAt(start, 1.0f), 0.2f);
            EXPECT_NE(faster.partAt(start, 1.0f), route.partAt(start, 1.0f));

            // A route whose ends coincide has arrived, rather than dividing by nothing.
            const Route standing{ .mTo = "here", .mOrigin = start.mOrigin, .mTarget = start.mTarget, .mSpeed = 100.0f };
            EXPECT_EQ(standing.partAt(start, 1.0f), 1.0f);
        }
    }
}
