#pragma once

namespace RtxTool
{
    /// The clock a window's world runs on, which is not the wall's.
    ///
    /// **One clock, because the fog's drift and the sun's height are the same clock.** The drift is a
    /// distance the wind has carried the air and the height is an hour of the day, and the renderer
    /// this follows holds both on one clock so that speeding the day up speeds the weather with it.
    /// Held apart — the hour on a running clock and the air on the wall — a foggy calm crossed
    /// thirteen units a second under a sun that crossed the sky in a minute and a half, and read as
    /// fog that did not move at all.
    ///
    /// **Two readings come off it, and which one a thing takes is what kind of thing it is.** The
    /// world's seconds run at `sSpeed` while the clock runs, and the sea and the fog drift against
    /// them. The wall's seconds never do, and the actors, the emitters and a weather transition keep
    /// those: a walk at thirty times is not a walk.
    class WorldClock
    {
    public:
        /// How fast the clock runs when it is running: game hours per real second.
        ///
        /// **A whole day in a minute and a half**, which is fast enough to watch a sunrise arrive
        /// and slow enough to see it happen. The game runs at a thirtieth of this; nothing here is
        /// pretending to be a play session.
        static constexpr float sHoursPerSecond = 1.0f / 4.0f;

        /// What an hour of `sHoursPerSecond` is worth to `Sky::SkyRoll`, which counts in seconds.
        static constexpr float sSecondsPerHour = 3600.0f;

        /// Morrowind's own `timescale`: game seconds per real second at the rate the game is played.
        static constexpr float sGameTimescale = 30.0f;

        /// How many times faster than the game the running clock goes, and so how many times faster
        /// than the wall the world's seconds run. Thirty.
        ///
        /// The reference's own table puts a bank of fog crossing its own width at a second around
        /// sixty-four; thirty is close enough to read the same.
        static constexpr float sSpeed = sHoursPerSecond * sSecondsPerHour / sGameTimescale;

        /// The longest real interval one `advance` may carry, in seconds.
        ///
        /// **A clock is not a velocity.** A camera that missed a second simply did not move, but a
        /// clock that missed one at thirty times has to decide whether thirty seconds of weather
        /// passed. They did not: the frame was loading a cell. A tenth is a long frame and anything
        /// beyond it is a stall, so time stops for it rather than lurching.
        static constexpr float sLongestStep = 0.1f;

        WorldClock(int day, float hour);

        /// Carries the world forward by `realSeconds` of wall clock, less whatever a stall added.
        void advance(float realSeconds);

        void toggle() { mRunning = !mRunning; }
        bool isRunning() const { return mRunning; }

        /// Moves the hour by `hours` without moving anything else, round the clock rather than to
        /// a stop at either end.
        ///
        /// **Works while stopped, and that is the point of having it.** Running the clock to reach
        /// dusk drags the fog through an hour of wind on the way; this arrives there with the banks
        /// where they were, which is what makes two hours comparable.
        void nudgeHour(float hours);

        /// Moves the day by `days`, and never before the first: the rise-hour formula counts from a
        /// fixed date rather than a signed one.
        void nudgeDay(int days);

        /// The wall seconds the last `advance` carried, after the cap.
        float getStep() const { return mStep; }

        /// Seconds of wall clock the window has run, which the actors and the emitters walk on.
        float getWallSeconds() const { return mWallSeconds; }

        /// Seconds the world has run, which the sea and the fog drift against.
        float getWorldSeconds() const { return mWorldSeconds; }

        /// How many game seconds a real one is worth right now, which is what `Sky::SkyRoll` turns
        /// the stars by: nothing while the clock is stopped.
        float getTimeScale() const { return mRunning ? sHoursPerSecond * sSecondsPerHour : 0.0f; }

        float getHour() const { return mHour; }
        int getDay() const { return mDay; }

    private:
        int mDay;
        float mHour;
        bool mRunning = false;
        float mStep = 0.0f;
        float mWallSeconds = 0.0f;
        float mWorldSeconds = 0.0f;
    };
}
