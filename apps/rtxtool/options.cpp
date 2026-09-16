#include "options.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/option.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/contract.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/upscale.hpp>

#include "run.hpp"
#include "verbs.hpp"

namespace bpo = boost::program_options;

namespace RtxTool
{
    namespace
    {
        using StringsVector = std::vector<std::string>;

        /// The commands that can stand at one place named on the line — a cell, a camera, a view.
        /// A run of places — `bench` and `check`, and `shot` and `scene` under `--views` — takes
        /// its cells and its cameras from `views.cfg` instead.
        constexpr Verbs sPlaces = Verbs::Scene | Verbs::Shot | Verbs::View;

        /// The commands that visit a list of places.
        constexpr Verbs sRuns = Verbs::Scene | Verbs::Shot | Verbs::Bench | Verbs::Check;

        /// The commands that frame the world, which is every one that builds a `FrameRequest`.
        /// `info` is the one that does not: it reports on a device and draws nothing.
        constexpr Verbs sFramed = otherThan(Verbs::Info);

        /// Which commands read an option, said the shorter of the two ways.
        ///
        /// **Or the ones that do not, where that is the shorter list.** Naming eight commands to
        /// exclude one is a line nobody reads to the end, and the one is what the reader is
        /// actually being told.
        ///
        /// **The help line and the complaint say it the same way**, because they are the same fact
        /// before and after somebody gets it wrong.
        std::string describeReaders(const Verbs verbs)
        {
            const Verbs missing = otherThan(verbs);
            if (countVerbs(missing) < countVerbs(verbs))
                return std::format("every command but {}", describeVerbs(missing));

            return describeVerbs(verbs);
        }

        /// One option's help line, opening with the commands that read it.
        ///
        /// An option every command reads opens with nothing at all: there is no command it would
        /// be telling the reader about.
        std::string describeOption(const Verbs verbs, const std::string_view description)
        {
            if (verbs == Verbs::Every)
                return std::string(description);

            return std::format("with {}, {}", describeReaders(verbs), description);
        }
    }

    Verbs ToolOptions::readsOption(const std::string_view name) const
    {
        for (const OptionOwner& owner : mOwners)
        {
            if (owner.mName == name)
                return owner.mVerbs;
        }

        return Verbs::Every;
    }

    std::string ToolOptions::complainAbout(const bpo::parsed_options& line, const Verbs verb) const
    {
        std::string complaint;
        std::vector<std::string_view> said;

        for (const bpo::option& given : line.options)
        {
            const Verbs reads = readsOption(given.string_key);
            if (holds(reads, verb))
                continue;

            // A composing option arrives once per time it was written, and a run that named three
            // creatures is not owed three complaints about `--actor`.
            if (std::find(said.begin(), said.end(), given.string_key) != said.end())
                continue;

            said.push_back(given.string_key);
            complaint += std::format("`{}` does not read --{}, which belongs to {}.\n", verbName(verb),
                given.string_key, describeReaders(reads));
        }

        return complaint;
    }

    ToolOptions makeOptions(const Rtx::ValidationLevel validationByDefault)
    {
        ToolOptions result{ bpo::options_description("Options"), {} };
        auto declare = result.mDescription.add_options();

        // **Every option says which commands read it, and there is no second door that lets one
        // skip the question.** The record and the help line come from that one statement, so a run
        // that names an option under a command that does not read it is stopped rather than
        // quietly rendering something else. A door that defaulted to "every command" is how
        // `--size`, `--delight` and thirteen more came to be taken by `info` and thrown away.
        const auto option
            = [&](Verbs verbs, const char* name, const bpo::value_semantic* semantic, std::string_view description) {
                  result.mOwners.push_back(OptionOwner{ .mName = name, .mVerbs = verbs });
                  declare(name, semantic, describeOption(verbs, description).c_str());
              };

        // **What a frame is when nobody says**, read from one statement rather than restated as a
        // literal beside each option. The two drifted: `--distant-cells` defaulted to five cells
        // where the request and `settings-default.cfg` both said four, so a harness run built a
        // world one cell wider than the game does and measured it.
        const FrameRequest byDefault;

        option(Verbs::Every, "help", bpo::bool_switch(), "print this message and quit");

        option(Verbs::Every, "validation",
            bpo::value<std::string>()->default_value(std::string(Rtx::sValidationNames.name(validationByDefault))),
            std::format("which of VK_LAYER_KHRONOS_validation's checks to load: {}. `on` is the core "
                        "checks; `sync` adds synchronization validation, which catches a missing "
                        "barrier; `gpu` adds GPU-assisted validation instead, which instruments every "
                        "shader at about half the frame rate, and which the layer asks not to run "
                        "beside the core checks. `sync` by default outside a Release build and `off` "
                        "in one; a run that names a level and cannot have it fails rather than "
                        "reporting nothing",
                Rtx::sValidationNames.list())
                .c_str());

        option(sPlaces, "cell", bpo::value<std::string>()->default_value(""),
            "cell to read, addressed the way Morrowind does: a pair of integers is an exterior, "
            "anything else is an interior's name. Write --cell=-2,-9 rather than --cell -2,-9, or "
            "the leading minus reads as an option. Left out, the default view decides.");

        option(sPlaces, "view", bpo::value<std::string>()->default_value(""),
            "a named viewpoint from resources/rtx/views.cfg, which supplies the cell and usually the "
            "camera. Overrides --cell. A run of places names them with --views instead.");

        option(Verbs::Every, "list-views", bpo::bool_switch(), "print the named viewpoints and quit");

        option(sFramed, "delight", bpo::value<float>()->default_value(byDefault.mProfile.mDelight),
            "how much of the lighting painted into each texture to divide back out, from 0 to 1. "
            "Zero is the A/B that says what it did");
        option(sFramed, "filter",
            bpo::value<bool>()->default_value(byDefault.mProfile.mReconstruction.mFilter)->implicit_value(true),
            "run the denoiser over the indirect light. Off shows the raw bounce, and is what a "
            "reference is made with");
        option(Verbs::Shot, "doll", bpo::value<std::string>()->default_value(""),
            "also write the inventory doll of this person, by NPC record id -- fargoth, \"caius "
            "cosades\" -- traced against a scene of their own. They arrive dressed out of their "
            "own record, which is what the game equips them with. `scene --find=<text>` finds one");
        option(Verbs::Shot, "map", bpo::bool_switch(),
            "also write one local-map tile of the place, traced straight down and framed the way "
            "the game's own compass frames one");
        option(Verbs::Shot, "textures", bpo::bool_switch(),
            "also write every texture the world around the place uses, vanilla beside de-lit, as "
            "one sheet: what a frame there would actually sample, since a town's people wear "
            "textures the town itself never names");

        option(sFramed, "upscale",
            bpo::value<std::string>()->default_value(
                std::string(Rtx::sUpscaleNames.name(byDefault.mProfile.mUpscaling.mMode))),
            std::format("put DLSS Ray Reconstruction between the trace and the picture: {}. --size "
                        "is what comes out, and what gets traced is DLSS's answer for it. It "
                        "denoises for itself, so --filter stops applying. Quality by default, so a "
                        "plain run is the renderer with everything switched on without quartering "
                        "the pixels it traced; --upscale=performance is the 1920x1080 to 3840x2160 "
                        "the frame budget is written against, and --upscale=off is what an A/B "
                        "against the unupscaled path needs. A reference cannot be built through a "
                        "denoiser",
                Rtx::sUpscaleNames.list())
                .c_str());

        option(sFramed, "preset",
            bpo::value<std::string>()->default_value(
                std::string(Rtx::sPresetNames.name(byDefault.mProfile.mUpscaling.mPreset))),
            std::format("which Ray Reconstruction network to run: {}. Ray Reconstruction keeps its "
                        "own presets, and they are not super-resolution's -- A through C are retired, d is the "
                        "default transformer model and e is the latest. `default` hands the choice to the "
                        "installed library, which has changed between SDK versions and between the "
                        "convolutional and transformer models, so two runs under it are not the same "
                        "measurement. Pinned to d so that they are",
                Rtx::sPresetNames.list())
                .c_str());

        option(sFramed, "exposure", bpo::value<std::string>()->default_value("auto"),
            "what to scale the frame by before the display curve: auto measures it off the frame, "
            "and a number holds it there. A pixel test and a converged reference want it held, "
            "because a measured exposure makes every value depend on the whole frame");

        option(sFramed, "albedo", bpo::bool_switch(),
            "write the albedo with no shading over it, which is what a texture problem looks like "
            "when nothing else is in the way");

        option(sFramed, "weather", bpo::value<std::string>()->default_value(std::string(sDefaultWeather)),
            "which weather's sun, sky and precipitation an exterior stands under, named as the "
            "content files spell it: Clear, Cloudy, Foggy, Overcast, Rain, Thunderstorm, Ashstorm, "
            "Blight, Snow, Blizzard. The ones that drop something drop it here too. Given, it beats "
            "a weather a view fixes for itself");

        option(Verbs::Bench, "turn-weather", bpo::value<std::string>()->default_value(""),
            "turn the sky through these weathers while each place runs, comma "
            "separated and round again — --turn-weather=Rain,Foggy. Each transition takes four "
            "seconds of world, as a window's weather keys do, and the precipitation of the one "
            "arriving replaces the one leaving halfway through. **A run under it is not a "
            "benchmark**: no two places stand under the same sky. It is here because a weather "
            "turning frees a whole emitter's meshes and textures on an ordinary frame, which is "
            "the one thing the game does constantly that no other path in this tool could do");

        option(sFramed, "hour", bpo::value<float>()->default_value(sDefaultHour),
            "what time an exterior's sun is at, on a twenty-four hour clock. An interior is lit "
            "by its own lamps and does not care. Given, it beats an hour a view fixes for itself");

        option(sFramed, "day", bpo::value<int>()->default_value(byDefault.mDay),
            "which day the world stands on, counted from the one a new game starts — 16 Last Seed, "
            "where both moons are full. It is the moons this decides and nothing else: their phase "
            "runs on a three-day cycle and the hour they rise on a twenty-four day one");

        option(Verbs::View | Verbs::Bench, "frames", bpo::value<std::uint32_t>()->default_value(0),
            "how many frames to run: `view` closes after this many instead of waiting to be "
            "closed, and `bench` measures this many at each place instead of deriving them from "
            "--seconds");

        option(Verbs::Bench | Verbs::Check, "suite", bpo::value<std::string>()->default_value(""),
            "which list of places in resources/rtx/benches.cfg to visit: [default] for `bench` "
            "and [check] for `check` unless named. Overridden by --views");

        option(sRuns, "views", bpo::value<std::string>()->default_value(""),
            "which views.cfg views to visit, comma separated, by name rather than by suite. "
            "--views=all runs every view there is, which with `shot --against` is what says what "
            "a change moved");

        option(Verbs::Bench | Verbs::Check, "seconds", bpo::value<float>()->default_value(20.0f),
            "how many seconds of world to run at each place. World and not wall: the "
            "world steps a sixtieth of a second per frame however long the frame took, so this is "
            "twelve hundred frames either way and two builds render the same twelve hundred. Twenty "
            "because ten left the CPU medians moving by more than the changes being measured");

        option(otherThan(Verbs::Info | Verbs::View), "warmup", bpo::value<float>()->default_value(2.0f),
            "how many seconds of world to draw and throw away before measuring. This "
            "machine's GPU idles at 315 MHz and ramps under load, and a scene's first frames pay "
            "for its residency as well. Two rather than three because the ramp and the residency "
            "are over well inside it: measured interleaved on a hot card, three seconds ran 20 s "
            "and two ran 19");

        option(sFramed, "hud", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "draw the game's HUD over the picture: the bars, the compass and the cell's name. Off "
            "unless asked for, and a window's F11 toggles it either way");

        option(sFramed, "vanity", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "let the game's vanity camera orbit the player after thirty idle seconds, as the played "
            "game does. Off unless asked for: a run is idle by nature");

        option(Verbs::Bench, "window", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "show the run while it happens. The swapchain is mailbox, so it does not "
            "pace the loop; --window=false is one fewer thing between the trace and the number");

        option(sFramed, "hold", bpo::value<double>()->default_value(0.0),
            "hold the queue this many milliseconds behind the host after every frame's trace; "
            "`check` holds eight unless told otherwise. The other leg of `repeat` runs under it, "
            "and a `shot --against` its own unheld pictures is the same question of a still: a "
            "picture that is a function of the frames alone comes out the same however far the "
            "device trails, and one that read the clock does not");

        option(Verbs::Bench, "json", bpo::value<std::string>()->default_value(""),
            "also write the run to this file as one record, for comparing against the "
            "same run on another commit");

        option(Verbs::Bench, "perf-control", bpo::value<std::string>()->default_value(""),
            "turn a `perf record --delay=-1 --control=fifo:<path>` on around each "
            "place's measured frames, so the profile holds those frames and not the cell being "
            "loaded either side of them. profile.sh passes this");

        option(Verbs::Shot, "repeat", bpo::value<std::uint32_t>()->default_value(8),
            "trace the frame this many times and report the best. One submit times "
            "the GPU's clock rather than the shader; a comparison worth making wants hundreds");

        option(Verbs::Scene, "find", bpo::value<std::string>()->default_value(""),
            "print where every placement wearing a texture whose path contains this stands. A mesh "
            "keeps no name of its own once it is a run of triangles, so the material it arrived "
            "wearing is what it is found by. How the coordinates in a view are found.");

        option(sFramed, "distant-statics",
            bpo::value<bool>()->default_value(byDefault.mDistantStatics)->implicit_value(true),
            "stand on the distant ground what the content files put there — the buildings, trees "
            "and rocks — as instances of their templates, read ahead of the eye on a thread of the "
            "renderer's own (`Rtx::CellRing`); it is the game's `object paging` setting, which "
            "this renderer never pages by. **Off is the A/B that says what they cost**: the same "
            "ground with nothing on it. The ground itself always stands, read off the land records "
            "by the same ring");

        option(sFramed, "distant-cells", bpo::value<float>()->default_value(byDefault.mDistantCells),
            "how far out the cell ring stands ground and statics, in cells. Outside the active grid "
            "a cell's layer stack is flattened into one baked texture, so this is also how many "
            "cells that path is reached for. Zero hands `viewing distance` back the decision, which "
            "is 7168 against a cell of 8192 and so barely leaves the active grid");

        option(Verbs::Shot | Verbs::Bench, "against", bpo::value<std::string>()->default_value(""),
            "what to subtract this run from: the directory a previous `shot` wrote, or the file "
            "a previous `bench --hashes` wrote, which says which frames of the run now draw "
            "something else and which parts of the scene moved. The reference is always a run of "
            "the previous build on this machine and never a corpus in the tree: the picture is a "
            "function of the driver and the card as much as of the code. Two runs of one build "
            "write the same bytes, upscaled or not, so a picture that differs is a change");

        option(Verbs::Bench, "hashes", bpo::value<std::string>()->default_value(""),
            "write one hash a frame to this file — the oracle a moving camera has "
            "instead of `shot`'s stills, since six hundred frames of pictures is a few hundred "
            "megabytes. Reading a frame back waits on the device, so a run under this or "
            "--against is not a benchmark and its times are not comparable with one");

        option(Verbs::Shot | Verbs::Check, "out", bpo::value<std::string>()->default_value(""),
            "the directory to write every picture into, as <view>.png beside <view>-doll.png, "
            "<view>-map.png and <view>-textures.png: \"shot\" and \"check\" unless named");
        option(sFramed, "size",
            bpo::value<std::string>()->default_value(std::format("{}x{}", byDefault.mWidth, byDefault.mHeight)),
            "image size, as WIDTHxHEIGHT");
        option(sFramed, "fov", bpo::value<float>()->default_value(byDefault.mFieldOfView),
            "vertical field of view, in degrees");
        option(sPlaces, "pos", bpo::value<std::string>()->default_value(""),
            "where to put the camera, as x,y,z. Defaults to a view of the whole cell from outside it, "
            "which is a poor view of an interior. Write --pos=-100,200,300, or a leading minus reads "
            "as an option.");
        option(sPlaces, "look", bpo::value<std::string>()->default_value(""),
            "what the camera looks at, as x,y,z. Defaults to the centre of the cell.");

        option(Verbs::Shot, "accumulate", bpo::value<std::uint32_t>()->default_value(0),
            "average this many differently-seeded frames into one picture. A converged reference, "
            "which is the only ground truth a sampled renderer has: error falls as the square root "
            "of this, so a hundred is a clean picture and a thousand is a reference. Wants "
            "--upscale=off, because a denoiser resolves every frame towards its own opinion rather "
            "than towards the integral");

        option(sFramed, "jitter",
            bpo::value<bool>()->default_value(byDefault.mProfile.mReconstruction.mJitter)->implicit_value(true),
            "sample a different point inside each pixel every frame. Only worth anything to "
            "something putting several frames together, and forced on whenever anything upscales");

        option(Verbs::Every, "data",
            bpo::value<Files::MaybeQuotedPathContainer>()
                ->default_value(Files::MaybeQuotedPathContainer(), "data")
                ->multitoken()
                ->composing(),
            "set data directories (later directories have higher priority)");

        option(Verbs::Every, "data-local",
            bpo::value<Files::MaybeQuotedPathContainer::value_type>()->default_value(
                Files::MaybeQuotedPathContainer::value_type(), ""),
            "set local data directory (highest priority)");

        option(Verbs::Every, "fallback-archive",
            bpo::value<StringsVector>()->default_value(StringsVector(), "fallback-archive")->multitoken()->composing(),
            "set fallback BSA archives (later archives have higher priority)");

        option(Verbs::Every, "content",
            bpo::value<StringsVector>()->default_value(StringsVector(), "")->multitoken()->composing(),
            "content file(s): esm/esp, or omwgame/omwaddon/omwscripts");

        option(Verbs::Every, "encoding", bpo::value<std::string>()->default_value("win1252"),
            "character encoding of the content files");

        option(Verbs::Every, "fallback",
            bpo::value<Fallback::FallbackMap>()->default_value(Fallback::FallbackMap(), "")->multitoken()->composing(),
            "fallback values");

        // **The engine's own two, because a hosted run starts a real game.** Where to stand is a
        // savegame's business — it restores the player, the camera, the hour and every cell the
        // session had loaded, which no pair of coordinates can — and what the world draws at random
        // is the seed's, which is what makes two runs of one build the same run.
        option(Verbs::Every, "load-savegame",
            bpo::value<Files::MaybeQuotedPath>()->default_value(Files::MaybeQuotedPath(), ""),
            "start from this savegame rather than from a new game");

        option(Verbs::Every, "random-seed", bpo::value<unsigned int>()->default_value(42),
            "seed the world's random draws, so two runs of one build draw the same world");

        Files::ConfigurationManager::addCommonOptions(result.mDescription);

        return result;
    }

    std::optional<float> parseFloat(std::string_view text)
    {
        // Not `std::from_chars`: libc++ ships the floating-point overload only from macOS 26. `eof`
        // is what says the whole field was consumed — the same question `from_chars` answers with
        // its end pointer.
        std::istringstream stream{ std::string(text) };
        stream.imbue(std::locale::classic());

        float value = 0.0f;
        if (!(stream >> value) || !stream.eof())
            return std::nullopt;

        return value;
    }

    std::optional<osg::Vec3f> parseVec3(std::string_view text, std::string_view what)
    {
        if (text.empty())
            return std::nullopt;

        const auto fail = [&] {
            throw std::runtime_error(
                std::string(what) + " is not three numbers separated by commas: \"" + std::string(text) + '"');
        };

        osg::Vec3f result;
        for (int axis = 0; axis < 3; ++axis)
        {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);

            const std::size_t comma = text.find(',');
            const std::string_view field = text.substr(0, comma);

            const std::optional<float> value = parseFloat(field);
            if (!value.has_value())
                fail();

            result[axis] = *value;

            const bool last = axis == 2;
            if ((comma == std::string_view::npos) != last)
                fail();

            if (!last)
                text = text.substr(comma + 1);
        }

        return result;
    }

    namespace
    {
        namespace bpo = boost::program_options;
    }

    std::filesystem::path ownConfigDirectory(const Files::ConfigurationManager& config)
    {
        return config.getCachePath() / "rtxtool";
    }

    void adoptConfigDirectory(bpo::variables_map& variables, const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);

        // **What the engine saved on its last way out is the last run's overrides, and read back
        // it would shadow the player's file on this one.** `Settings::Manager::load` takes the last
        // directory's file as the user layer over every other, and the engine writes that layer
        // with everything a run set into it — so a `view` that read the player's `distant land
        // cells` found the four a `shot` had left here, whatever the player's own file said.
        std::filesystem::remove(directory / "settings.cfg");

        // **The map's own entry and not a second parse.** `config` is a composing option, so a value
        // stored from a second source would be merged by rules that are Boost's to keep; the container
        // the first parse left is appended to directly, and `readConfiguration` reads it as it finds it.
        //
        // Present whenever the line was parsed against `ConfigurationManager::addCommonOptions`,
        // because `store` writes a defaulted value for every option of its description that the line
        // left out — so a map without it was parsed against the wrong description.
        const auto found = variables.find("config");
        Rtx::contract(found != variables.end(), "the variables were not parsed against the engine's common options");

        found->second.as<Files::MaybeQuotedPathContainer>().push_back(Files::MaybeQuotedPath{ directory });
    }
}
