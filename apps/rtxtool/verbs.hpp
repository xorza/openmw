#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RtxTool
{
    /// Which of the harness's commands something is about, as a set of them.
    ///
    /// **A set and not one name, because that is the question the options ask.** An option is read
    /// by one command, or by two, or by every one of them, and that fact written only in the prose
    /// of its own help line is one nothing can check: `shot --views=balmora` rendering the default
    /// place and reporting it without a word.
    enum class Verbs : std::uint16_t
    {
        None = 0,

        Info = 1 << 0,
        Scene = 1 << 1,
        Shot = 1 << 2,
        View = 1 << 3,
        Bench = 1 << 4,
        Check = 1 << 5,
        Film = 1 << 6,

        /// Every command, which is what an option nobody restricted is read by: every bit up to
        /// the last one named above, so a command added there is in it by being there.
        Every = (Film << 1) - 1,
    };

    constexpr Verbs operator|(Verbs a, Verbs b)
    {
        return static_cast<Verbs>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
    }

    /// Whether `one` is in `set`.
    constexpr bool holds(Verbs set, Verbs one)
    {
        return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(one)) != 0;
    }

    /// Every command that is not in `set`.
    constexpr Verbs otherThan(Verbs set)
    {
        return static_cast<Verbs>(~static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(Verbs::Every));
    }

    /// How many commands are in `set`.
    constexpr std::size_t countVerbs(Verbs set)
    {
        return static_cast<std::size_t>(std::popcount(static_cast<std::uint16_t>(set)));
    }

    /// What one command is typed as, or empty for a set that is not exactly one of them.
    std::string_view verbName(Verbs one);

    /// The command called `name`, or `Verbs::None` where nothing is called that.
    Verbs verbNamed(std::string_view name);

    /// The commands in `set`, spelled the way a help line spells them: "`bench` and `verify`".
    std::string describeVerbs(Verbs set);

    /// What one command does with the places it runs at: one row a command, which the staging, the
    /// hold and the setup every command shares all read, so what `shot` does with a routed view is
    /// a fact of the table and not of whichever branch staged it.
    struct VerbPolicy
    {
        /// Whether the world's clock stops at every place, so a frame traced again is the same frame:
        /// what a picture, a digest and a claim about a still are each about.
        bool mFreezes = false;

        /// Whether a view's route is flown. A command that flies none stands at the route's start,
        /// and a route flown runs with the clock going, whatever `mFreezes` says: a camera moving
        /// through a stopped world measures the streaming and nothing that lives in it.
        bool mFliesRoutes = false;

        /// Whether a film's track moves the eye, the clock and the sky.
        bool mFollowsTracks = false;

        /// Whether the frames are measured or filmed, which runs them without the layers unless the
        /// line asks for some: `Rtx::sValidationByDefault` is a debugging build's, and a number or a
        /// frame taken under instrumentation is not the build's.
        bool mMeasures = false;

        /// Whether every measured frame is hashed, whatever the line asks.
        bool mHashes = false;
    };

    /// The row of `one`, which is one command.
    const VerbPolicy& policyOf(Verbs one);
}
