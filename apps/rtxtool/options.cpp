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

#include "verbs.hpp"
#include "views.hpp"

namespace bpo = boost::program_options;

namespace RtxTool
{
    namespace
    {
        using StringsVector = std::vector<std::string>;

        /// What `--upscale` reads when nobody names it.
        ///
        /// **It follows the build**, because the two are one decision: `-DOPENMW_RTX_DLSS=OFF` is a
        /// deliberate opt-out, and a tool that then refused every default invocation would be
        /// telling its user to turn on the thing they had just turned off.
        ///
        /// Quality rather than performance, so a plain run is the renderer with everything switched
        /// on and not one that quietly quartered the pixels it traced. `--upscale=performance` is
        /// the 1920x1080 to 3840x2160 the frame budget is written against.
#ifdef OPENMW_RTX_DLSS
        constexpr std::string_view sUpscaleByDefault = "quality";
#else
        constexpr std::string_view sUpscaleByDefault = "off";
#endif

        /// The commands that stand at one place, which is what `chooseView` reads a camera for.
        /// A run of places — `bench` and `verify` — takes its cell and its camera from `--views`.
        constexpr Verbs sPlaces = Verbs::Scene | Verbs::Shot | Verbs::View | Verbs::Textures | Verbs::Map;

        /// The commands that frame a camera on the world, which is every one that builds a
        /// `FrameRequest`. `doll` is not one: its picture is framed on a person by the game's own
        /// inventory rules, and `info` draws nothing at all.
        constexpr Verbs sFramed = sPlaces | Verbs::Bench | Verbs::Verify;

        /// How an owned option's help line opens: the commands that read it.
        ///
        /// **Or the ones that do not, where that is the shorter list.** Naming seven commands to
        /// exclude two is a line nobody reads to the end, and the two are what the reader is
        /// actually being told.
        std::string describeOwnership(const Verbs verbs)
        {
            const Verbs missing = otherThan(verbs);
            if (countVerbs(missing) < countVerbs(verbs))
                return std::format("with every command but {}", describeVerbs(missing));

            return std::format("with {}", describeVerbs(verbs));
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
                given.string_key, describeVerbs(reads));
        }

        return complaint;
    }

    ToolOptions makeOptions(const bool validationByDefault)
    {
        ToolOptions result{ bpo::options_description("Options"), {} };
        auto addOption = result.mDescription.add_options();

        // An option only some commands read. **Recorded and printed from one statement**: the help
        // line opens with the commands that read it, and a run that names it under any other one
        // is stopped rather than quietly rendering something else.
        const auto owned
            = [&](Verbs verbs, const char* name, const bpo::value_semantic* semantic, std::string_view description) {
                  result.mOwners.push_back(OptionOwner{ .mName = name, .mVerbs = verbs });
                  addOption(name, semantic, std::format("{}, {}", describeOwnership(verbs), description).c_str());
              };

        addOption("help", "print this message and quit");

        // On unless this was built for release, and `--validation=false` turns any of them off
        // again. An implicit value is what lets the bare `--validation` still mean "yes".
        addOption("validation", bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "load VK_LAYER_KHRONOS_validation. On by default outside a Release build");
        addOption("sync-validation", bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "add synchronization validation, which catches missing barriers (implies --validation)");
        addOption("gpu-validation", bpo::value<bool>()->default_value(validationByDefault)->implicit_value(true),
            "add GPU-assisted validation, which instruments shaders and catches what a ray query "
            "does with its own arguments (implies --validation). Costs about half the frame rate, "
            "and is left off by `view` unless asked for: a window under it loses the device");

        owned(sPlaces, "cell", bpo::value<std::string>()->default_value(""),
            "cell to read, addressed the way Morrowind does: a pair of integers is an exterior, "
            "anything else is an interior's name. Write --cell=-2,-9 rather than --cell -2,-9, or "
            "the leading minus reads as an option. Left out, the default view decides.");

        owned(Verbs::Scene, "twice", bpo::bool_switch(),
            "extract the cell a second time and report what the second pass added, which should "
            "be nothing");

        owned(sPlaces, "view", bpo::value<std::string>()->default_value(""),
            "a named viewpoint from resources/rtx/views.cfg, which supplies the cell and usually the "
            "camera. Overrides --cell. A run of places names them with --views instead.");

        addOption("list-views", bpo::bool_switch(), "print the named viewpoints and quit");

        addOption("delight", bpo::value<float>()->default_value(1.0f),
            "how much of the lighting painted into each texture to divide back out, from 0 to 1. "
            "Zero is the A/B that says what it did");
        addOption("filter", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "run the denoiser over the indirect light. Off shows the raw bounce, and is what a "
            "reference is made with");
        // Defaulted to an empty list rather than left absent, because `readConfiguration` walks
        // every option in this description and casts it: a composing option with no value in the
        // map is a `bad_any_cast` on every run that did not name one.
        addOption("actor", bpo::value<StringsVector>()->default_value(StringsVector(), "")->composing(),
            "put an animated creature in front of the camera, by the model path a CREA record "
            "holds — meshes/r/cliffracer.nif, not the x-prefixed skeleton beside it. Repeatable, "
            "and several stand in a row across the view. This is the only way to see skinned "
            "geometry without starting the game, which is what it is for");
        addOption("npc", bpo::value<StringsVector>()->default_value(StringsVector(), "")->composing(),
            "put a person in front of the camera, by their NPC record id -- fargoth, "
            "\"caius cosades\". Repeatable, and they stand in the same row the creatures do. They "
            "arrive dressed out of their own record, which --clothes is what turns off");
        addOption("people", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "put the region's own residents in it, assembled from their races' body parts and "
            "standing where the cell puts them. On, because a town with nobody in it is not the "
            "picture this renderer is being judged on; off is the A/B that says what they cost, and "
            "what a profiling run should hold still");
        addOption("props", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "run the cell's particle emitters -- the candles, torches, braziers and fires. On, "
            "because a template's emitters are frozen at the seed the file authored and a lit room "
            "with no flames in it is not the picture; off leaves them as that seed, which is the "
            "A/B that says what a cell's emitters cost");
        addOption("clothes", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "dress and arm people out of what their own record carries, which is what the game "
            "equips them with -- clothes, armour, a shield, and a weapon in the hand. Off leaves "
            "everyone in their skin and empty-handed, which is worth looking at, because skin is "
            "the hardest surface in the game to get right and the one the shipped textures have the "
            "most light painted into");
        addOption("actor-time", bpo::value<float>()->default_value(0.0f),
            "how many seconds into its animation each actor stands, wrapped to the track's own "
            "length. A --repeat carries on from there at sixty frames a second, so a repeat with "
            "actors in it measures an animated frame -- the skinning, and the structure rebuild "
            "behind it -- rather than the same frame over again");

        addOption("upscale", bpo::value<std::string>()->default_value(std::string(sUpscaleByDefault)),
            "put DLSS Ray Reconstruction between the trace and the picture: off, performance, "
            "balanced, quality or dlaa. --size is what comes out, and what gets traced is DLSS's "
            "answer for it. It denoises for itself, so --filter stops applying. Quality by default, "
            "so a plain run is the renderer with everything switched on without quartering the "
            "pixels it traced; --upscale=performance is the 1920x1080 to 3840x2160 the frame budget "
            "is written against, and --upscale=off is what an A/B against the unupscaled path "
            "needs. A reference cannot be "
            "built through a denoiser");

        addOption("reorder", bpo::value<std::string>()->default_value("off"),
            "how the trace sorts its threads between the traversal and the shader that resolves what "
            "it found: off, hit, hint or both. Shader Execution Reordering regroups a warp so that "
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
            "last-bit difference into a different lamp");

        addOption("preset", bpo::value<std::string>()->default_value("d"),
            "which Ray Reconstruction network to run: default, d or e. Ray Reconstruction keeps its "
            "own presets, and they are not super-resolution's -- A through C are retired, d is the "
            "default transformer model and e is the latest. `default` hands the choice to the "
            "installed library, which has changed between SDK versions and between the "
            "convolutional and transformer models, so two runs under it are not the same "
            "measurement. Pinned to d so that they are");

        addOption("exposure", bpo::value<std::string>()->default_value("auto"),
            "what to scale the frame by before the display curve: auto measures it off the frame, "
            "and a number holds it there. A pixel test and a converged reference want it held, "
            "because a measured exposure makes every value depend on the whole frame");

        owned(Verbs::Shot, "dump", bpo::value<std::string>()->default_value(""),
            "also write the frame in linear radiance to this path: four floats a pixel, "
            "raw, at the render extent. What a measurement is taken on, where the PNG is what a "
            "picture is looked at as");
        addOption("albedo", bpo::bool_switch(),
            "write the albedo with no shading over it, which is what a texture problem looks like "
            "when nothing else is in the way");

        addOption("weather", bpo::value<std::string>()->default_value(std::string(sDefaultWeather)),
            "which weather's sun, sky and precipitation an exterior stands under, named as the "
            "content files spell it: Clear, Cloudy, Foggy, Overcast, Rain, Thunderstorm, Ashstorm, "
            "Blight, Snow, Blizzard. The ones that drop something drop it here too. Given, it beats "
            "a weather a view fixes for itself");

        owned(Verbs::Bench, "turn-weather", bpo::value<std::string>()->default_value(""),
            "turn the sky through these weathers while each place runs, comma "
            "separated and round again — --turn-weather=Rain,Foggy. Each transition takes four "
            "seconds of world, as a window's weather keys do, and the precipitation of the one "
            "arriving replaces the one leaving halfway through. **A run under it is not a "
            "benchmark**: no two places stand under the same sky. It is here because a weather "
            "turning frees a whole emitter's meshes and textures on an ordinary frame, which is "
            "the one thing the game does constantly that no other path in this tool could do");

        addOption("hour", bpo::value<float>()->default_value(sDefaultHour),
            "what time an exterior's sun is at, on a twenty-four hour clock. An interior is lit "
            "by its own lamps and does not care. Given, it beats an hour a view fixes for itself");

        addOption("day", bpo::value<int>()->default_value(0),
            "which day the world stands on, counted from the one a new game starts — 16 Last Seed, "
            "where both moons are full. It is the moons this decides and nothing else: their phase "
            "runs on a three-day cycle and the hour they rise on a twenty-four day one");

        owned(Verbs::View | Verbs::Bench, "frames", bpo::value<std::uint32_t>()->default_value(0),
            "how many frames to run: `view` closes after this many instead of waiting to be "
            "closed, and `bench` measures this many at each place instead of deriving them from "
            "--seconds");

        owned(Verbs::Bench, "suite", bpo::value<std::string>()->default_value("default"),
            "which list of places in resources/rtx/benches.cfg to profile. Overridden "
            "by --views");

        owned(Verbs::Bench | Verbs::Verify, "views", bpo::value<std::string>()->default_value(""),
            "which views.cfg views to visit, by name rather than by suite — the places `bench` "
            "profiles and the ones `verify` renders. --views=all runs every view there is");

        owned(Verbs::Bench, "seconds", bpo::value<float>()->default_value(20.0f),
            "how many seconds of world to run at each place. World and not wall: the "
            "world steps a sixtieth of a second per frame however long the frame took, so this is "
            "twelve hundred frames either way and two builds render the same twelve hundred. Twenty "
            "because ten left the CPU medians moving by more than the changes being measured");

        owned(Verbs::Bench | Verbs::Shot | Verbs::Verify, "warmup", bpo::value<float>()->default_value(3.0f),
            "how many seconds of world to draw and throw away before measuring. This "
            "machine's GPU idles at 315 MHz and ramps under load, and a scene's first frames pay "
            "for its residency as well");

        owned(Verbs::Bench, "window", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "show the run while it happens. The swapchain is mailbox, so it does not "
            "pace the loop; --window=false is one fewer thing between the trace and the number");

        owned(Verbs::Bench, "json", bpo::value<std::string>()->default_value(""),
            "also write the run to this file as one record, for comparing against the "
            "same run on another commit");

        owned(Verbs::Bench, "perf-control", bpo::value<std::string>()->default_value(""),
            "turn a `perf record --delay=-1 --control=fifo:<path>` on around each "
            "place's measured frames, so the profile holds those frames and not the cell being "
            "loaded either side of them. profile.sh passes this");

        owned(Verbs::Shot, "repeat", bpo::value<std::uint32_t>()->default_value(8),
            "trace the frame this many times and report the best. One submit times "
            "the GPU's clock rather than the shader; a comparison worth making wants hundreds");

        owned(Verbs::Scene, "find", bpo::value<std::string>()->default_value(""),
            "print the world position of every object whose model path contains this. "
            "How the coordinates in a view are found.");

        addOption("distant-terrain", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "page the terrain the way the game does with `distant terrain` on, through "
            "Terrain::QuadTreeWorld instead of Terrain::TerrainGrid. **The one terrain a mirror "
            "cannot find by walking**: a quad tree resolves its chunks inside a cull and parents "
            "them to nothing, so this is the only way anything headless can see whether the ground "
            "is reached at all. On by default, since a radius means nothing without it; "
            "`--distant-terrain=false` puts the staged cells back");

        addOption("distant-statics", bpo::value<bool>()->default_value(true)->implicit_value(true),
            "with `--distant-terrain`, stand on the distant ground what the content files put there "
            "— the buildings, trees and rocks — through the same `Terrain::ObjectPaging` the game "
            "registers under `object paging`. **Off is the A/B that says what they cost**: the same "
            "ground with nothing on it, which is also every run of this harness before they arrived");

        addOption("distant-cells", bpo::value<float>()->default_value(5.0f),
            "with `--distant-terrain`, how far out the quad tree may make ground, in cells. Past a "
            "cell a chunk's layer stack is flattened into one baked texture, so this is also what "
            "decides whether that path is reached at all. Zero hands `viewing distance` back the "
            "decision, which is 7168 against a cell of 8192 and so barely leaves the active grid");

        owned(Verbs::Bench | Verbs::Verify, "against", bpo::value<std::string>()->default_value(""),
            "what to subtract this run from: the directory a previous `verify` wrote, or the file "
            "a previous `bench --hashes` wrote, which says which frames of the run now draw "
            "something else. The reference is always a run of the previous build on this machine "
            "and never a corpus in the tree: the picture is a function of the driver and the card "
            "as much as of the code");

        owned(Verbs::Bench, "hashes", bpo::value<std::string>()->default_value(""),
            "write one hash a frame to this file — the oracle a moving camera has "
            "instead of `verify`'s stills, since six hundred frames of pictures is a few hundred "
            "megabytes. Reading a frame back waits on the device, so a run under this or "
            "--against is not a benchmark and its times are not comparable with one");

        owned(Verbs::Shot | Verbs::Textures | Verbs::Doll | Verbs::Map | Verbs::Verify, "out",
            bpo::value<std::string>()->default_value("shot.png"),
            "where to write the image, or with `verify` the directory to write every view into "
            "(\"verify\" unless named)");
        addOption("size", bpo::value<std::string>()->default_value("1920x1080"), "image size, as WIDTHxHEIGHT");
        owned(sFramed, "fov", bpo::value<float>()->default_value(60.0f), "vertical field of view, in degrees");
        owned(sPlaces | Verbs::Doll, "pos", bpo::value<std::string>()->default_value(""),
            "where to put the camera, as x,y,z. Defaults to a view of the whole cell from outside it, "
            "which is a poor view of an interior. Write --pos=-100,200,300, or a leading minus reads "
            "as an option.");
        owned(sPlaces | Verbs::Doll, "look", bpo::value<std::string>()->default_value(""),
            "what the camera looks at, as x,y,z. Defaults to the centre of the cell.");

        owned(Verbs::Shot, "accumulate", bpo::value<std::uint32_t>()->default_value(0),
            "average this many differently-seeded frames into one picture. A converged reference, "
            "which is the only ground truth a sampled renderer has: error falls as the square root "
            "of this, so a hundred is a clean picture and a thousand is a reference. Wants "
            "--upscale=off, because a denoiser resolves every frame towards its own opinion rather "
            "than towards the integral");

        owned(Verbs::Shot, "tail", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "report the share of pixels whose bounce luminance passes each of a ladder of "
            "thresholds. What a firefly is counted in, and the one thing bytes cannot say. Wants "
            "--upscale=off so the wavelet and its accumulator run at all");

        addOption("jitter", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "sample a different point inside each pixel every frame. Only worth anything to "
            "something putting several frames together, and forced on whenever anything upscales");

        addOption("crossings", bpo::value<bool>()->default_value(false)->implicit_value(true),
            "also count the see-through surfaces each primary ray crosses. A second traversal a "
            "pixel, so a frame time taken under it measures the census rather than the picture");

        addOption("data",
            bpo::value<Files::MaybeQuotedPathContainer>()
                ->default_value(Files::MaybeQuotedPathContainer(), "data")
                ->multitoken()
                ->composing(),
            "set data directories (later directories have higher priority)");

        addOption("data-local",
            bpo::value<Files::MaybeQuotedPathContainer::value_type>()->default_value(
                Files::MaybeQuotedPathContainer::value_type(), ""),
            "set local data directory (highest priority)");

        addOption("fallback-archive",
            bpo::value<StringsVector>()->default_value(StringsVector(), "fallback-archive")->multitoken()->composing(),
            "set fallback BSA archives (later archives have higher priority)");

        addOption("content", bpo::value<StringsVector>()->default_value(StringsVector(), "")->multitoken()->composing(),
            "content file(s): esm/esp, or omwgame/omwaddon/omwscripts");

        addOption(
            "encoding", bpo::value<std::string>()->default_value("win1252"), "character encoding of the content files");

        addOption("fallback",
            bpo::value<Fallback::FallbackMap>()->default_value(Fallback::FallbackMap(), "")->multitoken()->composing(),
            "fallback values");

        // **The engine's own two, because a hosted run starts a real game.** Where to stand is a
        // savegame's business — it restores the player, the camera, the hour and every cell the
        // session had loaded, which no pair of coordinates can — and what the world draws at random
        // is the seed's, which is what makes two runs of one build the same run.
        addOption("load-savegame", bpo::value<Files::MaybeQuotedPath>()->default_value(Files::MaybeQuotedPath(), ""),
            "start from this savegame rather than from a new game");

        addOption("random-seed", bpo::value<unsigned int>()->default_value(42),
            "seed the world's random draws, so two runs of one build draw the same world");

        Files::ConfigurationManager::addCommonOptions(result.mDescription);

        return result;
    }
}
