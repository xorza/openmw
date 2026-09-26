#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include <components/esm/refid.hpp>

#include "model/benchrun.hpp"

namespace RtxTool
{
    /// Where the run stands and under what sky, as a stop a person could paste: noted every frame,
    /// printed whole where Home goes down, written into the window's title, and handed back as where
    /// the run was left.
    ///
    /// **Taken on a frame that still has a world**, because `OMW::Engine::~Engine` clears the world
    /// before the renderer that holds the run, so what a run ends at cannot be asked of the world
    /// once it has ended.
    class StandingNote
    {
    public:
        /// Starts the note of `stop`: its name, its note and its cell. The eye and the sky are the
        /// frame's, from the next `take`.
        void begin(const Stop& stop);

        /// Notes where the eye stands and the sky over it, of the frame about to be drawn.
        void take();

        /// Prints the note, whole, on the frame Home goes down: what a window prints where it was
        /// left, printed now, so a frame somebody is looking at can be drawn again without closing
        /// the window on it — and appends it to `keys` as a film's key, where that names a file.
        void printIfAsked(const std::filesystem::path& keys);

        /// The weather and the hour the run stands under, `Thunderstorm, 14:32`, and which weather
        /// is crossing in where one is, `Clear → Overcast 37%, 14:32`, off the last note; empty until
        /// a stop has begun and been noted.
        std::string_view describeTitle();

        /// Where the run was left, as a stop that would put a camera back there, or null where no
        /// stop began.
        const Stop* getLeft() const { return mStood.has_value() ? &*mStood : nullptr; }

    private:
        /// The note itself. The weather is assigned per frame into room the string already has.
        std::optional<Stop> mStood;

        /// What was crossing into that weather on the same frame, and how far. Beside the note and
        /// not in it, because a stop names the weather it stands under, and what is arriving is the
        /// title's alone. Empty while nothing is.
        std::string_view mArriving;
        float mCrossed = 0.0f;

        /// The cell the note names, by id: what a frame compares against to spell it only when the
        /// player crosses into another. An id and not the store, since a load frees every store and
        /// may build the next where the last one was.
        ESM::RefId mNotedCell;

        /// Whether Home was down on the last frame, so a press prints once.
        bool mPrintKeyHeld = false;

        /// What `describeTitle` is written into, once a second and never allocated: room for the
        /// longest note `writeSkyNote` names.
        std::array<char, 48> mTitleNote{};
    };
}
