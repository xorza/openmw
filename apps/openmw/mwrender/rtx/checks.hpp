#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Rtx
{
    struct Crossings;
}

namespace MWRender
{
    class RtxRenderer;

    /// One thing a run asserts about what the renderer was handed or what it drew.
    ///
    /// **A claim against the running game, where these used to be tests against a world of the
    /// harness's own.** That world read its cells by hand, dressed its people by rules of its own
    /// and derived its sky from the content files, so a claim proved there was a claim about a
    /// world nobody plays. What is here is the same claim asked of the world a player stands in.
    enum class Check
    {
        /// A second walk over the same graph adds no mesh and no material.
        ///
        /// **The property the incremental mirror rests on**, and the only way to ask it is to ask
        /// twice: the same crate met again has to resolve to the mesh already uploaded rather than
        /// to a copy of it.
        WalkTwice,

        /// Every placement wears a material something described.
        ///
        /// **A canary, and it should be nought.** A placement wearing nothing is a surface the
        /// content stated and the extractor could not read, which reaches the screen as grey.
        SurfacesDescribed,

        /// A room holds lights to cast.
        ///
        /// **Asked of an interior and answered yes by every exterior**, because a hillside at noon
        /// legitimately places none. A room with lamps in it that placed none is lit by its ambient
        /// alone, which is the failure that looks like a dark room rather than like a fault.
        LightsPlaced,

        /// A route crossed cell boundaries, and not every crossing had to rebuild.
        ///
        /// **The single most useful number a route produces.** An append builds the structures the
        /// ring brought; a rebuild builds every structure in the scene and re-describes the whole
        /// texture table, and the two are an order of magnitude apart.
        CrossingsAppend,

        /// A still camera over a still world resolves to a still picture.
        ///
        /// **What a temporal reconstruction has to promise.** Two frames of one scene from one eye
        /// differ only if something carried state it should not have, and that shows as a shimmer
        /// nothing else in this fork can see.
        PictureSettles,
    };

    /// What a check is called on a command line and in a report.
    std::string_view checkName(Check check);

    /// The check called `name`, or nothing where nothing is called that.
    std::optional<Check> checkNamed(std::string_view name);

    /// Every check there is, in the order they are run.
    std::span<const Check> everyCheck();

    /// What a check is called on a command line and in a report.
    std::string_view checkName(Check check);

    /// The check called `name`, or nothing where nothing is called that.
    std::optional<Check> checkNamed(std::string_view name);

    /// Every check there is, in the order they are run.
    std::span<const Check> everyCheck();

    /// Whether one check holds of what `owner` was handed and what it drew, with what it found in
    /// `found` either way.
    ///
    /// @param crossings what the stop's route came to, which only that check reads.
    /// @param settled whether the last two measured frames were the same picture.
    bool checkHolds(RtxRenderer& owner, Check check, const Rtx::Crossings& crossings, bool settled, std::string& found);
}
