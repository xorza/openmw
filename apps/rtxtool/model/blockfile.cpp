#include "blockfile.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <components/files/conversion.hpp>
#include <components/rtx/skylight.hpp>

#include "benchrun.hpp"

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

    std::optional<osg::Vec3f> parseVec3(std::string_view text)
    {
        osg::Vec3f result;
        for (int axis = 0; axis < 3; ++axis)
        {
            const bool last = axis == 2;
            const std::size_t comma = text.find(',');
            if ((comma == std::string_view::npos) != last)
                return std::nullopt;

            const std::optional<float> value = parseFloat(trimmed(text.substr(0, comma)));
            if (!value.has_value())
                return std::nullopt;

            result[axis] = *value;
            if (!last)
                text.remove_prefix(comma + 1);
        }

        return result;
    }

    std::string_view trimmed(std::string_view text)
    {
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (!text.empty() && blank(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && blank(text.back()))
            text.remove_suffix(1);
        return text;
    }

    BlockFile::BlockFile(std::istream& in, std::string source)
        : mSource(std::move(source))
    {
        std::size_t number = 0;
        for (std::string line; std::getline(in, line);)
        {
            ++number;
            const std::string_view text = trimmed(line);
            if (text.empty() || text.front() == '#')
                continue;

            if (text.front() == '[')
            {
                if (text.back() != ']')
                    refuse(number, "a section's name is not closed by ]");

                mBlocks.push_back(
                    Block{ .mName = std::string(trimmed(text.substr(1, text.size() - 2))), .mLine = number });
                continue;
            }

            const std::size_t equals = text.find('=');
            if (equals == std::string_view::npos)
                refuse(number, std::format("\"{}\" is neither a [section], a field = value, nor a # comment", text));
            if (mBlocks.empty())
                refuse(number, "a field comes before the first [section]");

            mBlocks.back().mFields.push_back(BlockField{ .mName = std::string(trimmed(text.substr(0, equals))),
                .mValue = std::string(trimmed(text.substr(equals + 1))),
                .mLine = number });
        }
    }

    BlockFile BlockFile::load(const std::filesystem::path& path)
    {
        std::ifstream in(path);
        if (!in)
            throw std::runtime_error("cannot read " + Files::pathToUnicodeString(path));

        return BlockFile(in, Files::pathToUnicodeString(path));
    }

    void BlockFile::refuse(const std::size_t line, const std::string_view why) const
    {
        throw std::runtime_error(std::format("{}:{}: {}", mSource, line, why));
    }

    void BlockFile::refuseValue(const BlockField& field, const std::string_view what) const
    {
        refuse(field.mLine, std::format("{} \"{}\" {}", field.mName, field.mValue, what));
    }

    void BlockFile::refuseUnknown(const BlockField& field, const std::string_view kind) const
    {
        refuse(field.mLine, std::format("a {} has no field called \"{}\"", kind, field.mName));
    }

    void BlockFile::refuseRepeat(const Block& block, const Block& first, const std::string_view kind) const
    {
        refuse(block.mLine,
            std::format("a second {} called \"{}\", which line {} already defines", kind, block.mName, first.mLine));
    }

    float BlockFile::number(const BlockField& field) const
    {
        const std::optional<float> value = parseFloat(field.mValue);
        if (!value.has_value())
            refuseValue(field, "is not a number");

        return *value;
    }

    float BlockFile::positive(const BlockField& field, const std::string_view what) const
    {
        const float value = number(field);
        if (!(value > 0.0f))
            refuseValue(field, std::format("is not {}", what));

        return value;
    }

    float BlockFile::notNegative(const BlockField& field, const std::string_view what) const
    {
        const float value = number(field);
        if (!(value >= 0.0f))
            refuseValue(field, std::format("is not {}", what));

        return value;
    }

    float BlockFile::hour(const BlockField& field) const
    {
        const float value = number(field);
        if (!(value >= 0.0f) || !(value < 24.0f))
            refuseValue(field, "is not from 0 up to but not including 24");

        return value;
    }

    const std::string& BlockFile::weather(const BlockField& field) const
    {
        // **Checked here rather than at the frame**, for the reason a mistyped view id is: a place
        // that quietly stood under another sky reports a number against a frame nobody asked for.
        if (!Rtx::weatherIndex(field.mValue).has_value())
            refuseValue(field, "is none of the weathers the content files name");

        return field.mValue;
    }

    osg::Vec3f BlockFile::point(const BlockField& field) const
    {
        const std::optional<osg::Vec3f> value = parseVec3(field.mValue);
        if (!value.has_value())
            refuseValue(field, "is not three numbers separated by commas");

        return *value;
    }

    bool BlockFile::boolean(const BlockField& field) const
    {
        if (field.mValue != "true" && field.mValue != "false")
            refuseValue(field, "is not true or false");

        return field.mValue == "true";
    }

    int BlockFile::day(const BlockField& field) const
    {
        const std::string& text = field.mValue;
        int value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size() || value < 0)
            refuseValue(field, "is not a whole number of days from nought");

        return value;
    }

    bool BlockFile::readPlace(const BlockField& field, Stop& stop) const
    {
        if (field.mName == "cell")
            stop.mStand.mCell = field.mValue;
        else if (field.mName == "pos")
            stop.mStand.mEye = point(field);
        else if (field.mName == "look")
            stop.mStand.mLook = point(field);
        else if (field.mName == "note")
            stop.mNote = field.mValue;
        else if (field.mName == "hour")
            stop.mSky.mHour = hour(field);
        else if (field.mName == "weather")
            stop.mSky.mWeather = weather(field);
        else
            return false;

        return true;
    }
}
