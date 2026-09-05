#include "parsefloat.hpp"
#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>

#include <locale>
#include <sstream>
#include <string>

namespace RtxTool
{
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
}
