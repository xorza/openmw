#include "benchspec.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <system_error>

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

    std::string BenchSpan::describe() const
    {
        return mFrames > 0 ? std::format("{} frames", mFrames) : std::format("{:.0f} s", mSeconds);
    }

    std::optional<BenchSpan> readSpan(std::string_view text)
    {
        const bool timed = !text.empty() && text.back() == 's';
        if (timed)
            text.remove_suffix(1);

        std::uint32_t value = 0;
        const auto* end = text.data() + text.size();
        const std::from_chars_result read = std::from_chars(text.data(), end, value);
        if (read.ec != std::errc{} || read.ptr != end)
            return std::nullopt;

        return timed ? BenchSpan{ .mSeconds = static_cast<float>(value) } : BenchSpan{ .mFrames = value };
    }

    std::optional<BenchSpec> readSpec(std::string_view text, std::string& complaint)
    {
        BenchSpec spec;

        // **The speed comes off the end first**, so what is left is the run and its warm-up and the
        // two spellings need not know about each other.
        if (const std::size_t at = text.find('@'); at != std::string_view::npos)
        {
            const std::string_view speed = text.substr(at + 1);
            const auto* end = speed.data() + speed.size();
            if (std::from_chars(speed.data(), end, spec.mSpeed).ec != std::errc{} || !(spec.mSpeed > 0.0f))
            {
                complaint = std::format("\"{}\" is not a positive speed", speed);
                return std::nullopt;
            }

            text = text.substr(0, at);
        }

        const std::size_t split = text.find(':');
        const std::optional<BenchSpan> run = readSpan(text.substr(0, split));
        if (!run.has_value() || run->empty())
        {
            complaint = std::format("\"{}\" is neither a frame count nor a duration", text.substr(0, split));
            return std::nullopt;
        }

        spec.mRun = *run;

        if (split != std::string_view::npos)
        {
            const std::string_view warm = text.substr(split + 1);
            const std::optional<BenchSpan> read = readSpan(warm);
            if (!read.has_value())
            {
                complaint = std::format("\"{}\" is neither a frame count nor a duration", warm);
                return std::nullopt;
            }

            spec.mWarm = *read;
        }

        return spec;
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
