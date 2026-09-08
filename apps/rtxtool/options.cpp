#include "options.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/option.hpp>
#include <boost/program_options/value_semantic.hpp>

#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/reorder.hpp>
#include <components/rtx/upscale.hpp>

#include "framerequest.hpp"
#include "verbs.hpp"
#include "views.hpp"

namespace bpo = boost::program_options;

namespace RtxTool
{
    namespace
    {
        using StringsVector = std::vector<std::string>;

        /// The commands that stand at one place, which is every one that calls `chooseView`.
        /// A run of places — `bench`, `verify` and `check` — takes its cell and its camera from
        /// `--views` instead.
        constexpr Verbs sPlaces = Verbs::Scene | Verbs::Shot | Verbs::View | Verbs::Textures | Verbs::Map | Verbs::Doll;

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

    ToolOptions makeOptions(const bool validationByDefault)
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

        // On unless this was built for release, and `--validation=false` turns any of them off
        // again. An implicit value is what lets the bare `--validation` still mean "yes".
        option(Verbs::Every, "validation", bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "load VK_LAYER_KHRONOS_validation. On by default outside a Release build");
        option(Verbs::Every, "sync-validation",
            bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "add synchronization validation, which catches missing barriers (implies --validation)");
        option(Verbs::Every, "gpu-validation",
            bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "add GPU-assisted validation, which instruments shaders and catches what a ray query "
            "does with its own arguments (implies --validation). Costs about half the frame rate, "
            "and is left off by `view` unless asked for: a window under it loses the device");

        option(sPlaces, "cell", bpo::value<std::string>()->default_value(""),
            "cell to read, addressed the way Morrowind does: a pair of integers is an exterior, "
            "anything else is an interior's name. Write --cell=-2,-9 rather than --cell -2,-9, or "
            "the leading minus reads as an option. Left out, the default view decides.");

        option(Verbs::Scene, "twice", bpo::bool_switch(),
            "extract the cell a second time and report what the second pass added, which should "
            "be nothing");

        option(sPlaces, "view", bpo::value<std::string>()->default_value(""),
            "a named viewpoint from resources/rtx/views.cfg, which supplies the cell and usually the "
            "camera. Overrides --cell. A run of places names them with --views instead.");

        option(Verbs::Every, "list-views", bpo::bool_switch(), "print the named viewpoints and quit");

        option(sFramed, "delight", bpo::value<float>()->default_value(byDefault.mDelight),
            "how much of the lighting painted into each texture to divide back out, from 0 to 1. "
            "Zero is the A/B that says what it did");
        option(sFramed, "filter", bpo::value<bool>()->default_value(byDefault.mFilter)->implicit_value(true),
            "run the denoiser over the indirect light. Off shows the raw bounce, and is what a "
            "reference is made with");
        // Defaulted to an empty list rather than left absent, because `readConfiguration` walks
        // every option in this description and casts it: a composing option with no value in the
        // map is a `bad_any_cast` on every run that did not name one.
        option(Verbs::Doll, "npc", bpo::value<StringsVector>()->default_value(StringsVector(), "")->composing(),
            "whose inventory doll to draw, by NPC record id -- fargoth, \"caius cosades\". "
            "Repeatable, and each one is written beside the last. They arrive dressed out of their "
            "own record, which is what the game equips them with");

        option(sFramed, "upscale",
            bpo::value<std::string>()->default_value(std::string(Rtx::upscaleName(byDefault.mUpscale))),
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

        option(sFramed, "micromaps", bpo::value<bool>()->default_value(byDefault.mMicromaps)->implicit_value(true),
            "bake each cutout's mask into an opacity micromap, so traversal resolves every "
            "microtriangle it knows about without stopping the ray. Off leaves every cutout to the "
            "any-hit shader, which is the leg a micromap has to be timed against — and below Ada a "
            "micromap is the driver's emulation rather than the hardware, so what it saves there is "
            "not what it saves here. The picture is the same either way");

        option(sFramed, "reorder",
            bpo::value<std::string>()->default_value(std::string(Rtx::reorderName(byDefault.mReorder))),
            std::format("how the trace sorts its threads between the traversal and the shader that resolves "
                        "what it found: {}. Shader Execution Reordering regroups a warp so that "
                        "its lanes are about to run the same shader on the same data. `hit` sorts on the hit "
                        "object the traversal answered, `hint` sorts on a coherence hint instead and so keeps "
                        "the launch's own locality, and `both` is the two together. The shader a hit object "
                        "names is picked by traversal either way, so the frame is split across a closest-hit "
                        "shader per material kind whatever this says. Off by default because off is faster here: "
                        "every form of the call costs 7 to 17 percent at each view of the default suite and buys "
                        "nothing back, since the trace ends in eleven channel writes laid out along the launch's "
                        "own neighbourhood and a sort is what gives that neighbourhood up. It also moves the "
                        "picture on a handful of pixels rather than on none: the call is a barrier the driver "
                        "rebuilds the code around, and one bounce sample and one lamp draw a pixel turn a "
                        "last-bit difference into a different lamp",
                Rtx::sReorderNames.list())
                .c_str());

        option(sFramed, "preset",
            bpo::value<std::string>()->default_value(std::string(Rtx::presetName(byDefault.mPreset))),
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

        option(Verbs::Shot, "dump", bpo::value<std::string>()->default_value(""),
            "also write the frame in linear radiance to this path: four floats a pixel, "
            "raw, at the render extent. What a measurement is taken on, where the PNG is what a "
            "picture is looked at as");
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

        option(Verbs::Bench | Verbs::Check, "suite", bpo::value<std::string>()->default_value("default"),
            "which list of places in resources/rtx/benches.cfg to profile. Overridden "
            "by --views");

        option(Verbs::Bench | Verbs::Verify | Verbs::Check, "views", bpo::value<std::string>()->default_value(""),
            "which views.cfg views to visit, by name rather than by suite — the places `bench` "
            "profiles and the ones `verify` renders. --views=all runs every view there is");

        option(Verbs::Bench | Verbs::Check, "seconds", bpo::value<float>()->default_value(20.0f),
            "how many seconds of world to run at each place. World and not wall: the "
            "world steps a sixtieth of a second per frame however long the frame took, so this is "
            "twelve hundred frames either way and two builds render the same twelve hundred. Twenty "
            "because ten left the CPU medians moving by more than the changes being measured");

        option(otherThan(Verbs::Info | Verbs::View), "warmup", bpo::value<float>()->default_value(3.0f),
            "how many seconds of world to draw and throw away before measuring. This "
            "machine's GPU idles at 315 MHz and ramps under load, and a scene's first frames pay "
            "for its residency as well");

        option(Verbs::Bench, "window", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "show the run while it happens. The swapchain is mailbox, so it does not "
            "pace the loop; --window=false is one fewer thing between the trace and the number");

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
            "and rocks — which is the game's own `object paging`. **Off is the A/B that says what "
            "they cost**: the same ground with nothing on it. The ground itself is always paged, "
            "because `Renderer::wantsPagedTerrain` answers for a renderer that traces rather than "
            "draws");

        option(sFramed, "distant-cells", bpo::value<float>()->default_value(byDefault.mDistantCells),
            "with `--distant-statics`, how far out the quad tree may make ground, in cells. Past a "
            "cell a chunk's layer stack is flattened into one baked texture, so this is also what "
            "decides whether that path is reached at all. Zero hands `viewing distance` back the "
            "decision, which is 7168 against a cell of 8192 and so barely leaves the active grid");

        option(Verbs::Bench | Verbs::Verify, "against", bpo::value<std::string>()->default_value(""),
            "what to subtract this run from: the directory a previous `verify` wrote, or the file "
            "a previous `bench --hashes` wrote, which says which frames of the run now draw "
            "something else. The reference is always a run of the previous build on this machine "
            "and never a corpus in the tree: the picture is a function of the driver and the card "
            "as much as of the code");

        option(Verbs::Bench, "hashes", bpo::value<std::string>()->default_value(""),
            "write one hash a frame to this file — the oracle a moving camera has "
            "instead of `verify`'s stills, since six hundred frames of pictures is a few hundred "
            "megabytes. Reading a frame back waits on the device, so a run under this or "
            "--against is not a benchmark and its times are not comparable with one");

        option(Verbs::Shot | Verbs::Textures | Verbs::Doll | Verbs::Map | Verbs::Verify, "out",
            bpo::value<std::string>()->default_value("shot.png"),
            "where to write the image, or with `verify` the directory to write every view into "
            "(\"verify\" unless named)");
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

        option(Verbs::Shot, "tail", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "report the share of pixels whose bounce luminance passes each of a ladder of "
            "thresholds. What a firefly is counted in, and the one thing bytes cannot say. Wants "
            "--upscale=off so the wavelet and its accumulator run at all");

        option(sFramed, "jitter", bpo::value<bool>()->default_value(byDefault.mJitter)->implicit_value(true),
            "sample a different point inside each pixel every frame. Only worth anything to "
            "something putting several frames together, and forced on whenever anything upscales");

        option(sFramed, "crossings", bpo::value<bool>()->default_value(byDefault.mCountCrossings)->implicit_value(true),
            "also count the see-through surfaces each primary ray crosses. A second traversal a "
            "pixel, so a frame time taken under it measures the census rather than the picture");

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
}
