#include "parsefloat.hpp"

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
}
