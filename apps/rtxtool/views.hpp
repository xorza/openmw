#pragma once

#include <filesystem>
#include <optional>
#include <string>
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

        std::string mNote;

        /// Where a bench run flies from here, or absent for a place that stands still. A shot and a
        /// window ignore it: one is a still and the other is flown by hand.
        std::optional<Route> mRoute;
    };

    /// Reads the view file. Throws when it is missing or malformed — a mistyped view should say so
    /// rather than quietly render somewhere else.
    std::vector<View> loadViews(const std::filesystem::path& path);

    /// The view called `name`, or null.
    const View* findView(const std::vector<View>& views, std::string_view name);
}
