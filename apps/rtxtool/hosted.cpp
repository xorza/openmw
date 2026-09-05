#include "hosted.hpp"

#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options/variables_map.hpp>

#include <components/debug/debugging.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/settings/values.hpp>
#include <components/toutf8/toutf8.hpp>

#include <apps/openmw/engine.hpp>

#include "viewpoint.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;
    }

    int runHosted(const bpo::variables_map& variables, Files::ConfigurationManager& config,
        const std::filesystem::path& resources, MWRender::SessionRequest request, const Viewpoint* spot)
    {
        std::ostream& out = Debug::getRawStdout();

        const StringsVector content = variables["content"].as<StringsVector>();
        if (content.empty())
        {
            out << "no content file given: name one with --content, or point openmw.cfg at an "
                   "installation.\n";
            return 1;
        }

        // **The renderer this tool exists to drive, whatever the user's settings file says.** The
        // choice is read once inside `Engine::go` and never revisited, so it is set here rather
        // than left to whichever value a played session was last configured with.
        Settings::rtx().mEnabled.set(true);

        // **Every run this tool makes is a measured one**, so the world steps by the frame index
        // and never by the clock — which is what makes two runs of one build the same run.
        Settings::rtx().mFixedStep.set(Rtx::sStepSeconds);

        // **The limiter comes off, because there is nobody to pace for.** A hosted run is measured
        // or it is written to a file, and a frame held back to meet a refresh is a frame spent
        // waiting.
        Settings::video().mFramerateLimit.set(0);

        // **Whether the run was meant to end on its own**, which is what says an empty report is a
        // failure. A window somebody closes has finished no stop and owes no numbers.
        const bool scheduled = request.mQuitAtEnd;

        // **The engine is destroyed before the result is read, and that is what makes the result
        // exist.** `MWRender::Session` publishes from its own destructor, because a run that ends
        // its last stop and a window somebody closes both have to be reported and only the first
        // ever reaches `finish`. `OMW::Engine` declares the world before the renderer that holds
        // the session, so the world is still standing when it is asked where the eye was left.
        {
            OMW::Engine engine(config);
            engine.setRecastMaxLogLevel(Debug::getRecastMaxLogLevel());

            engine.setEncoding(ToUTF8::calculateEncoding(variables["encoding"].as<std::string>()));
            engine.setResourceDir(resources);

            Files::PathContainer dataDirs(
                Files::asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>()));
            if (Files::PathContainer::value_type local(
                    variables["data-local"].as<Files::MaybeQuotedPathContainer::value_type>().u8string());
                !local.empty())
                dataDirs.push_back(std::move(local));

            config.filterOutNonExistingPaths(dataDirs);
            engine.setDataDirs(dataDirs);

            for (const std::string& archive : variables["fallback-archive"].as<StringsVector>())
                engine.addArchive(archive);

            // **The same first file and the same refusal of a repeat as `apps/openmw/main.cpp`.** A
            // content list read here and there by different rules is two installations described as
            // one, which is the drift this whole path exists to remove.
            engine.addContentFile("builtin.omwscripts");
            std::set<std::string> once{ "builtin.omwscripts" };
            for (const std::string& file : content)
            {
                if (!once.insert(file).second)
                {
                    out << "content file specified more than once: " << file << '\n';
                    return 1;
                }

                engine.addContentFile(file);
            }

            Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);

            // **Straight into the world, with no character generation.** `setSkipMenu(true, false)`
            // reaches `StateManager::newGame(true)`, which is the bypass a session wants: a stop says
            // where it stands, and standing anywhere at all is the only thing the start has to do.
            engine.setSkipMenu(true, false);
            engine.setSaveGameFile(variables["load-savegame"].as<Files::MaybeQuotedPath>().u8string());
            engine.setRandomSeed(variables["random-seed"].as<unsigned int>());

            // **No sound and no mouse, because nobody is here.** A run measured with an audio device
            // open measures the mixer as well, and a grabbed pointer in a headless run is a pointer
            // somebody has to get back.
            engine.setSoundUsage(false);
            engine.setGrabMouse(false);

            MWRender::installSession(std::move(request));

            engine.go();
        }

        const MWRender::SessionResult result = MWRender::takeSessionResult();
        out << result.mReport;

        // **Where it was left, so a session that ended somewhere worth keeping did not lose it.**
        // The names are this tool's — a view id and a cell spelling are what it was asked for — and
        // where the eye stood is the game's.
        if (spot != nullptr)
        {
            Viewpoint left = *spot;
            left.mOrigin = result.mEye;
            left.mTarget = result.mLook;
            left.mHour = result.mHour;
            left.mDay = result.mDay;
            left.mWeather = result.mWeather;

            out << describeSpot(left) << describeBlock(left);
        }

        // **A run that reached no stop is a failure and not an empty report.** A cell that could
        // not be loaded and a save that would not open both end here, and each of them is a command
        // that did not do what it was asked. A window somebody closed is not one of them.
        if (scheduled && result.mPlaces.empty())
        {
            out << "\nnothing was measured: the run ended before a stop finished\n";
            return 1;
        }

        return result.mExitStatus;
    }
}
