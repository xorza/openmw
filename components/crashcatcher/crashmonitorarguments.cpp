#include "crashmonitorarguments.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>

#include <components/files/conversion.hpp>

namespace Crash
{
    namespace
    {
        constexpr std::string_view sClient = "--openmw-client";
        constexpr std::string_view sNotes = "--openmw-notes";
        constexpr std::string_view sApplication = "--openmw-application";
        constexpr std::string_view sDialog = "--openmw-dialog";
        constexpr std::string_view sDatabase = "--database";

        std::string option(std::string_view name, std::string_view value)
        {
            return std::string(name) + "=" + std::string(value);
        }

        /// The value of `--name=value` in `argument`, where it is that option.
        std::optional<std::string_view> valueOf(std::string_view argument, std::string_view name)
        {
            if (!argument.starts_with(name) || argument.size() <= name.size() || argument[name.size()] != '=')
                return std::nullopt;
            return argument.substr(name.size() + 1);
        }
    }

    std::vector<std::string> MonitorArguments::write() const
    {
        char notes[48];
        std::snprintf(notes, sizeof(notes), "0x%llx:%llu", static_cast<unsigned long long>(mNotes),
            static_cast<unsigned long long>(mNotesSize));

        return {
            std::string(sMonitorSwitch),
            option(sClient, std::to_string(mClient)),
            option(sNotes, notes),
            option(sApplication, mApplication),
            option(sDialog, mDialog ? "1" : "0"),
        };
    }

    MonitorArguments MonitorArguments::read(std::span<const std::string> arguments, std::vector<std::string>& handler)
    {
        MonitorArguments read;
        handler.clear();
        for (const std::string& argument : arguments)
        {
            if (argument == sMonitorSwitch)
                continue;

            if (const auto client = valueOf(argument, sClient))
                read.mClient = static_cast<std::uint32_t>(std::strtoul(std::string(*client).c_str(), nullptr, 10));
            else if (const auto notes = valueOf(argument, sNotes))
            {
                // An address and a length that do not both read as numbers are no table at all.
                const std::string text(*notes);
                char* end = nullptr;
                read.mNotes = std::strtoull(text.c_str(), &end, 16);
                read.mNotesSize = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0;
                if (read.mNotesSize == 0)
                    read.mNotes = 0;
            }
            else if (const auto application = valueOf(argument, sApplication))
                read.mApplication = *application;
            else if (const auto dialog = valueOf(argument, sDialog))
                read.mDialog = *dialog != "0";
            else
            {
                if (const auto database = valueOf(argument, sDatabase))
                    read.mDatabase = Files::pathFromUnicodeString(*database);
                handler.push_back(argument);
            }
        }
        return read;
    }
}
