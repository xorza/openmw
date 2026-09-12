#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchrun.hpp>

#include "framereport.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    class RunRecord;
}

namespace MWRender
{
    class OffscreenView;

    /// What a stop asked for and what it came to, beside the frame it drew.
    ///
    /// **Named for the reason `FrameContext` is.** Each of these is read by one claim and by nothing
    /// else, so one value carries them past `write` and `runChecks`, which read neither.
    ///
    /// Borrowed and valid for one stop.
    struct StopFacts
    {
        /// What the stop's route came to, which only `CrossingsAppend` reads.
        const Rtx::Crossings& mCrossings;

        /// What the stop asked its camera to be, which only `CameraStands` reads.
        const Rtx::Stand& mStand;
    };

    /// Writes what `Rtx::Actions` asks of the place a stop stood at.
    ///
    /// **What a stop produces, apart from what drove it there.** A picture, a radiance dump, a
    /// bounce tail, a scene report, a contact sheet, a map tile, a doll, a listing and a set of
    /// claims each need the renderer and a path. None of them needs the schedule, the route, the
    /// camera or the sky, so none of them is a member of the thing that owns those.
    ///
    /// **Held for the whole run for its read-back buffer alone.** Three of the writers land a frame
    /// in host memory, and a stop that allocates one is a stop measuring its own allocator.
    class StopWriter
    {
    public:
        /// Does everything `actions` asks of the frame `owner` last drew, and says what it came to
        /// in `record`.
        ///
        /// **The last measured frame, and never a later one.** Every figure the stop reports
        /// describes those frames, so a picture taken after them is a picture of a different run.
        ///
        /// @param reconstruction what put that frame back together, which decides what channels it
        ///        has. `Rtx::hasFrameImage` is where that rule lives.
        /// @param facts what the stop asked for and came to, which only a check reads.
        void write(const FrameContext& context, const FrameReport& report, const Rtx::Actions& actions,
            const StopFacts& facts, Rtx::RunRecord& record);

    private:
        /// The frame a writer reads, what put it together, and where it says its answer.
        ///
        /// **None of the three is a member.** The renderer and the reconstruction both arrive per
        /// frame — `Session` is built inside `RtxRenderer`'s own constructor, so there is no
        /// renderer to hold — and the record belongs to the run rather than to the writing.
        /// Bundling them keeps the three off every writer's signature.
        struct Writing
        {
            const FrameContext& mContext;
            const FrameReport& mReport;
            Rtx::RunRecord& mRecord;
        };

        /// The last measured frame, as a PNG.
        void writeCapture(const Writing& into, const std::filesystem::path& file);

        /// The share of that frame's pixels whose accumulated bounce passes each of a ladder of
        /// luminances. Fails the run where the frame carries no accumulated bounce.
        void reportTail(const Writing& into);

        /// That frame's linear radiance, four floats a pixel, raw.
        ///
        /// **No container around it**, because what reads it is a script computing an error against
        /// another one, and every image format that carries floats would have to be decoded first.
        void writeDump(const Writing& into, const std::filesystem::path& file);

        /// What the renderer was handed, as `scene` reports it.
        void reportScene(const Writing& into);

        /// Every texture the scene holds, vanilla beside de-lit, as one sheet.
        void writeSheet(const Writing& into, const std::filesystem::path& sheet);

        /// One local-map tile of wherever the stop stands.
        void writeMapTile(const Writing& into, const std::filesystem::path& file);

        /// The inventory doll of one person.
        void writeDoll(const Writing& into, const std::string& who, const std::filesystem::path& file);

        /// Lists the textures whose path holds `needle`, and where the meshes wearing them stand.
        void reportFound(const Writing& into, const std::string& needle);

        /// Draws `view` and writes what it drew, right way up.
        ///
        /// **A picture inside the interface is written bottom row first**, which is what
        /// `OffscreenView::getTexture` promises and what the widgets showing one invert V for. A
        /// file wants the other order, so the rows are turned over on the way out.
        void writeView(const Writing& into, OffscreenView& view, const std::filesystem::path& file);

        /// Writes a picture that is already in main memory, bottom row first, as a PNG.
        void writeImage(const Writing& into, const osg::Image& drawn, const std::filesystem::path& file);

        /// Asks every check the stop named, and reports each one's answer.
        void runChecks(const Writing& into, std::span<const Rtx::Check> checks, const StopFacts& facts);

        /// Whether one check holds of what the run was handed and what it drew, with what it found
        /// in `found` either way.
        static bool checkHolds(const FrameContext& context, const FrameReport& report, Rtx::Check check,
            const StopFacts& facts, std::string& found);

        /// What a read back lands in, refilled per stop and never freed.
        std::vector<std::uint8_t> mPixels;
    };
}
