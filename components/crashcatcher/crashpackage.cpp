#include "crashpackage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

#include <components/files/conversion.hpp>
#include <components/platform/process.hpp>

namespace Crash
{
    namespace
    {
        /// What the central directory repeats of an entry once its data is written.
        struct Entry
        {
            const PackageFile* mFile = nullptr;
            std::uint16_t mFlags = 0;
            std::uint32_t mCrc = 0;
            std::uint32_t mCompressed = 0;
            std::uint32_t mSize = 0;
            std::uint32_t mOffset = 0;
        };

        /// 2.0, the first version with deflate: what an entry needs to be read, and what wrote it,
        /// with the host byte nought, which says nothing of Unix permissions.
        constexpr std::uint16_t sVersion = 20;
        constexpr std::uint16_t sDeflated = 8;

        /// The name is UTF-8, where the format's default is code page 437.
        constexpr std::uint16_t sUtf8Name = 1 << 11;

        /// Every size and offset of a zip without Zip64 is four bytes.
        constexpr std::uint64_t sLargest = 0xFFFFFFFFu;

        /// The longest new-issue address to hand GitHub: measured in September 2026, a body of 6000
        /// bytes opens the page, and 7500 answer 500 and 8200 answer 414.
        constexpr std::size_t sLongestUrl = 6000;

        /// Appends `text` to `into` with every byte but the unreserved ones and `kept` percent-encoded:
        /// UTF-8 as it is, byte by byte.
        void percentEncode(std::string& into, std::string_view text, std::string_view kept)
        {
            static constexpr char sHex[] = "0123456789ABCDEF";
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '.' || c == '_' || c == '~';
                if (unreserved || kept.find(c) != std::string_view::npos)
                    into += c;
                else
                {
                    into += '%';
                    into += sHex[byte >> 4];
                    into += sHex[byte & 0xF];
                }
            }
        }

        /// Where a local header keeps the CRC, which the sizes follow: known once the data is written.
        constexpr std::streamoff sCrcAt = 14;

        void put16(std::string& into, std::uint16_t value)
        {
            into.push_back(static_cast<char>(value & 0xFF));
            into.push_back(static_cast<char>(value >> 8));
        }

        void put32(std::string& into, std::uint32_t value)
        {
            put16(into, static_cast<std::uint16_t>(value & 0xFFFF));
            put16(into, static_cast<std::uint16_t>(value >> 16));
        }

        /// The MS-DOS date and time, the time in the low half, as both headers store them in turn:
        /// seconds halved, years from 1980, and nothing outside 1980 to 2107.
        std::uint32_t dosTime(const std::tm& local)
        {
            const int year = local.tm_year + 1900;
            if (year < 1980)
                return (1u << 5 | 1u) << 16;
            if (year > 2107)
                return ((127u << 9 | 12u << 5 | 31u) << 16) | (23u << 11 | 59u << 5 | 29u);

            const auto date = static_cast<std::uint32_t>((year - 1980) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday);
            // A leap second's 60 would halve to 30, which the five bits allow and no reader expects.
            const auto time
                = static_cast<std::uint32_t>(local.tm_hour << 11 | local.tm_min << 5 | std::min(local.tm_sec, 59) / 2);
            return date << 16 | time;
        }

        bool isAscii(std::string_view text)
        {
            return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
        }

        class Deflater
        {
        public:
            Deflater()
            {
                // Raw deflate, the stream a zip entry holds: no zlib header, no trailer.
                mStarted = deflateInit2(&mStream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY)
                    == Z_OK;
            }

            Deflater(const Deflater&) = delete;
            Deflater& operator=(const Deflater&) = delete;

            ~Deflater()
            {
                if (mStarted)
                    deflateEnd(&mStream);
            }

            z_stream mStream{};
            bool mStarted = false;
        };

        /// Deflates `file` into `archive` after its local header, and fills the rest of `entry`.
        std::optional<std::string> writeData(std::ofstream& archive, const PackageFile& file, Entry& entry,
            std::vector<char>& in, std::vector<char>& out)
        {
            // **The size first, and every byte of it read**: a stream ends a read that failed as it ends
            // one at the end of the file, and a short entry must not pass as a whole one.
            std::error_code error;
            const std::uintmax_t expected = std::filesystem::file_size(file.mSource, error);
            if (error)
                return Files::pathToUnicodeString(file.mSource) + " could not be read: " + error.message();
            if (expected > sLargest)
                return file.mName + " is 4 GiB or more, which a zip without Zip64 cannot hold";

            std::ifstream source(file.mSource, std::ios::binary);
            if (!source)
                return Files::pathToUnicodeString(file.mSource) + " could not be read";

            Deflater deflater;
            if (!deflater.mStarted)
                return "zlib would not start a deflate stream";
            z_stream& stream = deflater.mStream;

            uLong crc = crc32(0, Z_NULL, 0);
            std::uint64_t size = 0;
            std::uint64_t compressed = 0;
            int result = Z_OK;
            for (;;)
            {
                source.read(in.data(), static_cast<std::streamsize>(in.size()));
                const auto got = static_cast<uInt>(source.gcount());
                size += got;
                if (source.bad())
                    return Files::pathToUnicodeString(file.mSource) + " could not be read to its end";
                if (size > expected)
                    return Files::pathToUnicodeString(file.mSource) + " grew while it was read";
                crc = crc32(crc, reinterpret_cast<const Bytef*>(in.data()), got);

                const int flush = source.eof() ? Z_FINISH : Z_NO_FLUSH;
                stream.next_in = reinterpret_cast<Bytef*>(in.data());
                stream.avail_in = got;
                // Until deflate leaves room in the output: it has then taken all the input, and, at
                // the finish, ended the stream.
                do
                {
                    stream.next_out = reinterpret_cast<Bytef*>(out.data());
                    stream.avail_out = static_cast<uInt>(out.size());
                    result = deflate(&stream, flush);
                    const std::size_t made = out.size() - stream.avail_out;
                    archive.write(out.data(), static_cast<std::streamsize>(made));
                    compressed += made;
                } while (stream.avail_out == 0);

                if (flush == Z_FINISH)
                    break;
            }

            if (size != expected)
                return Files::pathToUnicodeString(file.mSource) + " could not be read to its end";
            if (result != Z_STREAM_END)
                return "zlib did not end the deflate stream of " + file.mName;
            if (!archive)
                return "the package could not be written: the disk refused " + file.mName;
            if (compressed > sLargest)
                return file.mName + " deflates to 4 GiB or more, which a zip without Zip64 cannot hold";

            entry.mCrc = static_cast<std::uint32_t>(crc);
            entry.mCompressed = static_cast<std::uint32_t>(compressed);
            entry.mSize = static_cast<std::uint32_t>(size);
            return {};
        }

        std::optional<std::string> writeArchive(const std::filesystem::path& part, std::span<const PackageFile> files,
            std::uint32_t stamp, std::vector<Entry>& entries)
        {
            std::ofstream archive(part, std::ios::binary | std::ios::trunc);
            if (!archive)
                return Files::pathToUnicodeString(part) + " could not be made";

            std::vector<char> in(1 << 16);
            std::vector<char> out(1 << 16);
            std::string header;
            for (const PackageFile& file : files)
            {
                const std::streamoff offset = archive.tellp();
                if (offset < 0 || static_cast<std::uint64_t>(offset) > sLargest)
                    return "the package reached 4 GiB, which a zip without Zip64 cannot hold";

                Entry& entry = entries.emplace_back();
                entry.mFile = &file;
                entry.mFlags = isAscii(file.mName) ? 0 : sUtf8Name;
                entry.mOffset = static_cast<std::uint32_t>(offset);

                // The CRC and the sizes as noughts for now, written over once the data is written.
                header.clear();
                put32(header, 0x04034b50);
                put16(header, sVersion);
                put16(header, entry.mFlags);
                put16(header, sDeflated);
                put32(header, stamp);
                put32(header, 0);
                put32(header, 0);
                put32(header, 0);
                put16(header, static_cast<std::uint16_t>(file.mName.size()));
                put16(header, 0);
                header += file.mName;
                archive.write(header.data(), static_cast<std::streamsize>(header.size()));

                if (std::optional<std::string> why = writeData(archive, file, entry, in, out))
                    return why;

                header.clear();
                put32(header, entry.mCrc);
                put32(header, entry.mCompressed);
                put32(header, entry.mSize);
                const std::streamoff end = archive.tellp();
                archive.seekp(offset + sCrcAt);
                archive.write(header.data(), static_cast<std::streamsize>(header.size()));
                archive.seekp(end);
            }

            const std::streamoff directory = archive.tellp();
            header.clear();
            for (const Entry& entry : entries)
            {
                put32(header, 0x02014b50);
                put16(header, sVersion);
                put16(header, sVersion);
                put16(header, entry.mFlags);
                put16(header, sDeflated);
                put32(header, stamp);
                put32(header, entry.mCrc);
                put32(header, entry.mCompressed);
                put32(header, entry.mSize);
                put16(header, static_cast<std::uint16_t>(entry.mFile->mName.size()));
                put16(header, 0);
                put16(header, 0);
                put16(header, 0);
                put16(header, 0);
                put32(header, 0);
                put32(header, entry.mOffset);
                header += entry.mFile->mName;
            }
            if (directory < 0 || static_cast<std::uint64_t>(directory) + header.size() > sLargest)
                return "the package reached 4 GiB, which a zip without Zip64 cannot hold";

            const auto directorySize = static_cast<std::uint32_t>(header.size());
            put32(header, 0x06054b50);
            put16(header, 0);
            put16(header, 0);
            put16(header, static_cast<std::uint16_t>(entries.size()));
            put16(header, static_cast<std::uint16_t>(entries.size()));
            put32(header, directorySize);
            put32(header, static_cast<std::uint32_t>(directory));
            put16(header, 0);
            archive.write(header.data(), static_cast<std::streamsize>(header.size()));

            archive.close();
            if (!archive)
                return Files::pathToUnicodeString(part) + " could not be written";
            return {};
        }
    }

    std::optional<std::string> writePackage(
        const std::filesystem::path& zip, std::span<const PackageFile> files, const std::tm& local)
    {
        // A zip without Zip64 counts its entries in two bytes.
        if (files.size() > 0xFFFF)
            return "a zip without Zip64 holds 65535 files at most";

        // Named after the process too, so two monitors that chose one name never write one file.
        std::filesystem::path part = zip;
        part += "." + std::to_string(Platform::Process::currentId()) + ".part";

        std::vector<Entry> entries;
        entries.reserve(files.size());
        std::optional<std::string> why = writeArchive(part, files, dosTime(local), entries);

        std::error_code error;
        if (!why)
        {
            std::filesystem::rename(part, zip, error);
            if (error)
                why = Files::pathToUnicodeString(zip) + " could not be put in place: " + error.message();
        }
        if (why)
            std::filesystem::remove(part, error);
        return why;
    }

    SessionPackage writeSessionPackage(const std::filesystem::path& folder, std::string_view application,
        const std::filesystem::path& log, std::span<const std::filesystem::path> dumps, const std::tm& local)
    {
        SessionPackage package;
        if (dumps.empty())
            return package;

        try
        {
            std::vector<PackageFile> files;
            files.reserve(dumps.size() + 1);
            const auto take = [&](const std::filesystem::path& path) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error))
                    files.push_back({ Files::pathToUnicodeString(path.filename()), path });
                else
                    package.mMissing.push_back(path);
            };

            // A game that crashed before it set its log up has none, which is not a missing one.
            if (!log.empty())
                take(log);
            for (const std::filesystem::path& dump : dumps)
                take(dump);
            if (files.empty())
            {
                package.mFailure = "neither the log nor a dump is on disk";
                return package;
            }

            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(folder, error).lexically_normal();
            const std::filesystem::path zip = freePackagePath(error ? folder : absolute, application, local);
            if (std::optional<std::string> why = writePackage(zip, files, local))
                package.mFailure = std::move(*why);
            else
                package.mZip = zip;
        }
        catch (const std::exception& error)
        {
            package.mZip.clear();
            package.mFailure = error.what();
        }
        return package;
    }

    std::filesystem::path freePackagePath(
        const std::filesystem::path& folder, std::string_view application, const std::tm& local)
    {
        char time[32];
        std::strftime(time, sizeof(time), "%Y-%m-%d-%H%M%S", &local);
        const std::string stem = std::string(application) + "-crash-" + time;

        std::filesystem::path path = folder / Files::pathFromUnicodeString(stem + ".zip");
        // A folder that cannot be looked in has the first name free, and the write then says why not.
        std::error_code error;
        for (int i = 2; std::filesystem::exists(path, error); ++i)
            path = folder / Files::pathFromUnicodeString(stem + "-" + std::to_string(i) + ".zip");
        return path;
    }

    std::string folderUrl(const std::filesystem::path& folder)
    {
        const std::u8string path = folder.generic_u8string();
        std::string url = "file://";
        // A drive's path, "C:/...", gets the slash a POSIX one begins with.
        if (!path.starts_with(u8'/'))
            url += '/';
        percentEncode(url, { reinterpret_cast<const char*>(path.data()), path.size() }, "/:");
        return url;
    }

    std::string newIssueUrl(
        std::string_view issues, std::string_view title, std::span<const std::string> summary, std::string_view attach)
    {
        std::string url(issues);
        url += "/new?title=";
        percentEncode(url, title, {});
        url += "&body=";
        percentEncode(
            url, "<!-- Drag " + std::string(attach) + " from the folder that opened into this box. -->\n\n```\n", {});

        std::string closing;
        percentEncode(closing, "```\n", {});
        std::string cut;
        percentEncode(cut, "[the rest is in " + std::string(attach) + "]\n", {});

        // A line fits where what must follow it fits too: the closing after the last, and before it,
        // the line that says the rest was cut, which the next line may need.
        std::string line;
        for (std::size_t i = 0; i < summary.size(); ++i)
        {
            line.clear();
            percentEncode(line, summary[i] + "\n", {});
            const std::size_t after = i + 1 == summary.size() ? 0 : cut.size();
            if (url.size() + line.size() + after + closing.size() > sLongestUrl)
            {
                url += cut;
                break;
            }
            url += line;
        }
        url += closing;
        return url;
    }
}
