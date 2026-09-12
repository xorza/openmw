#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/variables_map.hpp>
#include <osg/Vec3f>

#include <components/rtx/renderprofile.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/benchrun.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    /// Where a camera is standing and under what, as this tool writes a place down.
    ///
    /// **How a place found by flying is written into `views.cfg`.** A run that opened a window
    /// prints one of these where the eye was left, so somebody who flew somewhere worth keeping
    /// closes the window and pastes what it said.
    ///
    /// **A type rather than a handful of `format` calls at whatever prints it**, because everything
    /// it writes is something the tool has to be able to read back — a `views.cfg` section and a
    /// command line — and a format that drifts from its parser is not a thing an eye catches in a
    /// log. Held apart from whatever prints it, so the tests can assert both without a device.
    struct Viewpoint
    {
        /// The `views.cfg` id this was opened as, or empty where it was opened by `--cell`. Kept so
        /// that flying somewhere better and saving it is a replacement rather than a new entry.
        std::string mView;
        std::string mNote;

        /// The cell as `--cell` spells it: a pair of integers for an exterior, a name for an
        /// interior.
        ///
        /// **Where a run enters, and not the square a camera has since flown into.** The world is
        /// streamed around the player, and a stop moves the player to `mOrigin` once it is standing
        /// somewhere — so the two disagreeing costs nothing, and the block still opens on the frame
        /// it was printed from. Naming the containing square would ask the game to spell a cell
        /// back, which is a second spelling of a name `--cell` already reads.
        std::string mCell;

        /// Where the eye was left and what it stood under, as the run reported it.
        ///
        /// **`Rtx::Standing` and not five fields of this type's own**, because the run states the
        /// same five and a launcher only writes them down — held apart, a field added to the report
        /// reached the file that has to be able to read it back only if somebody carried it across.
        Rtx::Standing mAt;

        /// Degrees clockwise from north, in `[0, 360)`.
        ///
        /// **North is +Y and east is +X**, so the arguments come the other way round from the usual
        /// `atan2`.
        float getBearing() const;

        /// Degrees above the horizon, in `[-90, 90]`.
        float getClimb() const;
    };

    /// One line for a person: where this is, in numbers worth reading rather than round-tripping.
    ///
    /// A `#` comment in both of the formats below, so a file of these can be fed to either.
    std::string describeSpot(const Viewpoint& spot);

    /// The whole `views.cfg` section, ready to paste into it.
    ///
    /// **The whole section and not two of its lines.** A block with no `cell` in it is one the view
    /// file refuses to load, so what was printed could never have gone where it was printed to go.
    ///
    /// **Shortest-round-trip numbers and not the rounded ones `describeSpot` prints**: these exist
    /// to be read back into the same floats, and a position rounded to the unit is a different
    /// frame when the camera is a hand's width from a wall.
    std::string describeBlock(const Viewpoint& spot);

    /// What a frame is upscaled by when nobody names a mode.
    ///
    /// **It follows the build**, because the two are one decision: `-DOPENMW_RTX_DLSS=OFF` is a
    /// deliberate opt-out, and a tool that then refused every default invocation would be telling
    /// its user to turn on the thing they had just turned off.
    ///
    /// Quality rather than performance, so a plain run is the renderer with everything switched on
    /// and not one that quietly quartered the pixels it traced.
#ifdef OPENMW_RTX_DLSS
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Quality;
#else
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Off;
#endif

    /// What a command's frames are traced with.
    ///
    /// **One block and not four.** A shot, a window, a profiling run and an A/B differ in what they
    /// keep — a PNG, a swapchain, a table of times, a comparison — and in nothing about the frame
    /// itself. Held apart, each of the four named these fields itself and `dispatch` read the
    /// command line into them four times.
    ///
    /// **Where a run stands is not here.** The hour and the sky belong to the place, because a view
    /// may fix either and the command line may overrule it — `stopFor` is where the two meet.
    struct FrameRequest
    {
        /// The size the frame is presented at. What it is traced at follows from `mProfile.mUpscaling`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;
        float mFieldOfView = 60.0f;

        /// How far out from the eye the world is built, in cells.
        ///
        /// **How much world exists, which is a property of the structure rays are cast against and
        /// not of the camera.** The air is tuned to it as well as the ground, so a ring of ground
        /// four cells out and a fog measured over thirty thousand units are one number.
        float mDistantCells = 4.0f;

        /// Whether what the content files stand on the distant ground is paged in with it.
        ///
        /// **The game's own `object paging`, set rather than answered here.** What a run measures
        /// is the paging a player gets, so the A/B that says what the buildings and the trees cost
        /// is that setting turned off and not a second way of building the world. The ground itself
        /// carries no flag: `Renderer::wantsPagedTerrain` says why a tracing renderer always pages
        /// it.
        bool mDistantStatics = true;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

        /// What the trace itself is configured by, handed to the renderer through `RendererSpec`.
        ///
        /// **Held whole rather than spelled out again.** Everything above is the engine's — a
        /// window, a camera, how much world to build — and everything the trace decides is one type
        /// that the game reads out of `[RTX]` and this fills from the command line.
        Rtx::RenderProfile mProfile{ .mUpscaling = { .mMode = sUpscaleByDefault } };
    };

    /// Runs `request` against a real game, headless, and gives back what it came to.
    ///
    /// **The game and not a world of this tool's own.** A staged world re-walks only its actors, so
    /// it never pays for the whole-graph walk, the sweep, or a cell arriving — the three things
    /// that actually cost a frame. What it also cannot do is stand in the world the player stands
    /// in: its cells are read by hand, its people are dressed by rules of this tool's own, and its
    /// sky is derived from the content files rather than reported by a weather system. So a
    /// picture taken here and a picture played were two pictures.
    ///
    /// **The engine is built exactly as `apps/openmw/main.cpp` builds one**, out of the same option
    /// table, so a run reaches the same content through the same loader. What this adds is a
    /// schedule and the switches a measurement needs, and nothing else.
    ///
    /// @param variables the parsed command line, which carries the data directories, the content
    ///        files and the encoding the engine is configured from.
    /// @param spot where the run was asked to stand, or null for a run whose end nobody wants
    ///        written down. Given one, the place the eye was left at is printed as a `views.cfg`
    ///        block — which is what a window is for as much as the picture is.
    /// @return a process exit status.
    int runHosted(const boost::program_options::variables_map& variables, Files::ConfigurationManager& config,
        const std::filesystem::path& resources, Rtx::RenderProfile profile, Rtx::SessionRequest request,
        const Viewpoint* spot = nullptr);

    /// A list of places to profile, by name.
    ///
    /// **View ids and not coordinates.** A place worth measuring is a place worth looking at, so a
    /// suite borrows `views.cfg` rather than restating it — which is what keeps the frame a
    /// screenshot shows and the frame a number was measured on the same frame.
    struct BenchSuite
    {
        std::string mName;
        std::string mNote;

        /// In the order they were written, which is the order they are run in.
        std::vector<std::string> mViews;
    };

    /// Reads the suite file. Throws when it is missing or malformed — a mistyped suite should say
    /// so rather than quietly profiling somewhere else.
    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path);

    /// The suite called `name`, or null.
    const BenchSuite* findSuite(const std::vector<BenchSuite>& suites, std::string_view name);

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

    /// One stop, from a view file entry and whatever the command line named.
    ///
    /// **The command line wins over a view, as it already does for `pos` and `look`.** A view that
    /// fixes an hour names a condition its frame is about; it does not overrule the person running
    /// the tool. Noon under a clear sky where neither says anything.
    ///
    /// **The one place that rule is applied, and what comes back has both conditions settled.**
    /// `Rtx::StopSky` keeps them optional because the plain game measuring itself names neither;
    /// nothing downstream of this has to ask which of the file and the command line won.
    ///
    /// @param hour what `--hour` named, or nothing where it was left at its default.
    /// @param weather the same for `--weather`.
    /// @param day which day of Morrowind's calendar the run stands on. Only the moons read it.
    Rtx::Stop stopFor(
        const View& view, const std::optional<float>& hour, const std::optional<std::string>& weather, int day);

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
