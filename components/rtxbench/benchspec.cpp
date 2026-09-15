#include "benchspec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Rtx
{
    std::uint32_t BenchSpan::getFrames() const
    {
        if (mFrames > 0)
            return mFrames;

        if (!(mSeconds > 0.0f))
            return 0;

        // At least one, so a span short enough to round to nothing still measures the frame it
        // asked for rather than silently measuring none.
        return std::max(1u, static_cast<std::uint32_t>(std::lround(mSeconds * sStepRate)));
    }

    std::vector<std::string> splitNames(std::string_view text)
    {
        std::vector<std::string> names;

        for (std::size_t at = 0; at <= text.size();)
        {
            const std::size_t comma = std::min(text.find(',', at), text.size());
            std::string_view name = text.substr(at, comma - at);

            while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                name.remove_prefix(1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.remove_suffix(1);

            if (!name.empty())
                names.emplace_back(name);

            at = comma + 1;
        }

        return names;
    }
}
