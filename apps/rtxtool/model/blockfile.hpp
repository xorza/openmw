#pragma once

#include <cstddef>
#include <filesystem>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

namespace RtxTool
{
    struct Stop;

    /// The number `text` spells, or nothing where it spells anything else — the whole of the text,
    /// so `speed = 1500u` is a refusal and not a run that flew at 1500. Read under the classic
    /// locale, because a decimal point read where a comma is the separator flies at one.
    std::optional<float> parseFloat(std::string_view text);

    /// The point `x,y,z` spells, with spaces around each number, or nothing where the text is
    /// anything else, empty text included. **It writes no refusal**: the caller knows what was
    /// being read and where, and quotes the text it was given whole.
    std::optional<osg::Vec3f> parseVec3(std::string_view text);

    /// `text` without the spaces, tabs and carriage returns around it.
    std::string_view trimmed(std::string_view text);

    /// One `field = value` line of a block, and the line it was written on.
    struct BlockField
    {
        std::string mName;
        std::string mValue;
        std::size_t mLine = 0;
    };

    /// One `[name]` section and its fields, in the order they were written.
    struct Block
    {
        std::string mName;

        /// The line the section opens on, which a refusal of the whole block names.
        std::size_t mLine = 0;

        std::vector<BlockField> mFields;
    };

    /// A file of `[name]` sections and `field = value` lines, read in the order it was written:
    /// the views, the suites and a film's keys are three schemas over this one reader, and a field
    /// they share is read by the one parser below.
    ///
    /// **In order and whole, and never through the settings parser.** That one keeps a map keyed by
    /// section and field, so the views came back sorted by name rather than as the file lists them,
    /// and two sections of one name were one section: a window pressed twice at one place writes
    /// two keys under one name, and a view file that defined a name twice ran whichever it pleased.
    /// What a schema does with a repeated name is the schema's to say.
    ///
    /// Every refusal names the file and the line, because a field misread is a picture of
    /// somewhere else.
    class BlockFile
    {
    public:
        /// Reads `in`, which `source` names in whatever is refused: blank lines and `#` comments
        /// are passed over, and anything else that is neither a `[name]` nor a `field = value`, or
        /// a field before the first section, is refused.
        BlockFile(std::istream& in, std::string source);

        /// The same, from the file at `path`, which is refused where it cannot be read.
        static BlockFile load(const std::filesystem::path& path);

        std::span<const Block> getBlocks() const { return mBlocks; }

        const std::string& getSource() const { return mSource; }

        /// Refuses what the file says at `line`, saying `why`.
        [[noreturn]] void refuse(std::size_t line, std::string_view why) const;

        /// Refuses `field`, quoted whole, as `what` it is not: `pos "1,2" is not three numbers
        /// separated by commas`.
        [[noreturn]] void refuseValue(const BlockField& field, std::string_view what) const;

        /// Refuses `field` as one `block`'s schema does not have, `kind` naming what the block is.
        [[noreturn]] void refuseUnknown(const BlockField& field, std::string_view kind) const;

        /// Refuses `block` for naming what an earlier one did, where a schema reads a name as one
        /// thing: `first` is the block that named it first.
        [[noreturn]] void refuseRepeat(const Block& block, const Block& first, std::string_view kind) const;

        /// The field parsers every schema shares. Each reads the whole value, under the classic
        /// locale, and refuses it by `refuseValue` where it is not what it says.
        float number(const BlockField& field) const;

        /// More than nought, as a speed or a length of time is.
        float positive(const BlockField& field, std::string_view what) const;

        /// Nought or more, as a rest is.
        float notNegative(const BlockField& field, std::string_view what) const;

        /// An hour of the day, from nought up to but not including twenty-four.
        float hour(const BlockField& field) const;

        /// One of the ten weathers the content files name, spelt as they spell it.
        const std::string& weather(const BlockField& field) const;

        /// Three numbers separated by commas; an empty value is no point, and not one left unsaid.
        osg::Vec3f point(const BlockField& field) const;

        /// `true` or `false`, which is how the settings spell one, so a block file and a settings
        /// file agree.
        bool boolean(const BlockField& field) const;

        /// A whole number of days from the one a new game begins on.
        int day(const BlockField& field) const;

        /// Reads `field` into `stop` where it is one of the fields every place states — `cell`,
        /// `pos`, `look`, `note`, `hour` and `weather` — and says whether it was: what a view and a
        /// film's key have in common is a stop.
        bool readPlace(const BlockField& field, Stop& stop) const;

    private:
        std::string mSource;
        std::vector<Block> mBlocks;
    };
}
