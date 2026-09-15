#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/variables_map.hpp>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/benchrun.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    /// Degrees clockwise from north, in `[0, 360)`, of the way a stand faces. North is +Y and east
    /// is +X, so the arguments come the other way round from the usual `atan2`.
    float bearingOf(const Rtx::Stand& stand);

    /// Degrees above the horizon, in `[-90, 90]`, of the way a stand faces.
    float climbOf(const Rtx::Stand& stand);

    /// One line for a person: where `stop` stands, in numbers worth reading rather than
    /// round-tripping, as a `#` comment either file format takes. A run that opened a window prints
    /// this and `describeBlock` where the eye was left, so a place found by flying can be pasted.
    std::string describeSpot(const Rtx::Stop& stop);

    /// The whole `views.cfg` section for `stop`, ready to paste, under a slug of its name. The whole
    /// section, because a block with no `cell` is one the view file refuses to load; and
    /// shortest-round-trip numbers, because these are read back into the same floats.
    std::string describeBlock(const Rtx::Stop& stop);

    /// What a frame is upscaled by when nobody names a mode. It follows the build, because
    /// `-DOPENMW_RTX_DLSS=OFF` is a deliberate opt-out; Quality rather than Performance, so a plain
    /// run is the renderer with everything on and not one that quietly quartered its pixels.
#ifdef OPENMW_RTX_DLSS
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Quality;
#else
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Off;
#endif

    /// What a command's frames are traced with — one block for a shot, a window, a profiling run
    /// and an A/B, which differ only in what they keep. Where a run stands is not here: the hour
    /// and the sky belong to the place, and `stopFor` is where the command line meets it.
    struct FrameRequest
    {
        /// The size the frame is presented at. What it is traced at follows from `mProfile.mUpscaling`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        float mFieldOfView = 60.0f;

        /// How far out from the eye the world is built, in cells. The air is tuned to it as well as
        /// the ground.
        float mDistantCells = 4.0f;

        /// Whether what the content files stand on the distant ground is paged in with it: the
        /// game's own `object paging`, set here, so the A/B that says what the buildings cost is
        /// that setting turned off.
        bool mDistantStatics = true;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

        /// What the trace itself is configured by, handed to the renderer through `RendererSpec`:
        /// the one type the game reads out of `[RTX]` and this fills from the command line.
        Rtx::RenderProfile mProfile{ .mUpscaling = { .mMode = sUpscaleByDefault } };
    };

    /// Runs `request` against a real game, headless, and gives back a process exit status. The
    /// game and not a world of this tool's own, because a staged world never pays for the
    /// whole-graph walk, the sweep or a cell arriving, and stands in a world nobody plays. The
    /// engine is built exactly as `apps/openmw/main.cpp` builds one, out of `variables`.
    /// `printLeft` prints where the eye was left as a `views.cfg` block.
    int runHosted(const boost::program_options::variables_map& variables, Files::ConfigurationManager& config,
        const std::filesystem::path& resources, Rtx::RenderProfile profile, Rtx::SessionRequest request,
        bool printLeft = false);

    /// A list of places to profile, by view id and not by coordinates, so the frame a screenshot
    /// shows and the frame a number was measured on are the same frame.
    struct BenchSuite
    {
        std::string mName;
        std::string mNote;

        /// In the order they were written, which is the order they are run in.
        std::vector<std::string> mViews;

        /// Whether each hand-over waits for the distant ground it collects, or nothing to let the
        /// frame clock decide: `Rtx::SessionRequest::mSettled`. A suite that times the streaming
        /// path says no, because waiting is most of what that path then measures — and a run
        /// under it may not be compared with a picture.
        std::optional<bool> mSettled;
    };

    /// Reads the suite file. Throws when it is missing or malformed, rather than quietly profiling
    /// somewhere else.
    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path);

    /// The suite called `name`, or null.
    const BenchSuite* findSuite(const std::vector<BenchSuite>& suites, std::string_view name);

    /// The hour and the weather a place stands under where neither the view nor the command line
    /// names one: what a picture of a place is taken at. Not the hour a budget is written against
    /// — a low sun doubles the trace — which is why the views the target is judged on fix `hour`.
    inline constexpr float sDefaultHour = 12.0f;
    inline constexpr std::string_view sDefaultWeather = "Clear";

    /// One stop from a view file entry and whatever the command line named. A view id names one
    /// frame, so a place measured at dawn says so in `mSky.mHour`; the command line still wins, as
    /// it does for `pos` and `look`. What comes back has both conditions settled, so nothing
    /// downstream asks which won. `day` is only for the moons.
    Rtx::Stop stopFor(
        const Rtx::Stop& view, const std::optional<float>& hour, const std::optional<std::string>& weather, int day);

    /// Reads the view file. Throws when it is missing or malformed, rather than quietly rendering
    /// somewhere else.
    std::vector<Rtx::Stop> loadViews(const std::filesystem::path& path);

    /// The view called `name`, or null.
    const Rtx::Stop* findView(const std::vector<Rtx::Stop>& views, std::string_view name);

    /// The views `named` asks for, in the order it names them; every one where it names none or
    /// "all". Throws naming a view that is not there. One place, because `bench` reaches it through
    /// a suite and `shot` directly, and a run of one has to be reproducible with the other.
    std::vector<Rtx::Stop> chooseViews(const std::vector<Rtx::Stop>& views, const std::vector<std::string>& named);
}
