#pragma once

#include <cstddef>
#include <functional>
#include <unordered_map>

#include <components/vfs/pathutil.hpp>

#include "alphaimage.hpp"
#include "texels.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// The mean texel of every map this has been asked about, kept for the life of the process
    /// and keyed by the file, because a file's mean never changes and a session meets tens of
    /// additive maps. Kept here and not beside the texture's slot, which the scene gives back when
    /// the last material naming the image goes — and a spell's map goes with every burst, so a
    /// cache that died with the slot read every texel again on the frame of the next cast. One
    /// instance a thread, like `AlphaScratch`: the ring's reader has its own.
    class MeanTexels
    {
    public:
        /// `image`'s mean, read at the first ask under its file name and found at every ask after.
        /// The reference stands for the life of this object, so a caller that asks every frame
        /// keeps it and asks once. An image that is not a file has no name to keep it under: it
        /// is read at every ask into one place the next such ask overwrites, so its reference is
        /// good until then. Nothing in the game draws an additive sheet with one.
        const MeanTexel& of(const osg::Image& image);

        std::size_t size() const { return mByFile.size(); }

    private:
        std::unordered_map<VFS::Path::Normalized, MeanTexel, VFS::Path::Hash, std::equal_to<>> mByFile;

        /// Where an unnamed image's mean is put, good until the next ask.
        MeanTexel mUnnamed;

        AlphaScratch mScratch;
    };
}
