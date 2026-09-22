#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/variables_map.hpp>

#include <components/rtx/cellworld.hpp>
#include <components/rtx/pacing.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/sdlutil/vsyncmode.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    /// What a hosted run returns where the process compiled the launches and primed the driver's
    /// cache instead of measuring — `SessionResult::mPrimed` — and what `main` starts the process
    /// again on. `EX_TEMPFAIL`, which no other path returns.
    inline constexpr int sPrimedStatus = 75;

    /// The word a process that primed leaves in the environment of the one it starts in its place,
    /// so a process that compiled the launches all the same is a failure and not another prime.
    inline constexpr const char* sPrimedEnvironment = "OPENMW_RTX_PRIMED";

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

    /// The same place as one `view` command line, as a `#` comment: the cell, the camera, the hour,
    /// the day and the weather, every one of them named, so a frame somebody saw is a frame the
    /// next run draws again. The day too, which the block has no key for and the moons hang on.
    std::string describeCommand(const Rtx::Stop& stop);

    /// Where a window stands, whole: the line for a person, the block for the view file and the
    /// command for the next run. What a window prints on the key and again where it was left.
    std::string describeStanding(const Rtx::Stop& stop);

    /// How long a sky asked to turn takes to cross into each weather, in seconds of world, which is
    /// also how often the next is asked for.
    ///
    /// **One number, so every ask is a crossing and every crossing swaps.** The game runs one
    /// transition and keeps one asked behind it, so asking faster than it crosses only rewrites the
    /// one behind; and at a weather's own `Transition_Delta` — a minute for most — the precipitation
    /// swap halfway, which is what a turn exists for, lands past the twenty seconds a place runs.
    inline constexpr float sTurnSeconds = 4.0f;

    /// The fallback values that make every weather in `turnThrough` cross in `sTurnSeconds`:
    /// `Weather_<name>_Transition_Delta` set to its reciprocal, written over `fallback` before the
    /// `Fallback::Map` the world reads is made from it. A name that is none of the ten is left to
    /// the session, which says so; a run that turns nothing changes nothing.
    void setTurnCrossings(std::span<const std::string> turnThrough, std::map<std::string, std::string>& fallback);

    /// What the sky is doing, as a window's title says it after the rate: the weather and the
    /// clock, and while one weather crosses into another, which one and how far.
    struct SkyNote
    {
        std::string_view mWeather;

        /// The weather crossing in, or empty while none is. A crossing takes the weather's own
        /// `Transition_Delta` — a minute for most, or `sTurnSeconds` under a turn — and the title
        /// is where a window shows it, because the HUD a script's message lands on is off unless
        /// `--hud` asked for it.
        std::string_view mArriving;

        /// How far the crossing has come, nought to one.
        float mCrossed = 0.0f;

        float mHour = 0.0f;
    };

    /// `Thunderstorm, 14:32`, or `Clear → Overcast 37%, 14:32` while a crossing runs, written into
    /// `room` and never allocated: a title is written on the frame path. `room` holds the longest
    /// note, `Thunderstorm → Blizzard 100%, 23:59` at thirty-seven bytes, which is asserted.
    std::string_view writeSkyNote(std::span<char> room, const SkyNote& note);

    /// What a frame is upscaled by when nobody names a mode. The one knob whose default is the
    /// harness's own and not `settings-default.cfg`'s: the file says `quality`, and a build with
    /// `-DOPENMW_RTX_DLSS=OFF` refuses every mode but `off` by name, so a run of that build has to
    /// ask for what it can have. Quality rather than Performance where there is a choice, so a
    /// plain run is the renderer with everything on and not one that quietly quartered its pixels.
#ifdef OPENMW_RTX_DLSS
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Quality;
#else
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Off;
#endif

    /// Where a hosted run's frames are presented: what goes into the settings the engine makes its
    /// window from, and nothing the renderer is made with.
    struct WindowRequest
    {
        /// The size the frame is presented at. What it is traced at follows from the profile's
        /// upscaling.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        float mFieldOfView = 60.0f;

        /// How the present paces the frame. Off for a measured run, or the wait for the refresh
        /// lands in `wait ms`; a watched window keeps the player's own setting.
        SDLUtil::VSyncMode mVerticalSync = SDLUtil::VSyncMode::Disabled;
    };

    /// What a command's frames are traced with, read once off the command line into the two records
    /// the engine takes — the window, and the `RunSetup` the renderer is made with — and the one
    /// thing the place takes. Where a run stands is not here: the hour and the sky belong to the
    /// place, and `stopFor` is where the command line meets it.
    struct Framed
    {
        WindowRequest mWindow;

        /// The three parts of the `RunSetup` the line frames: what the trace is configured by, how
        /// much world the mirror builds, and how the driver paces the frame. The rest of the setup
        /// — the validation, the step, whether there is a window — is each verb's to say, on the
        /// request `sessionFor` builds.
        Rtx::RenderProfile mProfile{ .mUpscaling = { .mMode = sUpscaleByDefault } };
        Rtx::MirrorKnobs mMirror;
        Rtx::LatencyMode mLatency = Rtx::LatencyMode::Off;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;
    };

    /// What a setting is where nobody has set it: the shipped default, out of the `defaults.bin`
    /// beside the first configuration file `config` found, and never the player's own value. A
    /// measured run reads these so that two runs of it are one run whatever a settings file says.
    /// Not `Settings::Manager::mDefaultSettings`, which layers every configuration directory but
    /// the last over the shipped file — and the harness's own directory is the last.
    std::string shippedDefault(
        const Files::ConfigurationManager& config, std::string_view category, std::string_view setting);

    /// Runs `request` against a real game, headless, and gives back a process exit status. The
    /// game and not a world of this tool's own, because a staged world never pays for the
    /// whole-graph walk, the sweep or a cell arriving, and stands in a world nobody plays. The
    /// engine is built exactly as `apps/openmw/main.cpp` builds one, out of `variables`.
    /// `printLeft` prints where the eye was left as a `views.cfg` block.
    int runHosted(const boost::program_options::variables_map& variables, Files::ConfigurationManager& config,
        const std::filesystem::path& resources, Rtx::SessionRequest request, bool printLeft = false);

    /// A list of places to profile, by view id and not by coordinates, so the frame a screenshot
    /// shows and the frame a number was measured on are the same frame.
    struct BenchSuite
    {
        std::string mName;
        std::string mNote;

        /// In the order they were written, which is the order they are run in.
        std::vector<std::string> mViews;

        /// Whether each hand-over waits for the distant ground it collects, or nothing to let the
        /// frame clock decide: `Rtx::RunSetup::mSettled`. A suite that times the streaming
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
