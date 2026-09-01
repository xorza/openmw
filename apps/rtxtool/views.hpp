#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include "placement.hpp"

namespace RtxTool
{
    /// Where a bench flies to from a view, and how fast.
    ///
    /// **A route is what puts a cell arriving into a benchmark at all.** A camera standing still
    /// measures a frame; the cost this harness exists to see — the ring read off disk, the models
    /// built, the sweep that follows the cells that left — only happens to a camera that goes
    /// somewhere. Flown at a fixed speed from a frame index, so the crossings land on the same
    /// frames on every machine and on every build.
    struct Route
    {
        /// The view it ends at, named for the report. The two below were copied out of it when the
        /// file was read, so nothing downstream resolves anything.
        std::string mTo;

        osg::Vec3f mOrigin;
        osg::Vec3f mTarget;

        /// World units a second. A Morrowind exterior cell is 8,192 across, so this times the run's
        /// length is roughly how many boundaries get crossed.
        float mSpeed = 0.0f;

        /// How far along this route the camera stands `seconds` in, as a fraction of the way.
        ///
        /// **Clamped at one**, so a run longer than the route is stands at the far end rather than
        /// sailing off into the sea. A route whose ends coincide is arrived at immediately.
        float partAt(const Placement& start, float seconds) const;

        /// Where the camera stands and looks, `part` of the way along. Both ends interpolate, so a
        /// pair of endpoints whose looks are the same offset from their positions gives a camera
        /// that faces one direction throughout.
        Placement at(const Placement& start, float part) const;
    };

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
        std::optional<Route> mRoute;
    };

    /// The hour a place stands at, from what the command line named and what the view fixes.
    ///
    /// **The command line wins over a view, as it already does for `pos` and `look`.** A view that
    /// fixes an hour names a condition its frame is about; it does not overrule the person running
    /// the tool. Noon where neither says anything.
    ///
    /// @param given what `--hour` named, or nothing where it was left at its default.
    /// @param fixed what the view fixes, or nothing where it fixes none.
    float hourFor(const std::optional<float>& given, const std::optional<float>& fixed);

    /// The weather a place stands under, from what the command line named and what the view fixes.
    /// `hourFor`'s rule, over the other condition a place can fix.
    std::string weatherFor(const std::optional<std::string>& given, const std::optional<std::string>& fixed);

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
