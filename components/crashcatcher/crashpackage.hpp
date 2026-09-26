#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Crash
{
    /// One file of a package: the file on disk, and its name inside the archive.
    struct PackageFile
    {
        /// Forward slashes, UTF-8.
        std::string mName;
        std::filesystem::path mSource;
    };

    /// `files`, deflated into a zip at `zip` in their order, each entry stamped `local`, which a zip
    /// keeps as local time to two seconds. Nothing where it was written, and why not where it was not: a source that
    /// could not be read, a file of 4 GiB or more, which a zip without Zip64 cannot hold, or a folder
    /// that refused the write. The archive appears whole or not at all: it is written beside `zip`
    /// and renamed into place.
    std::optional<std::string> writePackage(
        const std::filesystem::path& zip, std::span<const PackageFile> files, const std::tm& local);

    /// What `writeSessionPackage` did.
    struct SessionPackage
    {
        /// Where the package is. Empty where none was written.
        std::filesystem::path mZip;

        /// Why none was written, where the session had something to package. Empty otherwise.
        std::string mFailure;

        /// What the session named and the disk does not have, which the package goes without.
        std::vector<std::filesystem::path> mMissing;
    };

    /// **The one file a player sends**, once the game is gone and its log is whole: `log`, where the
    /// game had one, and every dump of `dumps`, in a new package in `folder` named after
    /// `application` and `local`. Nothing where `dumps` is empty, since then nothing was reported.
    /// Throws nothing: the monitor that packages a crash must not become one, so whatever goes
    /// wrong, a bug in this code included, is a failure it reports.
    SessionPackage writeSessionPackage(const std::filesystem::path& folder, std::string_view application,
        const std::filesystem::path& log, std::span<const std::filesystem::path> dumps, const std::tm& local);

    /// A name for a package in `folder` that no file has yet: `<application>-crash-<local time>.zip`,
    /// with a number after the time where a package of the same second is there already.
    std::filesystem::path freePackagePath(
        const std::filesystem::path& folder, std::string_view application, const std::tm& local);

    /// The address the system's file manager opens `folder` at, from an absolute path: `file://`
    /// and the path with forward slashes, every byte but the unreserved ones and the separators
    /// percent-encoded as UTF-8.
    std::string folderUrl(const std::filesystem::path& folder);

    /// **A new issue, filled in**: the address of GitHub's new-issue page of `issues`, a repository's
    /// issues page, with `title` and a body that asks, in a comment the issue does not show, for
    /// `attach` to be dragged in, then holds `summary` as code. Lines of the summary that would make
    /// the address longer than GitHub serves are left out, and the body says where they are.
    std::string newIssueUrl(
        std::string_view issues, std::string_view title, std::span<const std::string> summary, std::string_view attach);
}
