#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace Rtx
{
    namespace
    {
        /// What the tree says about itself, asked of the sources rather than of a build: two rules
        /// that were each a shell script somebody had to remember to run, and each found its
        /// holdouts only when somebody did.
        const std::filesystem::path sBackend
            = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "components" / "rtxvulkan";

        std::vector<std::string> linesOf(const std::filesystem::path& file)
        {
            std::ifstream in(file);
            std::vector<std::string> lines;
            for (std::string line; std::getline(in, line);)
                lines.push_back(line);
            return lines;
        }

        /// The `vkDestroyX(` or `mDestroyX(` call on `code`, as the name called, or nothing.
        ///
        /// By hand and not by `std::regex`, because GCC 16 under AddressSanitizer reads
        /// `<regex>`'s own `std::function` as maybe-uninitialized and `-Werror` makes that a
        /// build that does not exist.
        std::optional<std::string> destroyCallOn(const std::string_view code)
        {
            for (std::size_t at = code.find("Destroy"); at != std::string_view::npos; at = code.find("Destroy", at + 1))
            {
                std::size_t start = 0;
                if (at >= 2 && code.substr(at - 2, 2) == "vk")
                    start = at - 2;
                else if (at >= 1 && code[at - 1] == 'm')
                    start = at - 1;
                else
                    continue;

                const auto word = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                if (start > 0 && word(code[start - 1]))
                    continue;

                std::size_t end = at + std::string_view("Destroy").size();
                while (end < code.size() && std::isalpha(static_cast<unsigned char>(code[end])))
                    ++end;

                std::size_t paren = end;
                while (paren < code.size() && std::isspace(static_cast<unsigned char>(code[paren])))
                    ++paren;

                if (paren < code.size() && code[paren] == '(')
                    return std::string(code.substr(start, end - start));
            }

            return std::nullopt;
        }

        /// What a `#include "..."` line names, or nothing for any other line.
        std::optional<std::string> includedBy(const std::string_view line)
        {
            if (!line.starts_with("#include"))
                return std::nullopt;

            const std::size_t open = line.find('"');
            const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
            if (close == std::string_view::npos)
                return std::nullopt;

            return std::string(line.substr(open + 1, close - open - 1));
        }

        std::string joined(const std::vector<std::string>& lines)
        {
            std::string all;
            for (const std::string& line : lines)
                all += line + '\n';
            return all;
        }

        /// Every Vulkan handle the backend owns is held by `Rtx::Owned`.
        ///
        /// **The rule is mechanical, and the holdouts accumulated silently.** `owned.hpp` says it
        /// is "the one place `vkDestroyX(device, handle, allocator)` is spelled", and a class that
        /// spelled it itself paid a destructor, a null check and a `const Device&` member that
        /// existed so the destructor could reach the device. Nothing made the next class adopt the
        /// type, so this is what does. A loaded destroyer (`mDestroyX`) is matched as well as a
        /// declared one, because the first of them stood in three classes for a year with the
        /// gate reporting that every handle was held.
        ///
        /// The exemptions, each for a reason a match cannot see: `vkDestroyInstance` and
        /// `vkDestroyDevice` take no parent handle, so `Owned`'s shape does not fit them;
        /// `vkDestroySurfaceKHR` and the messenger take the instance rather than the device, two
        /// sites not worth a second template parameter; `graveyard.cpp` destroys handles it was
        /// *given*, the counterpart of `Owned::release`; `accelerationstructure.cpp` destroys
        /// through a pointer the device loaded, which `Owned`'s template argument cannot name, so
        /// it is the `Owned` for that handle. Comments are stripped before the match, because
        /// `owned.hpp`'s own prose and half a dozen others name these calls to explain them.
        TEST(RtxSourceTreeTest, everyDeviceParentedVulkanHandleIsHeldByOwned)
        {
            const std::set<std::string> exemptFiles{ "owned.hpp", "graveyard.cpp", "accelerationstructure.cpp" };
            const std::set<std::string> allowed{ "vkDestroyInstance", "vkDestroyDevice", "vkDestroySurfaceKHR",
                "vkDestroyDebugUtilsMessengerEXT", "mDestroyMessenger" };

            std::vector<std::string> found;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(sBackend))
            {
                const std::filesystem::path& file = entry.path();
                if (file.extension() != ".cpp" && file.extension() != ".hpp")
                    continue;
                if (exemptFiles.contains(file.filename().string()))
                    continue;

                const std::vector<std::string> lines = linesOf(file);
                for (std::size_t at = 0; at < lines.size(); ++at)
                {
                    const std::string code = lines[at].substr(0, lines[at].find("//"));
                    const std::optional<std::string> call = destroyCallOn(code);
                    if (call.has_value() && !allowed.contains(*call))
                        found.push_back(file.filename().string() + ':' + std::to_string(at + 1) + ": " + code);
                }
            }

            EXPECT_TRUE(found.empty())
                << "a Vulkan handle is destroyed by hand where Rtx::Owned would do it — hold it "
                   "as Owned<Handle, vkDestroyX> and delete the destructor, or add the site to the "
                   "exemptions above with the reason it cannot be one:\n"
                << joined(found);
        }

        /// A device object answers for its own readers, and the device answers for none of them.
        ///
        /// **`Device::mayDestroy` was the question every destructor asked**, and it answered "the
        /// queue is idle, or the graveyard is freeing": a flag set for a whole sweep, under which
        /// no destructor could tell a buried object from one destroyed by mistake. Every object a
        /// submit can name carries a `ReadStamp` now and asks it, so the one place the word is
        /// allowed is the object's own file, and a device-wide answer cannot come back.
        TEST(RtxSourceTreeTest, onlyAnObjectAnswersWhetherItMayBeDestroyed)
        {
            const std::set<std::string> owners{ "buffer.hpp", "buffer.cpp", "image.hpp", "image.cpp" };

            std::vector<std::string> found;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(sBackend))
            {
                const std::filesystem::path& file = entry.path();
                if (file.extension() != ".cpp" && file.extension() != ".hpp")
                    continue;
                if (owners.contains(file.filename().string()))
                    continue;

                const std::vector<std::string> lines = linesOf(file);
                for (std::size_t at = 0; at < lines.size(); ++at)
                {
                    const std::string code = lines[at].substr(0, lines[at].find("//"));
                    if (code.find("mayDestroy") != std::string::npos || code.find("isReaping") != std::string::npos)
                        found.push_back(file.filename().string() + ':' + std::to_string(at + 1) + ": " + code);
                }
            }

            EXPECT_TRUE(found.empty())
                << "a device object asks something other than its own stamp whether it may be destroyed — "
                   "give it a ReadStamp and ask that:\n"
                << joined(found);
        }

        /// Every file `shader` reaches through `#include`, itself included, by the paths the
        /// shaders spell: relative to the including file.
        void reachedBy(const std::filesystem::path& shader, std::set<std::filesystem::path>& reached)
        {
            if (!reached.insert(shader).second)
                return;

            for (const std::string& line : linesOf(shader))
            {
                if (const std::optional<std::string> included = includedBy(line))
                {
                    const std::filesystem::path named = shader.parent_path() / *included;
                    if (std::filesystem::exists(named))
                        reachedBy(named, reached);
                }
            }
        }

        /// No compute shader traces a ray.
        ///
        /// **A ray query inside a compute dispatch answered differently from run to run** — a
        /// candidate counted twice or not at all — while another process shared the card, and
        /// never inside a ray-tracing launch. The fog's two ray passes became launches for it,
        /// and the gate was a script that ran the determinism walk beside a second harness for
        /// two and a half minutes a pair. The cause is a compute shader reaching `rayQueryEXT`,
        /// and that is what is asked here, of every `.comp` and everything it includes.
        TEST(RtxSourceTreeTest, noComputeShaderTracesARay)
        {
            const std::filesystem::path shaders = sBackend / "shaders";

            std::vector<std::string> found;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(shaders))
            {
                if (entry.path().extension() != ".comp")
                    continue;

                std::set<std::filesystem::path> reached;
                reachedBy(entry.path(), reached);

                for (const std::filesystem::path& file : reached)
                {
                    const std::vector<std::string> lines = linesOf(file);
                    const bool queries = std::any_of(lines.begin(), lines.end(),
                        [](const std::string& line) { return line.find("rayQueryEXT") != std::string::npos; });
                    if (queries)
                        found.push_back(entry.path().filename().string() + " reaches "
                            + std::filesystem::relative(file, shaders).string());
                }
            }

            EXPECT_TRUE(found.empty()) << "a compute shader traces a ray, which answers differently under another "
                                          "process's preemption — make the pass a launch:\n"
                                       << joined(found);
        }
    }
}
