#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include <components/rtxbench/benchrun.hpp>

namespace RtxTool
{
    /// The hour a place stands at where neither the view nor the command line names one.
    ///
    /// Noon, because it is the hour a picture of a place is taken at. **It is not the hour a budget
    /// is written against** — a low sun makes every shadow ray long and grazing, and doubles the
    /// trace — which is why the views the target is judged on fix `hour` themselves.
    inline constexpr float sDefaultHour = 12.0f;

    /// The weather a place stands under where neither the view nor the command line names one.
    ///
    /// Clear, for the reason noon is the default hour: it is the sky a picture of a place is taken
    /// under. A view whose sky is the point of it says so itself.
    inline constexpr std::string_view sDefaultWeather = "Clear";

    /// A place worth looking at, by name.
    ///
    /// A view id is the unit of comparison across commits: the same name renders the same frame
    /// today and after a change, which is what makes a screenshot evidence rather than an anecdote.
    struct View
    {
        std::string mName;

        /// Addressed the way Morrowind does: a pair of integers is an exterior, anything else is an
        /// interior's name.
        std::string mCell;

        /// Left out for a view that only names a cell, which then gets the default placement.
        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;

        /// The hour this place is looked at, or absent for whatever hour the run is at.
        ///
        /// **The conditions belong to the place, for the reason the coordinates do.** A view id has
        /// to name one frame, and a frame at dawn and the same camera at noon are not one frame —
        /// so a place measured at dawn says so here rather than in whoever remembers to pass
        /// `--hour`. An `--hour` on the command line still wins, which is the rule every other
        /// field a view fixes already follows.
        std::optional<float> mHour;

        /// The weather this place stands under, or absent for whatever weather the run is under.
        ///
        /// **A condition of the place, exactly as the hour is one.** An overcast deck and a clear
        /// one are not one frame — the cloud shadow, the fog and the sun's own glare all differ —
        /// and a saving that only pays under a heavy sky can be measured no other way. `--weather`
        /// still wins, which is the rule every field a view fixes follows.
        std::optional<std::string> mWeather;

        std::string mNote;

        /// Where a bench run flies from here, or absent for a place that stands still. A shot and a
        /// window ignore it: one is a still and the other is flown by hand.
        std::optional<Rtx::Route> mRoute;
    };

    /// A place a run actually stands at: a view file entry with its conditions settled.
    ///
    /// **A `View`'s two conditions are optional because the *file* may fix neither. A place's are
    /// not, because a run always stands at some hour under some sky.** Two types rather than one is
    /// what keeps every reader downstream from asking which of the file and the command line won,
    /// and from dereferencing an optional that only a comment says is full.
    struct Place
    {
        std::string mName;
        std::string mCell;

        /// Absent for a place that names only a cell, which then gets the default placement.
        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;

        float mHour = sDefaultHour;
        std::string mWeather = std::string(sDefaultWeather);

        std::string mNote;
        std::optional<Rtx::Route> mRoute;
    };

    /// `view` with whatever the command line named settled into it.
    ///
    /// **The command line wins over a view, as it already does for `pos` and `look`.** A view that
    /// fixes an hour names a condition its frame is about; it does not overrule the person running
    /// the tool. Noon under a clear sky where neither says anything.
    ///
    /// **The one place that rule is applied.** A run of places applied it by overwriting each
    /// view's own optionals before staging them, and a single place applied it again on the way
    /// into the frame — two mechanisms for one sentence, and only the order they ran in made them
    /// agree.
    ///
    /// @param hour what `--hour` named, or nothing where it was left at its default.
    /// @param weather the same for `--weather`.
    Place placeFrom(const View& view, const std::optional<float>& hour, const std::optional<std::string>& weather);

    /// Reads the view file. Throws when it is missing or malformed — a mistyped view should say so
    /// rather than quietly render somewhere else.
    std::vector<View> loadViews(const std::filesystem::path& path);

    /// The view called `name`, or null.
    const View* findView(const std::vector<View>& views, std::string_view name);

    /// The views `named` asks for, in the order it names them; every one of them where it names
    /// none or names "all". Throws `std::runtime_error` naming a view that is not there.
    ///
    /// **One place decides what a list of view names means.** `bench` reaches it through a suite as
    /// well as from the command line and `verify` names them directly, and a filter that behaved
    /// differently between the two would make a run of one impossible to reproduce with the other.
    std::vector<View> chooseViews(const std::vector<View>& views, const std::vector<std::string>& named);
}
