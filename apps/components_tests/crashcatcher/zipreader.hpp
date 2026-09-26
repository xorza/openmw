#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace CrashTests
{
    /// One entry of a zip, inflated.
    struct ZipEntry
    {
        std::string mName;
        std::uint16_t mFlags = 0;
        std::uint16_t mMethod = 0;
        std::uint32_t mDosTime = 0;
        std::uint32_t mCrc = 0;
        std::string mContent;
    };

    /// **Every entry of the zip at `path`, read as an unzip reads it**: through the end record and
    /// the central directory, each entry checked against its local header, inflated to the size it
    /// states and summed to the CRC it states, the entries end to end and the directory after them.
    /// Nothing where all of that holds, and the first thing that does not where one does not. A
    /// comment, an extra field or Zip64 is refused, since the package writes none.
    inline std::optional<std::string> readZip(const std::filesystem::path& path, std::vector<ZipEntry>& into)
    {
        into.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return "no zip at " + path.string();
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        bool inside = true;
        const auto u16 = [&](std::size_t at) -> std::uint32_t {
            if (at + 2 > bytes.size())
            {
                inside = false;
                return 0;
            }
            return static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8;
        };
        const auto u32 = [&](std::size_t at) { return u16(at) | u16(at + 2) << 16; };

        if (bytes.size() < 22)
            return "shorter than an end record";
        const std::size_t end = bytes.size() - 22;
        if (u32(end) != 0x06054b50)
            return "no end record at the end";
        const std::uint32_t count = u16(end + 10);
        if (u16(end + 4) != 0 || u16(end + 6) != 0 || u16(end + 8) != count || u16(end + 20) != 0)
            return "the end record is not one disk's, or has a comment";
        const std::uint32_t directorySize = u32(end + 12);
        const std::uint32_t directory = u32(end + 16);
        if (std::size_t{ directory } + directorySize != end)
            return "the central directory does not end where the end record begins";

        std::size_t central = directory;
        std::size_t local = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ZipEntry entry;
            if (u32(central) != 0x02014b50)
                return "no central header " + std::to_string(i);
            const std::uint32_t needed = u16(central + 6);
            entry.mFlags = static_cast<std::uint16_t>(u16(central + 8));
            entry.mMethod = static_cast<std::uint16_t>(u16(central + 10));
            entry.mDosTime = u32(central + 12);
            entry.mCrc = u32(central + 16);
            const std::uint32_t compressed = u32(central + 20);
            const std::uint32_t size = u32(central + 24);
            const std::uint32_t nameLength = u16(central + 28);
            if (u16(central + 30) != 0 || u16(central + 32) != 0)
                return "central header " + std::to_string(i) + " has an extra field or a comment";
            if (u32(central + 42) != local)
                return "entry " + std::to_string(i) + " does not begin where the one before it ends";
            if (central + 46 + nameLength > end)
                return "central header " + std::to_string(i) + " runs past the directory";
            entry.mName = bytes.substr(central + 46, nameLength);
            central += 46 + nameLength;

            if (local + 30 + nameLength > directory || u32(local) != 0x04034b50)
                return "no local header for " + entry.mName;
            if (u16(local + 4) != needed || u16(local + 6) != entry.mFlags || u16(local + 8) != entry.mMethod
                || u32(local + 10) != entry.mDosTime || u32(local + 14) != entry.mCrc || u32(local + 18) != compressed
                || u32(local + 22) != size || u16(local + 26) != nameLength || u16(local + 28) != 0
                || bytes.compare(local + 30, nameLength, entry.mName) != 0)
                return "the local header of " + entry.mName + " says something else than the central one";
            const std::size_t data = local + 30 + nameLength;
            if (data + compressed > directory)
                return "the data of " + entry.mName + " runs into the central directory";
            if (entry.mMethod != 8)
                return entry.mName + " is not deflated";

            entry.mContent.resize(size);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                return "zlib would not start";
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data() + data));
            stream.avail_in = compressed;
            stream.next_out = reinterpret_cast<Bytef*>(entry.mContent.data());
            stream.avail_out = size;
            const int result = inflate(&stream, Z_FINISH);
            const bool whole = result == Z_STREAM_END && stream.total_in == compressed && stream.total_out == size;
            inflateEnd(&stream);
            if (!whole)
                return entry.mName + " does not inflate to its stated size from its stated length";
            if (crc32(crc32(0, Z_NULL, 0), reinterpret_cast<const Bytef*>(entry.mContent.data()), size) != entry.mCrc)
                return entry.mName + " does not sum to its CRC";

            local = data + compressed;
            into.push_back(std::move(entry));
        }

        if (!inside)
            return "a header runs past the end of the file";
        if (local != directory || central != end)
            return "bytes between the entries, the directory and the end record";
        return std::nullopt;
    }
}
